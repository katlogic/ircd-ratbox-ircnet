/*
 *  ircd-ratbox: A slightly useful ircd.
 *  channel.h: The ircd channel header.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2005 ircd-ratbox development team
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

#ifndef INCLUDED_channel_h
#define INCLUDED_channel_h

#define MODEBUFLEN      200

/* Maximum mode changes allowed per client, per server is different */
#define MAXMODEPARAMS   3
#define MAXMODEPARAMSSERV 3

extern struct ev_entry *checksplit_ev;
struct Client;

/* mode structure for channels */
struct Mode
{
	unsigned int mode;
	int limit;
	char key[KEYLEN];
};

struct topic_info
{
	char *topic;
	char topic_info[USERHOST_REPLYLEN];
	time_t topic_time;
};

/* channel structure */
struct Channel
{
	rb_dlink_node node;
	struct Mode mode;
	struct topic_info *topic;
	time_t last_knock;	/* don't allow knock to flood */

	rb_dlink_list members;	/* channel members */
	rb_dlink_list locmembers;	/* local channel members */

	rb_dlink_list invites;
	rb_dlink_list banlist;
	rb_dlink_list exceptlist;
	rb_dlink_list invexlist;
	rb_dlink_list reoplist;

	time_t first_received_message_time;	/* channel flood control */
	int received_number_of_privmsgs;
	int info;				/* arbitrary info (see CHINFO_ */

	uint32_t ban_serial;
	time_t channelts;

	time_t reop;	/* since when we're considering the channel to be reopped */
	time_t chlock;	/* when some @user quitted, for chandelay */
	char *chname;
};

struct membership
{
	rb_dlink_node channode;
	rb_dlink_node locchannode;
	rb_dlink_node usernode;

	struct Channel *chptr;
	struct Client *client_p;
	uint8_t flags;

	uint32_t ban_serial;
};

#define BANLEN NICKLEN+USERLEN+HOSTLEN+6
struct Ban
{
	char *banstr;
	char *who;
	time_t when;
	rb_dlink_node node;
};

struct ChModeChange
{
	char letter;
	const char *arg;
	const char *id;
	int dir;
	int caps;
	int nocaps;
	int mems;
	struct Client *client;
};

struct ChCapCombo
{
	int count;
	int cap_yes;
	int cap_no;
};

/* can_send results */
#define CAN_SEND_NO	0
#define CAN_SEND_NONOP  1
#define CAN_SEND_OPV	2

/* channel status flags */
#define CHFL_PEON		0x0000	/* normal member of channel */
#define CHFL_CHANOP     	0x0001	/* Channel operator */
#define CHFL_VOICE      	0x0002	/* the power to speak */
#define CHFL_DEOPPED    	0x0004	/* deopped on sjoin, bounce modes */
#define CHFL_BANNED		0x0008	/* cached as banned */
#define ONLY_SERVERS		0x0010
#define CHFL_UNIQOP		0x0020	/* the user has just create a !channel */
#define ALL_MEMBERS		CHFL_PEON
#define ONLY_CHANOPS		CHFL_CHANOP
#define ONLY_CHANOPSVOICED	(CHFL_CHANOP|CHFL_VOICE)

#define is_chanop(x)	((x) && (x)->flags & CHFL_CHANOP)
#define is_uniqop(x)	((x) && ((x)->flags & (CHFL_CHANOP|CHFL_UNIQOP))==(CHFL_CHANOP|CHFL_UNIQOP))
#define is_voiced(x)	((x) && (x)->flags & CHFL_VOICE)
#define is_chanop_voiced(x) ((x) && (x)->flags & (CHFL_CHANOP|CHFL_VOICE))
#define is_deop(x)	((x) && (x)->flags & CHFL_DEOPPED)
#define can_send_banned(x) ((x) && (x)->flags & CHFL_BANNED)

/* channel modes ONLY */
#define MODE_PRIVATE    0x0001
#define MODE_SECRET     0x0002
#define MODE_MODERATED  0x0004
#define MODE_TOPICLIMIT 0x0008
#define MODE_INVITEONLY 0x0010
#define MODE_NOPRIVMSGS 0x0020
#define MODE_REGONLY	0x0040
#define MODE_REOP	0x0040
#define MODE_SSLONLY	0x0080
#define MODE_ANONYMOUS	0x0100

#define CHFL_BAN        0x0200	/* ban channel flag */
#define CHFL_EXCEPTION  0x0400	/* exception to ban channel flag */
#define CHFL_INVEX      0x0800
#define CHFL_REOP	0x1000  /* consider these hostmask for reopping */

#define CHINFO_FLOODED	0x0001 /* previously flood_noticed */
#define CHINFO_JIS	0x0002 /* the channel shoud be sent only to CAP_JAPANESE servers */
#define CHINFO_MASKED	0x0004 /* cache the fact the channel is masked */
#define CHINFO_SCH	0x0008 /* service channel; do NOT destroy (and for &channels, +i is overriden by opers) */

/* mode flags for direction indication */
#define MODE_QUERY     0
#define MODE_ADD       1
#define MODE_DEL       -1

#define IsMaskedChannel(x)	((x) && ((x)->info & CHINFO_MASKED))
#define IsJISChannel(x)		((x) && ((x)->info & CHINFO_JIS))

#define IsSCH(x)		((x) && ((x)->info & CHINFO_SCH))
#define SetChannelSCH(x)	if (x) { (x)->info |= CHINFO_SCH; }
#define UnSetChannelSCH(x)	if (x) { (x)->info &= ~CHINFO_SCH; }

#define IsChannelFlooded(x)	((x) && ((x)->info & CHINFO_FLOODED))
#define SetChannelFlooded(x)	if (x) { (x)->info |= CHINFO_FLOODED; }
#define SetChannelUnflooded(x)	if (x) { (x)->info &= ~CHINFO_FLOODED; }

#define IsAnonymous(x)          ((x) && ((x)->mode.mode & MODE_ANONYMOUS))
#define Anon(x)	(IsAnonymous(chptr)?"anonymous":(x))
#define Anonymize(chptr,n,u,h) (IsAnonymous(chptr)?"anonymous":(n)), (IsAnonymous(chptr)?"anonymous":(u)), (IsAnonymous(chptr)?"anonymous.":(h))

#define SecretChannel(x)        ((x) && ((x)->mode.mode & MODE_SECRET))
#define HiddenChannel(x)        ((x) && ((x)->mode.mode & MODE_PRIVATE))
#define PubChannel(x)           ((!x) || ((x)->mode.mode &\
				 (MODE_PRIVATE | MODE_SECRET)) == 0)

/* channel visible */
#define ShowChannel(v,c)        (PubChannel(c) || IsMember((v),(c)))

#define IsMember(who, chan) ((who && who->user && \
		find_channel_membership(chan, who)) ? 1 : 0)

#define IsChannelName(name) ((name) && IsChanPrefix(*name))
#define IsRemoteChannel(name) (IsChannelName(name) && *(name) != '&')
#define HasHistory(chptr) ((chptr->chlock + ConfigChannel.delay * ((chptr->chname[0]=='!')?3:1)) >= rb_current_time())
#define IsLocked(chptr) (*chptr->chname != '+' && HasHistory(chptr) && (rb_dlink_list_length(&chptr->members) == 0))


extern rb_dlink_list global_channel_list;
void init_channels(void);

struct Channel *allocate_channel(const char *chname);
void free_channel(struct Channel *chptr);
struct Ban *allocate_ban(const char *, const char *);
void free_ban(struct Ban *bptr);

void kill_channel_modes(struct Channel *chptr);
void destroy_channel(struct Channel *);

int can_send(struct Channel *chptr, struct Client *who, struct membership *);
int is_banned(struct Channel *chptr, struct Client *who,
	      struct membership *msptr);

struct membership *find_channel_membership(struct Channel *, struct Client *);
const char *find_channel_status(struct membership *msptr, int combine);
void add_user_to_channel(struct Channel *, struct Client *, int flags);
void remove_user_from_channel(struct membership *);
void remove_user_from_channels(struct Client *);
void invalidate_bancache_user(struct Client *);

void free_channel_list(rb_dlink_list *);

int check_channel_name(const char *name);
char *channel_tok(char *name);
int check_channelmask(struct Client *client_p, struct Channel *chptr);
const char	*get_channelmask(const char *chname);
void channel_cacheflags(struct Channel *chptr);
int check_channel_burst(struct Client *client_p, struct Channel *chptr);


void channel_member_names(struct Channel *chptr, struct Client *, int show_eon);

void del_invite(struct Channel *chptr, struct Client *who);

const char *channel_modes(struct Channel *chptr, struct Client *who);

void check_spambot_warning(struct Client *source_p, const char *name);

void check_splitmode(void *);

void set_channel_topic(struct Channel *chptr, const char *topic,
		       const char *topic_info, time_t topicts);

void init_chcap_usage_counts(void);
void set_chcap_usage_counts(struct Client *serv_p);
void unset_chcap_usage_counts(struct Client *serv_p);
void send_cap_mode_changes(struct Client *client_p, struct Client *source_p,
			   struct Channel *chptr, struct ChModeChange foo[], int);


struct	Ban *match_ban(rb_dlink_list *bl, struct Client *who, char *nuhs, int init);
void	expire_chandelay(void *unused);
#endif /* INCLUDED_channel_h */
