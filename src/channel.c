/*
 *  ircd-ratbox: A slightly useful ircd.
 *  channel.c: Controls channels.
 *
 * Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center 
 * Copyright (C) 1996-2002 Hybrid Development Team 
 * Copyright (C) 2002-2005 ircd-ratbox development team 
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
 *
 *  $Id$
 */

#include "stdinc.h"
#include "struct.h"
#include "channel.h"
#include "client.h"
#include "hash.h"
#include "hook.h"
#include "match.h"
#include "ircd.h"
#include "numeric.h"
#include "s_serv.h"		/* captab */
#include "s_user.h"
#include "send.h"
#include "whowas.h"
#include "s_conf.h"		/* ConfigFileEntry, ConfigChannel */
#include "s_newconf.h"
#include "s_log.h"

struct config_channel_entry ConfigChannel;
rb_dlink_list global_channel_list;
static rb_bh *channel_heap;
static rb_bh *ban_heap;
static rb_bh *topic_heap;
static rb_bh *member_heap;
struct ev_entry *checksplit_ev;

static int channel_capabs[] = { CAP_EX, CAP_IE, CAP_IRCNET,
#ifdef ENABLE_SERVICES
	CAP_SERVICE,
#endif
	CAP_TS6
};

#define NCHCAPS         (sizeof(channel_capabs)/sizeof(int))
#define NCHCAP_COMBOS   (1 << NCHCAPS)

static struct ChCapCombo chcap_combos[NCHCAP_COMBOS];

static void free_topic(struct Channel *chptr);

/* init_channels()
 *
 * input	-
 * output	-
 * side effects - initialises the various blockheaps
 */
void
init_channels(void)
{
	channel_heap = rb_bh_create(sizeof(struct Channel), CHANNEL_HEAP_SIZE, "channel_heap");
	ban_heap = rb_bh_create(sizeof(struct Ban), BAN_HEAP_SIZE, "ban_heap");
	topic_heap = rb_bh_create(sizeof(struct topic_info), TOPIC_HEAP_SIZE, "topic_heap");
	member_heap = rb_bh_create(sizeof(struct membership), MEMBER_HEAP_SIZE, "member_heap");
}

/*
 * allocate_channel - Allocates a channel
 */
struct Channel *
allocate_channel(const char *chname)
{
	struct Channel *chptr;
	chptr = rb_bh_alloc(channel_heap);
	chptr->chname = rb_strndup(chname, CHANNELLEN);
	if (*chname == '+')
		chptr->mode.mode |= MODE_TOPICLIMIT;
	return (chptr);
}

void
free_channel(struct Channel *chptr)
{
	rb_free(chptr->chname);
	rb_bh_free(channel_heap, chptr);
}

struct Ban *
allocate_ban(const char *banstr, const char *who)
{
	struct Ban *bptr;
	bptr = rb_bh_alloc(ban_heap);
	bptr->banstr = rb_strndup(banstr, BANLEN);
	bptr->who = rb_strndup(who, BANLEN);

	return (bptr);
}

void
free_ban(struct Ban *bptr)
{
	rb_free(bptr->banstr);
	rb_free(bptr->who);
	rb_bh_free(ban_heap, bptr);
}


/* find_channel_membership()
 *
 * input	- channel to find them in, client to find
 * output	- membership of client in channel, else NULL
 * side effects	-
 */
struct membership *
find_channel_membership(struct Channel *chptr, struct Client *client_p)
{
	struct membership *msptr;
	rb_dlink_node *ptr;

	if(!IsClient(client_p))
		return NULL;

	/* Pick the most efficient list to use to be nice to things like
	 * CHANSERV which could be in a large number of channels
	 */
	if(rb_dlink_list_length(&chptr->members) < rb_dlink_list_length(&client_p->user->channel))
	{
		RB_DLINK_FOREACH(ptr, chptr->members.head)
		{
			msptr = ptr->data;

			if(msptr->client_p == client_p)
				return msptr;
		}
	}
	else
	{
		RB_DLINK_FOREACH(ptr, client_p->user->channel.head)
		{
			msptr = ptr->data;

			if(msptr->chptr == chptr)
				return msptr;
		}
	}

	return NULL;
}

/* find_channel_status()
 *
 * input	- membership to get status for, whether we can combine flags
 * output	- flags of user on channel
 * side effects -
 */
const char *
find_channel_status(struct membership *msptr, int combine)
{
	static char buffer[3];
	char *p;

	p = buffer;

	if(is_chanop(msptr))
	{
		if(!combine)
			return "@";
		*p++ = '@';
	}

	if (is_uniqop(msptr) && combine > 0)
		*p++ = '@';

	if(is_voiced(msptr))
		*p++ = '+';

	*p = '\0';
	return buffer;
}

/* add_user_to_channel()
 *
 * input	- channel to add client to, client to add, channel flags
 * output	- 
 * side effects - user is added to channel
 */
void
add_user_to_channel(struct Channel *chptr, struct Client *client_p, int flags)
{
	struct membership *msptr;

	s_assert(client_p->user != NULL);
	if(client_p->user == NULL)
		return;

	msptr = rb_bh_alloc(member_heap);

	msptr->chptr = chptr;
	msptr->client_p = client_p;
	msptr->flags = flags;

	rb_dlinkAdd(msptr, &msptr->usernode, &client_p->user->channel);
	rb_dlinkAdd(msptr, &msptr->channode, &chptr->members);

	if(MyClient(client_p))
		rb_dlinkAdd(msptr, &msptr->locchannode, &chptr->locmembers);
}

/* remove_user_from_channel()
 *
 * input	- membership pointer to remove from channel
 * output	-
 * side effects - membership (thus user) is removed from channel
 */
void
remove_user_from_channel(struct membership *msptr)
{
	struct Client *client_p;
	struct Channel *chptr;
	s_assert(msptr != NULL);
	if(msptr == NULL)
		return;

	client_p = msptr->client_p;
	chptr = msptr->chptr;

	if (!MyConnect(client_p) && (is_chanop(msptr) || !ConfigChannel.no_opless_hack)) {
		chptr->reop = rb_current_time();
		if (client_p->flags & FLAGS_KILLED)
			chptr->chlock = chptr->reop;
	}

	rb_dlinkDelete(&msptr->usernode, &client_p->user->channel);
	rb_dlinkDelete(&msptr->channode, &chptr->members);

	if(client_p->servptr == &me)
		rb_dlinkDelete(&msptr->locchannode, &chptr->locmembers);

	if(rb_dlink_list_length(&chptr->members) <= 0)
		destroy_channel(chptr);

	rb_bh_free(member_heap, msptr);

	return;
}

/* remove_user_from_channels()
 *
 * input        - user to remove from all channels
 * output       -
 * side effects - user is removed from all channels
 */
void
remove_user_from_channels(struct Client *client_p)
{
	struct Channel *chptr;
	struct membership *msptr;
	rb_dlink_node *ptr;
	rb_dlink_node *next_ptr;

	if(client_p == NULL)
		return;

	RB_DLINK_FOREACH_SAFE(ptr, next_ptr, client_p->user->channel.head)
	{
		msptr = ptr->data;
		chptr = msptr->chptr;

		rb_dlinkDelete(&msptr->channode, &chptr->members);

		if(client_p->servptr == &me)
			rb_dlinkDelete(&msptr->locchannode, &chptr->locmembers);

		if(rb_dlink_list_length(&chptr->members) <= 0)
			destroy_channel(chptr);

		rb_bh_free(member_heap, msptr);
	}

	client_p->user->channel.head = client_p->user->channel.tail = NULL;
	client_p->user->channel.length = 0;
}

/* invalidate_bancache_user()
 *
 * input	- user to invalidate ban cache for
 * output	-
 * side effects - ban cache is invalidated for all memberships of that user
 *                to be used after a nick change
 */
void
invalidate_bancache_user(struct Client *client_p)
{
	struct membership *msptr;
	rb_dlink_node *ptr;

	if(client_p == NULL)
		return;

	RB_DLINK_FOREACH(ptr, client_p->user->channel.head)
	{
		msptr = ptr->data;
		msptr->ban_serial = 0;
		msptr->flags &= ~CHFL_BANNED;
	}
}

/* get_channelmask()
 *
 * input	- channel name
 * output	- mask string ptr, or NULL if not masked
 * side effects -
 */
const char	*get_channelmask(const char *chname)
{
	char	*mask;

	mask = rindex(chname, ':');
	if (!mask || index(mask, '\033'))
	{
		/* If '\033' is in the mask, well, it's not a real mask,
		** but a JIS encoded channel name. --Beeth */
		return NULL;
	}
	return mask+1;
}

/* channel_cacheflags()
 *
 * Cache the fact that the channel is masked and/or in JIS encoding
 *
 * input	- chptr
 * output	- 
 * side effects - mode.mode updated as appropiate
 */
void channel_cacheflags(struct Channel *chptr)
{
	const char *mask = get_channelmask(chptr->chname);
	char *p;

	/* broken mask -> its a JIS encoded channel */
	if (!mask && rindex(chptr->chname, ':'))
		chptr->info |= CHINFO_JIS;

	if ((p = strchr(chptr->chname, ',')))
		chptr->info |= CHINFO_JIS;
	if (mask)
		chptr->info |= CHINFO_MASKED;
}

/* check_channel_burst()
 *
 * Check if it's ok to burst the channel to a particular server.
 *
 * input	- server to check against and channel name possibly including mask
 * output	- 1 if its ok to send (server is matching channel mask), 0 if not
 * side effects -
 */
int check_channel_burst(struct Client *client_p, struct Channel *chptr)
{
	const char *mask;

	/* do not bother with local channels */
	if (!IsRemoteChannel(chptr->chname))
		return 0;

	/* channel in JIS encoding, yet the target is not capable of handling it */
	if (IsJISChannel(chptr) && NotCapable(client_p, CAP_JAPANESE))
		return 0;

	if (IsMaskedChannel(chptr)) {
		mask  = get_channelmask(chptr->chname);
		/* no mask, its ok to send. */
		if (!mask)
			return 1;
		return match(mask, client_p->name);
	}
	return 1;
}

/* channel_tok()
 *
 * input	- comma separated channel list
 * output	- pointer to next channel, or NULL in case of last channel
 * side effects - channel name in the input will be zero-terminated at place of separator
 */
char *channel_tok(char *name)
{
	int flag = 1;
	for(; *name && (!(*name == ',' && flag)); ++name)
	{
		/* \e$B start of JIS (flag=0) ending in the middle is an error),
		 * \e(B end of JIS (flag=1) */
		if (*name == '\033' &&
			(name[1] == '$' || name[1] == '(') &&
			name[2] == 'B')
		{
			flag = (name[1] == '$') ? 0 : 1;
			name += 2;
		}
	}

	if (*name == ',') {
		*name++ = 0;
		/* ignore empty tokens */
		if (!*name) return NULL;
		return name;
	}
	return NULL;
}


/* check_channel_name()
 *
 * Check if a client is allowed to create such a channel.
 *
 * input	- channel name
 * output	- 1 if valid channel name, else 0
 * side effects -
 */
int
check_channel_name(const char *name)
{
	int flag = 1;
	const char *mask;

	s_assert(name != NULL);
	if(name == NULL)
		return 0;

	mask = get_channelmask(name);

	for(; *name; ++name)
	{
		if(!IsChanChar(*name) && (flag && *name == ','))
			return 0;
		/* \e$B start of JIS (flag=0) ending in the middle is an error),
		 * \e$(B end of JIS (flag=1) */
		if (*name == '\033' &&
			(name[1] == '$' || name[1] == '(') &&
			name[2] == 'B')
		{
			flag = (name[1] == '$') ? 0 : 1;
			name += 2;
		}
	}

	if (mask && (!*mask || !match(mask, me.name)))
		return 0;

	return flag;
}

/* free_channel_list()
 *
 * input	- dlink list to free
 * output	-
 * side effects - list of b/e/I modes is cleared
 */
void
free_channel_list(rb_dlink_list *list)
{
	rb_dlink_node *ptr;
	rb_dlink_node *next_ptr;
	struct Ban *actualBan;

	RB_DLINK_FOREACH_SAFE(ptr, next_ptr, list->head)
	{
		actualBan = ptr->data;
		free_ban(actualBan);
	}

	list->head = list->tail = NULL;
	list->length = 0;
}

void
kill_channel_modes(struct Channel *chptr)
{
	rb_dlink_node *ptr, *next_ptr;

	chptr->mode.mode = 0;
	RB_DLINK_FOREACH_SAFE(ptr, next_ptr, chptr->invites.head)
	{
		del_invite(chptr, ptr->data);
	}

	/* free all bans/exceptions/denies */
	free_channel_list(&chptr->banlist);
	free_channel_list(&chptr->exceptlist);
	free_channel_list(&chptr->invexlist);
}

/* destroy_channel()
 *
 * input	- channel to destroy
 * output	-
 * side effects - channel is obliterated
 */
void
destroy_channel(struct Channel *chptr)
{

	/* CHANDELAY: will be destroyed by the timer eventually */
	if (IsSCH(chptr) || HasHistory(chptr))
		return;

	kill_channel_modes(chptr);

	/* Free the topic */
	free_topic(chptr);

	rb_dlinkDelete(&chptr->node, &global_channel_list);
	del_from_hash(HASH_CHANNEL, chptr->chname, chptr);
	free_channel(chptr);
}

/* channel_pub_or_secret()
 *
 * input	- channel
 * output	- "=" if public, "@" if secret, else "*"
 * side effects	-
 */
static const char *
channel_pub_or_secret(struct Channel *chptr)
{
	if(PubChannel(chptr))
		return ("=");
	else if(SecretChannel(chptr))
		return ("@");
	return ("*");
}

/* channel_member_names()
 *
 * input	- channel to list, client to list to, show endofnames
 * output	-
 * side effects - client is given list of users on channel
 */
void
channel_member_names(struct Channel *chptr, struct Client *client_p, int show_eon)
{
	struct membership *msptr;
	struct Client *target_p;
	rb_dlink_node *ptr;
	char lbuf[BUFSIZE];
	char *t;
	int mlen;
	int tlen;
	int cur_len;
	int is_member;
	int stack = IsCapable(client_p, CLICAP_MULTI_PREFIX);
	SetCork(client_p);
	if(ShowChannel(client_p, chptr))
	{
		is_member = IsMember(client_p, chptr);

		cur_len = mlen = rb_sprintf(lbuf, form_str(RPL_NAMREPLY),
					    me.name, client_p->name,
					    channel_pub_or_secret(chptr), chptr->chname);

		t = lbuf + cur_len;

		RB_DLINK_FOREACH(ptr, chptr->members.head)
		{
			msptr = ptr->data;
			target_p = msptr->client_p;

			if(IsInvisible(target_p) && !is_member)
				continue;

			if (IsAnonymous(chptr) && (target_p != client_p))
				continue;

			/* space, possible "@+" prefix */
			if(cur_len + strlen(target_p->name) + 3 >= BUFSIZE - 3)
			{
				*(t - 1) = '\0';
				sendto_one_buffer(client_p, lbuf);
				cur_len = mlen;
				t = lbuf + mlen;
			}

			tlen = rb_sprintf(t, "%s%s ", find_channel_status(msptr, stack),
					  target_p->name);

			cur_len += tlen;
			t += tlen;
		}

		/* The old behaviour here was to always output our buffer,
		 * even if there are no clients we can show.  This happens
		 * when a client does "NAMES" with no parameters, and all
		 * the clients on a -sp channel are +i.  I dont see a good
		 * reason for keeping that behaviour, as it just wastes
		 * bandwidth.  --anfl
		 */
		if(cur_len != mlen)
		{
			*(t - 1) = '\0';
			sendto_one_buffer(client_p, lbuf);
		}
	}

	if(show_eon)
		sendto_one(client_p, form_str(RPL_ENDOFNAMES),
			   me.name, client_p->name, chptr->chname);
	ClearCork(client_p);
	send_pop_queue(client_p);
}

/* del_invite()
 *
 * input	- channel to remove invite from, client to remove
 * output	-
 * side effects - user is removed from invite list, if exists
 */
void
del_invite(struct Channel *chptr, struct Client *who)
{
	rb_dlinkFindDestroy(who, &chptr->invites);
	rb_dlinkFindDestroy(chptr, &who->localClient->invited);
}

/*
 * match_ban()
 *
 * input  - banlist (&chptr->banlist etc), 'who' is to be matched
 *	    if 'nuhs' is specified it must point to buffer of MATCHBANSZ
 *	    'init' being non-zero initializes the cache, otherwise values
 *	    from previous call are used.
 * output - the ban matched by the user 'who'
 *
 */
struct	Ban *match_ban(rb_dlink_list *bl, struct Client *who, char *nuhs, int init)
{
	char my_nuhs[MATCHBANSZ];
	struct Ban *ban;
	rb_dlink_node *ptr;

	if (!nuhs) {
		nuhs = my_nuhs;
		init = 1;
	}

 	if (init) {
		rb_sprintf(&nuhs[0], "%s!%s@%s", who->name, who->username, who->host);
		rb_sprintf(&nuhs[USERHOST_REPLYLEN], "%s!%s@%s", who->name, who->username, who->sockhost);
		rb_sprintf(&nuhs[USERHOST_REPLYLEN*2], "%s!%s@%s", who->id, who->username, who->host);
		rb_sprintf(&nuhs[USERHOST_REPLYLEN*3], "%s!%s@%s", who->id, who->username, who->sockhost);
	}
	
	RB_DLINK_FOREACH(ptr, bl->head) {
		int i;
		ban = ptr->data;
		for (i = 0; i < 4; i++)
			if (match(ban->banstr, &nuhs[i*USERHOST_REPLYLEN]))
				return ban;
	};
	return NULL;
}

/* is_banned()
 *
 * input	- channel to check bans for, user to check bans against
 *                optional prebuilt buffers
 * output	- 1 if banned, else 0
 * side effects -
 */
int
is_banned(struct Channel *chptr, struct Client *who, struct membership *msptr)
{
	char bufmb[MATCHBANSZ];

	if(!MyClient(who))
		return 0;

	if (match_ban(&chptr->banlist, who, bufmb, 1)) {
		if (ConfigChannel.use_except && match_ban(&chptr->exceptlist, who, bufmb, 0)) {
			if (msptr)
				msptr->flags &= ~CHFL_BANNED;
			return CHFL_EXCEPTION;
		}
		if (msptr)
			msptr->flags |= CHFL_BANNED;
		return CHFL_BAN;
	}
	return 0;
}

/* can_send()
 *
 * input	- user to check in channel, membership pointer
 * output	- whether can explicitly send or not, else CAN_SEND_NONOP
 * side effects -
 */
int
can_send(struct Channel *chptr, struct Client *source_p, struct membership *msptr)
{
	if(IsServer(source_p))
		return CAN_SEND_OPV;

	if(MyClient(source_p) && hash_find_resv(chptr->chname) &&
	   !IsOper(source_p) && !IsExemptResv(source_p))
		return CAN_SEND_NO;

	if(msptr == NULL)
	{
		msptr = find_channel_membership(chptr, source_p);

		if(msptr == NULL)
		{
			/* if its +m or +n and theyre not in the channel,
			 * they cant send.  we dont check bans here because
			 * theres no possibility of caching them --fl
			 */
			if(chptr->mode.mode & MODE_NOPRIVMSGS || chptr->mode.mode & MODE_MODERATED)
				return CAN_SEND_NO;
			else
				return CAN_SEND_NONOP;
		}
	}

	if(is_chanop_voiced(msptr))
		return CAN_SEND_OPV;

	if(chptr->mode.mode & MODE_MODERATED)
		return CAN_SEND_NO;

	if(ConfigChannel.quiet_on_ban && MyClient(source_p))
	{
		/* cached can_send */
		if(msptr->ban_serial == chptr->ban_serial)
		{
			if(can_send_banned(msptr))
				return CAN_SEND_NO;
		}
		else if(is_banned(chptr, source_p, msptr) == CHFL_BAN)
			return CAN_SEND_NO;
	}

	return CAN_SEND_NONOP;
}

/* void check_spambot_warning(struct Client *source_p)
 * Input: Client to check, channel name or NULL if this is a part.
 * Output: none
 * Side-effects: Updates the client's oper_warn_count_down, warns the
 *    IRC operators if necessary, and updates join_leave_countdown as
 *    needed.
 */
void
check_spambot_warning(struct Client *source_p, const char *name)
{
	int t_delta;
	int decrement_count;
	if((GlobalSetOptions.spam_num &&
	    (source_p->localClient->join_leave_count >= GlobalSetOptions.spam_num)))
	{
		if(source_p->localClient->oper_warn_count_down > 0)
			source_p->localClient->oper_warn_count_down--;
		else
			source_p->localClient->oper_warn_count_down = 0;
		if(source_p->localClient->oper_warn_count_down == 0)
		{
			/* Its already known as a possible spambot */
			if(name != NULL)
				sendto_realops_flags(UMODE_BOTS, L_ALL,
						     "User %s (%s@%s) trying to join %s is a possible spambot",
						     source_p->name,
						     source_p->username, source_p->host, name);
			else
				sendto_realops_flags(UMODE_BOTS, L_ALL,
						     "User %s (%s@%s) is a possible spambot",
						     source_p->name,
						     source_p->username, source_p->host);
			source_p->localClient->oper_warn_count_down = OPER_SPAM_COUNTDOWN;
		}
	}
	else
	{
		if((t_delta =
		    (rb_current_time() - source_p->localClient->last_leave_time)) >
		   JOIN_LEAVE_COUNT_EXPIRE_TIME)
		{
			decrement_count = (t_delta / JOIN_LEAVE_COUNT_EXPIRE_TIME);
			if(decrement_count > source_p->localClient->join_leave_count)
				source_p->localClient->join_leave_count = 0;
			else
				source_p->localClient->join_leave_count -= decrement_count;
		}
		else
		{
			if((rb_current_time() -
			    (source_p->localClient->last_join_time)) < GlobalSetOptions.spam_time)
			{
				/* oh, its a possible spambot */
				source_p->localClient->join_leave_count++;
			}
		}
		if(name != NULL)
			source_p->localClient->last_join_time = rb_current_time();
		else
			source_p->localClient->last_leave_time = rb_current_time();
	}
}

/* check_splitmode()
 *
 * input	-
 * output	-
 * side effects - compares usercount and servercount against their split
 *                values and adjusts splitmode accordingly
 */
void
check_splitmode(void *unused)
{
	if(splitchecking && (ConfigChannel.no_join_on_split || ConfigChannel.no_create_on_split || ConfigChannel.no_ops_on_split))
	{
		/* not split, we're being asked to check now because someone
		 * has left
		 */
		if(!splitmode)
		{
			if(eob_count < split_servers || Count.total < split_users)
			{
				splitmode = 1;
				sendto_realops_flags(UMODE_ALL, L_ALL,
						     "Network split, activating splitmode");
				checksplit_ev =
					rb_event_addish("check_splitmode", check_splitmode, NULL,
							5);
			}
		}
		/* in splitmode, check whether its finished */
		else if(eob_count >= split_servers && Count.total >= split_users)
		{
			splitmode = 0;

			sendto_realops_flags(UMODE_ALL, L_ALL,
					     "Network rejoined, deactivating splitmode");

			rb_event_delete(checksplit_ev);
			checksplit_ev = NULL;
		}
	}
}


/* allocate_topic()
 *
 * input	- channel to allocate topic for
 * output	- 1 on success, else 0
 * side effects - channel gets a topic allocated
 */
static void
allocate_topic(struct Channel *chptr)
{
	if(chptr == NULL)
		return;

	chptr->topic = rb_bh_alloc(topic_heap);
}

/* free_topic()
 *
 * input	- channel which has topic to free
 * output	-
 * side effects - channels topic is free'd
 */
static void
free_topic(struct Channel *chptr)
{
	if(chptr == NULL || chptr->topic == NULL)
		return;

	/* This is safe for now - If you change allocate_topic you
	 * MUST change this as well
	 */
	rb_free(chptr->topic->topic);
	rb_bh_free(topic_heap, chptr->topic);
	chptr->topic = NULL;
}

/* set_channel_topic()
 *
 * input	- channel, topic to set, topic info and topic ts
 * output	-
 * side effects - channels topic, topic info and TS are set.
 */
void
set_channel_topic(struct Channel *chptr, const char *topic, const char *topic_info, time_t topicts)
{
	if(strlen(topic) > 0)
	{
		if(chptr->topic == NULL)
			allocate_topic(chptr);
		else
			rb_free(chptr->topic->topic);

		chptr->topic->topic = rb_strndup(topic, ConfigChannel.topiclen + 1);	/* the + 1 for the \0 */
		rb_strlcpy(chptr->topic->topic_info, topic_info, sizeof(chptr->topic->topic_info));
		chptr->topic->topic_time = topicts;
	}
	else
	{
		if(chptr->topic != NULL)
			free_topic(chptr);
	}
}

/* channel_modes()
 *
 * input	- channel, client to build for, modebufs to build to
 * output	-
 * side effects - user gets list of "simple" modes based on channel access.
 *                NOTE: m_join.c depends on trailing spaces in pbuf
 */
const char *
channel_modes(struct Channel *chptr, struct Client *client_p)
{
	static char buf[BUFSIZE];
	char *mbuf = buf;

	if (*chptr->chname == '+')
		return "+";

	*mbuf++ = '+';

	if(chptr->mode.mode & MODE_SECRET)
		*mbuf++ = 's';
	if(chptr->mode.mode & MODE_PRIVATE)
		*mbuf++ = 'p';
	if(chptr->mode.mode & MODE_MODERATED)
		*mbuf++ = 'm';
	if(chptr->mode.mode & MODE_TOPICLIMIT)
		*mbuf++ = 't';
	if(chptr->mode.mode & MODE_INVITEONLY)
		*mbuf++ = 'i';
	if(chptr->mode.mode & MODE_NOPRIVMSGS)
		*mbuf++ = 'n';
	if(chptr->mode.mode & (MODE_REGONLY|MODE_REOP))
		*mbuf++ = 'r';
	if(chptr->mode.mode & MODE_SSLONLY)
		*mbuf++ = 'S';
	if(chptr->mode.mode & MODE_ANONYMOUS)
		*mbuf++ = 'a';

	if(chptr->mode.limit && *chptr->mode.key)
	{
		if(IsMe(client_p) || !MyClient(client_p) || IsMember(client_p, chptr))
			rb_sprintf(mbuf, "lk %d %s", chptr->mode.limit, chptr->mode.key);
		else
			strcpy(mbuf, "lk");
	}
	else if(chptr->mode.limit)
	{
		if(IsMe(client_p) || !MyClient(client_p) || IsMember(client_p, chptr))
			rb_sprintf(mbuf, "l %d", chptr->mode.limit);
		else
			strcpy(mbuf, "l");
	}
	else if(*chptr->mode.key)
	{
		if(IsMe(client_p) || !MyClient(client_p) || IsMember(client_p, chptr))
			rb_sprintf(mbuf, "k %s", chptr->mode.key);
		else
			strcpy(mbuf, "k");
	}
	else
		*mbuf = '\0';

	return buf;
}

/* Now lets do some stuff to keep track of what combinations of
 * servers exist...
 * Note that the number of combinations doubles each time you add
 * something to this list. Each one is only quick if no servers use that
 * combination, but if the numbers get too high here MODE will get too
 * slow. I suggest if you get more than 7 here, you consider getting rid
 * of some and merging or something. If it wasn't for irc+cs we would
 * probably not even need to bother about most of these, but unfortunately
 * we do. -A1kmm
 */

/* void init_chcap_usage_counts(void)
 *
 * Inputs	- none
 * Output	- none
 * Side-effects	- Initialises the usage counts to zero. Fills in the
 *                chcap_yes and chcap_no combination tables.
 */
void
init_chcap_usage_counts(void)
{
	unsigned long m, c, y, n;

	memset(chcap_combos, 0, sizeof(chcap_combos));

	/* For every possible combination */
	for(m = 0; m < NCHCAP_COMBOS; m++)
	{
		/* Check each capab */
		for(c = y = n = 0; c < NCHCAPS; c++)
		{
			if((m & (1 << c)) == 0)
				n |= channel_capabs[c];
			else
				y |= channel_capabs[c];
		}
		chcap_combos[m].cap_yes = y;
		chcap_combos[m].cap_no = n;
	}
}

/* void set_chcap_usage_counts(struct Client *serv_p)
 * Input: serv_p; The client whose capabs to register.
 * Output: none
 * Side-effects: Increments the usage counts for the correct capab
 *               combination.
 */
void
set_chcap_usage_counts(struct Client *serv_p)
{
	int n;

	for(n = 0; n < NCHCAP_COMBOS; n++)
	{
		if(IsCapable(serv_p, chcap_combos[n].cap_yes) &&
		   NotCapable(serv_p, chcap_combos[n].cap_no))
		{
			chcap_combos[n].count++;
			return;
		}
	}

	/* This should be impossible -A1kmm. */
	s_assert(0);
}

/* void set_chcap_usage_counts(struct Client *serv_p)
 *
 * Inputs	- serv_p; The client whose capabs to register.
 * Output	- none
 * Side-effects	- Decrements the usage counts for the correct capab
 *                combination.
 */
void
unset_chcap_usage_counts(struct Client *serv_p)
{
	int n;

	for(n = 0; n < NCHCAP_COMBOS; n++)
	{
		if(IsCapable(serv_p, chcap_combos[n].cap_yes) &&
		   NotCapable(serv_p, chcap_combos[n].cap_no))
		{
			/* Hopefully capabs can't change dynamically or anything... */
			s_assert(chcap_combos[n].count > 0);

			if(chcap_combos[n].count > 0)
				chcap_combos[n].count--;
			return;
		}
	}

	/* This should be impossible -A1kmm. */
	s_assert(0);
}

/* void send_cap_mode_changes(struct Client *client_p,
 *                        struct Client *source_p,
 *                        struct Channel *chptr, int cap, int nocap)
 * Input: The client sending(client_p), the source client(source_p),
 *        the channel to send mode changes for(chptr)
 * Output: None.
 * Side-effects: Sends the appropriate mode changes to capable servers.
 *
 * Reverted back to my original design, except that we now keep a count
 * of the number of servers which each combination as an optimisation, so
 * the capabs combinations which are not needed are not worked out. -A1kmm
 */
void
send_cap_mode_changes(struct Client *client_p, struct Client *source_p,
		      struct Channel *chptr, struct ChModeChange mode_changes[], int mode_count)
{
	static char modebuf[BUFSIZE];
	static char parabuf[BUFSIZE];
	int i, mbl, pbl, nc, mc, preflen, len;
	char *pbuf;
	const char *arg;
	int dir;
	int j;
	int cap;
	int nocap;
	int arglen;

	/* Now send to servers... */
	for(j = 0; j < NCHCAP_COMBOS; j++)
	{
		if(chcap_combos[j].count == 0)
			continue;

		mc = 0;
		nc = 0;
		pbl = 0;
		parabuf[0] = 0;
		pbuf = parabuf;
		dir = MODE_QUERY;

		cap = chcap_combos[j].cap_yes;
		nocap = chcap_combos[j].cap_no;

		mbl = preflen = rb_sprintf(modebuf, ":%s TMODE %ld %s ",
						   source_p->id, (long)chptr->channelts,
						   chptr->chname);

		/* loop the list of - modes we have */
		for(i = 0; i < mode_count; i++)
		{
			/* if they dont support the cap we need, or they do support a cap they
			 * cant have, then dont add it to the modebuf.. that way they wont see
			 * the mode
			 */
			if((mode_changes[i].letter == 0) ||
			   ((cap & mode_changes[i].caps) != mode_changes[i].caps)
			   || ((nocap & mode_changes[i].nocaps) != mode_changes[i].nocaps))
				continue;

			if(!EmptyString(mode_changes[i].id))
				arg = mode_changes[i].id;
			else
				arg = mode_changes[i].arg;

			if(arg)
			{
				arglen = strlen(arg);

				/* dont even think about it! --fl */
				if(arglen > MODEBUFLEN - 5)
					continue;
			}

			/* if we're creeping past the buf size, we need to send it and make
			 * another line for the other modes
			 * XXX - this could give away server topology with uids being
			 * different lengths, but not much we can do, except possibly break
			 * them as if they were the longest of the nick or uid at all times,
			 * which even then won't work as we don't always know the uid -A1kmm.
			 */
			if(arg && ((mc == MAXMODEPARAMSSERV) ||
				   ((mbl + pbl + arglen + 4) > (BUFSIZE - 3))))
			{
				if(nc != 0) {
					sendto_server(client_p, chptr, cap, nocap|CAP_211,
						      "%s %s", modebuf, parabuf);
#ifdef COMPAT_211
					sendto_server(client_p, chptr, cap|CAP_211, nocap, ":%s MODE %s %s %s",
						source_p->id, chptr->chname, modebuf + preflen, parabuf);
#endif
				}
				nc = 0;
				mc = 0;

				mbl = preflen;
				pbl = 0;
				pbuf = parabuf;
				parabuf[0] = 0;
				dir = MODE_QUERY;
			}

			if(dir != mode_changes[i].dir)
			{
				modebuf[mbl++] = (mode_changes[i].dir == MODE_ADD) ? '+' : '-';
				dir = mode_changes[i].dir;
			}

			modebuf[mbl++] = mode_changes[i].letter;
			modebuf[mbl] = 0;
			nc++;

			if(arg != NULL)
			{
				len = rb_sprintf(pbuf, "%s ", arg);
				pbuf += len;
				pbl += len;
				mc++;
			}
		}

		if(pbl && parabuf[pbl - 1] == ' ')
			parabuf[pbl - 1] = 0;

		if(nc != 0) {
			sendto_server(client_p, chptr, cap, nocap|CAP_211, "%s %s", modebuf, parabuf);
#ifdef COMPAT_211
			sendto_server(client_p, chptr, cap|CAP_211, nocap, ":%s MODE %s %s %s",
				source_p->id, chptr->chname, modebuf + preflen, parabuf);
#endif
		}
	}
}

/* try to find the least idle client and give it +o */
static void	reop_channel(struct Channel *chptr)
{
	struct membership *msptr;
	struct membership *notidler = NULL;
	struct membership *matched = NULL;
	char enforcing = 'r';
	rb_dlink_node *ptr;

	RB_DLINK_FOREACH(ptr, chptr->members.head)
	{
		msptr = ptr->data;
		if (is_chanop(msptr)) {
			chptr->chlock = chptr->reop = 0;
			/* no reop needed */
			return;
		}
		if (!MyClient(msptr->client_p))
			continue;
		if (!notidler ||
			notidler->client_p->localClient->lasttime < msptr->client_p->localClient->lasttime)
		{
			notidler = msptr;
		}

		if ((!matched ||
			(matched->client_p->localClient->lasttime < msptr->client_p->localClient->lasttime)) &&
			match_ban(&chptr->reoplist, msptr->client_p, NULL, 0))
		{
			matched = msptr;
		}
	}
	if (matched) {
		enforcing = 'R';
	} else if (chptr->mode.mode & MODE_REOP) {
		matched = notidler;
	}
	if (matched) {
		sendto_channel_flags(&me, ALL_MEMBERS, &me, chptr, "NOTICE %s :Enforcing channel mode +%c (%ld)",
			chptr->chname, enforcing, rb_current_time() - chptr->reop);
		matched->flags |= CHFL_CHANOP;
		sendto_channel_local(ALL_MEMBERS, chptr, ":%s MODE %s +o %s",
			me.name, chptr->chname, Anon(matched->client_p->name));
		sendto_server(&me, chptr, CAP_TS6, NOCAPS, ":%s TMODE %ld %s +o %s",
			me.id, (long)chptr->channelts, chptr->chname, matched->client_p->name);
		chptr->reop = chptr->chlock = 0;
		return;
	}
	return;
}

void	expire_chandelay(void *unused)
{
	rb_dlink_node *ptr, *next_ptr;
	RB_DLINK_FOREACH_SAFE(ptr, next_ptr, global_channel_list.head)
	{
		struct Channel *chptr = ptr->data;

		if (rb_dlink_list_length(&chptr->members) <= 0)
			destroy_channel(chptr);
		else if (ConfigChannel.reop) {
			/* if ops were lost in netsplit, wait for delay to expire. */
			if (chptr->reop &&
				(chptr->reop + ConfigChannel.reop < rb_current_time()) &&
				!HasHistory(chptr))
			{
				reop_channel(chptr);
			}
		}
	}
}

