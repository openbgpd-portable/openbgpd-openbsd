/*	$OpenBSD: bgpd.h,v 1.53 2004/01/05 16:21:14 henning Exp $ */

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
#ifndef __BGPD_H__
#define	__BGPD_H__

#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <poll.h>
#include <stdarg.h>
#include <syslog.h>

#define	BGP_VERSION			4
#define	BGP_PORT			179
#define	CONFFILE			"/etc/bgpd.conf"
#define	BGPD_USER			"_bgpd"
#define	PEER_DESCR_LEN			32

#define	MAX_PKTSIZE			4096
#define	MIN_HOLDTIME			3
#define	READ_BUF_SIZE			65535
#define	RT_BUF_SIZE			16384

#define	BGPD_OPT_VERBOSE		0x0001
#define	BGPD_OPT_VERBOSE2		0x0002
#define	BGPD_OPT_NOACTION		0x0004

#define	BGPD_FLAG_NO_FIB_UPDATE		0x0001

#define BGPD_LOG_UPDATES		0x0001

#define	SOCKET_NAME			"/var/run/bgpd.sock"

enum {
	PROC_MAIN,
	PROC_SE,
	PROC_RDE
} bgpd_process;

enum reconf_action {
	RECONF_NONE,
	RECONF_KEEP,
	RECONF_REINIT,
	RECONF_DELETE
};

struct buf {
	TAILQ_ENTRY(buf)	 entries;
	u_char			*buf;
	ssize_t			 size;
	ssize_t			 wpos;
	ssize_t			 rpos;
};

struct msgbuf {
	u_int32_t		 queued;
	int			 sock;
	TAILQ_HEAD(bufs, buf)	 bufs;
};

struct bgpd_addr {
	sa_family_t	af;
	union {
		struct in_addr		v4;
		struct in6_addr		v6;
	} ba;		    /* 128-bit address */
#define v4	ba.v4
#define v6	ba.v6
};

struct bgpd_config {
	int			 opts;
	u_int16_t		 as;
	u_int32_t		 bgpid;
	u_int16_t		 holdtime;
	u_int16_t		 min_holdtime;
	int			 flags;
	int			 log;
	struct sockaddr_in	 listen_addr;
};

struct buf_read {
	u_char			 buf[READ_BUF_SIZE];
	u_char			*rptr;
	ssize_t			 wpos;
};

struct peer_config {
	u_int32_t		 id;
	char			 group[PEER_DESCR_LEN];
	char			 descr[PEER_DESCR_LEN];
	struct sockaddr_in	 remote_addr;
	struct sockaddr_in	 local_addr;
	u_int16_t		 remote_as;
	u_int8_t		 ebgp;		/* 1 = ebgp, 0 = ibgp */
	u_int8_t		 distance;	/* 1 = direct, >1 = multihop */
	u_int8_t		 passive;
	enum reconf_action	 reconf_action;
};

#define	MRT_FILE_LEN	512
enum mrtdump_type {
	MRT_NONE,
	MRT_TABLE_DUMP
/*
 *	MRT_UPDATE_START,
 *	MRT_SESSION_START,
 *	MRT_UPDATE_STOP,
 *	MRT_SESSION_STOP,
 */
};

enum mrtdump_state {
	MRT_STATE_OPEN,
	MRT_STATE_RUNNING,
	MRT_STATE_DONE,
	MRT_STATE_CLOSE,
	MRT_STATE_REOPEN
};

LIST_HEAD(mrt_config, mrtdump_config);

struct mrtdump_config {
	enum mrtdump_type	 type;
	u_int32_t		 id;
	struct msgbuf		 msgbuf;
	char			 name[MRT_FILE_LEN];	/* base file name */
	char			 file[MRT_FILE_LEN];	/* actual file name */
	time_t			 ReopenTimer;
	time_t			 ReopenTimerInterval;
	enum mrtdump_state	 state;
	LIST_ENTRY(mrtdump_config)
				 list;
};

/* ipc messages */

#define	IMSG_HEADER_SIZE	sizeof(struct imsg_hdr)
#define	MAX_IMSGSIZE		8192

struct imsgbuf {
	int			sock;
	struct buf_read		r;
	struct msgbuf		w;
};

enum imsg_type {
	IMSG_NONE,
	IMSG_RECONF_CONF,
	IMSG_RECONF_PEER,
	IMSG_RECONF_DONE,
	IMSG_UPDATE,
	IMSG_UPDATE_ERR,
	IMSG_SESSION_UP,
	IMSG_SESSION_DOWN,
	IMSG_MRT_REQ,
	IMSG_MRT_MSG,
	IMSG_MRT_END,
	IMSG_KROUTE_CHANGE,
	IMSG_KROUTE_DELETE,
	IMSG_NEXTHOP_ADD,
	IMSG_NEXTHOP_REMOVE,
	IMSG_NEXTHOP_UPDATE,
	IMSG_CTL_SHOW_NEIGHBOR,
	IMSG_CTL_END,
	IMSG_CTL_RELOAD,
	IMSG_CTL_FIB_COUPLE,
	IMSG_CTL_FIB_DECOUPLE
};

struct imsg_hdr {
	enum imsg_type	type;
	u_int16_t	len;
	u_int32_t	peerid;
};

struct imsg {
	struct imsg_hdr	 hdr;
	void		*data;
};

/* error subcode for UPDATE; needed in SE and RDE */
enum suberr_update {
	ERR_UPD_ATTRLIST = 1,
	ERR_UPD_UNKNWN_WK_ATTR,
	ERR_UPD_MISSNG_WK_ATTR,
	ERR_UPD_ATTRFLAGS,
	ERR_UPD_ATTRLEN,
	ERR_UPD_ORIGIN,
	ERR_UPD_LOOP,
	ERR_UPD_NEXTHOP,
	ERR_UPD_OPTATTR,
	ERR_UPD_NETWORK,
	ERR_UPD_ASPATH
};

struct kroute {
	in_addr_t	prefix;
	u_int8_t	prefixlen;
	in_addr_t	nexthop;
};

struct kroute_nexthop {
	in_addr_t	nexthop;
	u_int8_t	valid;
	u_int8_t	connected;
	in_addr_t	gateway;
};

/* prototypes */
/* bgpd.c */
void		 send_nexthop_update(struct kroute_nexthop *);

/* buffer.c */
struct buf	*buf_open(ssize_t);
int		 buf_add(struct buf *, void *, ssize_t);
void		*buf_reserve(struct buf *, ssize_t);
int		 buf_close(struct msgbuf *, struct buf *);
void		 buf_free(struct buf *);
void		 msgbuf_init(struct msgbuf *);
void		 msgbuf_clear(struct msgbuf *);
int		 msgbuf_write(struct msgbuf *);

/* log.c */
void		 log_init(int);
void		 logit(int, const char *, ...);
void		 vlog(int, const char *, va_list);
void		 log_err(const char *, ...);
void		 fatal(const char *);
void		 fatalx(const char *);
void		 fatal_ensure(const char *, int, const char *);
char		*log_ntoa(in_addr_t);

/* parse.y */
int	 cmdline_symset(char *);

/* imsg.c */
void	 imsg_init(struct imsgbuf *, int);
int	 imsg_read(struct imsgbuf *);
int	 imsg_get(struct imsgbuf *, struct imsg *);
int	 imsg_compose(struct imsgbuf *, int, u_int32_t, void *, u_int16_t);
void	 imsg_free(struct imsg *);

/* mrt.c */
int	 mrt_mergeconfig(struct mrt_config *, struct mrt_config *);

/* kroute.c */
int	kroute_init(int);
int	kroute_change(struct kroute *);
int	kroute_delete(struct kroute *);
void	kroute_shutdown(void);
void	kroute_fib_couple(void);
void	kroute_fib_decouple(void);
int	kroute_dispatch_msg(void);
int	kroute_nexthop_add(in_addr_t);
void	kroute_nexthop_delete(in_addr_t);

/* control.c */
int	control_init(void);
void	control_cleanup(void);

#endif /* __BGPD_H__ */
