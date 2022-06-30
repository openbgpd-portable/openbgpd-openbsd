/*	$OpenBSD: kroute.c,v 1.271 2022/06/30 20:33:14 claudio Exp $ */

/*
 * Copyright (c) 2022 Claudio Jeker <claudio@openbsd.org>
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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/tree.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netmpls/mpls.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bgpd.h"
#include "log.h"

#define	RTP_MINE	0xff

struct ktable		**krt;
u_int			  krt_size;

struct {
	uint32_t		rtseq;
	pid_t			pid;
	int			fd;
	uint8_t			fib_prio;
} kr_state;

struct kroute {
	RB_ENTRY(kroute)	 entry;
	struct kroute		*next;
	struct in_addr		 prefix;
	struct in_addr		 nexthop;
	uint32_t		 mplslabel;
	uint16_t		 flags;
	uint16_t		 labelid;
	u_short			 ifindex;
	uint8_t			 prefixlen;
	uint8_t			 priority;
};

struct kroute6 {
	RB_ENTRY(kroute6)	 entry;
	struct kroute6		*next;
	struct in6_addr		 prefix;
	struct in6_addr		 nexthop;
	uint32_t		 prefix_scope_id;	/* because ... */
	uint32_t		 nexthop_scope_id;
	uint32_t		 mplslabel;
	uint16_t		 flags;
	uint16_t		 labelid;
	u_short			 ifindex;
	uint8_t			 prefixlen;
	uint8_t			 priority;
};

struct knexthop {
	RB_ENTRY(knexthop)	 entry;
	struct bgpd_addr	 nexthop;
	void			*kroute;
};

struct kredist_node {
	RB_ENTRY(kredist_node)	 entry;
	struct bgpd_addr	 prefix;
	uint64_t		 rd;
	uint8_t			 prefixlen;
	uint8_t			 dynamic;
};

struct kif_kr {
	LIST_ENTRY(kif_kr)	 entry;
	struct kroute		*kr;
};

struct kif_kr6 {
	LIST_ENTRY(kif_kr6)	 entry;
	struct kroute6		*kr;
};

LIST_HEAD(kif_kr_head, kif_kr);
LIST_HEAD(kif_kr6_head, kif_kr6);

struct kif {
	char			 ifname[IFNAMSIZ];
	uint64_t		 baudrate;
	u_int			 rdomain;
	int			 flags;
	u_short			 ifindex;
	uint8_t			 if_type;
	uint8_t			 link_state;
	uint8_t			 nh_reachable;	/* for nexthop verification */
	uint8_t			 depend_state;	/* for session depend on */
};

struct kif_node {
	RB_ENTRY(kif_node)	 entry;
	struct kif		 k;
	struct kif_kr_head	 kroute_l;
	struct kif_kr6_head	 kroute6_l;
};

int	ktable_new(u_int, u_int, char *, int);
void	ktable_free(u_int);
void	ktable_destroy(struct ktable *);
struct ktable	*ktable_get(u_int);

int	kr4_change(struct ktable *, struct kroute_full *);
int	kr6_change(struct ktable *, struct kroute_full *);
int	krVPN4_change(struct ktable *, struct kroute_full *);
int	krVPN6_change(struct ktable *, struct kroute_full *);
int	kr4_delete(struct ktable *, struct kroute_full *);
int	kr6_delete(struct ktable *, struct kroute_full *);
int	krVPN4_delete(struct ktable *, struct kroute_full *);
int	krVPN6_delete(struct ktable *, struct kroute_full *);
void	kr_net_delete(struct network *);
int	kr_net_match(struct ktable *, struct network_config *, uint16_t, int);
struct network *kr_net_find(struct ktable *, struct network *);
void	kr_net_clear(struct ktable *);
void	kr_redistribute(int, struct ktable *, struct kroute *);
void	kr_redistribute6(int, struct ktable *, struct kroute6 *);
struct kroute_full *kr_tofull(struct kroute *);
struct kroute_full *kr6_tofull(struct kroute6 *);
int	kroute_compare(struct kroute *, struct kroute *);
int	kroute6_compare(struct kroute6 *, struct kroute6 *);
int	knexthop_compare(struct knexthop *, struct knexthop *);
int	kredist_compare(struct kredist_node *, struct kredist_node *);
int	kif_compare(struct kif_node *, struct kif_node *);

struct kroute	*kroute_find(struct ktable *, const struct bgpd_addr *,
			    uint8_t, uint8_t);
struct kroute	*kroute_matchgw(struct kroute *, struct bgpd_addr *);
int		 kroute_insert(struct ktable *, struct kroute *);
int		 kroute_remove(struct ktable *, struct kroute *);
void		 kroute_clear(struct ktable *);

struct kroute6	*kroute6_find(struct ktable *, const struct bgpd_addr *,
			    uint8_t, uint8_t);
struct kroute6	*kroute6_matchgw(struct kroute6 *, struct bgpd_addr *);
int		 kroute6_insert(struct ktable *, struct kroute6 *);
int		 kroute6_remove(struct ktable *, struct kroute6 *);
void		 kroute6_clear(struct ktable *);

struct knexthop	*knexthop_find(struct ktable *, struct bgpd_addr *);
int		 knexthop_insert(struct ktable *, struct knexthop *);
int		 knexthop_remove(struct ktable *, struct knexthop *);
void		 knexthop_clear(struct ktable *);

struct kif_node	*kif_find(int);
int		 kif_insert(struct kif_node *);
int		 kif_remove(struct kif_node *);
void		 kif_clear(void);

int		 kif_kr_insert(struct kroute *);
int		 kif_kr_remove(struct kroute *);

int		 kif_kr6_insert(struct kroute6 *);
int		 kif_kr6_remove(struct kroute6 *);

int		 kroute_validate(struct kroute *);
int		 kroute6_validate(struct kroute6 *);
void		 knexthop_validate(struct ktable *, struct knexthop *);
void		 knexthop_track(struct ktable *, void *);
void		 knexthop_send_update(struct knexthop *);
struct kroute	*kroute_match(struct ktable *, struct bgpd_addr *, int);
struct kroute6	*kroute6_match(struct ktable *, struct bgpd_addr *, int);
void		 kroute_detach_nexthop(struct ktable *, struct knexthop *);

uint8_t		prefixlen_classful(in_addr_t);
uint8_t		mask2prefixlen(in_addr_t);
uint8_t		mask2prefixlen6(struct sockaddr_in6 *);
uint64_t	ift2ifm(uint8_t);
const char	*get_media_descr(uint64_t);
const char	*get_linkstate(uint8_t, int);
void		get_rtaddrs(int, struct sockaddr *, struct sockaddr **);
void		if_change(u_short, int, struct if_data *);
void		if_announce(void *);

int		send_rtmsg(int, int, struct ktable *, struct kroute_full *);
int		dispatch_rtmsg(void);
int		fetchtable(struct ktable *);
int		fetchifs(int);
int		dispatch_rtmsg_addr(struct rt_msghdr *, struct kroute_full *);
int		kr_fib_delete(struct ktable *, struct kroute_full *, int);
int		kr_fib_change(struct ktable *, struct kroute_full *, int, int);

RB_PROTOTYPE(kroute_tree, kroute, entry, kroute_compare)
RB_GENERATE(kroute_tree, kroute, entry, kroute_compare)

RB_PROTOTYPE(kroute6_tree, kroute6, entry, kroute6_compare)
RB_GENERATE(kroute6_tree, kroute6, entry, kroute6_compare)

RB_PROTOTYPE(knexthop_tree, knexthop, entry, knexthop_compare)
RB_GENERATE(knexthop_tree, knexthop, entry, knexthop_compare)

RB_PROTOTYPE(kredist_tree, kredist_node, entry, kredist_compare)
RB_GENERATE(kredist_tree, kredist_node, entry, kredist_compare)

RB_HEAD(kif_tree, kif_node)		kit;
RB_PROTOTYPE(kif_tree, kif_node, entry, kif_compare)
RB_GENERATE(kif_tree, kif_node, entry, kif_compare)

#define KT2KNT(x)	(&(ktable_get((x)->nhtableid)->knt))

/*
 * exported functions
 */

int
kr_init(int *fd, uint8_t fib_prio)
{
	int		opt = 0, rcvbuf, default_rcvbuf;
	unsigned int	tid = RTABLE_ANY;
	socklen_t	optlen;

	if ((kr_state.fd = socket(AF_ROUTE,
	    SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) == -1) {
		log_warn("%s: socket", __func__);
		return (-1);
	}

	/* not interested in my own messages */
	if (setsockopt(kr_state.fd, SOL_SOCKET, SO_USELOOPBACK,
	    &opt, sizeof(opt)) == -1)
		log_warn("%s: setsockopt", __func__);	/* not fatal */

	/* grow receive buffer, don't wanna miss messages */
	optlen = sizeof(default_rcvbuf);
	if (getsockopt(kr_state.fd, SOL_SOCKET, SO_RCVBUF,
	    &default_rcvbuf, &optlen) == -1)
		log_warn("%s: getsockopt SOL_SOCKET SO_RCVBUF", __func__);
	else
		for (rcvbuf = MAX_RTSOCK_BUF;
		    rcvbuf > default_rcvbuf &&
		    setsockopt(kr_state.fd, SOL_SOCKET, SO_RCVBUF,
		    &rcvbuf, sizeof(rcvbuf)) == -1 && errno == ENOBUFS;
		    rcvbuf /= 2)
			;	/* nothing */

	if (setsockopt(kr_state.fd, AF_ROUTE, ROUTE_TABLEFILTER, &tid,
	    sizeof(tid)) == -1) {
		log_warn("%s: setsockopt AF_ROUTE ROUTE_TABLEFILTER", __func__);
		return (-1);
	}

	kr_state.pid = getpid();
	kr_state.rtseq = 1;
	kr_state.fib_prio = fib_prio;

	RB_INIT(&kit);

	if (fetchifs(0) == -1)
		return (-1);

	*fd = kr_state.fd;
	return (0);
}

int
ktable_new(u_int rtableid, u_int rdomid, char *name, int fs)
{
	struct ktable	**xkrt;
	struct ktable	 *kt;
	size_t		  oldsize;

	/* resize index table if needed */
	if (rtableid >= krt_size) {
		oldsize = sizeof(struct ktable *) * krt_size;
		if ((xkrt = reallocarray(krt, rtableid + 1,
		    sizeof(struct ktable *))) == NULL) {
			log_warn("%s", __func__);
			return (-1);
		}
		krt = xkrt;
		krt_size = rtableid + 1;
		bzero((char *)krt + oldsize,
		    krt_size * sizeof(struct ktable *) - oldsize);
	}

	if (krt[rtableid])
		fatalx("ktable_new: table already exists.");

	/* allocate new element */
	kt = krt[rtableid] = calloc(1, sizeof(struct ktable));
	if (kt == NULL) {
		log_warn("%s", __func__);
		return (-1);
	}

	/* initialize structure ... */
	strlcpy(kt->descr, name, sizeof(kt->descr));
	RB_INIT(&kt->krt);
	RB_INIT(&kt->krt6);
	RB_INIT(&kt->knt);
	TAILQ_INIT(&kt->krn);
	kt->fib_conf = kt->fib_sync = fs;
	kt->rtableid = rtableid;
	kt->nhtableid = rdomid;
	/* bump refcount of rdomain table for the nexthop lookups */
	ktable_get(kt->nhtableid)->nhrefcnt++;

	/* ... and load it */
	if (fetchtable(kt) == -1)
		return (-1);

	/* everything is up and running */
	kt->state = RECONF_REINIT;
	log_debug("%s: %s with rtableid %d rdomain %d", __func__, name,
	    rtableid, rdomid);
	return (0);
}

void
ktable_free(u_int rtableid)
{
	struct ktable	*kt, *nkt;

	if ((kt = ktable_get(rtableid)) == NULL)
		return;

	/* decouple from kernel, no new routes will be entered from here */
	kr_fib_decouple(kt->rtableid);

	/* first unhook from the nexthop table */
	nkt = ktable_get(kt->nhtableid);
	nkt->nhrefcnt--;

	/*
	 * Evil little details:
	 *   If kt->nhrefcnt > 0 then kt == nkt and nothing needs to be done.
	 *   If kt != nkt then kt->nhrefcnt must be 0 and kt must be killed.
	 *   If nkt is no longer referenced it must be killed (possible double
	 *   free so check that kt != nkt).
	 */
	if (kt != nkt && nkt->nhrefcnt <= 0)
		ktable_destroy(nkt);
	if (kt->nhrefcnt <= 0)
		ktable_destroy(kt);
}

void
ktable_destroy(struct ktable *kt)
{
	/* decouple just to be sure, does not hurt */
	kr_fib_decouple(kt->rtableid);

	log_debug("%s: freeing ktable %s rtableid %u", __func__, kt->descr,
	    kt->rtableid);
	/* only clear nexthop table if it is the main rdomain table */
	if (kt->rtableid == kt->nhtableid)
		knexthop_clear(kt);
	kroute_clear(kt);
	kroute6_clear(kt);
	kr_net_clear(kt);

	krt[kt->rtableid] = NULL;
	free(kt);
}

struct ktable *
ktable_get(u_int rtableid)
{
	if (rtableid >= krt_size)
		return (NULL);
	return (krt[rtableid]);
}

int
ktable_update(u_int rtableid, char *name, int flags)
{
	struct ktable	*kt, *rkt;
	u_int		 rdomid;

	if (!ktable_exists(rtableid, &rdomid))
		fatalx("King Bula lost a table");	/* may not happen */

	if (rdomid != rtableid || flags & F_RIB_NOFIB) {
		rkt = ktable_get(rdomid);
		if (rkt == NULL) {
			char buf[32];
			snprintf(buf, sizeof(buf), "rdomain_%d", rdomid);
			if (ktable_new(rdomid, rdomid, buf, 0))
				return (-1);
		} else {
			/* there is no need for full fib synchronisation if
			 * the table is only used for nexthop lookups.
			 */
			if (rkt->state == RECONF_DELETE) {
				rkt->fib_conf = 0;
				rkt->state = RECONF_KEEP;
			}
		}
	}

	if (flags & (F_RIB_NOFIB | F_RIB_NOEVALUATE))
		/* only rdomain table must exist */
		return (0);

	kt = ktable_get(rtableid);
	if (kt == NULL) {
		if (ktable_new(rtableid, rdomid, name,
		    !(flags & F_RIB_NOFIBSYNC)))
			return (-1);
	} else {
		/* fib sync has higher preference then no sync */
		if (kt->state == RECONF_DELETE) {
			kt->fib_conf = !(flags & F_RIB_NOFIBSYNC);
			kt->state = RECONF_KEEP;
		} else if (!kt->fib_conf)
			kt->fib_conf = !(flags & F_RIB_NOFIBSYNC);

		strlcpy(kt->descr, name, sizeof(kt->descr));
	}
	return (0);
}

int
ktable_exists(u_int rtableid, u_int *rdomid)
{
	size_t			 len;
	struct rt_tableinfo	 info;
	int			 mib[6];

	mib[0] = CTL_NET;
	mib[1] = PF_ROUTE;
	mib[2] = 0;
	mib[3] = 0;
	mib[4] = NET_RT_TABLE;
	mib[5] = rtableid;

	len = sizeof(info);
	if (sysctl(mib, 6, &info, &len, NULL, 0) == -1) {
		if (errno == ENOENT)
			/* table nonexistent */
			return (0);
		log_warn("sysctl net.route.rtableid");
		/* must return 0 so that the table is considered non-existent */
		return (0);
	}
	if (rdomid)
		*rdomid = info.rti_domainid;
	return (1);
}

int
kr_change(u_int rtableid, struct kroute_full *kl)
{
	struct ktable		*kt;

	if ((kt = ktable_get(rtableid)) == NULL)
		/* too noisy during reloads, just ignore */
		return (0);
	switch (kl->prefix.aid) {
	case AID_INET:
		return (kr4_change(kt, kl));
	case AID_INET6:
		return (kr6_change(kt, kl));
	case AID_VPN_IPv4:
		return (krVPN4_change(kt, kl));
	case AID_VPN_IPv6:
		return (krVPN6_change(kt, kl));
	}
	log_warnx("%s: not handled AID", __func__);
	return (-1);
}

int
kr4_change(struct ktable *kt, struct kroute_full *kf)
{
	struct kroute	*kr;
	int		 action = RTM_ADD;
	uint16_t	 labelid;

	/* for blackhole and reject routes nexthop needs to be 127.0.0.1 */
	if (kf->flags & (F_BLACKHOLE|F_REJECT))
		kf->nexthop.v4.s_addr = htonl(INADDR_LOOPBACK);
	/* nexthop within 127/8 -> ignore silently */
	else if ((kf->nexthop.v4.s_addr & htonl(IN_CLASSA_NET)) ==
	    htonl(INADDR_LOOPBACK & IN_CLASSA_NET))
		return (0);

	labelid = rtlabel_name2id(kf->label);

	if ((kr = kroute_find(kt, &kf->prefix, kf->prefixlen,
	    RTP_MINE)) != NULL)
		action = RTM_CHANGE;

	if (action == RTM_ADD) {
		if ((kr = calloc(1, sizeof(*kr))) == NULL) {
			log_warn("%s", __func__);
			return (-1);
		}
		kr->prefix.s_addr = kf->prefix.v4.s_addr;
		kr->prefixlen = kf->prefixlen;
		kr->nexthop.s_addr = kf->nexthop.v4.s_addr;
		kr->flags = kf->flags | F_BGPD;
		kr->priority = RTP_MINE;
		kr->labelid = labelid;

		if (kroute_insert(kt, kr) == -1) {
			rtlabel_unref(kr->labelid);
			free(kr);
			return (-1);
		}
	} else {
		kr->nexthop.s_addr = kf->nexthop.v4.s_addr;
		rtlabel_unref(kr->labelid);
		kr->labelid = labelid;
		if (kf->flags & F_BLACKHOLE)
			kr->flags |= F_BLACKHOLE;
		else
			kr->flags &= ~F_BLACKHOLE;
		if (kf->flags & F_REJECT)
			kr->flags |= F_REJECT;
		else
			kr->flags &= ~F_REJECT;
	}

	if (send_rtmsg(kr_state.fd, action, kt, kf))
		kr->flags |= F_BGPD_INSERTED;

	return (0);
}

int
kr6_change(struct ktable *kt, struct kroute_full *kf)
{
	struct kroute6	*kr6;
	struct in6_addr	 lo6 = IN6ADDR_LOOPBACK_INIT;
	int		 action = RTM_ADD;
	uint16_t	 labelid;

	/* for blackhole and reject routes nexthop needs to be ::1 */
	if (kf->flags & (F_BLACKHOLE|F_REJECT))
		bcopy(&lo6, &kf->nexthop.v6, sizeof(kf->nexthop.v6));
	/* nexthop to loopback -> ignore silently */
	else if (IN6_IS_ADDR_LOOPBACK(&kf->nexthop.v6))
		return (0);

	labelid = rtlabel_name2id(kf->label);

	if ((kr6 = kroute6_find(kt, &kf->prefix, kf->prefixlen,
	    RTP_MINE)) != NULL)
		action = RTM_CHANGE;

	if (action == RTM_ADD) {
		if ((kr6 = calloc(1, sizeof(*kr6))) == NULL) {
			log_warn("%s", __func__);
			return (-1);
		}
		memcpy(&kr6->prefix, &kf->prefix.v6, sizeof(struct in6_addr));
		kr6->prefix_scope_id = kf->prefix.scope_id;
		kr6->prefixlen = kf->prefixlen;
		memcpy(&kr6->nexthop, &kf->nexthop.v6, sizeof(struct in6_addr));
		kr6->nexthop_scope_id = kf->nexthop.scope_id;
		kr6->flags = kf->flags | F_BGPD;
		kr6->priority = RTP_MINE;
		kr6->labelid = labelid;

		if (kroute6_insert(kt, kr6) == -1) {
			rtlabel_unref(kr6->labelid);
			free(kr6);
			return (-1);
		}
	} else {
		memcpy(&kr6->nexthop, &kf->nexthop.v6, sizeof(struct in6_addr));
		kr6->nexthop_scope_id = kf->nexthop.scope_id;
		rtlabel_unref(kr6->labelid);
		kr6->labelid = labelid;
		if (kf->flags & F_BLACKHOLE)
			kr6->flags |= F_BLACKHOLE;
		else
			kr6->flags &= ~F_BLACKHOLE;
		if (kf->flags & F_REJECT)
			kr6->flags |= F_REJECT;
		else
			kr6->flags &= ~F_REJECT;
	}

	if (send_rtmsg(kr_state.fd, action, kt, kf))
		kr6->flags |= F_BGPD_INSERTED;

	return (0);
}

int
krVPN4_change(struct ktable *kt, struct kroute_full *kf)
{
	struct kroute	*kr;
	int		 action = RTM_ADD;
	uint32_t	 mplslabel = 0;
	uint16_t	 labelid;

	/* nexthop within 127/8 -> ignore silently */
	if ((kf->nexthop.v4.s_addr & htonl(IN_CLASSA_NET)) ==
	    htonl(INADDR_LOOPBACK & IN_CLASSA_NET))
		return (0);

	/* only a single MPLS label is supported for now */
	if (kf->prefix.labellen != 3) {
		log_warnx("%s: %s/%u has not a single label", __func__,
		    log_addr(&kf->prefix), kf->prefixlen);
		return (0);
	}
	mplslabel = (kf->prefix.labelstack[0] << 24) |
	    (kf->prefix.labelstack[1] << 16) |
	    (kf->prefix.labelstack[2] << 8);
	mplslabel = htonl(mplslabel);

	labelid = rtlabel_name2id(kf->label);

	/* for blackhole and reject routes nexthop needs to be 127.0.0.1 */
	if (kf->flags & (F_BLACKHOLE|F_REJECT))
		kf->nexthop.v4.s_addr = htonl(INADDR_LOOPBACK);

	if ((kr = kroute_find(kt, &kf->prefix, kf->prefixlen,
	    RTP_MINE)) != NULL)
		action = RTM_CHANGE;

	if (action == RTM_ADD) {
		if ((kr = calloc(1, sizeof(*kr))) == NULL) {
			log_warn("%s", __func__);
			return (-1);
		}
		kr->prefix.s_addr = kf->prefix.v4.s_addr;
		kr->prefixlen = kf->prefixlen;
		kr->nexthop.s_addr = kf->nexthop.v4.s_addr;
		kr->flags = kf->flags | F_BGPD | F_MPLS;
		kr->priority = RTP_MINE;
		kr->labelid = labelid;
		kr->mplslabel = mplslabel;
		kr->ifindex = kf->ifindex;

		if (kroute_insert(kt, kr) == -1) {
			rtlabel_unref(kr->labelid);
			free(kr);
			return (-1);
		}
	} else {
		kr->mplslabel = mplslabel;
		kr->ifindex = kf->ifindex;
		kr->nexthop.s_addr = kf->nexthop.v4.s_addr;
		rtlabel_unref(kr->labelid);
		kr->labelid = labelid;
		if (kf->flags & F_BLACKHOLE)
			kr->flags |= F_BLACKHOLE;
		else
			kr->flags &= ~F_BLACKHOLE;
		if (kf->flags & F_REJECT)
			kr->flags |= F_REJECT;
		else
			kr->flags &= ~F_REJECT;
	}

	if (send_rtmsg(kr_state.fd, action, kt, kf))
		kr->flags |= F_BGPD_INSERTED;
	return (0);
}

int
krVPN6_change(struct ktable *kt, struct kroute_full *kf)
{
	struct kroute6	*kr6;
	struct in6_addr	 lo6 = IN6ADDR_LOOPBACK_INIT;
	int		 action = RTM_ADD;
	uint32_t	 mplslabel = 0;
	uint16_t	 labelid;

	/* nexthop to loopback -> ignore silently */
	if (IN6_IS_ADDR_LOOPBACK(&kf->nexthop.v6))
		return (0);

	/* only a single MPLS label is supported for now */
	if (kf->prefix.labellen != 3) {
		log_warnx("%s: %s/%u has not a single label", __func__,
		    log_addr(&kf->prefix), kf->prefixlen);
		return (0);
	}
	mplslabel = (kf->prefix.labelstack[0] << 24) |
	    (kf->prefix.labelstack[1] << 16) |
	    (kf->prefix.labelstack[2] << 8);
	mplslabel = htonl(mplslabel);

	/* for blackhole and reject routes nexthop needs to be ::1 */
	if (kf->flags & (F_BLACKHOLE|F_REJECT))
		bcopy(&lo6, &kf->nexthop.v6, sizeof(kf->nexthop.v6));

	labelid = rtlabel_name2id(kf->label);

	if ((kr6 = kroute6_find(kt, &kf->prefix, kf->prefixlen,
	    RTP_MINE)) != NULL)
		action = RTM_CHANGE;

	if (action == RTM_ADD) {
		if ((kr6 = calloc(1, sizeof(*kr6))) == NULL) {
			log_warn("%s", __func__);
			return (-1);
		}
		memcpy(&kr6->prefix, &kf->prefix.v6, sizeof(struct in6_addr));
		kr6->prefixlen = kf->prefixlen;
		memcpy(&kr6->nexthop, &kf->nexthop.v6, sizeof(struct in6_addr));
		kr6->flags = kf->flags | F_BGPD | F_MPLS;
		kr6->priority = RTP_MINE;
		kr6->labelid = labelid;
		kr6->mplslabel = mplslabel;
		kr6->ifindex = kf->ifindex;

		if (kroute6_insert(kt, kr6) == -1) {
			rtlabel_unref(kr6->labelid);
			free(kr6);
			return (-1);
		}
	} else {
		kr6->mplslabel = mplslabel;
		kr6->ifindex = kf->ifindex;
		memcpy(&kr6->nexthop, &kf->nexthop.v6, sizeof(struct in6_addr));
		rtlabel_unref(kr6->labelid);
		kr6->labelid = labelid;
		if (kf->flags & F_BLACKHOLE)
			kr6->flags |= F_BLACKHOLE;
		else
			kr6->flags &= ~F_BLACKHOLE;
		if (kf->flags & F_REJECT)
			kr6->flags |= F_REJECT;
		else
			kr6->flags &= ~F_REJECT;
	}

	if (send_rtmsg(kr_state.fd, action, kt, kf))
		kr6->flags |= F_BGPD_INSERTED;

	return (0);
}

int
kr_delete(u_int rtableid, struct kroute_full *kl)
{
	struct ktable		*kt;

	if ((kt = ktable_get(rtableid)) == NULL)
		/* too noisy during reloads, just ignore */
		return (0);

	switch (kl->prefix.aid) {
	case AID_INET:
		return (kr4_delete(kt, kl));
	case AID_INET6:
		return (kr6_delete(kt, kl));
	case AID_VPN_IPv4:
		return (krVPN4_delete(kt, kl));
	case AID_VPN_IPv6:
		return (krVPN6_delete(kt, kl));
	}
	log_warnx("%s: not handled AID", __func__);
	return (-1);
}

int
kr_flush(u_int rtableid)
{
	struct ktable	*kt;
	struct kroute	*kr, *next;
	struct kroute6	*kr6, *next6;

	if ((kt = ktable_get(rtableid)) == NULL)
		/* too noisy during reloads, just ignore */
		return (0);

	RB_FOREACH_SAFE(kr, kroute_tree, &kt->krt, next)
		if ((kr->flags & F_BGPD_INSERTED)) {
			if (kt->fib_sync)	/* coupled */
				send_rtmsg(kr_state.fd, RTM_DELETE, kt,
				    kr_tofull(kr));
			if (kroute_remove(kt, kr) == -1)
				return (-1);
		}
	RB_FOREACH_SAFE(kr6, kroute6_tree, &kt->krt6, next6)
		if ((kr6->flags & F_BGPD_INSERTED)) {
			if (kt->fib_sync)	/* coupled */
				send_rtmsg(kr_state.fd, RTM_DELETE, kt,
				    kr6_tofull(kr6));
			if (kroute6_remove(kt, kr6) == -1)
				return (-1);
		}

	kt->fib_sync = 0;
	return (0);
}

int
kr4_delete(struct ktable *kt, struct kroute_full *kl)
{
	struct kroute	*kr;

	if ((kr = kroute_find(kt, &kl->prefix, kl->prefixlen,
	    RTP_MINE)) == NULL)
		return (0);

	if (!(kr->flags & F_BGPD_INSERTED))
		return (0);

	send_rtmsg(kr_state.fd, RTM_DELETE, kt, kr_tofull(kr));

	if (kroute_remove(kt, kr) == -1)
		return (-1);

	return (0);
}

int
kr6_delete(struct ktable *kt, struct kroute_full *kl)
{
	struct kroute6	*kr6;

	if ((kr6 = kroute6_find(kt, &kl->prefix, kl->prefixlen,
	    RTP_MINE)) == NULL)
		return (0);

	if (!(kr6->flags & F_BGPD_INSERTED))
		return (0);

	send_rtmsg(kr_state.fd, RTM_DELETE, kt, kr6_tofull(kr6));

	if (kroute6_remove(kt, kr6) == -1)
		return (-1);

	return (0);
}

int
krVPN4_delete(struct ktable *kt, struct kroute_full *kl)
{
	struct kroute	*kr;

	if ((kr = kroute_find(kt, &kl->prefix, kl->prefixlen,
	    RTP_MINE)) == NULL)
		return (0);

	if (!(kr->flags & F_BGPD_INSERTED))
		return (0);

	send_rtmsg(kr_state.fd, RTM_DELETE, kt, kr_tofull(kr));

	if (kroute_remove(kt, kr) == -1)
		return (-1);

	return (0);
}

int
krVPN6_delete(struct ktable *kt, struct kroute_full *kl)
{
	struct kroute6	*kr6;

	if ((kr6 = kroute6_find(kt, &kl->prefix, kl->prefixlen,
	    RTP_MINE)) == NULL)
		return (0);

	if (!(kr6->flags & F_BGPD_INSERTED))
		return (0);

	send_rtmsg(kr_state.fd, RTM_DELETE, kt, kr6_tofull(kr6));

	if (kroute6_remove(kt, kr6) == -1)
		return (-1);

	return (0);
}

void
kr_shutdown(void)
{
	u_int	i;

	for (i = krt_size; i > 0; i--)
		ktable_free(i - 1);
	kif_clear();
	free(krt);
}

void
kr_fib_couple(u_int rtableid)
{
	struct ktable	*kt;
	struct kroute	*kr;
	struct kroute6	*kr6;

	if ((kt = ktable_get(rtableid)) == NULL)  /* table does not exist */
		return;

	if (kt->fib_sync)	/* already coupled */
		return;

	kt->fib_sync = 1;

	RB_FOREACH(kr, kroute_tree, &kt->krt)
		if (kr->flags & F_BGPD) {
			if (send_rtmsg(kr_state.fd, RTM_ADD, kt,
			    kr_tofull(kr)))
				kr->flags |= F_BGPD_INSERTED;
		}
	RB_FOREACH(kr6, kroute6_tree, &kt->krt6)
		if (kr6->flags & F_BGPD) {
			if (send_rtmsg(kr_state.fd, RTM_ADD, kt,
			    kr6_tofull(kr6)))
				kr6->flags |= F_BGPD_INSERTED;
		}
	log_info("kernel routing table %u (%s) coupled", kt->rtableid,
	    kt->descr);
}

void
kr_fib_couple_all(void)
{
	u_int	 i;

	for (i = krt_size; i > 0; i--)
		kr_fib_couple(i - 1);
}

void
kr_fib_decouple(u_int rtableid)
{
	struct ktable	*kt;
	struct kroute	*kr;
	struct kroute6	*kr6;

	if ((kt = ktable_get(rtableid)) == NULL)  /* table does not exist */
		return;

	if (!kt->fib_sync)	/* already decoupled */
		return;

	RB_FOREACH(kr, kroute_tree, &kt->krt)
		if ((kr->flags & F_BGPD_INSERTED)) {
			if (send_rtmsg(kr_state.fd, RTM_DELETE, kt,
			    kr_tofull(kr)))
				kr->flags &= ~F_BGPD_INSERTED;
		}
	RB_FOREACH(kr6, kroute6_tree, &kt->krt6)
		if ((kr6->flags & F_BGPD_INSERTED)) {
			if (send_rtmsg(kr_state.fd, RTM_DELETE, kt,
			    kr6_tofull(kr6)))
				kr6->flags &= ~F_BGPD_INSERTED;
		}

	kt->fib_sync = 0;

	log_info("kernel routing table %u (%s) decoupled", kt->rtableid,
	    kt->descr);
}

void
kr_fib_decouple_all(void)
{
	u_int	 i;

	for (i = krt_size; i > 0; i--)
		kr_fib_decouple(i - 1);
}

void
kr_fib_prio_set(uint8_t prio)
{
	kr_state.fib_prio = prio;
}

int
kr_dispatch_msg(void)
{
	return (dispatch_rtmsg());
}

int
kr_nexthop_add(u_int rtableid, struct bgpd_addr *addr)
{
	struct ktable	*kt;
	struct knexthop	*h;

	if ((kt = ktable_get(rtableid)) == NULL) {
		log_warnx("%s: non-existent rtableid %d", __func__, rtableid);
		return (0);
	}
	if ((h = knexthop_find(kt, addr)) != NULL) {
		/* should not happen... this is actually an error path */
		knexthop_send_update(h);
	} else {
		if ((h = calloc(1, sizeof(*h))) == NULL) {
			log_warn("%s", __func__);
			return (-1);
		}
		memcpy(&h->nexthop, addr, sizeof(h->nexthop));

		if (knexthop_insert(kt, h) == -1)
			return (-1);
	}

	return (0);
}

void
kr_nexthop_delete(u_int rtableid, struct bgpd_addr *addr)
{
	struct ktable	*kt;
	struct knexthop	*kn;

	if ((kt = ktable_get(rtableid)) == NULL) {
		log_warnx("%s: non-existent rtableid %d", __func__,
		    rtableid);
		return;
	}
	if ((kn = knexthop_find(kt, addr)) == NULL)
		return;

	knexthop_remove(kt, kn);
}

static struct ctl_show_interface *
kr_show_interface(struct kif *kif)
{
	static struct ctl_show_interface iface;
	uint64_t ifms_type;

	bzero(&iface, sizeof(iface));
	strlcpy(iface.ifname, kif->ifname, sizeof(iface.ifname));

	snprintf(iface.linkstate, sizeof(iface.linkstate),
	    "%s", get_linkstate(kif->if_type, kif->link_state));

	if ((ifms_type = ift2ifm(kif->if_type)) != 0)
		snprintf(iface.media, sizeof(iface.media),
		    "%s", get_media_descr(ifms_type));

	iface.baudrate = kif->baudrate;
	iface.rdomain = kif->rdomain;
	iface.nh_reachable = kif->nh_reachable;
	iface.is_up = (kif->flags & IFF_UP) == IFF_UP;

	return &iface;
}

void
kr_show_route(struct imsg *imsg)
{
	struct ktable		*kt;
	struct kroute		*kr, *kn;
	struct kroute6		*kr6, *kn6;
	struct bgpd_addr	*addr;
	int			 flags;
	sa_family_t		 af;
	struct ctl_show_nexthop	 snh;
	struct knexthop		*h;
	struct kif_node		*kif;
	u_int			 i;
	u_short			 ifindex = 0;

	switch (imsg->hdr.type) {
	case IMSG_CTL_KROUTE:
		if (imsg->hdr.len != IMSG_HEADER_SIZE + sizeof(flags) +
		    sizeof(af)) {
			log_warnx("%s: wrong imsg len", __func__);
			break;
		}
		kt = ktable_get(imsg->hdr.peerid);
		if (kt == NULL) {
			log_warnx("%s: table %u does not exist", __func__,
			    imsg->hdr.peerid);
			break;
		}
		memcpy(&flags, imsg->data, sizeof(flags));
		memcpy(&af, (char *)imsg->data + sizeof(flags), sizeof(af));
		if (!af || af == AF_INET)
			RB_FOREACH(kr, kroute_tree, &kt->krt) {
				if (flags && (kr->flags & flags) == 0)
					continue;
				kn = kr;
				do {
					send_imsg_session(IMSG_CTL_KROUTE,
					    imsg->hdr.pid, kr_tofull(kn),
					    sizeof(struct kroute_full));
				} while ((kn = kn->next) != NULL);
			}
		if (!af || af == AF_INET6)
			RB_FOREACH(kr6, kroute6_tree, &kt->krt6) {
				if (flags && (kr6->flags & flags) == 0)
					continue;
				kn6 = kr6;
				do {
					send_imsg_session(IMSG_CTL_KROUTE,
					    imsg->hdr.pid, kr6_tofull(kn6),
					    sizeof(struct kroute_full));
				} while ((kn6 = kn6->next) != NULL);
			}
		break;
	case IMSG_CTL_KROUTE_ADDR:
		if (imsg->hdr.len != IMSG_HEADER_SIZE +
		    sizeof(struct bgpd_addr)) {
			log_warnx("%s: wrong imsg len", __func__);
			break;
		}
		kt = ktable_get(imsg->hdr.peerid);
		if (kt == NULL) {
			log_warnx("%s: table %u does not exist", __func__,
			    imsg->hdr.peerid);
			break;
		}
		addr = imsg->data;
		kr = NULL;
		switch (addr->aid) {
		case AID_INET:
			kr = kroute_match(kt, addr, 1);
			if (kr != NULL)
				send_imsg_session(IMSG_CTL_KROUTE,
				    imsg->hdr.pid, kr_tofull(kr),
				    sizeof(struct kroute_full));
			break;
		case AID_INET6:
			kr6 = kroute6_match(kt, addr, 1);
			if (kr6 != NULL)
				send_imsg_session(IMSG_CTL_KROUTE,
				    imsg->hdr.pid, kr6_tofull(kr6),
				    sizeof(struct kroute_full));
			break;
		}
		break;
	case IMSG_CTL_SHOW_NEXTHOP:
		kt = ktable_get(imsg->hdr.peerid);
		if (kt == NULL) {
			log_warnx("%s: table %u does not exist", __func__,
			    imsg->hdr.peerid);
			break;
		}
		RB_FOREACH(h, knexthop_tree, KT2KNT(kt)) {
			bzero(&snh, sizeof(snh));
			memcpy(&snh.addr, &h->nexthop, sizeof(snh.addr));
			if (h->kroute != NULL) {
				switch (h->nexthop.aid) {
				case AID_INET:
					kr = h->kroute;
					snh.valid = kroute_validate(kr);
					snh.krvalid = 1;
					snh.kr = *kr_tofull(kr);
					ifindex = kr->ifindex;
					break;
				case AID_INET6:
					kr6 = h->kroute;
					snh.valid = kroute6_validate(kr6);
					snh.krvalid = 1;
					snh.kr = *kr6_tofull(kr6);
					ifindex = kr6->ifindex;
					break;
				}
				if ((kif = kif_find(ifindex)) != NULL)
					memcpy(&snh.iface,
					    kr_show_interface(&kif->k),
					    sizeof(snh.iface));
			}
			send_imsg_session(IMSG_CTL_SHOW_NEXTHOP, imsg->hdr.pid,
			    &snh, sizeof(snh));
		}
		break;
	case IMSG_CTL_SHOW_INTERFACE:
		RB_FOREACH(kif, kif_tree, &kit)
			send_imsg_session(IMSG_CTL_SHOW_INTERFACE,
			    imsg->hdr.pid, kr_show_interface(&kif->k),
			    sizeof(struct ctl_show_interface));
		break;
	case IMSG_CTL_SHOW_FIB_TABLES:
		for (i = 0; i < krt_size; i++) {
			struct ktable	ktab;

			if ((kt = ktable_get(i)) == NULL)
				continue;

			ktab = *kt;
			/* do not leak internal information */
			RB_INIT(&ktab.krt);
			RB_INIT(&ktab.krt6);
			RB_INIT(&ktab.knt);
			TAILQ_INIT(&ktab.krn);

			send_imsg_session(IMSG_CTL_SHOW_FIB_TABLES,
			    imsg->hdr.pid, &ktab, sizeof(ktab));
		}
		break;
	default:	/* nada */
		break;
	}

	send_imsg_session(IMSG_CTL_END, imsg->hdr.pid, NULL, 0);
}

static void
kr_send_dependon(struct kif *kif)
{
	struct session_dependon sdon = { 0 };

	strlcpy(sdon.ifname, kif->ifname, sizeof(sdon.ifname));
	sdon.depend_state = kif->depend_state;
	send_imsg_session(IMSG_SESSION_DEPENDON, 0, &sdon, sizeof(sdon));
}
	
void
kr_ifinfo(char *ifname)
{
	struct kif_node	*kif;

	RB_FOREACH(kif, kif_tree, &kit)
		if (!strcmp(ifname, kif->k.ifname)) {
			kr_send_dependon(&kif->k);
			return;
		}
}

void
kr_net_delete(struct network *n)
{
	filterset_free(&n->net.attrset);
	free(n);
}

static int
kr_net_redist_add(struct ktable *kt, struct network_config *net,
    struct filter_set_head *attr, int dynamic)
{
	struct kredist_node *r, *xr;

	if ((r = calloc(1, sizeof(*r))) == NULL)
		fatal("%s", __func__);
	r->prefix = net->prefix;
	r->prefixlen = net->prefixlen;
	r->rd = net->rd;
	r->dynamic = dynamic;

	xr = RB_INSERT(kredist_tree, &kt->kredist, r);
	if (xr != NULL) {
		free(r);

		if (dynamic != xr->dynamic && dynamic) {
			/*
			 * ignore update a non-dynamic announcement is
			 * already present which has preference.
			 */
			return 0;
		}
		/*
		 * only equal or non-dynamic announcement ends up here.
		 * In both cases reset the dynamic flag (nop for equal) and
		 * redistribute.
		 */
		xr->dynamic = dynamic;
	}

	if (send_network(IMSG_NETWORK_ADD, net, attr) == -1)
		log_warnx("%s: faild to send network update", __func__);
	return 1;
}

static void
kr_net_redist_del(struct ktable *kt, struct network_config *net, int dynamic)
{
	struct kredist_node *r, node;

	bzero(&node, sizeof(node));
	node.prefix = net->prefix;
	node.prefixlen = net->prefixlen;
	node.rd = net->rd;

	r = RB_FIND(kredist_tree, &kt->kredist, &node);
	if (r == NULL || dynamic != r->dynamic)
		return;

	if (RB_REMOVE(kredist_tree, &kt->kredist, r) == NULL) {
		log_warnx("%s: failed to remove network %s/%u", __func__,
		    log_addr(&node.prefix), node.prefixlen);
		return;
	}
	free(r);

	if (send_network(IMSG_NETWORK_REMOVE, net, NULL) == -1)
		log_warnx("%s: faild to send network removal", __func__);
}

int
kr_net_match(struct ktable *kt, struct network_config *net, uint16_t flags,
    int loopback)
{
	struct network		*xn;

	TAILQ_FOREACH(xn, &kt->krn, entry) {
		if (xn->net.prefix.aid != net->prefix.aid)
			continue;
		switch (xn->net.type) {
		case NETWORK_DEFAULT:
			/* static match already redistributed */
			continue;
		case NETWORK_STATIC:
			/* Skip networks with nexthop on loopback. */
			if (loopback)
				continue;
			if (flags & F_STATIC)
				break;
			continue;
		case NETWORK_CONNECTED:
			/* Skip networks with nexthop on loopback. */
			if (loopback)
				continue;
			if (flags & F_CONNECTED)
				break;
			continue;
		case NETWORK_RTLABEL:
			if (net->rtlabel == xn->net.rtlabel)
				break;
			continue;
		case NETWORK_PRIORITY:
			if (net->priority == xn->net.priority)
				break;
			continue;
		case NETWORK_MRTCLONE:
		case NETWORK_PREFIXSET:
			/* must not happen */
			log_warnx("%s: found a NETWORK_PREFIXSET, "
			    "please send a bug report", __func__);
			continue;
		}

		net->rd = xn->net.rd;
		if (kr_net_redist_add(kt, net, &xn->net.attrset, 1))
			return (1);
	}
	return (0);
}

struct network *
kr_net_find(struct ktable *kt, struct network *n)
{
	struct network		*xn;

	TAILQ_FOREACH(xn, &kt->krn, entry) {
		if (n->net.type != xn->net.type ||
		    n->net.prefixlen != xn->net.prefixlen ||
		    n->net.rd != xn->net.rd)
			continue;
		if (memcmp(&n->net.prefix, &xn->net.prefix,
		    sizeof(n->net.prefix)) == 0)
			return (xn);
	}
	return (NULL);
}

void
kr_net_reload(u_int rtableid, uint64_t rd, struct network_head *nh)
{
	struct network		*n, *xn;
	struct ktable		*kt;

	if ((kt = ktable_get(rtableid)) == NULL)
		fatalx("%s: non-existent rtableid %d", __func__, rtableid);

	while ((n = TAILQ_FIRST(nh)) != NULL) {
		TAILQ_REMOVE(nh, n, entry);
		n->net.old = 0;
		n->net.rd = rd;
		xn = kr_net_find(kt, n);
		if (xn) {
			xn->net.old = 0;
			filterset_free(&xn->net.attrset);
			filterset_move(&n->net.attrset, &xn->net.attrset);
			kr_net_delete(n);
		} else
			TAILQ_INSERT_TAIL(&kt->krn, n, entry);
	}
}

void
kr_net_clear(struct ktable *kt)
{
	struct network *n, *xn;

	TAILQ_FOREACH_SAFE(n, &kt->krn, entry, xn) {
		TAILQ_REMOVE(&kt->krn, n, entry);
		if (n->net.type == NETWORK_DEFAULT)
			kr_net_redist_del(kt, &n->net, 0);
		kr_net_delete(n);
	}
}

void
kr_redistribute(int type, struct ktable *kt, struct kroute *kr)
{
	struct network_config	 net;
	uint32_t		 a;
	int			 loflag = 0;

	bzero(&net, sizeof(net));
	net.prefix.aid = AID_INET;
	net.prefix.v4.s_addr = kr->prefix.s_addr;
	net.prefixlen = kr->prefixlen;
	net.rtlabel = kr->labelid;
	net.priority = kr->priority;

	/* shortcut for removals */
	if (type == IMSG_NETWORK_REMOVE) {
		kr_net_redist_del(kt, &net, 1);
		return;
	}

	if (!(kr->flags & F_KERNEL))
		return;

	/*
	 * We consider the loopback net and multicast addresses
	 * as not redistributable.
	 */
	a = ntohl(kr->prefix.s_addr);
	if (IN_MULTICAST(a) || (a >> IN_CLASSA_NSHIFT) == IN_LOOPBACKNET)
		return;

	/* Check if the nexthop is the loopback addr. */
	if (kr->nexthop.s_addr == htonl(INADDR_LOOPBACK))
		loflag = 1;

	/*
	 * never allow 0.0.0.0/0 the default route can only be redistributed
	 * with announce default.
	 */
	if (kr->prefix.s_addr == INADDR_ANY && kr->prefixlen == 0)
		return;

	if (kr_net_match(kt, &net, kr->flags, loflag) == 0)
		/* no longer matches, if still present remove it */
		kr_net_redist_del(kt, &net, 1);
}

void
kr_redistribute6(int type, struct ktable *kt, struct kroute6 *kr6)
{
	struct network_config	net;
	int			loflag = 0;

	bzero(&net, sizeof(net));
	net.prefix.aid = AID_INET6;
	memcpy(&net.prefix.v6, &kr6->prefix, sizeof(struct in6_addr));
	net.prefixlen = kr6->prefixlen;
	net.rtlabel = kr6->labelid;
	net.priority = kr6->priority;

	/* shortcut for removals */
	if (type == IMSG_NETWORK_REMOVE) {
		kr_net_redist_del(kt, &net, 1);
		return;
	}

	if (!(kr6->flags & F_KERNEL))
		return;

	/*
	 * We consider unspecified, loopback, multicast, link- and site-local,
	 * IPv4 mapped and IPv4 compatible addresses as not redistributable.
	 */
	if (IN6_IS_ADDR_UNSPECIFIED(&kr6->prefix) ||
	    IN6_IS_ADDR_LOOPBACK(&kr6->prefix) ||
	    IN6_IS_ADDR_MULTICAST(&kr6->prefix) ||
	    IN6_IS_ADDR_LINKLOCAL(&kr6->prefix) ||
	    IN6_IS_ADDR_SITELOCAL(&kr6->prefix) ||
	    IN6_IS_ADDR_V4MAPPED(&kr6->prefix) ||
	    IN6_IS_ADDR_V4COMPAT(&kr6->prefix))
		return;

	/* Check if the nexthop is the loopback addr. */
	if (IN6_IS_ADDR_LOOPBACK(&kr6->nexthop))
		loflag = 1;

	/*
	 * never allow ::/0 the default route can only be redistributed
	 * with announce default.
	 */
	if (kr6->prefixlen == 0 &&
	    memcmp(&kr6->prefix, &in6addr_any, sizeof(struct in6_addr)) == 0)
		return;

	if (kr_net_match(kt, &net, kr6->flags, loflag) == 0)
		/* no longer matches, if still present remove it */
		kr_net_redist_del(kt, &net, 1);
}

void
ktable_preload(void)
{
	struct ktable	*kt;
	struct network	*n;
	u_int		 i;

	for (i = 0; i < krt_size; i++) {
		if ((kt = ktable_get(i)) == NULL)
			continue;
		kt->state = RECONF_DELETE;

		/* mark all networks as old */
		TAILQ_FOREACH(n, &kt->krn, entry)
			n->net.old = 1;
	}
}

void
ktable_postload(void)
{
	struct ktable	*kt;
	struct network	*n, *xn;
	u_int		 i;

	for (i = krt_size; i > 0; i--) {
		if ((kt = ktable_get(i - 1)) == NULL)
			continue;
		if (kt->state == RECONF_DELETE) {
			ktable_free(i - 1);
			continue;
		} else if (kt->state == RECONF_REINIT)
			kt->fib_sync = kt->fib_conf;

		/* cleanup old networks */
		TAILQ_FOREACH_SAFE(n, &kt->krn, entry, xn) {
			if (n->net.old) {
				TAILQ_REMOVE(&kt->krn, n, entry);
				if (n->net.type == NETWORK_DEFAULT)
					kr_net_redist_del(kt, &n->net, 0);
				kr_net_delete(n);
			}
		}
	}
}

int
kr_reload(void)
{
	struct ktable	*kt;
	struct kroute	*kr;
	struct kroute6	*kr6;
	struct knexthop	*nh;
	struct network	*n;
	u_int		 rid;
	int		 hasdyn = 0;

	for (rid = 0; rid < krt_size; rid++) {
		if ((kt = ktable_get(rid)) == NULL)
			continue;

		/* if this is the main nexthop table revalidate nexthops */
		if (kt->rtableid == kt->nhtableid)
			RB_FOREACH(nh, knexthop_tree, KT2KNT(kt))
				knexthop_validate(kt, nh);

		TAILQ_FOREACH(n, &kt->krn, entry)
			if (n->net.type == NETWORK_DEFAULT) {
				kr_net_redist_add(kt, &n->net,
				    &n->net.attrset, 0);
			} else
				hasdyn = 1;

		if (hasdyn) {
			/* only evaluate the full tree if we need */
			RB_FOREACH(kr, kroute_tree, &kt->krt)
				kr_redistribute(IMSG_NETWORK_ADD, kt, kr);
			RB_FOREACH(kr6, kroute6_tree, &kt->krt6)
				kr_redistribute6(IMSG_NETWORK_ADD, kt, kr6);
		}
	}

	return (0);
}

struct kroute_full *
kr_tofull(struct kroute *kr)
{
	static struct kroute_full	kf;

	bzero(&kf, sizeof(kf));

	kf.prefix.aid = AID_INET;
	kf.prefix.v4.s_addr = kr->prefix.s_addr;
	kf.nexthop.aid = AID_INET;
	kf.nexthop.v4.s_addr = kr->nexthop.s_addr;
	strlcpy(kf.label, rtlabel_id2name(kr->labelid), sizeof(kf.label));
	kf.flags = kr->flags;
	kf.ifindex = kr->ifindex;
	kf.prefixlen = kr->prefixlen;
	kf.priority = kr->priority == RTP_MINE ?
	    kr_state.fib_prio : kr->priority;
	kf.mplslabel = kr->mplslabel;

	return (&kf);
}

struct kroute_full *
kr6_tofull(struct kroute6 *kr6)
{
	static struct kroute_full	kf;

	bzero(&kf, sizeof(kf));

	kf.prefix.aid = AID_INET6;
	memcpy(&kf.prefix.v6, &kr6->prefix, sizeof(struct in6_addr));
	kf.prefix.scope_id = kr6->prefix_scope_id;
	kf.nexthop.aid = AID_INET6;
	memcpy(&kf.nexthop.v6, &kr6->nexthop, sizeof(struct in6_addr));
	kf.nexthop.scope_id = kr6->nexthop_scope_id;
	strlcpy(kf.label, rtlabel_id2name(kr6->labelid), sizeof(kf.label));
	kf.flags = kr6->flags;
	kf.ifindex = kr6->ifindex;
	kf.prefixlen = kr6->prefixlen;
	kf.priority = kr6->priority == RTP_MINE ?
	    kr_state.fib_prio : kr6->priority;
	kf.mplslabel = kr6->mplslabel;

	return (&kf);
}

/*
 * RB-tree compare functions
 */

int
kroute_compare(struct kroute *a, struct kroute *b)
{
	if (ntohl(a->prefix.s_addr) < ntohl(b->prefix.s_addr))
		return (-1);
	if (ntohl(a->prefix.s_addr) > ntohl(b->prefix.s_addr))
		return (1);
	if (a->prefixlen < b->prefixlen)
		return (-1);
	if (a->prefixlen > b->prefixlen)
		return (1);

	/* if the priority is RTP_ANY finish on the first address hit */
	if (a->priority == RTP_ANY || b->priority == RTP_ANY)
		return (0);
	if (a->priority < b->priority)
		return (-1);
	if (a->priority > b->priority)
		return (1);
	return (0);
}

int
kroute6_compare(struct kroute6 *a, struct kroute6 *b)
{
	int i;

	for (i = 0; i < 16; i++) {
		if (a->prefix.s6_addr[i] < b->prefix.s6_addr[i])
			return (-1);
		if (a->prefix.s6_addr[i] > b->prefix.s6_addr[i])
			return (1);
	}

	if (a->prefixlen < b->prefixlen)
		return (-1);
	if (a->prefixlen > b->prefixlen)
		return (1);

	/* if the priority is RTP_ANY finish on the first address hit */
	if (a->priority == RTP_ANY || b->priority == RTP_ANY)
		return (0);
	if (a->priority < b->priority)
		return (-1);
	if (a->priority > b->priority)
		return (1);
	return (0);
}

int
knexthop_compare(struct knexthop *a, struct knexthop *b)
{
	int	i;

	if (a->nexthop.aid != b->nexthop.aid)
		return (b->nexthop.aid - a->nexthop.aid);

	switch (a->nexthop.aid) {
	case AID_INET:
		if (ntohl(a->nexthop.v4.s_addr) < ntohl(b->nexthop.v4.s_addr))
			return (-1);
		if (ntohl(a->nexthop.v4.s_addr) > ntohl(b->nexthop.v4.s_addr))
			return (1);
		break;
	case AID_INET6:
		for (i = 0; i < 16; i++) {
			if (a->nexthop.v6.s6_addr[i] < b->nexthop.v6.s6_addr[i])
				return (-1);
			if (a->nexthop.v6.s6_addr[i] > b->nexthop.v6.s6_addr[i])
				return (1);
		}
		break;
	default:
		fatalx("%s: unknown AF", __func__);
	}

	return (0);
}

int
kredist_compare(struct kredist_node *a, struct kredist_node *b)
{
	int	i;

	if (a->prefix.aid != b->prefix.aid)
		return (b->prefix.aid - a->prefix.aid);

	if (a->prefixlen < b->prefixlen)
		return (-1);
	if (a->prefixlen > b->prefixlen)
		return (1);

	switch (a->prefix.aid) {
	case AID_INET:
		if (ntohl(a->prefix.v4.s_addr) < ntohl(b->prefix.v4.s_addr))
			return (-1);
		if (ntohl(a->prefix.v4.s_addr) > ntohl(b->prefix.v4.s_addr))
			return (1);
		break;
	case AID_INET6:
		for (i = 0; i < 16; i++) {
			if (a->prefix.v6.s6_addr[i] < b->prefix.v6.s6_addr[i])
				return (-1);
			if (a->prefix.v6.s6_addr[i] > b->prefix.v6.s6_addr[i])
				return (1);
		}
		break;
	default:
		fatalx("%s: unknown AF", __func__);
	}

	if (a->rd < b->rd)
		return (-1);
	if (a->rd > b->rd)
		return (1);

	return (0);
}

int
kif_compare(struct kif_node *a, struct kif_node *b)
{
	return (b->k.ifindex - a->k.ifindex);
}


/*
 * tree management functions
 */

struct kroute *
kroute_find(struct ktable *kt, const struct bgpd_addr *prefix,
    uint8_t prefixlen, uint8_t prio)
{
	struct kroute	 s;
	struct kroute	*kn, *tmp;

	s.prefix = prefix->v4;
	s.prefixlen = prefixlen;
	s.priority = prio;

	kn = RB_FIND(kroute_tree, &kt->krt, &s);
	if (kn && prio == RTP_ANY) {
		tmp = RB_PREV(kroute_tree, &kt->krt, kn);
		while (tmp) {
			if (kroute_compare(&s, tmp) == 0)
				kn = tmp;
			else
				break;
			tmp = RB_PREV(kroute_tree, &kt->krt, kn);
		}
	}
	return (kn);
}

struct kroute *
kroute_matchgw(struct kroute *kr, struct bgpd_addr *gw)
{
	in_addr_t	nexthop;

	if (gw->aid != AID_INET) {
		log_warnx("%s: no nexthop defined", __func__);
		return (NULL);
	}
	nexthop = gw->v4.s_addr;

	do {
		if (kr->nexthop.s_addr == nexthop)
			return (kr);
		kr = kr->next;
	} while (kr);

	return (NULL);
}

int
kroute_insert(struct ktable *kt, struct kroute *kr)
{
	struct kroute	*krm;
	struct knexthop	*h;
	struct in_addr	 ina, inb;

	if ((krm = RB_INSERT(kroute_tree, &kt->krt, kr)) != NULL) {
		/* multipath route, add at end of list */
		while (krm->next != NULL)
			krm = krm->next;
		krm->next = kr;
		kr->next = NULL; /* to be sure */
	}

	/* XXX this is wrong for nexthop validated via BGP */
	if (kr->flags & F_KERNEL) {
		inet4applymask(&ina, &kr->prefix, kr->prefixlen);
		RB_FOREACH(h, knexthop_tree, KT2KNT(kt))
			if (h->nexthop.aid == AID_INET) {
				inet4applymask(&inb, &h->nexthop.v4,
				    kr->prefixlen);
				if (memcmp(&ina, &inb, sizeof(ina)) == 0)
					knexthop_validate(kt, h);
			}

		if (kr->flags & F_CONNECTED)
			if (kif_kr_insert(kr) == -1)
				return (-1);

		if (krm == NULL)
			/* redistribute multipath routes only once */
			kr_redistribute(IMSG_NETWORK_ADD, kt, kr);
	}
	return (0);
}


int
kroute_remove(struct ktable *kt, struct kroute *kr)
{
	struct kroute	*krm;
	struct knexthop	*s;

	if ((krm = RB_FIND(kroute_tree, &kt->krt, kr)) == NULL) {
		log_warnx("%s: failed to find %s/%u", __func__,
		    inet_ntoa(kr->prefix), kr->prefixlen);
		return (-1);
	}

	if (krm == kr) {
		/* head element */
		if (RB_REMOVE(kroute_tree, &kt->krt, kr) == NULL) {
			log_warnx("%s: failed for %s/%u", __func__,
			    inet_ntoa(kr->prefix), kr->prefixlen);
			return (-1);
		}
		if (kr->next != NULL) {
			if (RB_INSERT(kroute_tree, &kt->krt, kr->next) !=
			    NULL) {
				log_warnx("%s: failed to add %s/%u", __func__,
				    inet_ntoa(kr->prefix), kr->prefixlen);
				return (-1);
			}
		}
	} else {
		/* somewhere in the list */
		while (krm->next != kr && krm->next != NULL)
			krm = krm->next;
		if (krm->next == NULL) {
			log_warnx("%s: multipath list corrupted for %s/%u",
			    __func__, inet_ntoa(kr->prefix), kr->prefixlen);
			return (-1);
		}
		krm->next = kr->next;
	}

	/* check whether a nexthop depends on this kroute */
	if (kr->flags & F_NEXTHOP)
		RB_FOREACH(s, knexthop_tree, KT2KNT(kt))
			if (s->kroute == kr)
				knexthop_validate(kt, s);

	if (kr->flags & F_KERNEL && kr == krm && kr->next == NULL)
		/* again remove only once */
		kr_redistribute(IMSG_NETWORK_REMOVE, kt, kr);

	if (kr->flags & F_CONNECTED)
		if (kif_kr_remove(kr) == -1) {
			rtlabel_unref(kr->labelid);
			free(kr);
			return (-1);
		}

	rtlabel_unref(kr->labelid);

	free(kr);
	return (0);
}

void
kroute_clear(struct ktable *kt)
{
	struct kroute	*kr;

	while ((kr = RB_MIN(kroute_tree, &kt->krt)) != NULL)
		kroute_remove(kt, kr);
}

struct kroute6 *
kroute6_find(struct ktable *kt, const struct bgpd_addr *prefix,
    uint8_t prefixlen, uint8_t prio)
{
	struct kroute6	s;
	struct kroute6	*kn6, *tmp;

	s.prefix = prefix->v6;
	s.prefixlen = prefixlen;
	s.priority = prio;

	kn6 = RB_FIND(kroute6_tree, &kt->krt6, &s);
	if (kn6 && prio == RTP_ANY) {
		tmp = RB_PREV(kroute6_tree, &kt->krt6, kn6);
		while (tmp) {
			if (kroute6_compare(&s, tmp) == 0)
				kn6 = tmp;
			else
				break;
			tmp = RB_PREV(kroute6_tree, &kt->krt6, kn6);
		}
	}
	return (kn6);
}

struct kroute6 *
kroute6_matchgw(struct kroute6 *kr, struct bgpd_addr *gw)
{
	struct in6_addr	nexthop;

	if (gw->aid != AID_INET6) {
		log_warnx("%s: no nexthop defined", __func__);
		return (NULL);
	}
	nexthop = gw->v6;

	do {
		if (memcmp(&kr->nexthop, &nexthop, sizeof(nexthop)) == 0)
			return (kr);
		kr = kr->next;
	} while (kr);

	return (NULL);
}

int
kroute6_insert(struct ktable *kt, struct kroute6 *kr)
{
	struct kroute6	*krm;
	struct knexthop	*h;
	struct in6_addr	 ina, inb;

	if ((krm = RB_INSERT(kroute6_tree, &kt->krt6, kr)) != NULL) {
		/* multipath route, add at end of list */
		while (krm->next != NULL)
			krm = krm->next;
		krm->next = kr;
		kr->next = NULL; /* to be sure */
	}

	/* XXX this is wrong for nexthop validated via BGP */
	if (kr->flags & F_KERNEL) {
		inet6applymask(&ina, &kr->prefix, kr->prefixlen);
		RB_FOREACH(h, knexthop_tree, KT2KNT(kt))
			if (h->nexthop.aid == AID_INET6) {
				inet6applymask(&inb, &h->nexthop.v6,
				    kr->prefixlen);
				if (memcmp(&ina, &inb, sizeof(ina)) == 0)
					knexthop_validate(kt, h);
			}

		if (kr->flags & F_CONNECTED)
			if (kif_kr6_insert(kr) == -1)
				return (-1);

		if (krm == NULL)
			/* redistribute multipath routes only once */
			kr_redistribute6(IMSG_NETWORK_ADD, kt, kr);
	}

	return (0);
}

int
kroute6_remove(struct ktable *kt, struct kroute6 *kr)
{
	struct kroute6	*krm;
	struct knexthop	*s;

	if ((krm = RB_FIND(kroute6_tree, &kt->krt6, kr)) == NULL) {
		log_warnx("%s: failed for %s/%u", __func__,
		    log_in6addr(&kr->prefix), kr->prefixlen);
		return (-1);
	}

	if (krm == kr) {
		/* head element */
		if (RB_REMOVE(kroute6_tree, &kt->krt6, kr) == NULL) {
			log_warnx("%s: failed for %s/%u", __func__,
			    log_in6addr(&kr->prefix), kr->prefixlen);
			return (-1);
		}
		if (kr->next != NULL) {
			if (RB_INSERT(kroute6_tree, &kt->krt6, kr->next) !=
			    NULL) {
				log_warnx("%s: failed to add %s/%u", __func__,
				    log_in6addr(&kr->prefix), kr->prefixlen);
				return (-1);
			}
		}
	} else {
		/* somewhere in the list */
		while (krm->next != kr && krm->next != NULL)
			krm = krm->next;
		if (krm->next == NULL) {
			log_warnx("%s: multipath list corrupted for %s/%u",
			    __func__, log_in6addr(&kr->prefix), kr->prefixlen);
			return (-1);
		}
		krm->next = kr->next;
	}

	/* check whether a nexthop depends on this kroute */
	if (kr->flags & F_NEXTHOP)
		RB_FOREACH(s, knexthop_tree, KT2KNT(kt))
			if (s->kroute == kr)
				knexthop_validate(kt, s);

	if (kr->flags & F_KERNEL && kr == krm && kr->next == NULL)
		/* again remove only once */
		kr_redistribute6(IMSG_NETWORK_REMOVE, kt, kr);

	if (kr->flags & F_CONNECTED)
		if (kif_kr6_remove(kr) == -1) {
			rtlabel_unref(kr->labelid);
			free(kr);
			return (-1);
		}

	rtlabel_unref(kr->labelid);

	free(kr);
	return (0);
}

void
kroute6_clear(struct ktable *kt)
{
	struct kroute6	*kr;

	while ((kr = RB_MIN(kroute6_tree, &kt->krt6)) != NULL)
		kroute6_remove(kt, kr);
}

struct knexthop *
knexthop_find(struct ktable *kt, struct bgpd_addr *addr)
{
	struct knexthop	s;

	bzero(&s, sizeof(s));
	memcpy(&s.nexthop, addr, sizeof(s.nexthop));

	return (RB_FIND(knexthop_tree, KT2KNT(kt), &s));
}

int
knexthop_insert(struct ktable *kt, struct knexthop *kn)
{
	if (RB_INSERT(knexthop_tree, KT2KNT(kt), kn) != NULL) {
		log_warnx("%s: failed for %s", __func__,
		    log_addr(&kn->nexthop));
		free(kn);
		return (-1);
	}

	knexthop_validate(kt, kn);

	return (0);
}

int
knexthop_remove(struct ktable *kt, struct knexthop *kn)
{
	kroute_detach_nexthop(kt, kn);

	if (RB_REMOVE(knexthop_tree, KT2KNT(kt), kn) == NULL) {
		log_warnx("%s: failed for %s", __func__,
		    log_addr(&kn->nexthop));
		return (-1);
	}

	free(kn);
	return (0);
}

void
knexthop_clear(struct ktable *kt)
{
	struct knexthop	*kn;

	while ((kn = RB_MIN(knexthop_tree, KT2KNT(kt))) != NULL)
		knexthop_remove(kt, kn);
}

struct kif_node *
kif_find(int ifindex)
{
	struct kif_node	s;

	bzero(&s, sizeof(s));
	s.k.ifindex = ifindex;

	return (RB_FIND(kif_tree, &kit, &s));
}

int
kif_insert(struct kif_node *kif)
{
	LIST_INIT(&kif->kroute_l);
	LIST_INIT(&kif->kroute6_l);

	if (RB_INSERT(kif_tree, &kit, kif) != NULL) {
		log_warnx("RB_INSERT(kif_tree, &kit, kif)");
		free(kif);
		return (-1);
	}

	return (0);
}

int
kif_remove(struct kif_node *kif)
{
	struct ktable	*kt;
	struct kif_kr	*kkr;
	struct kif_kr6	*kkr6;

	if (RB_REMOVE(kif_tree, &kit, kif) == NULL) {
		log_warnx("RB_REMOVE(kif_tree, &kit, kif)");
		return (-1);
	}

	if ((kt = ktable_get(kif->k.rdomain)) == NULL)
		goto done;

	while ((kkr = LIST_FIRST(&kif->kroute_l)) != NULL) {
		LIST_REMOVE(kkr, entry);
		kkr->kr->flags &= ~F_NEXTHOP;
		kroute_remove(kt, kkr->kr);
		free(kkr);
	}

	while ((kkr6 = LIST_FIRST(&kif->kroute6_l)) != NULL) {
		LIST_REMOVE(kkr6, entry);
		kkr6->kr->flags &= ~F_NEXTHOP;
		kroute6_remove(kt, kkr6->kr);
		free(kkr6);
	}
done:
	free(kif);
	return (0);
}

void
kif_clear(void)
{
	struct kif_node	*kif;

	while ((kif = RB_MIN(kif_tree, &kit)) != NULL)
		kif_remove(kif);
}

int
kif_kr_insert(struct kroute *kr)
{
	struct kif_node	*kif;
	struct kif_kr	*kkr;

	if ((kif = kif_find(kr->ifindex)) == NULL) {
		if (kr->ifindex)
			log_warnx("%s: interface with index %u not found",
			    __func__, kr->ifindex);
		return (0);
	}

	if (kif->k.nh_reachable)
		kr->flags &= ~F_DOWN;
	else
		kr->flags |= F_DOWN;

	if ((kkr = calloc(1, sizeof(*kkr))) == NULL) {
		log_warn("%s", __func__);
		return (-1);
	}

	kkr->kr = kr;

	LIST_INSERT_HEAD(&kif->kroute_l, kkr, entry);

	return (0);
}

int
kif_kr_remove(struct kroute *kr)
{
	struct kif_node	*kif;
	struct kif_kr	*kkr;

	if ((kif = kif_find(kr->ifindex)) == NULL) {
		if (kr->ifindex)
			log_warnx("%s: interface with index %u not found",
			    __func__, kr->ifindex);
		return (0);
	}

	for (kkr = LIST_FIRST(&kif->kroute_l); kkr != NULL && kkr->kr != kr;
	    kkr = LIST_NEXT(kkr, entry))
		;	/* nothing */

	if (kkr == NULL) {
		log_warnx("%s: can't remove connected route from interface "
		    "with index %u: not found", __func__, kr->ifindex);
		return (-1);
	}

	LIST_REMOVE(kkr, entry);
	free(kkr);

	return (0);
}

int
kif_kr6_insert(struct kroute6 *kr)
{
	struct kif_node	*kif;
	struct kif_kr6	*kkr6;

	if ((kif = kif_find(kr->ifindex)) == NULL) {
		if (kr->ifindex)
			log_warnx("%s: interface with index %u not found",
			    __func__, kr->ifindex);
		return (0);
	}

	if (kif->k.nh_reachable)
		kr->flags &= ~F_DOWN;
	else
		kr->flags |= F_DOWN;

	if ((kkr6 = calloc(1, sizeof(*kkr6))) == NULL) {
		log_warn("%s", __func__);
		return (-1);
	}

	kkr6->kr = kr;

	LIST_INSERT_HEAD(&kif->kroute6_l, kkr6, entry);

	return (0);
}

int
kif_kr6_remove(struct kroute6 *kr)
{
	struct kif_node	*kif;
	struct kif_kr6	*kkr6;

	if ((kif = kif_find(kr->ifindex)) == NULL) {
		if (kr->ifindex)
			log_warnx("%s: interface with index %u not found",
			    __func__, kr->ifindex);
		return (0);
	}

	for (kkr6 = LIST_FIRST(&kif->kroute6_l); kkr6 != NULL && kkr6->kr != kr;
	    kkr6 = LIST_NEXT(kkr6, entry))
		;	/* nothing */

	if (kkr6 == NULL) {
		log_warnx("%s: can't remove connected route from interface "
		    "with index %u: not found", __func__, kr->ifindex);
		return (-1);
	}

	LIST_REMOVE(kkr6, entry);
	free(kkr6);

	return (0);
}

/*
 * nexthop validation
 */

static int
kif_validate(struct kif *kif)
{
	if (!(kif->flags & IFF_UP))
		return (0);

	/*
	 * we treat link_state == LINK_STATE_UNKNOWN as valid,
	 * not all interfaces have a concept of "link state" and/or
	 * do not report up
	 */

	if (kif->link_state == LINK_STATE_DOWN)
		return (0);

	return (1);
}

/*
 * return 1 when the interface is up and the link state is up or unknwown
 * except when this is a carp interface, then return 1 only when link state
 * is up
 */
static int
kif_depend_state(struct kif *kif)
{
	if (!(kif->flags & IFF_UP))
		return (0);


	if (kif->if_type == IFT_CARP &&
	    kif->link_state == LINK_STATE_UNKNOWN)
		return (0);

	return LINK_STATE_IS_UP(kif->link_state);
}


int
kroute_validate(struct kroute *kr)
{
	struct kif_node		*kif;

	if (kr->flags & (F_REJECT | F_BLACKHOLE))
		return (0);

	if ((kif = kif_find(kr->ifindex)) == NULL) {
		if (kr->ifindex)
			log_warnx("%s: interface with index %d not found, "
			    "referenced from route for %s/%u", __func__,
			    kr->ifindex, inet_ntoa(kr->prefix),
			    kr->prefixlen);
		return (1);
	}

	return (kif->k.nh_reachable);
}

int
kroute6_validate(struct kroute6 *kr)
{
	struct kif_node		*kif;

	if (kr->flags & (F_REJECT | F_BLACKHOLE))
		return (0);

	if ((kif = kif_find(kr->ifindex)) == NULL) {
		if (kr->ifindex)
			log_warnx("%s: interface with index %d not found, "
			    "referenced from route for %s/%u", __func__,
			    kr->ifindex, log_in6addr(&kr->prefix),
			    kr->prefixlen);
		return (1);
	}

	return (kif->k.nh_reachable);
}

void
knexthop_validate(struct ktable *kt, struct knexthop *kn)
{
	void		*oldk;
	struct kroute	*kr;
	struct kroute6	*kr6;

	oldk = kn->kroute;
	kroute_detach_nexthop(kt, kn);

	if ((kt = ktable_get(kt->nhtableid)) == NULL)
		fatalx("%s: lost nexthop routing table", __func__);

	switch (kn->nexthop.aid) {
	case AID_INET:
		kr = kroute_match(kt, &kn->nexthop, 0);

		if (kr) {
			kn->kroute = kr;
			kr->flags |= F_NEXTHOP;
		}

		/*
		 * Send update if nexthop route changed under us if
		 * the route remains the same then the NH state has not
		 * changed. State changes are tracked by knexthop_track().
		 */
		if (kr != oldk)
			knexthop_send_update(kn);
		break;
	case AID_INET6:
		kr6 = kroute6_match(kt, &kn->nexthop, 0);

		if (kr6) {
			kn->kroute = kr6;
			kr6->flags |= F_NEXTHOP;
		}

		if (kr6 != oldk)
			knexthop_send_update(kn);
		break;
	}
}

void
knexthop_track(struct ktable *kt, void *krp)
{
	struct knexthop	*kn;

	RB_FOREACH(kn, knexthop_tree, KT2KNT(kt))
		if (kn->kroute == krp)
			knexthop_send_update(kn);
}

void
knexthop_send_update(struct knexthop *kn)
{
	struct kroute_nexthop	 n;
	struct kroute		*kr;
	struct kroute6		*kr6;

	bzero(&n, sizeof(n));
	memcpy(&n.nexthop, &kn->nexthop, sizeof(n.nexthop));

	if (kn->kroute == NULL) {
		n.valid = 0;	/* NH is not valid */
		send_nexthop_update(&n);
		return;
	}

	switch (kn->nexthop.aid) {
	case AID_INET:
		kr = kn->kroute;
		n.valid = kroute_validate(kr);
		n.connected = kr->flags & F_CONNECTED;
		if (kr->nexthop.s_addr != 0) {
			n.gateway.aid = AID_INET;
			n.gateway.v4.s_addr = kr->nexthop.s_addr;
		}
		if (n.connected) {
			n.net.aid = AID_INET;
			n.net.v4.s_addr = kr->prefix.s_addr;
			n.netlen = kr->prefixlen;
		}
		break;
	case AID_INET6:
		kr6 = kn->kroute;
		n.valid = kroute6_validate(kr6);
		n.connected = kr6->flags & F_CONNECTED;
		if (memcmp(&kr6->nexthop, &in6addr_any,
		    sizeof(struct in6_addr)) != 0) {
			n.gateway.aid = AID_INET6;
			memcpy(&n.gateway.v6, &kr6->nexthop,
			    sizeof(struct in6_addr));
			n.gateway.scope_id = kr6->nexthop_scope_id;
		}
		if (n.connected) {
			n.net.aid = AID_INET6;
			memcpy(&n.net.v6, &kr6->prefix,
			    sizeof(struct in6_addr));
			n.net.scope_id = kr6->prefix_scope_id;
			n.netlen = kr6->prefixlen;
		}
		break;
	}
	send_nexthop_update(&n);
}

struct kroute *
kroute_match(struct ktable *kt, struct bgpd_addr *key, int matchall)
{
	int			 i;
	struct kroute		*kr;
	struct bgpd_addr	 masked;

	for (i = 32; i >= 0; i--) {
		applymask(&masked, key, i);
		if ((kr = kroute_find(kt, &masked, i, RTP_ANY)) != NULL)
			if (matchall ||
			    bgpd_filternexthop(kr_tofull(kr)) == 0)
				return (kr);
	}

	return (NULL);
}

struct kroute6 *
kroute6_match(struct ktable *kt, struct bgpd_addr *key, int matchall)
{
	int			 i;
	struct kroute6		*kr6;
	struct bgpd_addr	 masked;

	for (i = 128; i >= 0; i--) {
		applymask(&masked, key, i);
		if ((kr6 = kroute6_find(kt, &masked, i, RTP_ANY)) != NULL)
			if (matchall ||
			    bgpd_filternexthop(kr6_tofull(kr6)) == 0)
				return (kr6);
	}

	return (NULL);
}

void
kroute_detach_nexthop(struct ktable *kt, struct knexthop *kn)
{
	struct knexthop	*s;
	struct kroute	*k;
	struct kroute6	*k6;

	if (kn->kroute == NULL)
		return;

	/*
	 * check whether there's another nexthop depending on this kroute
	 * if not remove the flag
	 */
	RB_FOREACH(s, knexthop_tree, KT2KNT(kt))
		if (s->kroute == kn->kroute && s != kn)
			break;

	if (s == NULL) {
		switch (kn->nexthop.aid) {
		case AID_INET:
			k = kn->kroute;
			k->flags &= ~F_NEXTHOP;
			break;
		case AID_INET6:
			k6 = kn->kroute;
			k6->flags &= ~F_NEXTHOP;
			break;
		}
	}

	kn->kroute = NULL;
}

/*
 * misc helpers
 */

uint8_t
prefixlen_classful(in_addr_t ina)
{
	/* it hurt to write this. */

	if (ina >= 0xf0000000U)		/* class E */
		return (32);
	else if (ina >= 0xe0000000U)	/* class D */
		return (4);
	else if (ina >= 0xc0000000U)	/* class C */
		return (24);
	else if (ina >= 0x80000000U)	/* class B */
		return (16);
	else				/* class A */
		return (8);
}

uint8_t
mask2prefixlen(in_addr_t ina)
{
	if (ina == 0)
		return (0);
	else
		return (33 - ffs(ntohl(ina)));
}

uint8_t
mask2prefixlen6(struct sockaddr_in6 *sa_in6)
{
	uint8_t	*ap, *ep;
	u_int	 l = 0;

	/*
	 * sin6_len is the size of the sockaddr so substract the offset of
	 * the possibly truncated sin6_addr struct.
	 */
	ap = (uint8_t *)&sa_in6->sin6_addr;
	ep = (uint8_t *)sa_in6 + sa_in6->sin6_len;
	for (; ap < ep; ap++) {
		/* this "beauty" is adopted from sbin/route/show.c ... */
		switch (*ap) {
		case 0xff:
			l += 8;
			break;
		case 0xfe:
			l += 7;
			goto done;
		case 0xfc:
			l += 6;
			goto done;
		case 0xf8:
			l += 5;
			goto done;
		case 0xf0:
			l += 4;
			goto done;
		case 0xe0:
			l += 3;
			goto done;
		case 0xc0:
			l += 2;
			goto done;
		case 0x80:
			l += 1;
			goto done;
		case 0x00:
			goto done;
		default:
			fatalx("non contiguous inet6 netmask");
		}
	}

 done:
	if (l > sizeof(struct in6_addr) * 8)
		fatalx("%s: prefixlen %d out of bound", __func__, l);
	return (l);
}

const struct if_status_description
		if_status_descriptions[] = LINK_STATE_DESCRIPTIONS;
const struct ifmedia_description
		ifm_type_descriptions[] = IFM_TYPE_DESCRIPTIONS;

uint64_t
ift2ifm(uint8_t if_type)
{
	switch (if_type) {
	case IFT_ETHER:
		return (IFM_ETHER);
	case IFT_FDDI:
		return (IFM_FDDI);
	case IFT_CARP:
		return (IFM_CARP);
	case IFT_IEEE80211:
		return (IFM_IEEE80211);
	default:
		return (0);
	}
}

const char *
get_media_descr(uint64_t media_type)
{
	const struct ifmedia_description	*p;

	for (p = ifm_type_descriptions; p->ifmt_string != NULL; p++)
		if (media_type == p->ifmt_word)
			return (p->ifmt_string);

	return ("unknown media");
}

const char *
get_linkstate(uint8_t if_type, int link_state)
{
	const struct if_status_description *p;
	static char buf[8];

	for (p = if_status_descriptions; p->ifs_string != NULL; p++) {
		if (LINK_STATE_DESC_MATCH(p, if_type, link_state))
			return (p->ifs_string);
	}
	snprintf(buf, sizeof(buf), "[#%d]", link_state);
	return (buf);
}

#define ROUNDUP(a) \
	((a) > 0 ? (1 + (((a) - 1) | (sizeof(long) - 1))) : sizeof(long))

void
get_rtaddrs(int addrs, struct sockaddr *sa, struct sockaddr **rti_info)
{
	int	i;

	for (i = 0; i < RTAX_MAX; i++) {
		if (addrs & (1 << i)) {
			rti_info[i] = sa;
			sa = (struct sockaddr *)((char *)(sa) +
			    ROUNDUP(sa->sa_len));
		} else
			rti_info[i] = NULL;
	}
}

void
if_change(u_short ifindex, int flags, struct if_data *ifd)
{
	struct ktable		*kt;
	struct kif_node		*kif;
	struct kif_kr		*kkr;
	struct kif_kr6		*kkr6;
	uint8_t			 reachable;

	if ((kif = kif_find(ifindex)) == NULL) {
		log_warnx("%s: interface with index %u not found",
		    __func__, ifindex);
		return;
	}

	log_info("%s: %s: rdomain %u %s, %s, %s, %s",
	    __func__, kif->k.ifname, ifd->ifi_rdomain,
	    flags & IFF_UP ? "UP" : "DOWN",
	    get_media_descr(ift2ifm(ifd->ifi_type)),
	    get_linkstate(ifd->ifi_type, ifd->ifi_link_state),
	    get_baudrate(ifd->ifi_baudrate, "bps"));

	kif->k.flags = flags;
	kif->k.link_state = ifd->ifi_link_state;
	kif->k.if_type = ifd->ifi_type;
	kif->k.rdomain = ifd->ifi_rdomain;
	kif->k.baudrate = ifd->ifi_baudrate;
	kif->k.depend_state = kif_depend_state(&kif->k);

	kr_send_dependon(&kif->k);

	if ((reachable = kif_validate(&kif->k)) == kif->k.nh_reachable)
		return;		/* nothing changed wrt nexthop validity */

	kif->k.nh_reachable = reachable;

	kt = ktable_get(kif->k.rdomain);

	LIST_FOREACH(kkr, &kif->kroute_l, entry) {
		if (reachable)
			kkr->kr->flags &= ~F_DOWN;
		else
			kkr->kr->flags |= F_DOWN;

		if (kt == NULL)
			continue;

		knexthop_track(kt, kkr->kr);
	}
	LIST_FOREACH(kkr6, &kif->kroute6_l, entry) {
		if (reachable)
			kkr6->kr->flags &= ~F_DOWN;
		else
			kkr6->kr->flags |= F_DOWN;

		if (kt == NULL)
			continue;

		knexthop_track(kt, kkr6->kr);
	}
}

void
if_announce(void *msg)
{
	struct if_announcemsghdr	*ifan;
	struct kif_node			*kif;

	ifan = msg;

	switch (ifan->ifan_what) {
	case IFAN_ARRIVAL:
		if ((kif = calloc(1, sizeof(*kif))) == NULL) {
			log_warn("%s", __func__);
			return;
		}

		kif->k.ifindex = ifan->ifan_index;
		strlcpy(kif->k.ifname, ifan->ifan_name, sizeof(kif->k.ifname));
		kif_insert(kif);
		break;
	case IFAN_DEPARTURE:
		kif = kif_find(ifan->ifan_index);
		kif_remove(kif);
		break;
	}
}

int
get_mpe_config(const char *name, u_int *rdomain, u_int *label)
{
	struct  ifreq	ifr;
	struct shim_hdr	shim;
	int		s;

	*label = 0;
	*rdomain = 0;

	s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s == -1)
		return (-1);

	bzero(&shim, sizeof(shim));
	bzero(&ifr, sizeof(ifr));
	strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));
	ifr.ifr_data = (caddr_t)&shim;

	if (ioctl(s, SIOCGETLABEL, (caddr_t)&ifr) == -1) {
		close(s);
		return (-1);
	}

	ifr.ifr_data = NULL;
	if (ioctl(s, SIOCGIFRDOMAIN, (caddr_t)&ifr) == -1) {
		close(s);
		return (-1);
	}

	close(s);

	*rdomain = ifr.ifr_rdomainid;
	*label = shim.shim_label;

	return (0);
}

/*
 * rtsock related functions
 */
#define satosin6(sa)	((struct sockaddr_in6 *)(sa))

int
send_rtmsg(int fd, int action, struct ktable *kt, struct kroute_full *kf)
{
	struct iovec		 iov[7];
	struct rt_msghdr	 hdr;
	struct sockaddr_storage	 prefix, nexthop, mask, ifp, label, mpls;
	struct bgpd_addr	 netmask;
	struct sockaddr		*sa;
	struct sockaddr_dl	*dl;
	struct sockaddr_mpls	*mp;
	struct sockaddr_rtlabel	*la;
	socklen_t		 salen;
	int			 iovcnt = 0;

	if (!kt->fib_sync)
		return (0);

	/* initialize header */
	bzero(&hdr, sizeof(hdr));
	hdr.rtm_version = RTM_VERSION;
	hdr.rtm_type = action;
	hdr.rtm_tableid = kt->rtableid;
	hdr.rtm_priority = kr_state.fib_prio;
	if (kf->flags & F_BLACKHOLE)
		hdr.rtm_flags |= RTF_BLACKHOLE;
	if (kf->flags & F_REJECT)
		hdr.rtm_flags |= RTF_REJECT;
	if (action == RTM_CHANGE)	/* reset these flags on change */
		hdr.rtm_fmask = RTF_REJECT|RTF_BLACKHOLE;
	hdr.rtm_seq = kr_state.rtseq++;	/* overflow doesn't matter */
	hdr.rtm_msglen = sizeof(hdr);
	/* adjust iovec */
	iov[iovcnt].iov_base = &hdr;
	iov[iovcnt++].iov_len = sizeof(hdr);

	bzero(&prefix, sizeof(prefix));
	sa = addr2sa(&kf->prefix, 0, &salen);
	sa->sa_len = salen;
#ifdef __KAME__
	/* XXX need to embed the stupid scope for now */
	if (sa->sa_family == AF_INET6 &&
	    (IN6_IS_ADDR_LINKLOCAL(&satosin6(sa)->sin6_addr) ||
	    IN6_IS_ADDR_MC_LINKLOCAL(&satosin6(sa)->sin6_addr) ||
	    IN6_IS_ADDR_MC_NODELOCAL(&satosin6(sa)->sin6_addr))) {
		*(u_int16_t *)&satosin6(sa)->sin6_addr.s6_addr[2] =
		    htons(satosin6(sa)->sin6_scope_id);
		satosin6(sa)->sin6_scope_id = 0;
	}
#endif
	memcpy(&prefix, sa, salen);
	/* adjust header */
	hdr.rtm_addrs |= RTA_DST;
	hdr.rtm_msglen += ROUNDUP(salen);
	/* adjust iovec */
	iov[iovcnt].iov_base = &prefix;
	iov[iovcnt++].iov_len = ROUNDUP(salen);

	/* XXX can we even have no nexthop ??? */
	if (kf->nexthop.aid != AID_UNSPEC) {
		bzero(&nexthop, sizeof(nexthop));
		sa = addr2sa(&kf->nexthop, 0, &salen);
		sa->sa_len = salen;
#ifdef __KAME__
		/* XXX need to embed the stupid scope for now */
		if (sa->sa_family == AF_INET6 &&
		    (IN6_IS_ADDR_LINKLOCAL(&satosin6(sa)->sin6_addr) ||
		    IN6_IS_ADDR_MC_LINKLOCAL(&satosin6(sa)->sin6_addr) ||
		    IN6_IS_ADDR_MC_NODELOCAL(&satosin6(sa)->sin6_addr))) {
			*(u_int16_t *)&satosin6(sa)->sin6_addr.s6_addr[2] =
			    htons(satosin6(sa)->sin6_scope_id);
			satosin6(sa)->sin6_scope_id = 0;
		}
#endif
		memcpy(&nexthop, sa, salen);
		/* adjust header */
		hdr.rtm_flags |= RTF_GATEWAY;
		hdr.rtm_addrs |= RTA_GATEWAY;
		hdr.rtm_msglen += ROUNDUP(salen);
		/* adjust iovec */
		iov[iovcnt].iov_base = &nexthop;
		iov[iovcnt++].iov_len = ROUNDUP(salen);
	}

	bzero(&netmask, sizeof(netmask));
	memset(&netmask.v6, 0xff, sizeof(netmask.v6));
	netmask.aid = kf->prefix.aid;
	applymask(&netmask, &netmask, kf->prefixlen);
	bzero(&mask, sizeof(mask));
	sa = addr2sa(&netmask, 0, &salen);
	sa->sa_len = salen;
	memcpy(&mask, sa, salen);
	/* adjust header */
	hdr.rtm_addrs |= RTA_NETMASK;
	hdr.rtm_msglen += ROUNDUP(salen);
	/* adjust iovec */
	iov[iovcnt].iov_base = &mask;
	iov[iovcnt++].iov_len = ROUNDUP(salen);

	if (kf->flags & F_MPLS) {
		/* need to force interface for mpe(4) routes */
		bzero(&ifp, sizeof(ifp));
		dl = (struct sockaddr_dl *)&ifp;
		salen = sizeof(*dl);
		dl->sdl_len = salen;
		dl->sdl_family = AF_LINK;
		dl->sdl_index = kf->ifindex;
		/* adjust header */
		hdr.rtm_addrs |= RTA_IFP;
		hdr.rtm_msglen += ROUNDUP(salen);
		/* adjust iovec */
		iov[iovcnt].iov_base = &ifp;
		iov[iovcnt++].iov_len = ROUNDUP(salen);

		bzero(&mpls, sizeof(mpls));
		mp = (struct sockaddr_mpls *)&mpls;
		salen = sizeof(*mp);
		mp->smpls_len = salen;
		mp->smpls_family = AF_MPLS;
		mp->smpls_label = kf->mplslabel;
		/* adjust header */
		hdr.rtm_flags |= RTF_MPLS;
		hdr.rtm_mpls = MPLS_OP_PUSH;
		hdr.rtm_addrs |= RTA_SRC;
		hdr.rtm_msglen += ROUNDUP(salen);
		/* clear gateway flag since this is for mpe(4) */
		hdr.rtm_flags &= ~RTF_GATEWAY;
		/* adjust iovec */
		iov[iovcnt].iov_base = &mpls;
		iov[iovcnt++].iov_len = ROUNDUP(salen);
	}

	if (kf->label[0] != '\0') {
		bzero(&label, sizeof(label));
		la = (struct sockaddr_rtlabel *)&label;
		salen = sizeof(struct sockaddr_rtlabel);
		label.ss_len = salen;
		strlcpy(la->sr_label, kf->label, sizeof(la->sr_label));
		/* adjust header */
		hdr.rtm_addrs |= RTA_LABEL;
		hdr.rtm_msglen += ROUNDUP(salen);
		/* adjust iovec */
		iov[iovcnt].iov_base = &label;
		iov[iovcnt++].iov_len = ROUNDUP(salen);
	}

retry:
	if (writev(fd, iov, iovcnt) == -1) {
		if (errno == ESRCH) {
			if (hdr.rtm_type == RTM_CHANGE) {
				hdr.rtm_type = RTM_ADD;
				goto retry;
			} else if (hdr.rtm_type == RTM_DELETE) {
				log_info("route %s/%u vanished before delete",
				    log_addr(&kf->prefix), kf->prefixlen);
				return (1);
			}
		}
		log_warn("%s: action %u, prefix %s/%u", __func__, hdr.rtm_type,
		    log_addr(&kf->prefix), kf->prefixlen);
		return (0);
	}

	return (1);
}

int
fetchtable(struct ktable *kt)
{
	size_t			 len;
	int			 mib[7];
	char			*buf = NULL, *next, *lim;
	struct rt_msghdr	*rtm;
	struct kroute_full	 kl;
	struct kroute		*kr;
	struct kroute6		*kr6;

	mib[0] = CTL_NET;
	mib[1] = PF_ROUTE;
	mib[2] = 0;
	mib[3] = 0;
	mib[4] = NET_RT_DUMP;
	mib[5] = 0;
	mib[6] = kt->rtableid;

	if (sysctl(mib, 7, NULL, &len, NULL, 0) == -1) {
		if (kt->rtableid != 0 && errno == EINVAL)
			/* table nonexistent */
			return (0);
		log_warn("%s: sysctl", __func__);
		return (-1);
	}
	if (len > 0) {
		if ((buf = malloc(len)) == NULL) {
			log_warn("%s", __func__);
			return (-1);
		}
		if (sysctl(mib, 7, buf, &len, NULL, 0) == -1) {
			log_warn("%s: sysctl2", __func__);
			free(buf);
			return (-1);
		}
	}

	lim = buf + len;
	for (next = buf; next < lim; next += rtm->rtm_msglen) {
		rtm = (struct rt_msghdr *)next;
		if (rtm->rtm_version != RTM_VERSION)
			continue;

		if (dispatch_rtmsg_addr(rtm, &kl) == -1)
			continue;

		if (kl.prefix.aid == AID_INET) {
			if ((kr = calloc(1, sizeof(*kr))) == NULL) {
				log_warn("%s", __func__);
				return (-1);
			}
			kr->prefix.s_addr = kl.prefix.v4.s_addr;
			kr->prefixlen = kl.prefixlen;
			if (kl.nexthop.aid == AID_INET)
				kr->nexthop.s_addr = kl.nexthop.v4.s_addr;
			kr->flags = kl.flags;
			kr->ifindex = kl.ifindex;
			kr->priority = kl.priority;
			kr->labelid = rtlabel_name2id(kl.label);

			if (kl.priority == kr_state.fib_prio) {
				send_rtmsg(kr_state.fd, RTM_DELETE, kt, &kl);
				rtlabel_unref(kr->labelid);
				free(kr);
			} else
				kroute_insert(kt, kr);
		} else if (kl.prefix.aid == AID_INET6) {
			if ((kr6 = calloc(1, sizeof(*kr6))) == NULL) {
				log_warn("%s", __func__);
				return (-1);
			}
			kr6->prefix = kl.prefix.v6;
			kr6->prefix_scope_id = kl.prefix.scope_id;
			kr6->prefixlen = kl.prefixlen;
			if (kl.nexthop.aid == AID_INET6) {
				kr6->nexthop = kl.nexthop.v6;
				kr6->nexthop_scope_id = kl.nexthop.scope_id;
			} else
				kr6->nexthop = in6addr_any;
			kr6->flags = kl.flags;
			kr6->ifindex = kl.ifindex;
			kr6->priority = kl.priority;
			kr6->labelid = rtlabel_name2id(kl.label);

			if (kl.priority == kr_state.fib_prio) {
				send_rtmsg(kr_state.fd, RTM_DELETE, kt, &kl);
				rtlabel_unref(kr6->labelid);
				free(kr6);
			} else
				kroute6_insert(kt, kr6);
		}
	}
	free(buf);
	return (0);
}

int
fetchifs(int ifindex)
{
	size_t			 len;
	int			 mib[6];
	char			*buf, *next, *lim;
	struct if_msghdr	 ifm;
	struct kif_node		*kif;
	struct sockaddr		*sa, *rti_info[RTAX_MAX];
	struct sockaddr_dl	*sdl;

	mib[0] = CTL_NET;
	mib[1] = PF_ROUTE;
	mib[2] = 0;
	mib[3] = AF_INET;	/* AF does not matter but AF_INET is shorter */
	mib[4] = NET_RT_IFLIST;
	mib[5] = ifindex;

	if (sysctl(mib, 6, NULL, &len, NULL, 0) == -1) {
		log_warn("%s: sysctl", __func__);
		return (-1);
	}
	if ((buf = malloc(len)) == NULL) {
		log_warn("%s", __func__);
		return (-1);
	}
	if (sysctl(mib, 6, buf, &len, NULL, 0) == -1) {
		log_warn("%s: sysctl2", __func__);
		free(buf);
		return (-1);
	}

	lim = buf + len;
	for (next = buf; next < lim; next += ifm.ifm_msglen) {
		memcpy(&ifm, next, sizeof(ifm));
		if (ifm.ifm_version != RTM_VERSION)
			continue;
		if (ifm.ifm_type != RTM_IFINFO)
			continue;

		sa = (struct sockaddr *)(next + sizeof(ifm));
		get_rtaddrs(ifm.ifm_addrs, sa, rti_info);

		if ((kif = calloc(1, sizeof(*kif))) == NULL) {
			log_warn("%s", __func__);
			free(buf);
			return (-1);
		}

		kif->k.ifindex = ifm.ifm_index;
		kif->k.flags = ifm.ifm_flags;
		kif->k.link_state = ifm.ifm_data.ifi_link_state;
		kif->k.if_type = ifm.ifm_data.ifi_type;
		kif->k.rdomain = ifm.ifm_data.ifi_rdomain;
		kif->k.baudrate = ifm.ifm_data.ifi_baudrate;
		kif->k.nh_reachable = kif_validate(&kif->k);
		kif->k.depend_state = kif_depend_state(&kif->k);

		if ((sa = rti_info[RTAX_IFP]) != NULL)
			if (sa->sa_family == AF_LINK) {
				sdl = (struct sockaddr_dl *)sa;
				if (sdl->sdl_nlen >= sizeof(kif->k.ifname))
					memcpy(kif->k.ifname, sdl->sdl_data,
					    sizeof(kif->k.ifname) - 1);
				else if (sdl->sdl_nlen > 0)
					memcpy(kif->k.ifname, sdl->sdl_data,
					    sdl->sdl_nlen);
				/* string already terminated via calloc() */
			}

		kif_insert(kif);
	}
	free(buf);
	return (0);
}

int
dispatch_rtmsg(void)
{
	char			 buf[RT_BUF_SIZE];
	ssize_t			 n;
	char			*next, *lim;
	struct rt_msghdr	*rtm;
	struct if_msghdr	 ifm;
	struct kroute_full	 kl;
	struct ktable		*kt;
	int			 mpath = 0;

	if ((n = read(kr_state.fd, &buf, sizeof(buf))) == -1) {
		if (errno == EAGAIN || errno == EINTR)
			return (0);
		log_warn("%s: read error", __func__);
		return (-1);
	}

	if (n == 0) {
		log_warnx("routing socket closed");
		return (-1);
	}

	lim = buf + n;
	for (next = buf; next < lim; next += rtm->rtm_msglen) {
		rtm = (struct rt_msghdr *)next;
		if (lim < next + sizeof(u_short) ||
		    lim < next + rtm->rtm_msglen)
			fatalx("%s: partial rtm in buffer", __func__);
		if (rtm->rtm_version != RTM_VERSION)
			continue;

		switch (rtm->rtm_type) {
		case RTM_ADD:
		case RTM_CHANGE:
		case RTM_DELETE:
			if (rtm->rtm_pid == kr_state.pid) /* cause by us */
				continue;

			/* failed attempts */
			if (rtm->rtm_errno || !(rtm->rtm_flags & RTF_DONE))
				continue;

			if ((kt = ktable_get(rtm->rtm_tableid)) == NULL)
				continue;

			if (dispatch_rtmsg_addr(rtm, &kl) == -1)
				continue;

			if (rtm->rtm_flags & RTF_MPATH)
				mpath = 1;

			switch (rtm->rtm_type) {
			case RTM_ADD:
			case RTM_CHANGE:
				if (kr_fib_change(kt, &kl, rtm->rtm_type,
				    mpath) == -1)
					return -1;
				break;
			case RTM_DELETE:
				if (kr_fib_delete(kt, &kl, mpath) == -1)
					return -1;
				break;
			}
			break;
		case RTM_IFINFO:
			memcpy(&ifm, next, sizeof(ifm));
			if_change(ifm.ifm_index, ifm.ifm_flags, &ifm.ifm_data);
			break;
		case RTM_IFANNOUNCE:
			if_announce(next);
			break;
		default:
			/* ignore for now */
			break;
		}
	}
	return (0);
}

int
dispatch_rtmsg_addr(struct rt_msghdr *rtm, struct kroute_full *kl)
{
	struct sockaddr		*sa, *rti_info[RTAX_MAX];
	struct sockaddr_in	*sa_in;
	struct sockaddr_in6	*sa_in6;
	struct sockaddr_rtlabel	*label;

	sa = (struct sockaddr *)((char *)rtm + rtm->rtm_hdrlen);
	get_rtaddrs(rtm->rtm_addrs, sa, rti_info);

	/* Skip ARP/ND cache, broadcast and dynamic routes. */
	if (rtm->rtm_flags & (RTF_LLINFO|RTF_BROADCAST|RTF_DYNAMIC))
		return (-1);

	if ((sa = rti_info[RTAX_DST]) == NULL) {
		log_warnx("route message without destination");
		return (-1);
	}

	memset(kl, 0, sizeof(*kl));
	kl->flags = F_KERNEL;

	if (rtm->rtm_flags & RTF_STATIC)
		kl->flags |= F_STATIC;
	if (rtm->rtm_flags & RTF_BLACKHOLE)
		kl->flags |= F_BLACKHOLE;
	if (rtm->rtm_flags & RTF_REJECT)
		kl->flags |= F_REJECT;

	kl->priority = rtm->rtm_priority;
	label = (struct sockaddr_rtlabel *)rti_info[RTAX_LABEL];
	if (label != NULL)
		if (strlcpy(kl->label, label->sr_label, sizeof(kl->label)) >=
		    sizeof(kl->label))
			fatalx("rtm label overflow");

	sa2addr(sa, &kl->prefix, NULL);
	switch (sa->sa_family) {
	case AF_INET:
		sa_in = (struct sockaddr_in *)rti_info[RTAX_NETMASK];
		if (sa_in != NULL) {
			if (sa_in->sin_len != 0)
				kl->prefixlen =
				    mask2prefixlen(sa_in->sin_addr.s_addr);
		} else if (rtm->rtm_flags & RTF_HOST)
			kl->prefixlen = 32;
		else
			kl->prefixlen =
			    prefixlen_classful(kl->prefix.v4.s_addr);
		break;
	case AF_INET6:
		sa_in6 = (struct sockaddr_in6 *)rti_info[RTAX_NETMASK];
		if (sa_in6 != NULL) {
			if (sa_in6->sin6_len != 0)
				kl->prefixlen = mask2prefixlen6(sa_in6);
		} else if (rtm->rtm_flags & RTF_HOST)
			kl->prefixlen = 128;
		else
			fatalx("in6 net addr without netmask");
		break;
	default:
		return (-1);
	}

	if ((sa = rti_info[RTAX_GATEWAY]) == NULL) {
		log_warnx("route %s/%u without gateway",
		    log_addr(&kl->prefix), kl->prefixlen);
		return (-1);
	}

	kl->ifindex = rtm->rtm_index;
	if (rtm->rtm_flags & RTF_GATEWAY) {
		switch (sa->sa_family) {
		case AF_LINK:
			kl->flags |= F_CONNECTED;
			break;
		case AF_INET:
		case AF_INET6:
			sa2addr(rti_info[RTAX_GATEWAY], &kl->nexthop, NULL);
			break;
		}
	} else {
		kl->flags |= F_CONNECTED;
	}

	return (0);
}

int
kr_fib_delete(struct ktable *kt, struct kroute_full *kl, int mpath)
{
	struct kroute	*kr;
	struct kroute6	*kr6;

	switch (kl->prefix.aid) {
	case AID_INET:
		if ((kr = kroute_find(kt, &kl->prefix, kl->prefixlen,
		    kl->priority)) == NULL)
			return (0);
		if (!(kr->flags & F_KERNEL))
			return (0);

		if (mpath) {
			/* get the correct route */
			if ((kr = kroute_matchgw(kr, &kl->nexthop)) == NULL) {
				log_warnx("delete %s/%u: route not found",
				    log_addr(&kl->prefix), kl->prefixlen);
				return (0);
			}
		}
		if (kroute_remove(kt, kr) == -1)
			return (-1);
		break;
	case AID_INET6:
		if ((kr6 = kroute6_find(kt, &kl->prefix, kl->prefixlen,
		    kl->priority)) == NULL)
			return (0);
		if (!(kr6->flags & F_KERNEL))
			return (0);

		if (mpath) {
			/* get the correct route */
			if ((kr6 = kroute6_matchgw(kr6, &kl->nexthop)) ==
			    NULL) {
				log_warnx("delete %s/%u: route not found",
				    log_addr(&kl->prefix), kl->prefixlen);
				return (0);
			}
		}
		if (kroute6_remove(kt, kr6) == -1)
			return (-1);
		break;
	}
	return (0);
}

int
kr_fib_change(struct ktable *kt, struct kroute_full *kl, int type, int mpath)
{
	struct kroute	*kr;
	struct kroute6	*kr6;
	int		 flags, oflags;
	int		 changed = 0, rtlabel_changed = 0;
	uint16_t	 new_labelid;

	flags = kl->flags;
	switch (kl->prefix.aid) {
	case AID_INET:
		if ((kr = kroute_find(kt, &kl->prefix, kl->prefixlen,
		    kl->priority)) != NULL) {
			if (kr->flags & F_KERNEL) {
				/* get the correct route */
				if (mpath && type == RTM_CHANGE &&
				    (kr = kroute_matchgw(kr, &kl->nexthop)) ==
				    NULL) {
					log_warnx("%s[change]: "
					    "mpath route not found", __func__);
					goto add4;
				} else if (mpath && type == RTM_ADD)
					goto add4;

				if (kl->nexthop.aid == AID_INET) {
					if (kr->nexthop.s_addr !=
					    kl->nexthop.v4.s_addr)
						changed = 1;
					kr->nexthop.s_addr =
					    kl->nexthop.v4.s_addr;
					kr->ifindex = kl->ifindex;
				} else {
					if (kr->nexthop.s_addr != 0)
						changed = 1;
					kr->nexthop.s_addr = 0;
					kr->ifindex = kl->ifindex;
				}

				if (kr->flags & F_NEXTHOP)
					flags |= F_NEXTHOP;

				new_labelid = rtlabel_name2id(kl->label);
				if (kr->labelid != new_labelid) {
					rtlabel_unref(kr->labelid);
					kr->labelid = new_labelid;
					rtlabel_changed = 1;
				}

				oflags = kr->flags;
				if (flags != oflags)
					changed = 1;
				kr->flags = flags;

				if (rtlabel_changed)
					kr_redistribute(IMSG_NETWORK_ADD,
					    kt, kr);

				if ((oflags & F_CONNECTED) &&
				    !(flags & F_CONNECTED)) {
					kif_kr_remove(kr);
					kr_redistribute(IMSG_NETWORK_ADD,
					    kt, kr);
				}
				if ((flags & F_CONNECTED) &&
				    !(oflags & F_CONNECTED)) {
					kif_kr_insert(kr);
					kr_redistribute(IMSG_NETWORK_ADD,
					    kt, kr);
				}
				if (kr->flags & F_NEXTHOP && changed)
					knexthop_track(kt, kr);
			}
		} else {
add4:
			if ((kr = calloc(1, sizeof(*kr))) == NULL) {
				log_warn("%s", __func__);
				return (-1);
			}
			kr->prefix.s_addr = kl->prefix.v4.s_addr;
			kr->prefixlen = kl->prefixlen;
			if (kl->nexthop.aid == AID_INET)
				kr->nexthop.s_addr = kl->nexthop.v4.s_addr;
			kr->flags = flags;
			kr->ifindex = kl->ifindex;
			kr->priority = kl->priority;
			kr->labelid = rtlabel_name2id(kl->label);

			kroute_insert(kt, kr);
		}
		break;
	case AID_INET6:
		if ((kr6 = kroute6_find(kt, &kl->prefix, kl->prefixlen,
		    kl->priority)) != NULL) {
			if (kr6->flags & F_KERNEL) {
				/* get the correct route */
				if (mpath && type == RTM_CHANGE &&
				    (kr6 = kroute6_matchgw(kr6, &kl->nexthop))
				    == NULL) {
					log_warnx("%s[change]: IPv6 mpath "
					    "route not found", __func__);
					goto add6;
				} else if (mpath && type == RTM_ADD)
					goto add6;

				if (kl->nexthop.aid == AID_INET6) {
					if (memcmp(&kr6->nexthop,
					    &kl->nexthop.v6,
					    sizeof(struct in6_addr)))
						changed = 1;
					kr6->nexthop = kl->nexthop.v6;
					kr6->ifindex = kl->ifindex;
				} else {
					if (memcmp(&kr6->nexthop,
					    &in6addr_any,
					    sizeof(struct in6_addr)))
						changed = 1;
					kr6->nexthop = in6addr_any;
					kr6->ifindex = kl->ifindex;
				}

				if (kr6->flags & F_NEXTHOP)
					flags |= F_NEXTHOP;

				new_labelid = rtlabel_name2id(kl->label);
				if (kr6->labelid != new_labelid) {
					rtlabel_unref(kr6->labelid);
					kr6->labelid = new_labelid;
					rtlabel_changed = 1;
				}

				oflags = kr6->flags;
				if (flags != oflags)
					changed = 1;
				kr6->flags = flags;

				if (rtlabel_changed)
					kr_redistribute6(IMSG_NETWORK_ADD,
					    kt, kr6);

				if ((oflags & F_CONNECTED) &&
				    !(flags & F_CONNECTED)) {
					kif_kr6_remove(kr6);
					kr_redistribute6(IMSG_NETWORK_ADD,
					    kt, kr6);
				}
				if ((flags & F_CONNECTED) &&
				    !(oflags & F_CONNECTED)) {
					kif_kr6_insert(kr6);
					kr_redistribute6(IMSG_NETWORK_ADD,
					    kt, kr6);
				}

				if (kr6->flags & F_NEXTHOP && changed)
					knexthop_track(kt, kr6);
			}
		} else {
add6:
			if ((kr6 = calloc(1, sizeof(*kr6))) == NULL) {
				log_warn("%s", __func__);
				return (-1);
			}
			kr6->prefix = kl->prefix.v6;
			kr6->prefix_scope_id = kl->prefix.scope_id;
			kr6->prefixlen = kl->prefixlen;
			if (kl->nexthop.aid == AID_INET6) {
				kr6->nexthop = kl->nexthop.v6;
				kr6->nexthop_scope_id = kl->nexthop.scope_id;
			} else
				kr6->nexthop = in6addr_any;
			kr6->flags = flags;
			kr6->ifindex = kl->ifindex;
			kr6->priority = kl->priority;
			kr6->labelid = rtlabel_name2id(kl->label);

			kroute6_insert(kt, kr6);
		}
		break;
	}

	return (0);
}
