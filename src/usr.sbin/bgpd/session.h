/*	$OpenBSD: session.h,v 1.10 2004/01/04 20:07:30 henning Exp $ */

/*
 * Copyright (c) 2003 Henning Brauer <henning@openbsd.org>
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
#include <time.h>

#define	MAX_BACKLOG			5
#define	INTERVAL_CONNECTRETRY		120
#define	INTERVAL_HOLD_INITIAL		240
#define	INTERVAL_HOLD			90
#define INTERVAL_START			90
#define MSGSIZE_HEADER			19
#define MSGSIZE_HEADER_MARKER		16
#define	MSGSIZE_NOTIFICATION_MIN	21	/* 19 hdr + 1 code + 1 sub */
#define	MSGSIZE_OPEN_MIN		29
#define	MSGSIZE_UPDATE_MIN		23
#define	MSGSIZE_KEEPALIVE		MSGSIZE_HEADER

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
	KEEPALIVE
};

enum err_codes {
	ERR_HEADER = 1,
	ERR_OPEN,
	ERR_UPDATE,
	ERR_HOLDTIMEREXPIRED,
	ERR_FSM,
	ERR_CEASE
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
	ERR_OPEN_HOLDTIME
};

struct msg_header {
	u_char			 marker[16];
	u_int16_t		 len;
	u_int8_t		 type;
};

struct msg_open {
	struct msg_header	 header;
	u_int8_t		 version;
	u_int16_t		 myas;
	u_int16_t		 holdtime;
	u_int32_t		 bgpid;
	u_int8_t		 optparamlen;
};

struct ctl_conn {
	TAILQ_ENTRY(ctl_conn)	entries;
	struct imsgbuf		ibuf;
};

TAILQ_HEAD(ctl_conns, ctl_conn)	ctl_conns;

struct peer_stats {
	u_int64_t		 msg_rcvd_open;
	u_int64_t		 msg_rcvd_update;
	u_int64_t		 msg_rcvd_notification;
	u_int64_t		 msg_rcvd_keepalive;
	u_int64_t		 msg_sent_open;
	u_int64_t		 msg_sent_update;
	u_int64_t		 msg_sent_notification;
	u_int64_t		 msg_sent_keepalive;
	time_t			 last_updown;
	time_t			 last_read;
};

struct peer {
	struct peer_config	 conf;
	struct peer_stats	 stats;
	u_int32_t		 remote_bgpid;
	u_int16_t		 holdtime;
	enum session_state	 state;
	time_t			 ConnectRetryTimer;
	time_t			 KeepaliveTimer;
	time_t			 HoldTimer;
	time_t			 StartTimer;
	u_int			 StartTimerInterval;
	int			 sock;
	int			 events;
	struct msgbuf		 wbuf;
	struct buf_read		*rbuf;
	struct peer		*next;
};

struct peer	*peers;

/* session.c */
void		  session_socket_blockmode(int, enum blockmodes);
int		  session_main(struct bgpd_config *, struct peer *, int[2],
		    int[2]);
struct peer	*getpeerbyip(in_addr_t);
int		 imsg_compose_parent(int, u_int32_t, void *, u_int16_t);

/* log.c */
void		 log_peer_err(const struct peer *, const char *, ...);
void		 log_peer_errx(const struct peer *, const char *, ...);
void		 log_statechange(const struct peer *, enum session_state,
		    enum session_events);
void		 log_notification(const struct peer *, u_int8_t, u_int8_t,
		    u_char *, u_int16_t);
void		 log_conn_attempt(const struct peer *, struct in_addr);

/* parse.y */
int	 parse_config(char *, struct bgpd_config *, struct mrt_config *,
	    struct peer **);

/* config.c */
int	 merge_config(struct bgpd_config *, struct bgpd_config *,
	    struct peer *);

/* rde.c */
int	 rde_main(struct bgpd_config *, struct peer *, int[2], int[2]);

/* control.c */
int	control_listen(void);
void	control_shutdown(void);
int	control_dispatch_msg(struct pollfd *, int);
void	control_accept(int);
void	control_close(int);
