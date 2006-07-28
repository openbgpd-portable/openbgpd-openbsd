/*	$OpenBSD: session.h,v 1.85 2006/07/28 15:04:34 henning Exp $ */

/*
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>

#define	MAX_BACKLOG			5
#define	INTERVAL_CONNECTRETRY		120
#define	INTERVAL_HOLD_INITIAL		240
#define	INTERVAL_HOLD			90
#define	INTERVAL_IDLE_HOLD_INITIAL	30
#define	INTERVAL_HOLD_CLONED		3600
#define	INTERVAL_HOLD_DEMOTED		60
#define	MAX_IDLE_HOLD			3600
#define	MSGSIZE_HEADER			19
#define	MSGSIZE_HEADER_MARKER		16
#define	MSGSIZE_NOTIFICATION_MIN	21	/* 19 hdr + 1 code + 1 sub */
#define	MSGSIZE_OPEN_MIN		29
#define	MSGSIZE_UPDATE_MIN		23
#define	MSGSIZE_KEEPALIVE		MSGSIZE_HEADER
#define	MSGSIZE_RREFRESH		MSGSIZE_HEADER + 4
#define	MSG_PROCESS_LIMIT		25
#define	SESSION_CLEAR_DELAY		5

enum session_state {
	STATE_NONE,
	STATE_IDLE,
	STATE_CONNECT,
	STATE_ACTIVE,
	STATE_OPENSENT,
	STATE_OPENCONFIRM,
	STATE_ESTABLISHED
};

enum session_events {
	EVNT_NONE,
	EVNT_START,
	EVNT_STOP,
	EVNT_CON_OPEN,
	EVNT_CON_CLOSED,
	EVNT_CON_OPENFAIL,
	EVNT_CON_FATAL,
	EVNT_TIMER_CONNRETRY,
	EVNT_TIMER_HOLDTIME,
	EVNT_TIMER_KEEPALIVE,
	EVNT_RCVD_OPEN,
	EVNT_RCVD_KEEPALIVE,
	EVNT_RCVD_UPDATE,
	EVNT_RCVD_NOTIFICATION
};

enum blockmodes {
	BM_NORMAL,
	BM_NONBLOCK
};

enum msg_type {
	OPEN = 1,
	UPDATE,
	NOTIFICATION,
	KEEPALIVE,
	RREFRESH
};

enum suberr_header {
	ERR_HDR_SYNC = 1,
	ERR_HDR_LEN,
	ERR_HDR_TYPE
};

enum suberr_open {
	ERR_OPEN_VERSION = 1,
	ERR_OPEN_AS,
	ERR_OPEN_BGPID,
	ERR_OPEN_OPT,
	ERR_OPEN_AUTH,
	ERR_OPEN_HOLDTIME,
	ERR_OPEN_CAPA
};

enum opt_params {
	OPT_PARAM_NONE,
	OPT_PARAM_AUTH,
	OPT_PARAM_CAPABILITIES
};

enum capa_codes {
	CAPA_NONE,
	CAPA_MP,
	CAPA_REFRESH
};

struct bgp_msg {
	struct buf	*buf;
	enum msg_type	 type;
	u_int16_t	 len;
};

struct msg_header {
	u_char			 marker[MSGSIZE_HEADER_MARKER];
	u_int16_t		 len;
	u_int8_t		 type;
};

struct msg_open {
	struct msg_header	 header;
	u_int32_t		 bgpid;
	u_int16_t		 myas;
	u_int16_t		 holdtime;
	u_int8_t		 version;
	u_int8_t		 optparamlen;
};

struct bgpd_sysdep {
	u_int8_t		no_pfkey;
	u_int8_t		no_md5sig;
};

struct ctl_conn {
	TAILQ_ENTRY(ctl_conn)	entry;
	struct imsgbuf		ibuf;
	int			restricted;
};

TAILQ_HEAD(ctl_conns, ctl_conn)	ctl_conns;

struct peer_stats {
	u_int64_t		 msg_rcvd_open;
	u_int64_t		 msg_rcvd_update;
	u_int64_t		 msg_rcvd_notification;
	u_int64_t		 msg_rcvd_keepalive;
	u_int64_t		 msg_rcvd_rrefresh;
	u_int64_t		 msg_sent_open;
	u_int64_t		 msg_sent_update;
	u_int64_t		 msg_sent_notification;
	u_int64_t		 msg_sent_keepalive;
	u_int64_t		 msg_sent_rrefresh;
	time_t			 last_updown;
	time_t			 last_read;
	u_int32_t		 prefix_cnt;
	u_int8_t		 last_sent_errcode;
	u_int8_t		 last_sent_suberr;
};

struct peer {
	struct peer_config	 conf;
	struct peer_stats	 stats;
	struct {
		struct capabilities	ann;
		struct capabilities	peer;
	}			 capa;
	struct sockaddr_storage	 sa_local;
	struct sockaddr_storage	 sa_remote;
	struct msgbuf		 wbuf;
	struct buf_read		*rbuf;
	struct peer		*next;
	time_t			 ConnectRetryTimer;
	time_t			 KeepaliveTimer;
	time_t			 HoldTimer;
	time_t			 IdleHoldTimer;
	time_t			 IdleHoldResetTimer;
	int			 fd;
	int			 lasterr;
	u_int			 errcnt;
	u_int			 IdleHoldTime;
	u_int32_t		 remote_bgpid;
	enum session_state	 state;
	enum session_state	 prev_state;
	u_int16_t		 holdtime;
	u_int8_t		 auth_established;
	u_int8_t		 depend_ok;
	u_int8_t		 demoted;
	u_int8_t		 passive;
};

struct peer	*peers;

/* session.c */
void		 session_socket_blockmode(int, enum blockmodes);
pid_t		 session_main(struct bgpd_config *, struct peer *,
		    struct network_head *, struct filter_head *,
		    struct mrt_head *, int[2], int[2], int[2]);
void		 bgp_fsm(struct peer *, enum session_events);
int		 session_neighbor_rrefresh(struct peer *p);
struct peer	*getpeerbyaddr(struct bgpd_addr *);
struct peer	*getpeerbydesc(const char *);
int		 imsg_compose_parent(int, pid_t, void *, u_int16_t);
int		 imsg_compose_rde(int, pid_t, void *, u_int16_t);

/* log.c */
void		 log_statechange(struct peer *, enum session_state,
		    enum session_events);
void		 log_notification(const struct peer *, u_int8_t, u_int8_t,
		    u_char *, u_int16_t);
void		 log_conn_attempt(const struct peer *, struct sockaddr *);

/* parse.y */
int	 parse_config(char *, struct bgpd_config *, struct mrt_head *,
	    struct peer **, struct network_head *, struct filter_head *);

/* config.c */
int	 merge_config(struct bgpd_config *, struct bgpd_config *,
	    struct peer *, struct listen_addrs *);
void	 prepare_listeners(struct bgpd_config *);

/* rde.c */
pid_t	 rde_main(struct bgpd_config *, struct peer *, struct network_head *,
	    struct filter_head *, struct mrt_head *, int[2], int[2], int[2]);

/* control.c */
int	control_init(int, char *);
int	control_listen(int);
void	control_shutdown(int);
int	control_dispatch_msg(struct pollfd *, u_int *);
unsigned int	control_accept(int, int);

/* pfkey.c */
int	pfkey_establish(struct peer *);
int	pfkey_remove(struct peer *);
int	pfkey_init(struct bgpd_sysdep *);

/* printconf.c */
void	print_config(struct bgpd_config *, struct network_head *, struct peer *,
	    struct filter_head *, struct mrt_head *);

/* carp.c */
int	 carp_demote_init(char *, int);
void	 carp_demote_shutdown(void);
int	 carp_demote_get(char *);
int	 carp_demote_set(char *, int);
