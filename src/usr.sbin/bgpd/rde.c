/*	$OpenBSD: rde.c,v 1.510 2020/12/30 07:29:56 claudio Exp $ */

/*
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 * Copyright (c) 2016 Job Snijders <job@instituut.net>
 * Copyright (c) 2016 Peter Hessler <phessler@openbsd.org>
 * Copyright (c) 2018 Sebastian Benoit <benno@openbsd.org>
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
#include <sys/time.h>
#include <sys/resource.h>

#include <errno.h>
#include <pwd.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <err.h>

#include "bgpd.h"
#include "rde.h"
#include "session.h"
#include "log.h"

#define PFD_PIPE_MAIN		0
#define PFD_PIPE_SESSION	1
#define PFD_PIPE_SESSION_CTL	2
#define PFD_PIPE_COUNT		3

void		 rde_sighdlr(int);
void		 rde_dispatch_imsg_session(struct imsgbuf *);
void		 rde_dispatch_imsg_parent(struct imsgbuf *);
void		 rde_dispatch_imsg_peer(struct rde_peer *, void *);
void		 rde_update_dispatch(struct rde_peer *, struct imsg *);
int		 rde_update_update(struct rde_peer *, struct filterstate *,
		     struct bgpd_addr *, u_int8_t);
void		 rde_update_withdraw(struct rde_peer *, struct bgpd_addr *,
		     u_int8_t);
int		 rde_attr_parse(u_char *, u_int16_t, struct rde_peer *,
		     struct filterstate *, struct mpattr *);
int		 rde_attr_add(struct filterstate *, u_char *, u_int16_t);
u_int8_t	 rde_attr_missing(struct rde_aspath *, int, u_int16_t);
int		 rde_get_mp_nexthop(u_char *, u_int16_t, u_int8_t,
		     struct filterstate *);
void		 rde_as4byte_fixup(struct rde_peer *, struct rde_aspath *);
void		 rde_reflector(struct rde_peer *, struct rde_aspath *);

void		 rde_dump_ctx_new(struct ctl_show_rib_request *, pid_t,
		     enum imsg_type);
void		 rde_dump_ctx_throttle(pid_t, int);
void		 rde_dump_ctx_terminate(pid_t);
void		 rde_dump_mrt_new(struct mrt *, pid_t, int);

int		 rde_l3vpn_import(struct rde_community *, struct l3vpn *);
static void	 rde_commit_pftable(void);
void		 rde_reload_done(void);
static void	 rde_softreconfig_in_done(void *, u_int8_t);
static void	 rde_softreconfig_out_done(void *, u_int8_t);
static void	 rde_softreconfig_done(void);
static void	 rde_softreconfig_out(struct rib_entry *, void *);
static void	 rde_softreconfig_in(struct rib_entry *, void *);
static void	 rde_softreconfig_sync_reeval(struct rib_entry *, void *);
static void	 rde_softreconfig_sync_fib(struct rib_entry *, void *);
static void	 rde_softreconfig_sync_done(void *, u_int8_t);
int		 rde_update_queue_pending(void);
void		 rde_update_queue_runner(void);
void		 rde_update6_queue_runner(u_int8_t);
struct rde_prefixset *rde_find_prefixset(char *, struct rde_prefixset_head *);
void		 rde_mark_prefixsets_dirty(struct rde_prefixset_head *,
		     struct rde_prefixset_head *);
u_int8_t	 rde_roa_validity(struct rde_prefixset *,
		     struct bgpd_addr *, u_int8_t, u_int32_t);

static void	 rde_peer_recv_eor(struct rde_peer *, u_int8_t);
static void	 rde_peer_send_eor(struct rde_peer *, u_int8_t);

void		 network_add(struct network_config *, struct filterstate *);
void		 network_delete(struct network_config *);
static void	 network_dump_upcall(struct rib_entry *, void *);
static void	 network_flush_upcall(struct rib_entry *, void *);

void		 rde_shutdown(void);
int		 ovs_match(struct prefix *, u_int32_t);

static struct imsgbuf		*ibuf_se;
static struct imsgbuf		*ibuf_se_ctl;
static struct imsgbuf		*ibuf_main;
static struct bgpd_config	*conf, *nconf;

volatile sig_atomic_t	 rde_quit = 0;
struct filter_head	*out_rules, *out_rules_tmp;
struct rde_memstats	 rdemem;
int			 softreconfig;

extern struct rde_peer_head	 peerlist;
extern struct rde_peer		*peerself;

struct rde_dump_ctx {
	LIST_ENTRY(rde_dump_ctx)	entry;
	struct ctl_show_rib_request	req;
	u_int32_t			peerid;
	u_int8_t			throttled;
};

LIST_HEAD(, rde_dump_ctx) rde_dump_h = LIST_HEAD_INITIALIZER(rde_dump_h);

struct rde_mrt_ctx {
	LIST_ENTRY(rde_mrt_ctx)	entry;
	struct mrt		mrt;
};

LIST_HEAD(, rde_mrt_ctx) rde_mrts = LIST_HEAD_INITIALIZER(rde_mrts);
u_int rde_mrt_cnt;

void
rde_sighdlr(int sig)
{
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		rde_quit = 1;
		break;
	}
}

u_int32_t	peerhashsize = 1024;
u_int32_t	pathhashsize = 128 * 1024;
u_int32_t	attrhashsize = 16 * 1024;
u_int32_t	nexthophashsize = 1024;

void
rde_main(int debug, int verbose)
{
	struct passwd		*pw;
	struct pollfd		*pfd = NULL;
	struct rde_mrt_ctx	*mctx, *xmctx;
	void			*newp;
	u_int			 pfd_elms = 0, i, j;
	int			 timeout;
	u_int8_t		 aid;

	log_init(debug, LOG_DAEMON);
	log_setverbose(verbose);

	log_procinit(log_procnames[PROC_RDE]);

	if ((pw = getpwnam(BGPD_USER)) == NULL)
		fatal("getpwnam");

	if (chroot(pw->pw_dir) == -1)
		fatal("chroot");
	if (chdir("/") == -1)
		fatal("chdir(\"/\")");

	setproctitle("route decision engine");

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		fatal("can't drop privileges");

	if (pledge("stdio recvfd", NULL) == -1)
		fatal("pledge");

	signal(SIGTERM, rde_sighdlr);
	signal(SIGINT, rde_sighdlr);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	signal(SIGALRM, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);

	if ((ibuf_main = malloc(sizeof(struct imsgbuf))) == NULL)
		fatal(NULL);
	imsg_init(ibuf_main, 3);

	/* initialize the RIB structures */
	pt_init();
	path_init(pathhashsize);
	aspath_init(pathhashsize);
	communities_init(attrhashsize);
	attr_init(attrhashsize);
	nexthop_init(nexthophashsize);
	peer_init(peerhashsize);

	/* make sure the default RIBs are setup */
	rib_new("Adj-RIB-In", 0, F_RIB_NOFIB | F_RIB_NOEVALUATE);

	out_rules = calloc(1, sizeof(struct filter_head));
	if (out_rules == NULL)
		fatal(NULL);
	TAILQ_INIT(out_rules);

	conf = new_config();
	log_info("route decision engine ready");

	while (rde_quit == 0) {
		if (pfd_elms < PFD_PIPE_COUNT + rde_mrt_cnt) {
			if ((newp = reallocarray(pfd,
			    PFD_PIPE_COUNT + rde_mrt_cnt,
			    sizeof(struct pollfd))) == NULL) {
				/* panic for now  */
				log_warn("could not resize pfd from %u -> %u"
				    " entries", pfd_elms, PFD_PIPE_COUNT +
				    rde_mrt_cnt);
				fatalx("exiting");
			}
			pfd = newp;
			pfd_elms = PFD_PIPE_COUNT + rde_mrt_cnt;
		}
		timeout = -1;
		bzero(pfd, sizeof(struct pollfd) * pfd_elms);

		set_pollfd(&pfd[PFD_PIPE_MAIN], ibuf_main);
		set_pollfd(&pfd[PFD_PIPE_SESSION], ibuf_se);
		set_pollfd(&pfd[PFD_PIPE_SESSION_CTL], ibuf_se_ctl);

		i = PFD_PIPE_COUNT;
		for (mctx = LIST_FIRST(&rde_mrts); mctx != 0; mctx = xmctx) {
			xmctx = LIST_NEXT(mctx, entry);

			if (i >= pfd_elms)
				fatalx("poll pfd too small");
			if (mctx->mrt.wbuf.queued) {
				pfd[i].fd = mctx->mrt.wbuf.fd;
				pfd[i].events = POLLOUT;
				i++;
			} else if (mctx->mrt.state == MRT_STATE_REMOVE) {
				close(mctx->mrt.wbuf.fd);
				LIST_REMOVE(mctx, entry);
				free(mctx);
				rde_mrt_cnt--;
			}
		}

		if (rib_dump_pending() || rde_update_queue_pending() ||
		    nexthop_pending() || peer_imsg_pending())
			timeout = 0;

		if (poll(pfd, i, timeout) == -1) {
			if (errno != EINTR)
				fatal("poll error");
			continue;
		}

		if (handle_pollfd(&pfd[PFD_PIPE_MAIN], ibuf_main) == -1)
			fatalx("Lost connection to parent");
		else
			rde_dispatch_imsg_parent(ibuf_main);

		if (handle_pollfd(&pfd[PFD_PIPE_SESSION], ibuf_se) == -1) {
			log_warnx("RDE: Lost connection to SE");
			msgbuf_clear(&ibuf_se->w);
			free(ibuf_se);
			ibuf_se = NULL;
		} else
			rde_dispatch_imsg_session(ibuf_se);

		if (handle_pollfd(&pfd[PFD_PIPE_SESSION_CTL], ibuf_se_ctl) ==
		    -1) {
			log_warnx("RDE: Lost connection to SE control");
			msgbuf_clear(&ibuf_se_ctl->w);
			free(ibuf_se_ctl);
			ibuf_se_ctl = NULL;
		} else
			rde_dispatch_imsg_session(ibuf_se_ctl);

		for (j = PFD_PIPE_COUNT, mctx = LIST_FIRST(&rde_mrts);
		    j < i && mctx != 0; j++) {
			if (pfd[j].fd == mctx->mrt.wbuf.fd &&
			    pfd[j].revents & POLLOUT)
				mrt_write(&mctx->mrt);
			mctx = LIST_NEXT(mctx, entry);
		}

		peer_foreach(rde_dispatch_imsg_peer, NULL);
		rib_dump_runner();
		nexthop_runner();
		if (ibuf_se && ibuf_se->w.queued < SESS_MSG_HIGH_MARK) {
			rde_update_queue_runner();
			for (aid = AID_INET6; aid < AID_MAX; aid++)
				rde_update6_queue_runner(aid);
		}
		/* commit pftable once per poll loop */
		rde_commit_pftable();
	}

	/* do not clean up on shutdown on production, it takes ages. */
	if (debug)
		rde_shutdown();

	free_config(conf);
	free(pfd);

	/* close pipes */
	if (ibuf_se) {
		msgbuf_clear(&ibuf_se->w);
		close(ibuf_se->fd);
		free(ibuf_se);
	}
	if (ibuf_se_ctl) {
		msgbuf_clear(&ibuf_se_ctl->w);
		close(ibuf_se_ctl->fd);
		free(ibuf_se_ctl);
	}
	msgbuf_clear(&ibuf_main->w);
	close(ibuf_main->fd);
	free(ibuf_main);

	while ((mctx = LIST_FIRST(&rde_mrts)) != NULL) {
		msgbuf_clear(&mctx->mrt.wbuf);
		close(mctx->mrt.wbuf.fd);
		LIST_REMOVE(mctx, entry);
		free(mctx);
	}

	log_info("route decision engine exiting");
	exit(0);
}

struct network_config	netconf_s, netconf_p;
struct filterstate	netconf_state;
struct filter_set_head	session_set = TAILQ_HEAD_INITIALIZER(session_set);
struct filter_set_head	parent_set = TAILQ_HEAD_INITIALIZER(parent_set);

void
rde_dispatch_imsg_session(struct imsgbuf *ibuf)
{
	struct imsg		 imsg;
	struct peer		 p;
	struct peer_config	 pconf;
	struct ctl_show_set	 cset;
	struct ctl_show_rib	 csr;
	struct ctl_show_rib_request	req;
	struct rde_peer		*peer;
	struct rde_aspath	*asp;
	struct rde_hashstats	 rdehash;
	struct filter_set	*s;
	struct as_set		*aset;
	struct rde_prefixset	*pset;
	u_int8_t		*asdata;
	ssize_t			 n;
	size_t			 aslen;
	int			 verbose;
	u_int16_t		 len;

	while (ibuf) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("rde_dispatch_imsg_session: imsg_get error");
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_UPDATE:
		case IMSG_SESSION_UP:
		case IMSG_SESSION_DOWN:
		case IMSG_SESSION_STALE:
		case IMSG_SESSION_FLUSH:
		case IMSG_SESSION_RESTARTED:
		case IMSG_REFRESH:
			if ((peer = peer_get(imsg.hdr.peerid)) == NULL) {
				log_warnx("rde_dispatch: unknown peer id %d",
				    imsg.hdr.peerid);
				break;
			}
			peer_imsg_push(peer, &imsg);
			break;
		case IMSG_SESSION_ADD:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(pconf))
				fatalx("incorrect size of session request");
			memcpy(&pconf, imsg.data, sizeof(pconf));
			peer_add(imsg.hdr.peerid, &pconf);
			break;
		case IMSG_NETWORK_ADD:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct network_config)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&netconf_s, imsg.data, sizeof(netconf_s));
			TAILQ_INIT(&netconf_s.attrset);
			rde_filterstate_prep(&netconf_state, NULL, NULL, NULL,
			    0);
			asp = &netconf_state.aspath;
			asp->aspath = aspath_get(NULL, 0);
			asp->origin = ORIGIN_IGP;
			asp->flags = F_ATTR_ORIGIN | F_ATTR_ASPATH |
			    F_ATTR_LOCALPREF | F_PREFIX_ANNOUNCED |
			    F_ANN_DYNAMIC;
			break;
		case IMSG_NETWORK_ASPATH:
			if (imsg.hdr.len - IMSG_HEADER_SIZE <
			    sizeof(csr)) {
				log_warnx("rde_dispatch: wrong imsg len");
				bzero(&netconf_s, sizeof(netconf_s));
				break;
			}
			aslen = imsg.hdr.len - IMSG_HEADER_SIZE - sizeof(csr);
			asdata = imsg.data;
			asdata += sizeof(struct ctl_show_rib);
			memcpy(&csr, imsg.data, sizeof(csr));
			asp = &netconf_state.aspath;
			asp->lpref = csr.local_pref;
			asp->med = csr.med;
			asp->weight = csr.weight;
			asp->flags = csr.flags;
			asp->origin = csr.origin;
			asp->flags |= F_PREFIX_ANNOUNCED | F_ANN_DYNAMIC;
			aspath_put(asp->aspath);
			asp->aspath = aspath_get(asdata, aslen);
			break;
		case IMSG_NETWORK_ATTR:
			if (imsg.hdr.len <= IMSG_HEADER_SIZE) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			/* parse optional path attributes */
			len = imsg.hdr.len - IMSG_HEADER_SIZE;
			if (rde_attr_add(&netconf_state, imsg.data,
			    len) == -1) {
				log_warnx("rde_dispatch: bad network "
				    "attribute");
				rde_filterstate_clean(&netconf_state);
				bzero(&netconf_s, sizeof(netconf_s));
				break;
			}
			break;
		case IMSG_NETWORK_DONE:
			if (imsg.hdr.len != IMSG_HEADER_SIZE) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			TAILQ_CONCAT(&netconf_s.attrset, &session_set, entry);
			switch (netconf_s.prefix.aid) {
			case AID_INET:
				if (netconf_s.prefixlen > 32)
					goto badnet;
				network_add(&netconf_s, &netconf_state);
				break;
			case AID_INET6:
				if (netconf_s.prefixlen > 128)
					goto badnet;
				network_add(&netconf_s, &netconf_state);
				break;
			case 0:
				/* something failed beforehands */
				break;
			default:
badnet:
				log_warnx("request to insert invalid network");
				break;
			}
			rde_filterstate_clean(&netconf_state);
			break;
		case IMSG_NETWORK_REMOVE:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct network_config)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&netconf_s, imsg.data, sizeof(netconf_s));
			TAILQ_INIT(&netconf_s.attrset);

			switch (netconf_s.prefix.aid) {
			case AID_INET:
				if (netconf_s.prefixlen > 32)
					goto badnetdel;
				network_delete(&netconf_s);
				break;
			case AID_INET6:
				if (netconf_s.prefixlen > 128)
					goto badnetdel;
				network_delete(&netconf_s);
				break;
			default:
badnetdel:
				log_warnx("request to remove invalid network");
				break;
			}
			break;
		case IMSG_NETWORK_FLUSH:
			if (imsg.hdr.len != IMSG_HEADER_SIZE) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			if (rib_dump_new(RIB_ADJ_IN, AID_UNSPEC,
			    RDE_RUNNER_ROUNDS, peerself, network_flush_upcall,
			    NULL, NULL) == -1)
				log_warn("rde_dispatch: IMSG_NETWORK_FLUSH");
			break;
		case IMSG_FILTER_SET:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct filter_set)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			if ((s = malloc(sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			memcpy(s, imsg.data, sizeof(struct filter_set));
			if (s->type == ACTION_SET_NEXTHOP) {
				s->action.nh_ref =
				    nexthop_get(&s->action.nexthop);
				s->type = ACTION_SET_NEXTHOP_REF;
			}
			TAILQ_INSERT_TAIL(&session_set, s, entry);
			break;
		case IMSG_CTL_SHOW_NETWORK:
		case IMSG_CTL_SHOW_RIB:
		case IMSG_CTL_SHOW_RIB_PREFIX:
			if (imsg.hdr.len != IMSG_HEADER_SIZE + sizeof(req)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&req, imsg.data, sizeof(req));
			rde_dump_ctx_new(&req, imsg.hdr.pid, imsg.hdr.type);
			break;
		case IMSG_CTL_SHOW_NEIGHBOR:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct peer)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&p, imsg.data, sizeof(struct peer));
			peer = peer_get(p.conf.id);
			if (peer != NULL) {
				p.stats.prefix_cnt = peer->prefix_cnt;
				p.stats.prefix_out_cnt = peer->prefix_out_cnt;
				p.stats.prefix_rcvd_update =
				    peer->prefix_rcvd_update;
				p.stats.prefix_rcvd_withdraw =
				    peer->prefix_rcvd_withdraw;
				p.stats.prefix_rcvd_eor =
				    peer->prefix_rcvd_eor;
				p.stats.prefix_sent_update =
				    peer->prefix_sent_update;
				p.stats.prefix_sent_withdraw =
				    peer->prefix_sent_withdraw;
				p.stats.prefix_sent_eor =
				    peer->prefix_sent_eor;
			}
			imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_NEIGHBOR, 0,
			    imsg.hdr.pid, -1, &p, sizeof(struct peer));
			break;
		case IMSG_CTL_SHOW_RIB_MEM:
			imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_RIB_MEM, 0,
			    imsg.hdr.pid, -1, &rdemem, sizeof(rdemem));
			path_hash_stats(&rdehash);
			imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_RIB_HASH, 0,
			    imsg.hdr.pid, -1, &rdehash, sizeof(rdehash));
			aspath_hash_stats(&rdehash);
			imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_RIB_HASH, 0,
			    imsg.hdr.pid, -1, &rdehash, sizeof(rdehash));
			communities_hash_stats(&rdehash);
			imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_RIB_HASH, 0,
			    imsg.hdr.pid, -1, &rdehash, sizeof(rdehash));
			attr_hash_stats(&rdehash);
			imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_RIB_HASH, 0,
			    imsg.hdr.pid, -1, &rdehash, sizeof(rdehash));
			imsg_compose(ibuf_se_ctl, IMSG_CTL_END, 0, imsg.hdr.pid,
			    -1, NULL, 0);
			break;
		case IMSG_CTL_SHOW_SET:
			/* first roa set */
			pset = &conf->rde_roa;
			memset(&cset, 0, sizeof(cset));
			cset.type = ROA_SET;
			strlcpy(cset.name, "RPKI ROA", sizeof(cset.name));
			cset.lastchange = pset->lastchange;
			cset.v4_cnt = pset->th.v4_cnt;
			cset.v6_cnt = pset->th.v6_cnt;
			imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_SET, 0,
			    imsg.hdr.pid, -1, &cset, sizeof(cset));

			SIMPLEQ_FOREACH(aset, &conf->as_sets, entry) {
				memset(&cset, 0, sizeof(cset));
				cset.type = ASNUM_SET;
				strlcpy(cset.name, aset->name,
				    sizeof(cset.name));
				cset.lastchange = aset->lastchange;
				cset.as_cnt = set_nmemb(aset->set);
				imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_SET, 0,
				    imsg.hdr.pid, -1, &cset, sizeof(cset));
			}
			SIMPLEQ_FOREACH(pset, &conf->rde_prefixsets, entry) {
				memset(&cset, 0, sizeof(cset));
				cset.type = PREFIX_SET;
				strlcpy(cset.name, pset->name,
				    sizeof(cset.name));
				cset.lastchange = pset->lastchange;
				cset.v4_cnt = pset->th.v4_cnt;
				cset.v6_cnt = pset->th.v6_cnt;
				imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_SET, 0,
				    imsg.hdr.pid, -1, &cset, sizeof(cset));
			}
			SIMPLEQ_FOREACH(pset, &conf->rde_originsets, entry) {
				memset(&cset, 0, sizeof(cset));
				cset.type = ORIGIN_SET;
				strlcpy(cset.name, pset->name,
				    sizeof(cset.name));
				cset.lastchange = pset->lastchange;
				cset.v4_cnt = pset->th.v4_cnt;
				cset.v6_cnt = pset->th.v6_cnt;
				imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_SET, 0,
				    imsg.hdr.pid, -1, &cset, sizeof(cset));
			}
			imsg_compose(ibuf_se_ctl, IMSG_CTL_END, 0, imsg.hdr.pid,
			    -1, NULL, 0);
			break;
		case IMSG_CTL_LOG_VERBOSE:
			/* already checked by SE */
			memcpy(&verbose, imsg.data, sizeof(verbose));
			log_setverbose(verbose);
			break;
		case IMSG_CTL_END:
			imsg_compose(ibuf_se_ctl, IMSG_CTL_END, 0, imsg.hdr.pid,
			    -1, NULL, 0);
			break;
		case IMSG_CTL_TERMINATE:
			rde_dump_ctx_terminate(imsg.hdr.pid);
			break;
		case IMSG_XON:
			if (imsg.hdr.peerid) {
				peer = peer_get(imsg.hdr.peerid);
				if (peer)
					peer->throttled = 0;
			} else {
				rde_dump_ctx_throttle(imsg.hdr.pid, 0);
			}
			break;
		case IMSG_XOFF:
			if (imsg.hdr.peerid) {
				peer = peer_get(imsg.hdr.peerid);
				if (peer)
					peer->throttled = 1;
			} else {
				rde_dump_ctx_throttle(imsg.hdr.pid, 1);
			}
			break;
		default:
			break;
		}
		imsg_free(&imsg);
	}
}

void
rde_dispatch_imsg_parent(struct imsgbuf *ibuf)
{
	static struct rde_prefixset	*last_prefixset;
	static struct as_set	*last_as_set;
	static struct l3vpn	*vpn;
	struct imsg		 imsg;
	struct mrt		 xmrt;
	struct roa		 roa;
	struct rde_rib		 rr;
	struct filterstate	 state;
	struct imsgbuf		*i;
	struct filter_head	*nr;
	struct filter_rule	*r;
	struct filter_set	*s;
	struct rib		*rib;
	struct rde_prefixset	*ps;
	struct rde_aspath	*asp;
	struct prefixset_item	 psi;
	char			*name;
	size_t			 nmemb;
	int			 n, fd, rv;
	u_int16_t		 rid;

	while (ibuf) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("rde_dispatch_imsg_parent: imsg_get error");
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_SOCKET_CONN:
		case IMSG_SOCKET_CONN_CTL:
			if ((fd = imsg.fd) == -1) {
				log_warnx("expected to receive imsg fd to "
				    "SE but didn't receive any");
				break;
			}
			if ((i = malloc(sizeof(struct imsgbuf))) == NULL)
				fatal(NULL);
			imsg_init(i, fd);
			if (imsg.hdr.type == IMSG_SOCKET_CONN) {
				if (ibuf_se) {
					log_warnx("Unexpected imsg connection "
					    "to SE received");
					msgbuf_clear(&ibuf_se->w);
					free(ibuf_se);
				}
				ibuf_se = i;
			} else {
				if (ibuf_se_ctl) {
					log_warnx("Unexpected imsg ctl "
					    "connection to SE received");
					msgbuf_clear(&ibuf_se_ctl->w);
					free(ibuf_se_ctl);
				}
				ibuf_se_ctl = i;
			}
			break;
		case IMSG_NETWORK_ADD:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct network_config)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&netconf_p, imsg.data, sizeof(netconf_p));
			TAILQ_INIT(&netconf_p.attrset);
			break;
		case IMSG_NETWORK_DONE:
			TAILQ_CONCAT(&netconf_p.attrset, &parent_set, entry);

			rde_filterstate_prep(&state, NULL, NULL, NULL, 0);
			asp = &state.aspath;
			asp->aspath = aspath_get(NULL, 0);
			asp->origin = ORIGIN_IGP;
			asp->flags = F_ATTR_ORIGIN | F_ATTR_ASPATH |
			    F_ATTR_LOCALPREF | F_PREFIX_ANNOUNCED;

			network_add(&netconf_p, &state);
			rde_filterstate_clean(&state);
			break;
		case IMSG_NETWORK_REMOVE:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct network_config)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&netconf_p, imsg.data, sizeof(netconf_p));
			TAILQ_INIT(&netconf_p.attrset);
			network_delete(&netconf_p);
			break;
		case IMSG_RECONF_CONF:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct bgpd_config))
				fatalx("IMSG_RECONF_CONF bad len");
			out_rules_tmp = calloc(1, sizeof(struct filter_head));
			if (out_rules_tmp == NULL)
				fatal(NULL);
			TAILQ_INIT(out_rules_tmp);
			nconf = new_config();
			copy_config(nconf, imsg.data);

			for (rid = 0; rid < rib_size; rid++) {
				if ((rib = rib_byid(rid)) == NULL)
					continue;
				rib->state = RECONF_DELETE;
				rib->fibstate = RECONF_NONE;
			}
			break;
		case IMSG_RECONF_RIB:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct rde_rib))
				fatalx("IMSG_RECONF_RIB bad len");
			memcpy(&rr, imsg.data, sizeof(rr));
			rib = rib_byid(rib_find(rr.name));
			if (rib == NULL) {
				rib = rib_new(rr.name, rr.rtableid, rr.flags);
			} else if (rib->flags == rr.flags &&
			    rib->rtableid == rr.rtableid) {
				/* no change to rib apart from filters */
				rib->state = RECONF_KEEP;
			} else {
				/* reload rib because somehing changed */
				rib->flags_tmp = rr.flags;
				rib->rtableid_tmp = rr.rtableid;
				rib->state = RECONF_RELOAD;
			}
			break;
		case IMSG_RECONF_FILTER:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct filter_rule))
				fatalx("IMSG_RECONF_FILTER bad len");
			if ((r = malloc(sizeof(struct filter_rule))) == NULL)
				fatal(NULL);
			memcpy(r, imsg.data, sizeof(struct filter_rule));
			if (r->match.prefixset.name[0] != '\0') {
				r->match.prefixset.ps =
				    rde_find_prefixset(r->match.prefixset.name,
					&nconf->rde_prefixsets);
				if (r->match.prefixset.ps == NULL)
					log_warnx("%s: no prefixset for %s",
					    __func__, r->match.prefixset.name);
			}
			if (r->match.originset.name[0] != '\0') {
				r->match.originset.ps =
				    rde_find_prefixset(r->match.originset.name,
					&nconf->rde_originsets);
				if (r->match.originset.ps == NULL)
					log_warnx("%s: no origin-set for %s",
					    __func__, r->match.originset.name);
			}
			if (r->match.as.flags & AS_FLAG_AS_SET_NAME) {
				struct as_set * aset;

				aset = as_sets_lookup(&nconf->as_sets,
				    r->match.as.name);
				if (aset == NULL) {
					log_warnx("%s: no as-set for %s",
					    __func__, r->match.as.name);
				} else {
					r->match.as.flags = AS_FLAG_AS_SET;
					r->match.as.aset = aset;
				}
			}
			TAILQ_INIT(&r->set);
			TAILQ_CONCAT(&r->set, &parent_set, entry);
			if ((rib = rib_byid(rib_find(r->rib))) == NULL) {
				log_warnx("IMSG_RECONF_FILTER: filter rule "
				    "for nonexistent rib %s", r->rib);
				free(r);
				break;
			}
			r->peer.ribid = rib->id;
			if (r->dir == DIR_IN) {
				nr = rib->in_rules_tmp;
				if (nr == NULL) {
					nr = calloc(1,
					    sizeof(struct filter_head));
					if (nr == NULL)
						fatal(NULL);
					TAILQ_INIT(nr);
					rib->in_rules_tmp = nr;
				}
				TAILQ_INSERT_TAIL(nr, r, entry);
			} else
				TAILQ_INSERT_TAIL(out_rules_tmp, r, entry);
			break;
		case IMSG_RECONF_PREFIX_SET:
		case IMSG_RECONF_ORIGIN_SET:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(ps->name))
				fatalx("IMSG_RECONF_PREFIX_SET bad len");
			ps = calloc(1, sizeof(struct rde_prefixset));
			if (ps == NULL)
				fatal(NULL);
			memcpy(ps->name, imsg.data, sizeof(ps->name));
			if (imsg.hdr.type == IMSG_RECONF_ORIGIN_SET) {
				SIMPLEQ_INSERT_TAIL(&nconf->rde_originsets, ps,
				    entry);
			} else {
				SIMPLEQ_INSERT_TAIL(&nconf->rde_prefixsets, ps,
				    entry);
			}
			last_prefixset = ps;
			break;
		case IMSG_RECONF_ROA_SET:
			strlcpy(nconf->rde_roa.name, "RPKI ROA",
			    sizeof(nconf->rde_roa.name));
			last_prefixset = &nconf->rde_roa;
			break;
		case IMSG_RECONF_ROA_ITEM:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(roa))
				fatalx("IMSG_RECONF_ROA_ITEM bad len");
			memcpy(&roa, imsg.data, sizeof(roa));
			rv = trie_roa_add(&last_prefixset->th, &roa);
			break;
		case IMSG_RECONF_PREFIX_SET_ITEM:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(psi))
				fatalx("IMSG_RECONF_PREFIX_SET_ITEM bad len");
			memcpy(&psi, imsg.data, sizeof(psi));
			if (last_prefixset == NULL)
				fatalx("King Bula has no prefixset");
			rv = trie_add(&last_prefixset->th,
			    &psi.p.addr, psi.p.len,
			    psi.p.len_min, psi.p.len_max);
			if (rv == -1)
				log_warnx("trie_add(%s) %s/%u) failed",
				    last_prefixset->name, log_addr(&psi.p.addr),
				    psi.p.len);
			break;
		case IMSG_RECONF_AS_SET:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(nmemb) + SET_NAME_LEN)
				fatalx("IMSG_RECONF_AS_SET bad len");
			memcpy(&nmemb, imsg.data, sizeof(nmemb));
			name = (char *)imsg.data + sizeof(nmemb);
			if (as_sets_lookup(&nconf->as_sets, name) != NULL)
				fatalx("duplicate as-set %s", name);
			last_as_set = as_sets_new(&nconf->as_sets, name, nmemb,
			    sizeof(u_int32_t));
			break;
		case IMSG_RECONF_AS_SET_ITEMS:
			nmemb = imsg.hdr.len - IMSG_HEADER_SIZE;
			nmemb /= sizeof(u_int32_t);
			if (set_add(last_as_set->set, imsg.data, nmemb) != 0)
				fatal(NULL);
			break;
		case IMSG_RECONF_AS_SET_DONE:
			set_prep(last_as_set->set);
			last_as_set = NULL;
			break;
		case IMSG_RECONF_VPN:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct l3vpn))
				fatalx("IMSG_RECONF_VPN bad len");
			if ((vpn = malloc(sizeof(struct l3vpn))) == NULL)
				fatal(NULL);
			memcpy(vpn, imsg.data, sizeof(struct l3vpn));
			TAILQ_INIT(&vpn->import);
			TAILQ_INIT(&vpn->export);
			TAILQ_INIT(&vpn->net_l);
			SIMPLEQ_INSERT_TAIL(&nconf->l3vpns, vpn, entry);
			break;
		case IMSG_RECONF_VPN_EXPORT:
			if (vpn == NULL) {
				log_warnx("rde_dispatch_imsg_parent: "
				    "IMSG_RECONF_VPN_EXPORT unexpected");
				break;
			}
			TAILQ_CONCAT(&vpn->export, &parent_set, entry);
			break;
		case IMSG_RECONF_VPN_IMPORT:
			if (vpn == NULL) {
				log_warnx("rde_dispatch_imsg_parent: "
				    "IMSG_RECONF_VPN_IMPORT unexpected");
				break;
			}
			TAILQ_CONCAT(&vpn->import, &parent_set, entry);
			break;
		case IMSG_RECONF_VPN_DONE:
			break;
		case IMSG_RECONF_DRAIN:
			imsg_compose(ibuf_main, IMSG_RECONF_DRAIN, 0, 0,
			    -1, NULL, 0);
			break;
		case IMSG_RECONF_DONE:
			if (nconf == NULL)
				fatalx("got IMSG_RECONF_DONE but no config");
			last_prefixset = NULL;

			rde_reload_done();
			break;
		case IMSG_NEXTHOP_UPDATE:
			nexthop_update(imsg.data);
			break;
		case IMSG_FILTER_SET:
			if (imsg.hdr.len > IMSG_HEADER_SIZE +
			    sizeof(struct filter_set))
				fatalx("IMSG_FILTER_SET bad len");
			if ((s = malloc(sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			memcpy(s, imsg.data, sizeof(struct filter_set));
			if (s->type == ACTION_SET_NEXTHOP) {
				s->action.nh_ref =
				    nexthop_get(&s->action.nexthop);
				s->type = ACTION_SET_NEXTHOP_REF;
			}
			TAILQ_INSERT_TAIL(&parent_set, s, entry);
			break;
		case IMSG_MRT_OPEN:
		case IMSG_MRT_REOPEN:
			if (imsg.hdr.len > IMSG_HEADER_SIZE +
			    sizeof(struct mrt)) {
				log_warnx("wrong imsg len");
				break;
			}
			memcpy(&xmrt, imsg.data, sizeof(xmrt));
			if ((fd = imsg.fd) == -1)
				log_warnx("expected to receive fd for mrt dump "
				    "but didn't receive any");
			else if (xmrt.type == MRT_TABLE_DUMP ||
			    xmrt.type == MRT_TABLE_DUMP_MP ||
			    xmrt.type == MRT_TABLE_DUMP_V2) {
				rde_dump_mrt_new(&xmrt, imsg.hdr.pid, fd);
			} else
				close(fd);
			break;
		case IMSG_MRT_CLOSE:
			/* ignore end message because a dump is atomic */
			break;
		default:
			break;
		}
		imsg_free(&imsg);
	}
}

void
rde_dispatch_imsg_peer(struct rde_peer *peer, void *bula)
{
	struct session_up sup;
	struct imsg imsg;
	u_int8_t aid;

	if (!peer_imsg_pop(peer, &imsg))
		return;

	switch (imsg.hdr.type) {
	case IMSG_UPDATE:
		rde_update_dispatch(peer, &imsg);
		break;
	case IMSG_SESSION_UP:
		if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(sup))
			fatalx("incorrect size of session request");
		memcpy(&sup, imsg.data, sizeof(sup));
		if (peer_up(peer, &sup) == -1) {
			peer->state = PEER_DOWN;
			imsg_compose(ibuf_se, IMSG_SESSION_DOWN, peer->conf.id,
			    0, -1, NULL, 0);
		}
		break;
	case IMSG_SESSION_DOWN:
		peer_down(peer, NULL);
		break;
	case IMSG_SESSION_STALE:
	case IMSG_SESSION_FLUSH:
	case IMSG_SESSION_RESTARTED:
	case IMSG_REFRESH:
		if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(aid)) {
			log_warnx("%s: wrong imsg len", __func__);
			break;
		}
		memcpy(&aid, imsg.data, sizeof(aid));
		if (aid >= AID_MAX) {
			log_warnx("%s: bad AID", __func__);
			break;
		}

		switch (imsg.hdr.type) {
		case IMSG_SESSION_STALE:
			peer_stale(peer, aid);
			break;
		case IMSG_SESSION_FLUSH:
			peer_flush(peer, aid, peer->staletime[aid]);
			break;
		case IMSG_SESSION_RESTARTED:
			if (peer->staletime[aid])
				peer_flush(peer, aid, peer->staletime[aid]);
			break;
		case IMSG_REFRESH:
			peer_dump(peer, aid);
			break;
		}
		break;
	default:
		log_warnx("%s: unhandled imsg type %d", __func__,
		    imsg.hdr.type);
		break;
	}

	imsg_free(&imsg);
}

/* handle routing updates from the session engine. */
void
rde_update_dispatch(struct rde_peer *peer, struct imsg *imsg)
{
	struct filterstate	 state;
	struct bgpd_addr	 prefix;
	struct mpattr		 mpa;
	u_char			*p, *mpp = NULL;
	int			 pos = 0;
	u_int16_t		 afi, len, mplen;
	u_int16_t		 withdrawn_len;
	u_int16_t		 attrpath_len;
	u_int16_t		 nlri_len;
	u_int8_t		 aid, prefixlen, safi, subtype;
	u_int32_t		 fas;

	p = imsg->data;

	if (imsg->hdr.len < IMSG_HEADER_SIZE + 2) {
		rde_update_err(peer, ERR_UPDATE, ERR_UPD_ATTRLIST, NULL, 0);
		return;
	}

	memcpy(&len, p, 2);
	withdrawn_len = ntohs(len);
	p += 2;
	if (imsg->hdr.len < IMSG_HEADER_SIZE + 2 + withdrawn_len + 2) {
		rde_update_err(peer, ERR_UPDATE, ERR_UPD_ATTRLIST, NULL, 0);
		return;
	}

	p += withdrawn_len;
	memcpy(&len, p, 2);
	attrpath_len = len = ntohs(len);
	p += 2;
	if (imsg->hdr.len <
	    IMSG_HEADER_SIZE + 2 + withdrawn_len + 2 + attrpath_len) {
		rde_update_err(peer, ERR_UPDATE, ERR_UPD_ATTRLIST, NULL, 0);
		return;
	}

	nlri_len =
	    imsg->hdr.len - IMSG_HEADER_SIZE - 4 - withdrawn_len - attrpath_len;

	if (attrpath_len == 0) {
		/* 0 = no NLRI information in this message */
		if (nlri_len != 0) {
			/* crap at end of update which should not be there */
			rde_update_err(peer, ERR_UPDATE,
			    ERR_UPD_ATTRLIST, NULL, 0);
			return;
		}
		if (withdrawn_len == 0) {
			/* EoR marker */
			rde_peer_recv_eor(peer, AID_INET);
			return;
		}
	}

	bzero(&mpa, sizeof(mpa));
	rde_filterstate_prep(&state, NULL, NULL, NULL, 0);
	if (attrpath_len != 0) { /* 0 = no NLRI information in this message */
		/* parse path attributes */
		while (len > 0) {
			if ((pos = rde_attr_parse(p, len, peer, &state,
			    &mpa)) < 0)
				goto done;
			p += pos;
			len -= pos;
		}

		/* check for missing but necessary attributes */
		if ((subtype = rde_attr_missing(&state.aspath, peer->conf.ebgp,
		    nlri_len))) {
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_MISSNG_WK_ATTR,
			    &subtype, sizeof(u_int8_t));
			goto done;
		}

		rde_as4byte_fixup(peer, &state.aspath);

		/* enforce remote AS if requested */
		if (state.aspath.flags & F_ATTR_ASPATH &&
		    peer->conf.enforce_as == ENFORCE_AS_ON) {
			fas = aspath_neighbor(state.aspath.aspath);
			if (peer->conf.remote_as != fas) {
			    log_peer_warnx(&peer->conf, "bad path, "
				"starting with %s, "
				"enforce neighbor-as enabled", log_as(fas));
			    rde_update_err(peer, ERR_UPDATE, ERR_UPD_ASPATH,
				    NULL, 0);
			    goto done;
			}
		}

		/* aspath needs to be loop free. This is not a hard error. */
		if (state.aspath.flags & F_ATTR_ASPATH &&
		    peer->conf.ebgp &&
		    peer->conf.enforce_local_as == ENFORCE_AS_ON &&
		    !aspath_loopfree(state.aspath.aspath, peer->conf.local_as))
			state.aspath.flags |= F_ATTR_LOOP;

		rde_reflector(peer, &state.aspath);
	}

	p = imsg->data;
	len = withdrawn_len;
	p += 2;

	/* withdraw prefix */
	while (len > 0) {
		if ((pos = nlri_get_prefix(p, len, &prefix,
		    &prefixlen)) == -1) {
			/*
			 * the RFC does not mention what we should do in
			 * this case. Let's do the same as in the NLRI case.
			 */
			log_peer_warnx(&peer->conf, "bad withdraw prefix");
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_NETWORK,
			    NULL, 0);
			goto done;
		}
		p += pos;
		len -= pos;

		if (peer->capa.mp[AID_INET] == 0) {
			log_peer_warnx(&peer->conf,
			    "bad withdraw, %s disabled", aid2str(AID_INET));
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_OPTATTR,
			    NULL, 0);
			goto done;
		}

		rde_update_withdraw(peer, &prefix, prefixlen);
	}

	/* withdraw MP_UNREACH_NLRI if available */
	if (mpa.unreach_len != 0) {
		mpp = mpa.unreach;
		mplen = mpa.unreach_len;
		memcpy(&afi, mpp, 2);
		mpp += 2;
		mplen -= 2;
		afi = ntohs(afi);
		safi = *mpp++;
		mplen--;

		if (afi2aid(afi, safi, &aid) == -1) {
			log_peer_warnx(&peer->conf,
			    "bad AFI/SAFI pair in withdraw");
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_OPTATTR,
			    NULL, 0);
			goto done;
		}

		if (peer->capa.mp[aid] == 0) {
			log_peer_warnx(&peer->conf,
			    "bad withdraw, %s disabled", aid2str(aid));
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_OPTATTR,
			    NULL, 0);
			goto done;
		}

		if ((state.aspath.flags & ~F_ATTR_MP_UNREACH) == 0 &&
		    mplen == 0) {
			/* EoR marker */
			rde_peer_recv_eor(peer, aid);
		}

		switch (aid) {
		case AID_INET6:
			while (mplen > 0) {
				if ((pos = nlri_get_prefix6(mpp, mplen,
				    &prefix, &prefixlen)) == -1) {
					log_peer_warnx(&peer->conf,
					    "bad IPv6 withdraw prefix");
					rde_update_err(peer, ERR_UPDATE,
					    ERR_UPD_OPTATTR,
					    mpa.unreach, mpa.unreach_len);
					goto done;
				}
				mpp += pos;
				mplen -= pos;

				rde_update_withdraw(peer, &prefix, prefixlen);
			}
			break;
		case AID_VPN_IPv4:
			while (mplen > 0) {
				if ((pos = nlri_get_vpn4(mpp, mplen,
				    &prefix, &prefixlen, 1)) == -1) {
					log_peer_warnx(&peer->conf,
					    "bad VPNv4 withdraw prefix");
					rde_update_err(peer, ERR_UPDATE,
					    ERR_UPD_OPTATTR,
					    mpa.unreach, mpa.unreach_len);
					goto done;
				}
				mpp += pos;
				mplen -= pos;

				rde_update_withdraw(peer, &prefix, prefixlen);
			}
			break;
		case AID_VPN_IPv6:
			while (mplen > 0) {
				if ((pos = nlri_get_vpn6(mpp, mplen,
				    &prefix, &prefixlen, 1)) == -1) {
					log_peer_warnx(&peer->conf,
					    "bad VPNv6 withdraw prefix");
					rde_update_err(peer, ERR_UPDATE,
					    ERR_UPD_OPTATTR, mpa.unreach,
					    mpa.unreach_len);
					goto done;
				}
				mpp += pos;
				mplen -= pos;

				rde_update_withdraw(peer, &prefix, prefixlen);
			}
			break;
		default:
			/* silently ignore unsupported multiprotocol AF */
			break;
		}

		if ((state.aspath.flags & ~F_ATTR_MP_UNREACH) == 0)
			goto done;
	}

	/* shift to NLRI information */
	p += 2 + attrpath_len;

	/* parse nlri prefix */
	while (nlri_len > 0) {
		if ((pos = nlri_get_prefix(p, nlri_len, &prefix,
		    &prefixlen)) == -1) {
			log_peer_warnx(&peer->conf, "bad nlri prefix");
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_NETWORK,
			    NULL, 0);
			goto done;
		}
		p += pos;
		nlri_len -= pos;

		if (peer->capa.mp[AID_INET] == 0) {
			log_peer_warnx(&peer->conf,
			    "bad update, %s disabled", aid2str(AID_INET));
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_OPTATTR,
			    NULL, 0);
			goto done;
		}

		if (rde_update_update(peer, &state, &prefix, prefixlen) == -1)
			goto done;

	}

	/* add MP_REACH_NLRI if available */
	if (mpa.reach_len != 0) {
		mpp = mpa.reach;
		mplen = mpa.reach_len;
		memcpy(&afi, mpp, 2);
		mpp += 2;
		mplen -= 2;
		afi = ntohs(afi);
		safi = *mpp++;
		mplen--;

		if (afi2aid(afi, safi, &aid) == -1) {
			log_peer_warnx(&peer->conf,
			    "bad AFI/SAFI pair in update");
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_OPTATTR,
			    NULL, 0);
			goto done;
		}

		if (peer->capa.mp[aid] == 0) {
			log_peer_warnx(&peer->conf,
			    "bad update, %s disabled", aid2str(aid));
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_OPTATTR,
			    NULL, 0);
			goto done;
		}

		/* unlock the previously locked nexthop, it is no longer used */
		nexthop_unref(state.nexthop);
		state.nexthop = NULL;
		if ((pos = rde_get_mp_nexthop(mpp, mplen, aid, &state)) == -1) {
			log_peer_warnx(&peer->conf, "bad nlri nexthop");
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_OPTATTR,
			    mpa.reach, mpa.reach_len);
			goto done;
		}
		mpp += pos;
		mplen -= pos;

		switch (aid) {
		case AID_INET6:
			while (mplen > 0) {
				if ((pos = nlri_get_prefix6(mpp, mplen,
				    &prefix, &prefixlen)) == -1) {
					log_peer_warnx(&peer->conf,
					    "bad IPv6 nlri prefix");
					rde_update_err(peer, ERR_UPDATE,
					    ERR_UPD_OPTATTR,
					    mpa.reach, mpa.reach_len);
					goto done;
				}
				mpp += pos;
				mplen -= pos;

				if (rde_update_update(peer, &state, &prefix,
				    prefixlen) == -1)
					goto done;
			}
			break;
		case AID_VPN_IPv4:
			while (mplen > 0) {
				if ((pos = nlri_get_vpn4(mpp, mplen,
				    &prefix, &prefixlen, 0)) == -1) {
					log_peer_warnx(&peer->conf,
					    "bad VPNv4 nlri prefix");
					rde_update_err(peer, ERR_UPDATE,
					    ERR_UPD_OPTATTR,
					    mpa.reach, mpa.reach_len);
					goto done;
				}
				mpp += pos;
				mplen -= pos;

				if (rde_update_update(peer, &state, &prefix,
				    prefixlen) == -1)
					goto done;
			}
			break;
		case AID_VPN_IPv6:
			while (mplen > 0) {
				if ((pos = nlri_get_vpn6(mpp, mplen,
				    &prefix, &prefixlen, 0)) == -1) {
					log_peer_warnx(&peer->conf,
					    "bad VPNv6 nlri prefix");
					rde_update_err(peer, ERR_UPDATE,
					    ERR_UPD_OPTATTR,
					    mpa.reach, mpa.reach_len);
					goto done;
				}
				mpp += pos;
				mplen -= pos;

				if (rde_update_update(peer, &state, &prefix,
				    prefixlen) == -1)
					goto done;
			}
			break;
		default:
			/* silently ignore unsupported multiprotocol AF */
			break;
		}
	}

done:
	rde_filterstate_clean(&state);
}

int
rde_update_update(struct rde_peer *peer, struct filterstate *in,
    struct bgpd_addr *prefix, u_int8_t prefixlen)
{
	struct filterstate	 state;
	enum filter_actions	 action;
	u_int8_t		 vstate;
	u_int16_t		 i;
	const char		*wmsg = "filtered, withdraw";

	peer->prefix_rcvd_update++;
	vstate = rde_roa_validity(&conf->rde_roa, prefix, prefixlen,
	    aspath_origin(in->aspath.aspath));

	/* add original path to the Adj-RIB-In */
	if (prefix_update(rib_byid(RIB_ADJ_IN), peer, in, prefix, prefixlen,
	    vstate) == 1)
		peer->prefix_cnt++;

	/* max prefix checker */
	if (peer->conf.max_prefix && peer->prefix_cnt > peer->conf.max_prefix) {
		log_peer_warnx(&peer->conf, "prefix limit reached (>%u/%u)",
		    peer->prefix_cnt, peer->conf.max_prefix);
		rde_update_err(peer, ERR_CEASE, ERR_CEASE_MAX_PREFIX, NULL, 0);
		return (-1);
	}

	if (in->aspath.flags & F_ATTR_PARSE_ERR)
		wmsg = "path invalid, withdraw";

	for (i = RIB_LOC_START; i < rib_size; i++) {
		struct rib *rib = rib_byid(i);
		if (rib == NULL)
			continue;
		rde_filterstate_prep(&state, &in->aspath, &in->communities,
		    in->nexthop, in->nhflags);
		/* input filter */
		action = rde_filter(rib->in_rules, peer, peer, prefix,
		    prefixlen, vstate, &state);

		if (action == ACTION_ALLOW) {
			rde_update_log("update", i, peer,
			    &state.nexthop->exit_nexthop, prefix,
			    prefixlen);
			prefix_update(rib, peer, &state, prefix,
			    prefixlen, vstate);
		} else if (prefix_withdraw(rib, peer, prefix,
		    prefixlen)) {
			rde_update_log(wmsg, i, peer,
			    NULL, prefix, prefixlen);
		}

		/* clear state */
		rde_filterstate_clean(&state);
	}
	return (0);
}

void
rde_update_withdraw(struct rde_peer *peer, struct bgpd_addr *prefix,
    u_int8_t prefixlen)
{
	u_int16_t i;

	for (i = RIB_LOC_START; i < rib_size; i++) {
		struct rib *rib = rib_byid(i);
		if (rib == NULL)
			continue;
		if (prefix_withdraw(rib, peer, prefix, prefixlen))
			rde_update_log("withdraw", i, peer, NULL, prefix,
			    prefixlen);
	}

	/* remove original path form the Adj-RIB-In */
	if (prefix_withdraw(rib_byid(RIB_ADJ_IN), peer, prefix, prefixlen))
		peer->prefix_cnt--;

	peer->prefix_rcvd_withdraw++;
}

/*
 * BGP UPDATE parser functions
 */

/* attribute parser specific makros */
#define UPD_READ(t, p, plen, n) \
	do { \
		memcpy(t, p, n); \
		p += n; \
		plen += n; \
	} while (0)

#define CHECK_FLAGS(s, t, m)	\
	(((s) & ~(ATTR_DEFMASK | (m))) == (t))

int
rde_attr_parse(u_char *p, u_int16_t len, struct rde_peer *peer,
    struct filterstate *state, struct mpattr *mpa)
{
	struct bgpd_addr nexthop;
	struct rde_aspath *a = &state->aspath;
	u_char		*op = p, *npath;
	u_int32_t	 tmp32, zero = 0;
	int		 error;
	u_int16_t	 attr_len, nlen;
	u_int16_t	 plen = 0;
	u_int8_t	 flags;
	u_int8_t	 type;
	u_int8_t	 tmp8;

	if (len < 3) {
bad_len:
		rde_update_err(peer, ERR_UPDATE, ERR_UPD_ATTRLEN, op, len);
		return (-1);
	}

	UPD_READ(&flags, p, plen, 1);
	UPD_READ(&type, p, plen, 1);

	if (flags & ATTR_EXTLEN) {
		if (len - plen < 2)
			goto bad_len;
		UPD_READ(&attr_len, p, plen, 2);
		attr_len = ntohs(attr_len);
	} else {
		UPD_READ(&tmp8, p, plen, 1);
		attr_len = tmp8;
	}

	if (len - plen < attr_len)
		goto bad_len;

	/* adjust len to the actual attribute size including header */
	len = plen + attr_len;

	switch (type) {
	case ATTR_UNDEF:
		/* ignore and drop path attributes with a type code of 0 */
		plen += attr_len;
		break;
	case ATTR_ORIGIN:
		if (attr_len != 1)
			goto bad_len;

		if (!CHECK_FLAGS(flags, ATTR_WELL_KNOWN, 0)) {
bad_flags:
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_ATTRFLAGS,
			    op, len);
			return (-1);
		}

		UPD_READ(&a->origin, p, plen, 1);
		if (a->origin > ORIGIN_INCOMPLETE) {
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_ORIGIN,
			    op, len);
			return (-1);
		}
		if (a->flags & F_ATTR_ORIGIN)
			goto bad_list;
		a->flags |= F_ATTR_ORIGIN;
		break;
	case ATTR_ASPATH:
		if (!CHECK_FLAGS(flags, ATTR_WELL_KNOWN, 0))
			goto bad_flags;
		error = aspath_verify(p, attr_len, rde_as4byte(peer));
		if (error == AS_ERR_SOFT) {
			/*
			 * soft errors like unexpected segment types are
			 * not considered fatal and the path is just
			 * marked invalid.
			 */
			a->flags |= F_ATTR_PARSE_ERR;
			log_peer_warnx(&peer->conf, "bad ASPATH, "
			    "path invalidated and prefix withdrawn");
		} else if (error != 0) {
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_ASPATH,
			    NULL, 0);
			return (-1);
		}
		if (a->flags & F_ATTR_ASPATH)
			goto bad_list;
		if (rde_as4byte(peer)) {
			npath = p;
			nlen = attr_len;
		} else {
			npath = aspath_inflate(p, attr_len, &nlen);
			if (npath == NULL)
				fatal("aspath_inflate");
		}
		a->flags |= F_ATTR_ASPATH;
		a->aspath = aspath_get(npath, nlen);
		if (npath != p)
			free(npath);
		plen += attr_len;
		break;
	case ATTR_NEXTHOP:
		if (attr_len != 4)
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_WELL_KNOWN, 0))
			goto bad_flags;
		if (a->flags & F_ATTR_NEXTHOP)
			goto bad_list;
		a->flags |= F_ATTR_NEXTHOP;

		bzero(&nexthop, sizeof(nexthop));
		nexthop.aid = AID_INET;
		UPD_READ(&nexthop.v4.s_addr, p, plen, 4);
		/*
		 * Check if the nexthop is a valid IP address. We consider
		 * multicast and experimental addresses as invalid.
		 */
		tmp32 = ntohl(nexthop.v4.s_addr);
		if (IN_MULTICAST(tmp32) || IN_BADCLASS(tmp32)) {
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_NETWORK,
			    op, len);
			return (-1);
		}
		nexthop_unref(state->nexthop);	/* just to be sure */
		state->nexthop = nexthop_get(&nexthop);
		break;
	case ATTR_MED:
		if (attr_len != 4)
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL, 0))
			goto bad_flags;
		if (a->flags & F_ATTR_MED)
			goto bad_list;
		a->flags |= F_ATTR_MED;

		UPD_READ(&tmp32, p, plen, 4);
		a->med = ntohl(tmp32);
		break;
	case ATTR_LOCALPREF:
		if (attr_len != 4)
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_WELL_KNOWN, 0))
			goto bad_flags;
		if (peer->conf.ebgp) {
			/* ignore local-pref attr on non ibgp peers */
			plen += 4;
			break;
		}
		if (a->flags & F_ATTR_LOCALPREF)
			goto bad_list;
		a->flags |= F_ATTR_LOCALPREF;

		UPD_READ(&tmp32, p, plen, 4);
		a->lpref = ntohl(tmp32);
		break;
	case ATTR_ATOMIC_AGGREGATE:
		if (attr_len != 0)
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_WELL_KNOWN, 0))
			goto bad_flags;
		goto optattr;
	case ATTR_AGGREGATOR:
		if ((!rde_as4byte(peer) && attr_len != 6) ||
		    (rde_as4byte(peer) && attr_len != 8)) {
			/*
			 * ignore attribute in case of error as per
			 * RFC 7606
			 */
			log_peer_warnx(&peer->conf, "bad AGGREGATOR, "
			    "partial attribute ignored");
			plen += attr_len;
			break;
		}
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL|ATTR_TRANSITIVE,
		    ATTR_PARTIAL))
			goto bad_flags;
		if (!rde_as4byte(peer)) {
			/* need to inflate aggregator AS to 4-byte */
			u_char	t[8];
			t[0] = t[1] = 0;
			UPD_READ(&t[2], p, plen, 2);
			UPD_READ(&t[4], p, plen, 4);
			if (memcmp(t, &zero, sizeof(u_int32_t)) == 0) {
				/* As per RFC7606 use "attribute discard". */
				log_peer_warnx(&peer->conf, "bad AGGREGATOR, "
				    "AS 0 not allowed, attribute discarded");
				break;
			}
			if (attr_optadd(a, flags, type, t,
			    sizeof(t)) == -1)
				goto bad_list;
			break;
		}
		/* 4-byte ready server take the default route */
		if (memcmp(p, &zero, sizeof(u_int32_t)) == 0) {
			/* As per RFC7606 use "attribute discard" here. */
			char *pfmt = log_fmt_peer(&peer->conf);
			log_debug("%s: bad AGGREGATOR, "
			    "AS 0 not allowed, attribute discarded", pfmt);
			free(pfmt);
			plen += attr_len;
			break;
		}
		goto optattr;
	case ATTR_COMMUNITIES:
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL|ATTR_TRANSITIVE,
		    ATTR_PARTIAL))
			goto bad_flags;
		if (community_add(&state->communities, flags, p,
		    attr_len) == -1) {
			/*
			 * mark update as bad and withdraw all routes as per
			 * RFC 7606
			 */
			a->flags |= F_ATTR_PARSE_ERR;
			log_peer_warnx(&peer->conf, "bad COMMUNITIES, "
			    "path invalidated and prefix withdrawn");
			break;
		}
		plen += attr_len;
		break;
	case ATTR_LARGE_COMMUNITIES:
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL|ATTR_TRANSITIVE,
		    ATTR_PARTIAL))
			goto bad_flags;
		if (community_large_add(&state->communities, flags, p,
		    attr_len) == -1) {
			/*
			 * mark update as bad and withdraw all routes as per
			 * RFC 7606
			 */
			a->flags |= F_ATTR_PARSE_ERR;
			log_peer_warnx(&peer->conf, "bad LARGE COMMUNITIES, "
			    "path invalidated and prefix withdrawn");
			break;
		}
		plen += attr_len;
		break;
	case ATTR_EXT_COMMUNITIES:
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL|ATTR_TRANSITIVE,
		    ATTR_PARTIAL))
			goto bad_flags;
		if (community_ext_add(&state->communities, flags, p,
		    attr_len) == -1) {
			/*
			 * mark update as bad and withdraw all routes as per
			 * RFC 7606
			 */
			a->flags |= F_ATTR_PARSE_ERR;
			log_peer_warnx(&peer->conf, "bad EXT_COMMUNITIES, "
			    "path invalidated and prefix withdrawn");
			break;
		}
		plen += attr_len;
		break;
	case ATTR_ORIGINATOR_ID:
		if (attr_len != 4)
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL, 0))
			goto bad_flags;
		goto optattr;
	case ATTR_CLUSTER_LIST:
		if (attr_len % 4 != 0)
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL, 0))
			goto bad_flags;
		goto optattr;
	case ATTR_MP_REACH_NLRI:
		if (attr_len < 4)
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL, 0))
			goto bad_flags;
		/* the validity is checked in rde_update_dispatch() */
		if (a->flags & F_ATTR_MP_REACH)
			goto bad_list;
		a->flags |= F_ATTR_MP_REACH;

		mpa->reach = p;
		mpa->reach_len = attr_len;
		plen += attr_len;
		break;
	case ATTR_MP_UNREACH_NLRI:
		if (attr_len < 3)
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL, 0))
			goto bad_flags;
		/* the validity is checked in rde_update_dispatch() */
		if (a->flags & F_ATTR_MP_UNREACH)
			goto bad_list;
		a->flags |= F_ATTR_MP_UNREACH;

		mpa->unreach = p;
		mpa->unreach_len = attr_len;
		plen += attr_len;
		break;
	case ATTR_AS4_AGGREGATOR:
		if (attr_len != 8) {
			/* see ATTR_AGGREGATOR ... */
			if ((flags & ATTR_PARTIAL) == 0)
				goto bad_len;
			log_peer_warnx(&peer->conf, "bad AS4_AGGREGATOR, "
			    "partial attribute ignored");
			plen += attr_len;
			break;
		}
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL|ATTR_TRANSITIVE,
		    ATTR_PARTIAL))
			goto bad_flags;
		if (memcmp(p, &zero, sizeof(u_int32_t)) == 0) {
			/* As per RFC6793 use "attribute discard" here. */
			log_peer_warnx(&peer->conf, "bad AS4_AGGREGATOR, "
			    "AS 0 not allowed, attribute discarded");
			plen += attr_len;
			break;
		}
		a->flags |= F_ATTR_AS4BYTE_NEW;
		goto optattr;
	case ATTR_AS4_PATH:
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL|ATTR_TRANSITIVE,
		    ATTR_PARTIAL))
			goto bad_flags;
		if ((error = aspath_verify(p, attr_len, 1)) != 0) {
			/*
			 * XXX RFC does not specify how to handle errors.
			 * XXX Instead of dropping the session because of a
			 * XXX bad path just mark the full update as having
			 * XXX a parse error which makes the update no longer
			 * XXX eligible and will not be considered for routing
			 * XXX or redistribution.
			 * XXX We follow draft-ietf-idr-optional-transitive
			 * XXX by looking at the partial bit.
			 * XXX Consider soft errors similar to a partial attr.
			 */
			if (flags & ATTR_PARTIAL || error == AS_ERR_SOFT) {
				a->flags |= F_ATTR_PARSE_ERR;
				log_peer_warnx(&peer->conf, "bad AS4_PATH, "
				    "path invalidated and prefix withdrawn");
				goto optattr;
			} else {
				rde_update_err(peer, ERR_UPDATE, ERR_UPD_ASPATH,
				    NULL, 0);
				return (-1);
			}
		}
		a->flags |= F_ATTR_AS4BYTE_NEW;
		goto optattr;
	default:
		if ((flags & ATTR_OPTIONAL) == 0) {
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_UNKNWN_WK_ATTR,
			    op, len);
			return (-1);
		}
optattr:
		if (attr_optadd(a, flags, type, p, attr_len) == -1) {
bad_list:
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_ATTRLIST,
			    NULL, 0);
			return (-1);
		}

		plen += attr_len;
		break;
	}

	return (plen);
}

int
rde_attr_add(struct filterstate *state, u_char *p, u_int16_t len)
{
	u_int16_t	 attr_len;
	u_int16_t	 plen = 0;
	u_int8_t	 flags;
	u_int8_t	 type;
	u_int8_t	 tmp8;

	if (len < 3)
		return (-1);

	UPD_READ(&flags, p, plen, 1);
	UPD_READ(&type, p, plen, 1);

	if (flags & ATTR_EXTLEN) {
		if (len - plen < 2)
			return (-1);
		UPD_READ(&attr_len, p, plen, 2);
		attr_len = ntohs(attr_len);
	} else {
		UPD_READ(&tmp8, p, plen, 1);
		attr_len = tmp8;
	}

	if (len - plen < attr_len)
		return (-1);

	switch (type) {
	case ATTR_COMMUNITIES:
		return community_add(&state->communities, flags, p, attr_len);
	case ATTR_LARGE_COMMUNITIES:
		return community_large_add(&state->communities, flags, p,
		    attr_len);
	case ATTR_EXT_COMMUNITIES:
		return community_ext_add(&state->communities, flags, p,
		    attr_len);
	}

	if (attr_optadd(&state->aspath, flags, type, p, attr_len) == -1)
		return (-1);
	return (0);
}

#undef UPD_READ
#undef CHECK_FLAGS

u_int8_t
rde_attr_missing(struct rde_aspath *a, int ebgp, u_int16_t nlrilen)
{
	/* ATTR_MP_UNREACH_NLRI may be sent alone */
	if (nlrilen == 0 && a->flags & F_ATTR_MP_UNREACH &&
	    (a->flags & F_ATTR_MP_REACH) == 0)
		return (0);

	if ((a->flags & F_ATTR_ORIGIN) == 0)
		return (ATTR_ORIGIN);
	if ((a->flags & F_ATTR_ASPATH) == 0)
		return (ATTR_ASPATH);
	if ((a->flags & F_ATTR_MP_REACH) == 0 &&
	    (a->flags & F_ATTR_NEXTHOP) == 0)
		return (ATTR_NEXTHOP);
	if (!ebgp)
		if ((a->flags & F_ATTR_LOCALPREF) == 0)
			return (ATTR_LOCALPREF);
	return (0);
}

int
rde_get_mp_nexthop(u_char *data, u_int16_t len, u_int8_t aid,
    struct filterstate *state)
{
	struct bgpd_addr	nexthop;
	u_int8_t		totlen, nhlen;

	if (len == 0)
		return (-1);

	nhlen = *data++;
	totlen = 1;
	len--;

	if (nhlen > len)
		return (-1);

	bzero(&nexthop, sizeof(nexthop));
	nexthop.aid = aid;
	switch (aid) {
	case AID_INET6:
		/*
		 * RFC2545 describes that there may be a link-local
		 * address carried in nexthop. Yikes!
		 * This is not only silly, it is wrong and we just ignore
		 * this link-local nexthop. The bgpd session doesn't run
		 * over the link-local address so why should all other
		 * traffic.
		 */
		if (nhlen != 16 && nhlen != 32) {
			log_warnx("bad multiprotocol nexthop, bad size");
			return (-1);
		}
		memcpy(&nexthop.v6.s6_addr, data, 16);
		break;
	case AID_VPN_IPv6:
		if (nhlen != 24) {
			log_warnx("bad multiprotocol nexthop, bad size %d",
			    nhlen);
			return (-1);
		}
		memcpy(&nexthop.v6, data + sizeof(u_int64_t),
		    sizeof(nexthop.v6));
		nexthop.aid = AID_INET6;
		break;
	case AID_VPN_IPv4:
		/*
		 * Neither RFC4364 nor RFC3107 specify the format of the
		 * nexthop in an explicit way. The quality of RFC went down
		 * the toilet the larger the number got.
		 * RFC4364 is very confusing about VPN-IPv4 address and the
		 * VPN-IPv4 prefix that carries also a MPLS label.
		 * So the nexthop is a 12-byte address with a 64bit RD and
		 * an IPv4 address following. In the nexthop case the RD can
		 * be ignored.
		 * Since the nexthop has to be in the main IPv4 table just
		 * create an AID_INET nexthop. So we don't need to handle
		 * AID_VPN_IPv4 in nexthop and kroute.
		 */
		if (nhlen != 12) {
			log_warnx("bad multiprotocol nexthop, bad size");
			return (-1);
		}
		nexthop.aid = AID_INET;
		memcpy(&nexthop.v4, data + sizeof(u_int64_t),
		    sizeof(nexthop.v4));
		break;
	default:
		log_warnx("bad multiprotocol nexthop, bad AID");
		return (-1);
	}

	nexthop_unref(state->nexthop);	/* just to be sure */
	state->nexthop = nexthop_get(&nexthop);

	/* ignore reserved (old SNPA) field as per RFC4760 */
	totlen += nhlen + 1;
	data += nhlen + 1;

	return (totlen);
}

void
rde_update_err(struct rde_peer *peer, u_int8_t error, u_int8_t suberr,
    void *data, u_int16_t size)
{
	struct ibuf	*wbuf;

	if ((wbuf = imsg_create(ibuf_se, IMSG_UPDATE_ERR, peer->conf.id, 0,
	    size + sizeof(error) + sizeof(suberr))) == NULL)
		fatal("%s %d imsg_create error", __func__, __LINE__);
	if (imsg_add(wbuf, &error, sizeof(error)) == -1 ||
	    imsg_add(wbuf, &suberr, sizeof(suberr)) == -1 ||
	    imsg_add(wbuf, data, size) == -1)
		fatal("%s %d imsg_add error", __func__, __LINE__);
	imsg_close(ibuf_se, wbuf);
	peer->state = PEER_ERR;
}

void
rde_update_log(const char *message, u_int16_t rid,
    const struct rde_peer *peer, const struct bgpd_addr *next,
    const struct bgpd_addr *prefix, u_int8_t prefixlen)
{
	char		*l = NULL;
	char		*n = NULL;
	char		*p = NULL;

	if (!((conf->log & BGPD_LOG_UPDATES) ||
	    (peer->conf.flags & PEERFLAG_LOG_UPDATES)))
		return;

	if (next != NULL)
		if (asprintf(&n, " via %s", log_addr(next)) == -1)
			n = NULL;
	if (asprintf(&p, "%s/%u", log_addr(prefix), prefixlen) == -1)
		p = NULL;
	l = log_fmt_peer(&peer->conf);
	log_info("Rib %s: %s AS%s: %s %s%s", rib_byid(rid)->name,
	    l, log_as(peer->conf.remote_as), message,
	    p ? p : "out of memory", n ? n : "");

	free(l);
	free(n);
	free(p);
}

/*
 * 4-Byte ASN helper function.
 * Two scenarios need to be considered:
 * - NEW session with NEW attributes present -> just remove the attributes
 * - OLD session with NEW attributes present -> try to merge them
 */
void
rde_as4byte_fixup(struct rde_peer *peer, struct rde_aspath *a)
{
	struct attr	*nasp, *naggr, *oaggr;
	u_int32_t	 as;

	/*
	 * if either ATTR_AS4_AGGREGATOR or ATTR_AS4_PATH is present
	 * try to fixup the attributes.
	 * Do not fixup if F_ATTR_PARSE_ERR is set.
	 */
	if (!(a->flags & F_ATTR_AS4BYTE_NEW) || a->flags & F_ATTR_PARSE_ERR)
		return;

	/* first get the attributes */
	nasp = attr_optget(a, ATTR_AS4_PATH);
	naggr = attr_optget(a, ATTR_AS4_AGGREGATOR);

	if (rde_as4byte(peer)) {
		/* NEW session using 4-byte ASNs */
		if (nasp) {
			log_peer_warnx(&peer->conf, "uses 4-byte ASN "
			    "but sent AS4_PATH attribute.");
			attr_free(a, nasp);
		}
		if (naggr) {
			log_peer_warnx(&peer->conf, "uses 4-byte ASN "
			    "but sent AS4_AGGREGATOR attribute.");
			attr_free(a, naggr);
		}
		return;
	}
	/* OLD session using 2-byte ASNs */
	/* try to merge the new attributes into the old ones */
	if ((oaggr = attr_optget(a, ATTR_AGGREGATOR))) {
		memcpy(&as, oaggr->data, sizeof(as));
		if (ntohl(as) != AS_TRANS) {
			/* per RFC ignore AS4_PATH and AS4_AGGREGATOR */
			if (nasp)
				attr_free(a, nasp);
			if (naggr)
				attr_free(a, naggr);
			return;
		}
		if (naggr) {
			/* switch over to new AGGREGATOR */
			attr_free(a, oaggr);
			if (attr_optadd(a, ATTR_OPTIONAL | ATTR_TRANSITIVE,
			    ATTR_AGGREGATOR, naggr->data, naggr->len))
				fatalx("attr_optadd failed but impossible");
		}
	}
	/* there is no need for AS4_AGGREGATOR any more */
	if (naggr)
		attr_free(a, naggr);

	/* merge AS4_PATH with ASPATH */
	if (nasp)
		aspath_merge(a, nasp);
}


/*
 * route reflector helper function
 */
void
rde_reflector(struct rde_peer *peer, struct rde_aspath *asp)
{
	struct attr	*a;
	u_int8_t	*p;
	u_int16_t	 len;
	u_int32_t	 id;

	/* do not consider updates with parse errors */
	if (asp->flags & F_ATTR_PARSE_ERR)
		return;

	/* check for originator id if eq router_id drop */
	if ((a = attr_optget(asp, ATTR_ORIGINATOR_ID)) != NULL) {
		if (memcmp(&conf->bgpid, a->data, sizeof(conf->bgpid)) == 0) {
			/* this is coming from myself */
			asp->flags |= F_ATTR_LOOP;
			return;
		}
	} else if (conf->flags & BGPD_FLAG_REFLECTOR) {
		if (peer->conf.ebgp)
			id = conf->bgpid;
		else
			id = htonl(peer->remote_bgpid);
		if (attr_optadd(asp, ATTR_OPTIONAL, ATTR_ORIGINATOR_ID,
		    &id, sizeof(u_int32_t)) == -1)
			fatalx("attr_optadd failed but impossible");
	}

	/* check for own id in the cluster list */
	if (conf->flags & BGPD_FLAG_REFLECTOR) {
		if ((a = attr_optget(asp, ATTR_CLUSTER_LIST)) != NULL) {
			for (len = 0; len < a->len;
			    len += sizeof(conf->clusterid))
				/* check if coming from my cluster */
				if (memcmp(&conf->clusterid, a->data + len,
				    sizeof(conf->clusterid)) == 0) {
					asp->flags |= F_ATTR_LOOP;
					return;
				}

			/* prepend own clusterid by replacing attribute */
			len = a->len + sizeof(conf->clusterid);
			if (len < a->len)
				fatalx("rde_reflector: cluster-list overflow");
			if ((p = malloc(len)) == NULL)
				fatal("rde_reflector");
			memcpy(p, &conf->clusterid, sizeof(conf->clusterid));
			memcpy(p + sizeof(conf->clusterid), a->data, a->len);
			attr_free(asp, a);
			if (attr_optadd(asp, ATTR_OPTIONAL, ATTR_CLUSTER_LIST,
			    p, len) == -1)
				fatalx("attr_optadd failed but impossible");
			free(p);
		} else if (attr_optadd(asp, ATTR_OPTIONAL, ATTR_CLUSTER_LIST,
		    &conf->clusterid, sizeof(conf->clusterid)) == -1)
			fatalx("attr_optadd failed but impossible");
	}
}

/*
 * control specific functions
 */
static void
rde_dump_rib_as(struct prefix *p, struct rde_aspath *asp, pid_t pid, int flags)
{
	struct ctl_show_rib	 rib;
	struct ibuf		*wbuf;
	struct attr		*a;
	struct nexthop		*nexthop;
	void			*bp;
	time_t			 staletime;
	size_t			 aslen;
	u_int8_t		 l;

	nexthop = prefix_nexthop(p);
	bzero(&rib, sizeof(rib));
	rib.age = getmonotime() - p->lastchange;
	rib.local_pref = asp->lpref;
	rib.med = asp->med;
	rib.weight = asp->weight;
	strlcpy(rib.descr, prefix_peer(p)->conf.descr, sizeof(rib.descr));
	memcpy(&rib.remote_addr, &prefix_peer(p)->remote_addr,
	    sizeof(rib.remote_addr));
	rib.remote_id = prefix_peer(p)->remote_bgpid;
	if (nexthop != NULL) {
		memcpy(&rib.true_nexthop, &nexthop->true_nexthop,
		    sizeof(rib.true_nexthop));
		memcpy(&rib.exit_nexthop, &nexthop->exit_nexthop,
		    sizeof(rib.exit_nexthop));
	} else {
		/* announced network may have a NULL nexthop */
		bzero(&rib.true_nexthop, sizeof(rib.true_nexthop));
		bzero(&rib.exit_nexthop, sizeof(rib.exit_nexthop));
		rib.true_nexthop.aid = p->pt->aid;
		rib.exit_nexthop.aid = p->pt->aid;
	}
	pt_getaddr(p->pt, &rib.prefix);
	rib.prefixlen = p->pt->prefixlen;
	rib.origin = asp->origin;
	rib.validation_state = p->validation_state;
	rib.flags = 0;
	if (p->re != NULL && p->re->active == p)
		rib.flags |= F_PREF_ACTIVE;
	if (!prefix_peer(p)->conf.ebgp)
		rib.flags |= F_PREF_INTERNAL;
	if (asp->flags & F_PREFIX_ANNOUNCED)
		rib.flags |= F_PREF_ANNOUNCE;
	if (nexthop == NULL || nexthop->state == NEXTHOP_REACH)
		rib.flags |= F_PREF_ELIGIBLE;
	if (asp->flags & F_ATTR_LOOP)
		rib.flags &= ~F_PREF_ELIGIBLE;
	if (asp->flags & F_ATTR_PARSE_ERR)
		rib.flags |= F_PREF_INVALID;
	staletime = prefix_peer(p)->staletime[p->pt->aid];
	if (staletime && p->lastchange <= staletime)
		rib.flags |= F_PREF_STALE;
	aslen = aspath_length(asp->aspath);

	if ((wbuf = imsg_create(ibuf_se_ctl, IMSG_CTL_SHOW_RIB, 0, pid,
	    sizeof(rib) + aslen)) == NULL)
		return;
	if (imsg_add(wbuf, &rib, sizeof(rib)) == -1 ||
	    imsg_add(wbuf, aspath_dump(asp->aspath), aslen) == -1)
		return;
	imsg_close(ibuf_se_ctl, wbuf);

	if (flags & F_CTL_DETAIL) {
		struct rde_community *comm = prefix_communities(p);
		size_t len = comm->nentries * sizeof(struct community);
		if (comm->nentries > 0) {
			if ((wbuf = imsg_create(ibuf_se_ctl,
			    IMSG_CTL_SHOW_RIB_COMMUNITIES, 0, pid,
			    len)) == NULL)
				return;
			if ((bp = ibuf_reserve(wbuf, len)) == NULL) {
				ibuf_free(wbuf);
				return;
			}
			memcpy(bp, comm->communities, len);
			imsg_close(ibuf_se_ctl, wbuf);
		}
		for (l = 0; l < asp->others_len; l++) {
			if ((a = asp->others[l]) == NULL)
				break;
			if ((wbuf = imsg_create(ibuf_se_ctl,
			    IMSG_CTL_SHOW_RIB_ATTR, 0, pid,
			    attr_optlen(a))) == NULL)
				return;
			if ((bp = ibuf_reserve(wbuf, attr_optlen(a))) == NULL) {
				ibuf_free(wbuf);
				return;
			}
			if (attr_write(bp, attr_optlen(a), a->flags,
			    a->type, a->data, a->len) == -1) {
				ibuf_free(wbuf);
				return;
			}
			imsg_close(ibuf_se_ctl, wbuf);
		}
	}
}

int
rde_match_peer(struct rde_peer *p, struct ctl_neighbor *n)
{
	char *s;

	if (n && n->addr.aid) {
		if (memcmp(&p->conf.remote_addr, &n->addr,
		    sizeof(p->conf.remote_addr)))
			return 0;
	} else if (n && n->descr[0]) {
		s = n->is_group ? p->conf.group : p->conf.descr;
		if (strcmp(s, n->descr))
			return 0;
	}
	return 1;
}

static void
rde_dump_filter(struct prefix *p, struct ctl_show_rib_request *req)
{
	struct rde_aspath	*asp;

	if (!rde_match_peer(prefix_peer(p), &req->neighbor))
		return;

	asp = prefix_aspath(p);
	if (asp == NULL)	/* skip pending withdraw in Adj-RIB-Out */
		return;
	if ((req->flags & F_CTL_ACTIVE) && p->re->active != p)
		return;
	if ((req->flags & F_CTL_INVALID) &&
	    (asp->flags & F_ATTR_PARSE_ERR) == 0)
		return;
	if (req->as.type != AS_UNDEF &&
	    !aspath_match(asp->aspath, &req->as, 0))
		return;
	if (req->community.flags != 0) {
		if (!community_match(prefix_communities(p), &req->community,
		    NULL))
			return;
	}
	if (!ovs_match(p, req->flags))
		return;
	rde_dump_rib_as(p, asp, req->pid, req->flags);
}

static void
rde_dump_upcall(struct rib_entry *re, void *ptr)
{
	struct rde_dump_ctx	*ctx = ptr;
	struct prefix		*p;

	LIST_FOREACH(p, &re->prefix_h, entry.list.rib)
		rde_dump_filter(p, &ctx->req);
}

static void
rde_dump_prefix_upcall(struct rib_entry *re, void *ptr)
{
	struct rde_dump_ctx	*ctx = ptr;
	struct prefix		*p;
	struct pt_entry		*pt;
	struct bgpd_addr	 addr;

	pt = re->prefix;
	pt_getaddr(pt, &addr);
	if (addr.aid != ctx->req.prefix.aid)
		return;
	if (ctx->req.flags & F_LONGER) {
		if (ctx->req.prefixlen > pt->prefixlen)
			return;
		if (!prefix_compare(&ctx->req.prefix, &addr,
		    ctx->req.prefixlen))
			LIST_FOREACH(p, &re->prefix_h, entry.list.rib)
				rde_dump_filter(p, &ctx->req);
	} else {
		if (ctx->req.prefixlen < pt->prefixlen)
			return;
		if (!prefix_compare(&addr, &ctx->req.prefix,
		    pt->prefixlen))
			LIST_FOREACH(p, &re->prefix_h, entry.list.rib)
				rde_dump_filter(p, &ctx->req);
	}
}

static void
rde_dump_adjout_upcall(struct prefix *p, void *ptr)
{
	struct rde_dump_ctx	*ctx = ptr;

	if (p->flags & (PREFIX_FLAG_WITHDRAW | PREFIX_FLAG_DEAD))
		return;
	rde_dump_filter(p, &ctx->req);
}

static void
rde_dump_adjout_prefix_upcall(struct prefix *p, void *ptr)
{
	struct rde_dump_ctx	*ctx = ptr;
	struct bgpd_addr	 addr;

	if (p->flags & (PREFIX_FLAG_WITHDRAW | PREFIX_FLAG_DEAD))
		return;

	pt_getaddr(p->pt, &addr);
	if (addr.aid != ctx->req.prefix.aid)
		return;
	if (ctx->req.flags & F_LONGER) {
		if (ctx->req.prefixlen > p->pt->prefixlen)
			return;
		if (!prefix_compare(&ctx->req.prefix, &addr,
		    ctx->req.prefixlen))
			rde_dump_filter(p, &ctx->req);
	} else {
		if (ctx->req.prefixlen < p->pt->prefixlen)
			return;
		if (!prefix_compare(&addr, &ctx->req.prefix,
		    p->pt->prefixlen))
			rde_dump_filter(p, &ctx->req);
	}
}

static int
rde_dump_throttled(void *arg)
{
	struct rde_dump_ctx	*ctx = arg;

	return (ctx->throttled != 0);
}

static void
rde_dump_done(void *arg, u_int8_t aid)
{
	struct rde_dump_ctx	*ctx = arg;
	struct rde_peer		*peer;
	u_int			 error;

	if (ctx->req.flags & F_CTL_ADJ_OUT) {
		peer = peer_match(&ctx->req.neighbor, ctx->peerid);
		if (peer == NULL)
			goto done;
		ctx->peerid = peer->conf.id;
		switch (ctx->req.type) {
		case IMSG_CTL_SHOW_RIB:
			if (prefix_dump_new(peer, ctx->req.aid,
			    CTL_MSG_HIGH_MARK, ctx, rde_dump_adjout_upcall,
			    rde_dump_done, rde_dump_throttled) == -1)
				goto nomem;
			break;
		case IMSG_CTL_SHOW_RIB_PREFIX:
			if (prefix_dump_new(peer, ctx->req.aid,
			    CTL_MSG_HIGH_MARK, ctx,
			    rde_dump_adjout_prefix_upcall,
			    rde_dump_done, rde_dump_throttled) == -1)
				goto nomem;
			break;
		default:
			fatalx("%s: unsupported imsg type", __func__);
		}
		return;
	}
done:
	imsg_compose(ibuf_se_ctl, IMSG_CTL_END, 0, ctx->req.pid, -1, NULL, 0);
	LIST_REMOVE(ctx, entry);
	free(ctx);
	return;

nomem:
	log_warn(__func__);
	error = CTL_RES_NOMEM;
	imsg_compose(ibuf_se_ctl, IMSG_CTL_RESULT, 0, ctx->req.pid, -1, &error,
	    sizeof(error));
	return;
}

void
rde_dump_ctx_new(struct ctl_show_rib_request *req, pid_t pid,
    enum imsg_type type)
{
	struct rde_dump_ctx	*ctx;
	struct rib_entry	*re;
	struct prefix		*p;
	u_int			 error;
	u_int8_t		 hostplen;
	u_int16_t		 rid;

	if ((ctx = calloc(1, sizeof(*ctx))) == NULL) {
 nomem:
		log_warn(__func__);
		error = CTL_RES_NOMEM;
		imsg_compose(ibuf_se_ctl, IMSG_CTL_RESULT, 0, pid, -1, &error,
		    sizeof(error));
		return;
	}

	memcpy(&ctx->req, req, sizeof(struct ctl_show_rib_request));
	ctx->req.pid = pid;
	ctx->req.type = type;

	if (req->flags & (F_CTL_ADJ_IN | F_CTL_INVALID)) {
		rid = RIB_ADJ_IN;
	} else if (req->flags & F_CTL_ADJ_OUT) {
		struct rde_peer *peer;

		peer = peer_match(&req->neighbor, 0);
		if (peer == NULL) {
			log_warnx("%s: no peer found for adj-rib-out",
			    __func__);
			error = CTL_RES_NOSUCHPEER;
			imsg_compose(ibuf_se_ctl, IMSG_CTL_RESULT, 0, pid, -1,
			    &error, sizeof(error));
			free(ctx);
			return;
		}
		ctx->peerid = peer->conf.id;
		switch (ctx->req.type) {
		case IMSG_CTL_SHOW_RIB:
			if (prefix_dump_new(peer, ctx->req.aid,
			    CTL_MSG_HIGH_MARK, ctx, rde_dump_adjout_upcall,
			    rde_dump_done, rde_dump_throttled) == -1)
				goto nomem;
			break;
		case IMSG_CTL_SHOW_RIB_PREFIX:
			if (req->flags & (F_LONGER|F_SHORTER)) {
				if (prefix_dump_new(peer, ctx->req.aid,
				    CTL_MSG_HIGH_MARK, ctx,
				    rde_dump_adjout_prefix_upcall,
				    rde_dump_done, rde_dump_throttled) == -1)
					goto nomem;
				break;
			}
			switch (req->prefix.aid) {
			case AID_INET:
			case AID_VPN_IPv4:
				hostplen = 32;
				break;
			case AID_INET6:
			case AID_VPN_IPv6:
				hostplen = 128;
				break;
			default:
				fatalx("%s: unknown af", __func__);
			}

			do {
				if (req->prefixlen == hostplen)
					p = prefix_match(peer, &req->prefix);
				else
					p = prefix_lookup(peer, &req->prefix,
					    req->prefixlen);
				if (p)
					rde_dump_adjout_upcall(p, ctx);
			} while ((peer = peer_match(&req->neighbor,
			    peer->conf.id)));

			imsg_compose(ibuf_se_ctl, IMSG_CTL_END, 0, ctx->req.pid,
			    -1, NULL, 0);
			free(ctx);
			return;
		default:
			fatalx("%s: unsupported imsg type", __func__);
		}

		LIST_INSERT_HEAD(&rde_dump_h, ctx, entry);
		return;
	} else if ((rid = rib_find(req->rib)) == RIB_NOTFOUND) {
		log_warnx("%s: no such rib %s", __func__, req->rib);
		error = CTL_RES_NOSUCHRIB;
		imsg_compose(ibuf_se_ctl, IMSG_CTL_RESULT, 0, pid, -1, &error,
		    sizeof(error));
		free(ctx);
		return;
	}

	switch (ctx->req.type) {
	case IMSG_CTL_SHOW_NETWORK:
		if (rib_dump_new(rid, ctx->req.aid, CTL_MSG_HIGH_MARK, ctx,
		    network_dump_upcall, rde_dump_done,
		    rde_dump_throttled) == -1)
			goto nomem;
		break;
	case IMSG_CTL_SHOW_RIB:
		if (rib_dump_new(rid, ctx->req.aid, CTL_MSG_HIGH_MARK, ctx,
		    rde_dump_upcall, rde_dump_done, rde_dump_throttled) == -1)
			goto nomem;
		break;
	case IMSG_CTL_SHOW_RIB_PREFIX:
		if (req->flags & (F_LONGER|F_SHORTER)) {
			if (rib_dump_new(rid, ctx->req.aid,
			    CTL_MSG_HIGH_MARK, ctx, rde_dump_prefix_upcall,
			    rde_dump_done, rde_dump_throttled) == -1)
				goto nomem;
			break;
		}
		switch (req->prefix.aid) {
		case AID_INET:
		case AID_VPN_IPv4:
			hostplen = 32;
			break;
		case AID_INET6:
		case AID_VPN_IPv6:
			hostplen = 128;
			break;
		default:
			fatalx("%s: unknown af", __func__);
		}
		if (req->prefixlen == hostplen)
			re = rib_match(rib_byid(rid), &req->prefix);
		else
			re = rib_get(rib_byid(rid), &req->prefix,
			    req->prefixlen);
		if (re)
			rde_dump_upcall(re, ctx);
		imsg_compose(ibuf_se_ctl, IMSG_CTL_END, 0, ctx->req.pid,
		    -1, NULL, 0);
		free(ctx);
		return;
	default:
		fatalx("%s: unsupported imsg type", __func__);
	}
	LIST_INSERT_HEAD(&rde_dump_h, ctx, entry);
}

void
rde_dump_ctx_throttle(pid_t pid, int throttle)
{
	struct rde_dump_ctx	*ctx;

	LIST_FOREACH(ctx, &rde_dump_h, entry) {
		if (ctx->req.pid == pid) {
			ctx->throttled = throttle;
			return;
		}
	}
}

void
rde_dump_ctx_terminate(pid_t pid)
{
	struct rde_dump_ctx	*ctx;

	LIST_FOREACH(ctx, &rde_dump_h, entry) {
		if (ctx->req.pid == pid) {
			rib_dump_terminate(ctx);
			return;
		}
	}
}

static int
rde_mrt_throttled(void *arg)
{
	struct mrt	*mrt = arg;

	return (mrt->wbuf.queued > SESS_MSG_LOW_MARK);
}

static void
rde_mrt_done(void *ptr, u_int8_t aid)
{
	mrt_done(ptr);
}

void
rde_dump_mrt_new(struct mrt *mrt, pid_t pid, int fd)
{
	struct rde_mrt_ctx *ctx;
	u_int16_t rid;

	if ((ctx = calloc(1, sizeof(*ctx))) == NULL) {
		log_warn("rde_dump_mrt_new");
		return;
	}
	memcpy(&ctx->mrt, mrt, sizeof(struct mrt));
	TAILQ_INIT(&ctx->mrt.wbuf.bufs);
	ctx->mrt.wbuf.fd = fd;
	ctx->mrt.state = MRT_STATE_RUNNING;
	rid = rib_find(ctx->mrt.rib);
	if (rid == RIB_NOTFOUND) {
		log_warnx("non existing RIB %s for mrt dump", ctx->mrt.rib);
		free(ctx);
		return;
	}

	if (ctx->mrt.type == MRT_TABLE_DUMP_V2)
		mrt_dump_v2_hdr(&ctx->mrt, conf, &peerlist);

	if (rib_dump_new(rid, AID_UNSPEC, CTL_MSG_HIGH_MARK, &ctx->mrt,
	    mrt_dump_upcall, rde_mrt_done, rde_mrt_throttled) == -1)
		fatal("%s: rib_dump_new", __func__);

	LIST_INSERT_HEAD(&rde_mrts, ctx, entry);
	rde_mrt_cnt++;
}

/*
 * kroute specific functions
 */
int
rde_l3vpn_import(struct rde_community *comm, struct l3vpn *rd)
{
	struct filter_set	*s;

	TAILQ_FOREACH(s, &rd->import, entry) {
		if (community_match(comm, &s->action.community, 0))
			return (1);
	}
	return (0);
}

void
rde_send_kroute_flush(struct rib *rib)
{
	if (imsg_compose(ibuf_main, IMSG_KROUTE_FLUSH, rib->rtableid, 0, -1,
	    NULL, 0) == -1)
		fatal("%s %d imsg_compose error", __func__, __LINE__);
}

void
rde_send_kroute(struct rib *rib, struct prefix *new, struct prefix *old)
{
	struct kroute_full	 kr;
	struct bgpd_addr	 addr;
	struct prefix		*p;
	struct rde_aspath	*asp;
	struct l3vpn		*vpn;
	enum imsg_type		 type;

	/*
	 * Make sure that self announce prefixes are not committed to the
	 * FIB. If both prefixes are unreachable no update is needed.
	 */
	if ((old == NULL || prefix_aspath(old)->flags & F_PREFIX_ANNOUNCED) &&
	    (new == NULL || prefix_aspath(new)->flags & F_PREFIX_ANNOUNCED))
		return;

	if (new == NULL || prefix_aspath(new)->flags & F_PREFIX_ANNOUNCED) {
		type = IMSG_KROUTE_DELETE;
		p = old;
	} else {
		type = IMSG_KROUTE_CHANGE;
		p = new;
	}

	asp = prefix_aspath(p);
	pt_getaddr(p->pt, &addr);
	bzero(&kr, sizeof(kr));
	memcpy(&kr.prefix, &addr, sizeof(kr.prefix));
	kr.prefixlen = p->pt->prefixlen;
	if (prefix_nhflags(p) == NEXTHOP_REJECT)
		kr.flags |= F_REJECT;
	if (prefix_nhflags(p) == NEXTHOP_BLACKHOLE)
		kr.flags |= F_BLACKHOLE;
	if (type == IMSG_KROUTE_CHANGE)
		memcpy(&kr.nexthop, &prefix_nexthop(p)->true_nexthop,
		    sizeof(kr.nexthop));
	strlcpy(kr.label, rtlabel_id2name(asp->rtlabelid), sizeof(kr.label));

	switch (addr.aid) {
	case AID_VPN_IPv4:
	case AID_VPN_IPv6:
		if (!(rib->flags & F_RIB_LOCAL))
			/* not Loc-RIB, no update for VPNs */
			break;

		SIMPLEQ_FOREACH(vpn, &conf->l3vpns, entry) {
			if (!rde_l3vpn_import(prefix_communities(p), vpn))
				continue;
			/* must send exit_nexthop so that correct MPLS tunnel
			 * is chosen
			 */
			if (type == IMSG_KROUTE_CHANGE)
				memcpy(&kr.nexthop,
				    &prefix_nexthop(p)->exit_nexthop,
				    sizeof(kr.nexthop));
			/* XXX not ideal but this will change */
			kr.ifindex = if_nametoindex(vpn->ifmpe);
			if (imsg_compose(ibuf_main, type, vpn->rtableid, 0, -1,
			    &kr, sizeof(kr)) == -1)
				fatal("%s %d imsg_compose error", __func__,
				    __LINE__);
		}
		break;
	default:
		if (imsg_compose(ibuf_main, type, rib->rtableid, 0, -1,
		    &kr, sizeof(kr)) == -1)
			fatal("%s %d imsg_compose error", __func__, __LINE__);
		break;
	}
}

/*
 * update specific functions
 */
void
rde_generate_updates(struct rib *rib, struct prefix *new, struct prefix *old)
{
	struct rde_peer			*peer;

	/*
	 * If old is != NULL we know it was active and should be removed.
	 * If new is != NULL we know it is reachable and then we should
	 * generate an update.
	 */
	if (old == NULL && new == NULL)
		return;

	LIST_FOREACH(peer, &peerlist, peer_l) {
		if (peer->conf.id == 0)
			continue;
		if (peer->loc_rib_id != rib->id)
			continue;
		if (peer->state != PEER_UP)
			continue;
		up_generate_updates(out_rules, peer, new, old);
	}
}

static void
rde_up_flush_upcall(struct prefix *p, void *ptr)
{
	up_generate_updates(out_rules, prefix_peer(p), NULL, p);
}

u_char	queue_buf[4096];

int
rde_update_queue_pending(void)
{
	struct rde_peer *peer;
	u_int8_t aid;

	if (ibuf_se && ibuf_se->w.queued >= SESS_MSG_HIGH_MARK)
		return 0;

	LIST_FOREACH(peer, &peerlist, peer_l) {
		if (peer->conf.id == 0)
			continue;
		if (peer->state != PEER_UP)
			continue;
		if (peer->throttled)
			continue;
		for (aid = 0; aid < AID_MAX; aid++) {
			if (!RB_EMPTY(&peer->updates[aid]) ||
			    !RB_EMPTY(&peer->withdraws[aid]))
				return 1;
		}
	}
	return 0;
}

void
rde_update_queue_runner(void)
{
	struct rde_peer		*peer;
	int			 r, sent, max = RDE_RUNNER_ROUNDS, eor;
	u_int16_t		 len, wpos;

	len = sizeof(queue_buf) - MSGSIZE_HEADER;
	do {
		sent = 0;
		LIST_FOREACH(peer, &peerlist, peer_l) {
			if (peer->conf.id == 0)
				continue;
			if (peer->state != PEER_UP)
				continue;
			if (peer->throttled)
				continue;
			eor = 0;
			wpos = 0;
			/* first withdraws, save 2 bytes for path attributes */
			if ((r = up_dump_withdraws(queue_buf, len - 2, peer,
			    AID_INET)) == -1)
				continue;
			wpos += r;

			/* now bgp path attributes unless it is the EoR mark */
			if (up_is_eor(peer, AID_INET)) {
				eor = 1;
				bzero(queue_buf + wpos, 2);
				wpos += 2;
			} else {
				r = up_dump_attrnlri(queue_buf + wpos,
				    len - wpos, peer);
				wpos += r;
			}

			/* finally send message to SE */
			if (wpos > 4) {
				if (imsg_compose(ibuf_se, IMSG_UPDATE,
				    peer->conf.id, 0, -1, queue_buf,
				    wpos) == -1)
					fatal("%s %d imsg_compose error",
					    __func__, __LINE__);
				sent++;
			}
			if (eor)
				rde_peer_send_eor(peer, AID_INET);
		}
		max -= sent;
	} while (sent != 0 && max > 0);
}

void
rde_update6_queue_runner(u_int8_t aid)
{
	struct rde_peer		*peer;
	int			 r, sent, max = RDE_RUNNER_ROUNDS / 2;
	u_int16_t		 len;

	/* first withdraws ... */
	do {
		sent = 0;
		LIST_FOREACH(peer, &peerlist, peer_l) {
			if (peer->conf.id == 0)
				continue;
			if (peer->state != PEER_UP)
				continue;
			if (peer->throttled)
				continue;
			len = sizeof(queue_buf) - MSGSIZE_HEADER;
			r = up_dump_mp_unreach(queue_buf, len, peer, aid);
			if (r == -1)
				continue;
			/* finally send message to SE */
			if (imsg_compose(ibuf_se, IMSG_UPDATE, peer->conf.id,
			    0, -1, queue_buf, r) == -1)
				fatal("%s %d imsg_compose error", __func__,
				    __LINE__);
			sent++;
		}
		max -= sent;
	} while (sent != 0 && max > 0);

	/* ... then updates */
	max = RDE_RUNNER_ROUNDS / 2;
	do {
		sent = 0;
		LIST_FOREACH(peer, &peerlist, peer_l) {
			if (peer->conf.id == 0)
				continue;
			if (peer->state != PEER_UP)
				continue;
			if (peer->throttled)
				continue;
			len = sizeof(queue_buf) - MSGSIZE_HEADER;
			if (up_is_eor(peer, aid)) {
				rde_peer_send_eor(peer, aid);
				continue;
			}
			r = up_dump_mp_reach(queue_buf, len, peer, aid);
			if (r == 0)
				continue;

			/* finally send message to SE */
			if (imsg_compose(ibuf_se, IMSG_UPDATE, peer->conf.id,
			    0, -1, queue_buf, r) == -1)
				fatal("%s %d imsg_compose error", __func__,
				    __LINE__);
			sent++;
		}
		max -= sent;
	} while (sent != 0 && max > 0);
}

/*
 * pf table specific functions
 */
struct rde_pftable_node {
	RB_ENTRY(rde_pftable_node)	 entry;
	struct pt_entry			*prefix;
	int				 refcnt;
	u_int16_t			 id;
};
RB_HEAD(rde_pftable_tree, rde_pftable_node);

static inline int
rde_pftable_cmp(struct rde_pftable_node *a, struct rde_pftable_node *b)
{
	if (a->prefix > b->prefix)
		return 1;
	if (a->prefix < b->prefix)
		return -1;
	return (a->id - b->id);
}

RB_GENERATE_STATIC(rde_pftable_tree, rde_pftable_node, entry, rde_pftable_cmp);

struct rde_pftable_tree pftable_tree = RB_INITIALIZER(&pftable_tree);
int need_commit;

static void
rde_pftable_send(u_int16_t id, struct pt_entry *pt, int del)
{
	struct pftable_msg pfm;

	if (id == 0)
		return;

	/* do not run while cleaning up */
	if (rde_quit)
		return;

	bzero(&pfm, sizeof(pfm));
	strlcpy(pfm.pftable, pftable_id2name(id), sizeof(pfm.pftable));
	pt_getaddr(pt, &pfm.addr);
	pfm.len = pt->prefixlen;

	if (imsg_compose(ibuf_main,
	    del ? IMSG_PFTABLE_REMOVE : IMSG_PFTABLE_ADD,
	    0, 0, -1, &pfm, sizeof(pfm)) == -1)
		fatal("%s %d imsg_compose error", __func__, __LINE__);

	need_commit = 1;
}

void
rde_pftable_add(u_int16_t id, struct prefix *p)
{
	struct rde_pftable_node *pfn, node;

	memset(&node, 0, sizeof(node));
	node.prefix = p->pt;
	node.id = id;

	pfn = RB_FIND(rde_pftable_tree, &pftable_tree, &node);
	if (pfn == NULL) {
		if ((pfn = calloc(1, sizeof(*pfn))) == NULL)
			fatal("%s", __func__);
		pfn->prefix = pt_ref(p->pt);
		pfn->id = id;

		if (RB_INSERT(rde_pftable_tree, &pftable_tree, pfn) != NULL)
			fatalx("%s: tree corrupt", __func__);

		rde_pftable_send(id, p->pt, 0);
	}
	pfn->refcnt++;
}

void
rde_pftable_del(u_int16_t id, struct prefix *p)
{
	struct rde_pftable_node *pfn, node;

	memset(&node, 0, sizeof(node));
	node.prefix = p->pt;
	node.id = id;

	pfn = RB_FIND(rde_pftable_tree, &pftable_tree, &node);
	if (pfn == NULL)
		return;

	if (--pfn->refcnt <= 0) {
		rde_pftable_send(id, p->pt, 1);

		if (RB_REMOVE(rde_pftable_tree, &pftable_tree, pfn) == NULL)
			fatalx("%s: tree corrupt", __func__);

		pt_unref(pfn->prefix);
		free(pfn);
	}
}

void
rde_commit_pftable(void)
{
	/* do not run while cleaning up */
	if (rde_quit)
		return;

	if (!need_commit)
		return;

	if (imsg_compose(ibuf_main, IMSG_PFTABLE_COMMIT, 0, 0, -1, NULL, 0) ==
	    -1)
		fatal("%s %d imsg_compose error", __func__, __LINE__);

	need_commit = 0;
}

/*
 * nexthop specific functions
 */
void
rde_send_nexthop(struct bgpd_addr *next, int insert)
{
	int			 type;

	if (insert)
		type = IMSG_NEXTHOP_ADD;
	else
		type = IMSG_NEXTHOP_REMOVE;

	if (imsg_compose(ibuf_main, type, 0, 0, -1, next,
	    sizeof(struct bgpd_addr)) == -1)
		fatal("%s %d imsg_compose error", __func__, __LINE__);
}

/*
 * soft reconfig specific functions
 */
void
rde_reload_done(void)
{
	struct rde_peer		*peer;
	struct filter_head	*fh;
	struct rde_prefixset_head prefixsets_old;
	struct rde_prefixset_head originsets_old;
	struct rde_prefixset	 roa_old;
	struct as_set_head	 as_sets_old;
	u_int16_t		 rid;
	int			 reload = 0;

	softreconfig = 0;

	SIMPLEQ_INIT(&prefixsets_old);
	SIMPLEQ_INIT(&originsets_old);
	SIMPLEQ_INIT(&as_sets_old);
	SIMPLEQ_CONCAT(&prefixsets_old, &conf->rde_prefixsets);
	SIMPLEQ_CONCAT(&originsets_old, &conf->rde_originsets);
	SIMPLEQ_CONCAT(&as_sets_old, &conf->as_sets);
	roa_old = conf->rde_roa;

	/* merge the main config */
	copy_config(conf, nconf);

	/* need to copy the sets and roa table and clear them in nconf */
	SIMPLEQ_CONCAT(&conf->rde_prefixsets, &nconf->rde_prefixsets);
	SIMPLEQ_CONCAT(&conf->rde_originsets, &nconf->rde_originsets);
	SIMPLEQ_CONCAT(&conf->as_sets, &nconf->as_sets);

	conf->rde_roa = nconf->rde_roa;
	conf->rde_roa.lastchange = roa_old.lastchange;
	memset(&nconf->rde_roa, 0, sizeof(nconf->rde_roa));

	/* apply new set of l3vpn, sync will be done later */
	free_l3vpns(&conf->l3vpns);
	SIMPLEQ_CONCAT(&conf->l3vpns, &nconf->l3vpns);
	/* XXX WHERE IS THE SYNC ??? */

	free_config(nconf);
	nconf = NULL;

	/* sync peerself with conf */
	peerself->remote_bgpid = ntohl(conf->bgpid);
	peerself->conf.local_as = conf->as;
	peerself->conf.remote_as = conf->as;
	peerself->conf.remote_addr.aid = AID_INET;
	peerself->conf.remote_addr.v4.s_addr = conf->bgpid;
	peerself->conf.remote_masklen = 32;
	peerself->short_as = conf->short_as;

	/* check if roa changed */
	if (trie_equal(&conf->rde_roa.th, &roa_old.th) == 0) {
		log_debug("roa change: reloading Adj-RIB-In");
		conf->rde_roa.dirty = 1;
		conf->rde_roa.lastchange = getmonotime();
		reload++;	/* run softreconf in */
	}

	trie_free(&roa_old.th);	/* old roa no longer needed */

	rde_mark_prefixsets_dirty(&prefixsets_old, &conf->rde_prefixsets);
	rde_mark_prefixsets_dirty(&originsets_old, &conf->rde_originsets);
	as_sets_mark_dirty(&as_sets_old, &conf->as_sets);

	/*
	 * make the new filter rules the active one but keep the old for
	 * softrconfig. This is needed so that changes happening are using
	 * the right filters.
	 */
	fh = out_rules;
	out_rules = out_rules_tmp;
	out_rules_tmp = fh;

	rde_filter_calc_skip_steps(out_rules);

	/* check if filter changed */
	LIST_FOREACH(peer, &peerlist, peer_l) {
		if (peer->conf.id == 0)
			continue;
		peer->reconf_out = 0;
		peer->reconf_rib = 0;
		if (peer->loc_rib_id != rib_find(peer->conf.rib)) {
			log_peer_info(&peer->conf, "rib change, reloading");
			peer->loc_rib_id = rib_find(peer->conf.rib);
			if (peer->loc_rib_id == RIB_NOTFOUND)
				fatalx("King Bula's peer met an unknown RIB");
			peer->reconf_rib = 1;
			softreconfig++;
			if (prefix_dump_new(peer, AID_UNSPEC,
			    RDE_RUNNER_ROUNDS, NULL, rde_up_flush_upcall,
			    rde_softreconfig_in_done, NULL) == -1)
				fatal("%s: prefix_dump_new", __func__);
			log_peer_info(&peer->conf, "flushing Adj-RIB-Out");
			softreconfig++;	/* account for the running flush */
			continue;
		}
		if (!rde_filter_equal(out_rules, out_rules_tmp, peer)) {
			char *p = log_fmt_peer(&peer->conf);
			log_debug("out filter change: reloading peer %s", p);
			free(p);
			peer->reconf_out = 1;
		}
	}
	/* bring ribs in sync */
	for (rid = 0; rid < rib_size; rid++) {
		struct rib *rib = rib_byid(rid);
		if (rib == NULL)
			continue;
		rde_filter_calc_skip_steps(rib->in_rules_tmp);

		/* flip rules, make new active */
		fh = rib->in_rules;
		rib->in_rules = rib->in_rules_tmp;
		rib->in_rules_tmp = fh;

		switch (rib->state) {
		case RECONF_DELETE:
			rib_free(rib);
			break;
		case RECONF_RELOAD:
			rib_update(rib);
			rib->state = RECONF_KEEP;
			/* FALLTHROUGH */
		case RECONF_KEEP:
			if (rde_filter_equal(rib->in_rules,
			    rib->in_rules_tmp, NULL))
				/* rib is in sync */
				break;
			log_debug("in filter change: reloading RIB %s",
			    rib->name);
			rib->state = RECONF_RELOAD;
			reload++;
			break;
		case RECONF_REINIT:
			/* new rib */
			rib->state = RECONF_RELOAD;
			reload++;
			break;
		case RECONF_NONE:
			break;
		}
		filterlist_free(rib->in_rules_tmp);
		rib->in_rules_tmp = NULL;
	}

	filterlist_free(out_rules_tmp);
	out_rules_tmp = NULL;
	/* old filters removed, free all sets */
	free_rde_prefixsets(&prefixsets_old);
	free_rde_prefixsets(&originsets_old);
	as_sets_free(&as_sets_old);

	log_info("RDE reconfigured");

	if (reload > 0) {
		softreconfig++;
		if (rib_dump_new(RIB_ADJ_IN, AID_UNSPEC, RDE_RUNNER_ROUNDS,
		    rib_byid(RIB_ADJ_IN), rde_softreconfig_in,
		    rde_softreconfig_in_done, NULL) == -1)
			fatal("%s: rib_dump_new", __func__);
		log_info("running softreconfig in");
	} else {
		rde_softreconfig_in_done(NULL, AID_UNSPEC);
	}
}

static void
rde_softreconfig_in_done(void *arg, u_int8_t dummy)
{
	struct rde_peer	*peer;
	u_int16_t	 i;

	if (arg != NULL) {
		softreconfig--;
		/* one guy done but other dumps are still running */
		if (softreconfig > 0)
			return;

		log_info("softreconfig in done");
	}

	/* now do the Adj-RIB-Out sync and a possible FIB sync */
	softreconfig = 0;
	for (i = 0; i < rib_size; i++) {
		struct rib *rib = rib_byid(i);
		if (rib == NULL)
			continue;
		rib->state = RECONF_NONE;
		if (rib->fibstate == RECONF_RELOAD) {
			if (rib_dump_new(i, AID_UNSPEC, RDE_RUNNER_ROUNDS,
			    rib, rde_softreconfig_sync_fib,
			    rde_softreconfig_sync_done, NULL) == -1)
				fatal("%s: rib_dump_new", __func__);
			softreconfig++;
			log_info("starting fib sync for rib %s",
			    rib->name);
		} else if (rib->fibstate == RECONF_REINIT) {
			if (rib_dump_new(i, AID_UNSPEC, RDE_RUNNER_ROUNDS,
			    rib, rde_softreconfig_sync_reeval,
			    rde_softreconfig_sync_done, NULL) == -1)
				fatal("%s: rib_dump_new", __func__);
			softreconfig++;
			log_info("starting re-evaluation of rib %s",
			    rib->name);
		}
	}

	LIST_FOREACH(peer, &peerlist, peer_l) {
		u_int8_t aid;

		if (peer->reconf_out) {
			if (peer->conf.export_type == EXPORT_NONE) {
				/* nothing to do here */
				peer->reconf_out = 0;
			} else if (peer->conf.export_type ==
			    EXPORT_DEFAULT_ROUTE) {
				/* just resend the default route */
				for (aid = 0; aid < AID_MAX; aid++) {
					if (peer->capa.mp[aid])
						up_generate_default(out_rules,
						    peer, aid);
				}
				peer->reconf_out = 0;
			} else
				rib_byid(peer->loc_rib_id)->state =
				    RECONF_RELOAD;
		} else if (peer->reconf_rib) {
			/* dump the full table to neighbors that changed rib */
			for (aid = 0; aid < AID_MAX; aid++) {
				if (peer->capa.mp[aid])
					peer_dump(peer, aid);
			}
		}
	}

	for (i = 0; i < rib_size; i++) {
		struct rib *rib = rib_byid(i);
		if (rib == NULL)
			continue;
		if (rib->state == RECONF_RELOAD) {
			if (rib_dump_new(i, AID_UNSPEC, RDE_RUNNER_ROUNDS,
			    rib, rde_softreconfig_out,
			    rde_softreconfig_out_done, NULL) == -1)
				fatal("%s: rib_dump_new", __func__);
			softreconfig++;
			log_info("starting softreconfig out for rib %s",
			    rib->name);
		}
	}

	/* if nothing to do move to last stage */
	if (softreconfig == 0)
		rde_softreconfig_done();
}

static void
rde_softreconfig_out_done(void *arg, u_int8_t aid)
{
	struct rib	*rib = arg;

	/* this RIB dump is done */
	log_info("softreconfig out done for %s", rib->name);

	/* check if other dumps are still running */
	if (--softreconfig == 0)
		rde_softreconfig_done();
}

static void
rde_softreconfig_done(void)
{
	u_int16_t	i;

	for (i = 0; i < rib_size; i++) {
		struct rib *rib = rib_byid(i);
		if (rib == NULL)
			continue;
		rib->state = RECONF_NONE;
	}

	log_info("RDE soft reconfiguration done");
	imsg_compose(ibuf_main, IMSG_RECONF_DONE, 0, 0,
	    -1, NULL, 0);
}

static void
rde_softreconfig_in(struct rib_entry *re, void *bula)
{
	struct filterstate	 state;
	struct rib		*rib;
	struct prefix		*p;
	struct pt_entry		*pt;
	struct rde_peer		*peer;
	struct rde_aspath	*asp;
	enum filter_actions	 action;
	struct bgpd_addr	 prefix;
	int			 force_eval;
	u_int8_t		 vstate;
	u_int16_t		 i;

	pt = re->prefix;
	pt_getaddr(pt, &prefix);
	LIST_FOREACH(p, &re->prefix_h, entry.list.rib) {
		asp = prefix_aspath(p);
		peer = prefix_peer(p);
		force_eval = 0;

		if (conf->rde_roa.dirty) {
			/* ROA validation state update */
			vstate = rde_roa_validity(&conf->rde_roa,
			    &prefix, pt->prefixlen, aspath_origin(asp->aspath));
			if (vstate != p->validation_state) {
				force_eval = 1;
				p->validation_state = vstate;
			}
		}

		/* skip announced networks, they are never filtered */
		if (asp->flags & F_PREFIX_ANNOUNCED)
			continue;

		for (i = RIB_LOC_START; i < rib_size; i++) {
			rib = rib_byid(i);
			if (rib == NULL)
				continue;

			if (rib->state != RECONF_RELOAD && !force_eval)
				continue;

			rde_filterstate_prep(&state, asp, prefix_communities(p),
			    prefix_nexthop(p), prefix_nhflags(p));
			action = rde_filter(rib->in_rules, peer, peer, &prefix,
			    pt->prefixlen, p->validation_state, &state);

			if (action == ACTION_ALLOW) {
				/* update Local-RIB */
				prefix_update(rib, peer, &state, &prefix,
				    pt->prefixlen, p->validation_state);
			} else if (action == ACTION_DENY) {
				/* remove from Local-RIB */
				prefix_withdraw(rib, peer, &prefix,
				    pt->prefixlen);
			}

			rde_filterstate_clean(&state);
		}
	}
}

static void
rde_softreconfig_out(struct rib_entry *re, void *bula)
{
	struct prefix		*p = re->active;
	struct rde_peer		*peer;

	if (p == NULL)
		/* no valid path for prefix */
		return;

	LIST_FOREACH(peer, &peerlist, peer_l) {
		if (peer->loc_rib_id == re->rib_id && peer->reconf_out)
			/* Regenerate all updates. */
			up_generate_updates(out_rules, peer, p, p);
	}
}

static void
rde_softreconfig_sync_reeval(struct rib_entry *re, void *arg)
{
	struct prefix_list	prefixes;
	struct prefix		*p, *next;
	struct rib		*rib = arg;

	if (rib->flags & F_RIB_NOEVALUATE) {
		/*
		 * evaluation process is turned off
		 * so remove all prefixes from adj-rib-out
		 * also unlink nexthop if it was linked
		 */
		LIST_FOREACH(p, &re->prefix_h, entry.list.rib) {
			if (p->flags & PREFIX_NEXTHOP_LINKED)
				nexthop_unlink(p);
		}
		if (re->active) {
			rde_generate_updates(rib, NULL, re->active);
			re->active = NULL;
		}
		return;
	}

	/* evaluation process is turned on, so evaluate all prefixes again */
	re->active = NULL;
	prefixes = re->prefix_h;
	LIST_INIT(&re->prefix_h);

	LIST_FOREACH_SAFE(p, &prefixes, entry.list.rib, next) {
		/* need to re-link the nexthop if not already linked */
		if ((p->flags & PREFIX_NEXTHOP_LINKED) == 0)
			nexthop_link(p);
		LIST_REMOVE(p, entry.list.rib);
		prefix_evaluate(p, re);
	}
}

static void
rde_softreconfig_sync_fib(struct rib_entry *re, void *bula)
{
	if (re->active)
		rde_send_kroute(re_rib(re), re->active, NULL);
}

static void
rde_softreconfig_sync_done(void *arg, u_int8_t aid)
{
	struct rib *rib = arg;

	/* this RIB dump is done */
	if (rib->fibstate == RECONF_RELOAD)
		log_info("fib sync done for %s", rib->name);
	else
		log_info("re-evaluation done for %s", rib->name);
	rib->fibstate = RECONF_NONE;

	/* check if other dumps are still running */
	if (--softreconfig == 0)
		rde_softreconfig_done();
}

/*
 * generic helper function
 */
u_int32_t
rde_local_as(void)
{
	return (conf->as);
}

int
rde_decisionflags(void)
{
	return (conf->flags & BGPD_FLAG_DECISION_MASK);
}

int
rde_as4byte(struct rde_peer *peer)
{
	return (peer->capa.as4byte);
}

/* End-of-RIB marker, RFC 4724 */
static void
rde_peer_recv_eor(struct rde_peer *peer, u_int8_t aid)
{
	peer->prefix_rcvd_eor++;

	/*
	 * First notify SE to avert a possible race with the restart timeout.
	 * If the timeout fires before this imsg is processed by the SE it will
	 * result in the same operation since the timeout issues a FLUSH which
	 * does the same as the RESTARTED action (flushing stale routes).
	 * The logic in the SE is so that only one of FLUSH or RESTARTED will
	 * be sent back to the RDE and so peer_flush is only called once.
	 */
	if (imsg_compose(ibuf_se, IMSG_SESSION_RESTARTED, peer->conf.id,
	    0, -1, &aid, sizeof(aid)) == -1)
		fatal("imsg_compose error while receiving EoR");

	log_peer_info(&peer->conf, "received %s EOR marker",
	    aid2str(aid));
}

static void
rde_peer_send_eor(struct rde_peer *peer, u_int8_t aid)
{
	u_int16_t	afi;
	u_int8_t	safi;

	peer->prefix_sent_eor++;

	if (aid == AID_INET) {
		u_char null[4];

		bzero(&null, 4);
		if (imsg_compose(ibuf_se, IMSG_UPDATE, peer->conf.id,
		    0, -1, &null, 4) == -1)
			fatal("imsg_compose error while sending EoR");
	} else {
		u_int16_t	i;
		u_char		buf[10];

		if (aid2afi(aid, &afi, &safi) == -1)
			fatalx("peer_send_eor: bad AID");

		i = 0;	/* v4 withdrawn len */
		bcopy(&i, &buf[0], sizeof(i));
		i = htons(6);	/* path attr len */
		bcopy(&i, &buf[2], sizeof(i));
		buf[4] = ATTR_OPTIONAL;
		buf[5] = ATTR_MP_UNREACH_NLRI;
		buf[6] = 3;	/* withdrawn len */
		i = htons(afi);
		bcopy(&i, &buf[7], sizeof(i));
		buf[9] = safi;

		if (imsg_compose(ibuf_se, IMSG_UPDATE, peer->conf.id,
		    0, -1, &buf, 10) == -1)
			fatal("%s %d imsg_compose error in peer_send_eor",
			    __func__, __LINE__);
	}

	log_peer_info(&peer->conf, "sending %s EOR marker",
	    aid2str(aid));
}

/*
 * network announcement stuff
 */
void
network_add(struct network_config *nc, struct filterstate *state)
{
	struct l3vpn		*vpn;
	struct filter_set_head	*vpnset = NULL;
	in_addr_t		 prefix4;
	struct in6_addr		 prefix6;
	u_int8_t		 vstate;
	u_int16_t		 i;

	if (nc->rd != 0) {
		SIMPLEQ_FOREACH(vpn, &conf->l3vpns, entry) {
			if (vpn->rd != nc->rd)
				continue;
			switch (nc->prefix.aid) {
			case AID_INET:
				prefix4 = nc->prefix.v4.s_addr;
				bzero(&nc->prefix, sizeof(nc->prefix));
				nc->prefix.aid = AID_VPN_IPv4;
				nc->prefix.vpn4.rd = vpn->rd;
				nc->prefix.vpn4.addr.s_addr = prefix4;
				nc->prefix.vpn4.labellen = 3;
				nc->prefix.vpn4.labelstack[0] =
				    (vpn->label >> 12) & 0xff;
				nc->prefix.vpn4.labelstack[1] =
				    (vpn->label >> 4) & 0xff;
				nc->prefix.vpn4.labelstack[2] =
				    (vpn->label << 4) & 0xf0;
				nc->prefix.vpn4.labelstack[2] |= BGP_MPLS_BOS;
				vpnset = &vpn->export;
				break;
			case AID_INET6:
				memcpy(&prefix6, &nc->prefix.v6.s6_addr,
				    sizeof(struct in6_addr));
				memset(&nc->prefix, 0, sizeof(nc->prefix));
				nc->prefix.aid = AID_VPN_IPv6;
				nc->prefix.vpn6.rd = vpn->rd;
				memcpy(&nc->prefix.vpn6.addr.s6_addr, &prefix6,
				    sizeof(struct in6_addr));
				nc->prefix.vpn6.labellen = 3;
				nc->prefix.vpn6.labelstack[0] =
				    (vpn->label >> 12) & 0xff;
				nc->prefix.vpn6.labelstack[1] =
				    (vpn->label >> 4) & 0xff;
				nc->prefix.vpn6.labelstack[2] =
				    (vpn->label << 4) & 0xf0;
				nc->prefix.vpn6.labelstack[2] |= BGP_MPLS_BOS;
				vpnset = &vpn->export;
				break;
			default:
				log_warnx("unable to VPNize prefix");
				filterset_free(&nc->attrset);
				return;
			}
			break;
		}
		if (vpn == NULL) {
			log_warnx("network_add: "
			    "prefix %s/%u in non-existing l3vpn %s",
			    log_addr(&nc->prefix), nc->prefixlen,
			    log_rd(nc->rd));
			return;
		}
	}

	rde_apply_set(&nc->attrset, peerself, peerself, state, nc->prefix.aid);
	if (vpnset)
		rde_apply_set(vpnset, peerself, peerself, state,
		    nc->prefix.aid);

	vstate = rde_roa_validity(&conf->rde_roa, &nc->prefix,
	    nc->prefixlen, aspath_origin(state->aspath.aspath));
	if (prefix_update(rib_byid(RIB_ADJ_IN), peerself, state, &nc->prefix,
	    nc->prefixlen, vstate) == 1)
		peerself->prefix_cnt++;
	for (i = RIB_LOC_START; i < rib_size; i++) {
		struct rib *rib = rib_byid(i);
		if (rib == NULL)
			continue;
		rde_update_log("announce", i, peerself,
		    state->nexthop ? &state->nexthop->exit_nexthop : NULL,
		    &nc->prefix, nc->prefixlen);
		prefix_update(rib, peerself, state, &nc->prefix,
		    nc->prefixlen, vstate);
	}
	filterset_free(&nc->attrset);
}

void
network_delete(struct network_config *nc)
{
	struct l3vpn	*vpn;
	in_addr_t	 prefix4;
	struct in6_addr	 prefix6;
	u_int32_t	 i;

	if (nc->rd) {
		SIMPLEQ_FOREACH(vpn, &conf->l3vpns, entry) {
			if (vpn->rd != nc->rd)
				continue;
			switch (nc->prefix.aid) {
			case AID_INET:
				prefix4 = nc->prefix.v4.s_addr;
				bzero(&nc->prefix, sizeof(nc->prefix));
				nc->prefix.aid = AID_VPN_IPv4;
				nc->prefix.vpn4.rd = vpn->rd;
				nc->prefix.vpn4.addr.s_addr = prefix4;
				nc->prefix.vpn4.labellen = 3;
				nc->prefix.vpn4.labelstack[0] =
				    (vpn->label >> 12) & 0xff;
				nc->prefix.vpn4.labelstack[1] =
				    (vpn->label >> 4) & 0xff;
				nc->prefix.vpn4.labelstack[2] =
				    (vpn->label << 4) & 0xf0;
				nc->prefix.vpn4.labelstack[2] |= BGP_MPLS_BOS;
				break;
			case AID_INET6:
				memcpy(&prefix6, &nc->prefix.v6.s6_addr,
				    sizeof(struct in6_addr));
				memset(&nc->prefix, 0, sizeof(nc->prefix));
				nc->prefix.aid = AID_VPN_IPv6;
				nc->prefix.vpn6.rd = vpn->rd;
				memcpy(&nc->prefix.vpn6.addr.s6_addr, &prefix6,
				    sizeof(struct in6_addr));
				nc->prefix.vpn6.labellen = 3;
				nc->prefix.vpn6.labelstack[0] =
				    (vpn->label >> 12) & 0xff;
				nc->prefix.vpn6.labelstack[1] =
				    (vpn->label >> 4) & 0xff;
				nc->prefix.vpn6.labelstack[2] =
				    (vpn->label << 4) & 0xf0;
				nc->prefix.vpn6.labelstack[2] |= BGP_MPLS_BOS;
				break;
			default:
				log_warnx("unable to VPNize prefix");
				return;
			}
		}
	}

	for (i = RIB_LOC_START; i < rib_size; i++) {
		struct rib *rib = rib_byid(i);
		if (rib == NULL)
			continue;
		if (prefix_withdraw(rib, peerself, &nc->prefix,
		    nc->prefixlen))
			rde_update_log("withdraw announce", i, peerself,
			    NULL, &nc->prefix, nc->prefixlen);
	}
	if (prefix_withdraw(rib_byid(RIB_ADJ_IN), peerself, &nc->prefix,
	    nc->prefixlen))
		peerself->prefix_cnt--;
}

static void
network_dump_upcall(struct rib_entry *re, void *ptr)
{
	struct prefix		*p;
	struct rde_aspath	*asp;
	struct kroute_full	 k;
	struct bgpd_addr	 addr;
	struct rde_dump_ctx	*ctx = ptr;

	LIST_FOREACH(p, &re->prefix_h, entry.list.rib) {
		asp = prefix_aspath(p);
		if (!(asp->flags & F_PREFIX_ANNOUNCED))
			continue;
		pt_getaddr(p->pt, &addr);

		bzero(&k, sizeof(k));
		memcpy(&k.prefix, &addr, sizeof(k.prefix));
		if (prefix_nexthop(p) == NULL ||
		    prefix_nexthop(p)->state != NEXTHOP_REACH)
			k.nexthop.aid = k.prefix.aid;
		else
			memcpy(&k.nexthop, &prefix_nexthop(p)->true_nexthop,
			    sizeof(k.nexthop));
		k.prefixlen = p->pt->prefixlen;
		k.flags = F_KERNEL;
		if ((asp->flags & F_ANN_DYNAMIC) == 0)
			k.flags = F_STATIC;
		if (imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_NETWORK, 0,
		    ctx->req.pid, -1, &k, sizeof(k)) == -1)
			log_warnx("network_dump_upcall: "
			    "imsg_compose error");
	}
}

static void
network_flush_upcall(struct rib_entry *re, void *ptr)
{
	struct rde_peer *peer = ptr;
	struct bgpd_addr addr;
	struct prefix *p;
	u_int32_t i;
	u_int8_t prefixlen;

	p = prefix_bypeer(re, peer);
	if (p == NULL)
		return;
	if ((prefix_aspath(p)->flags & F_ANN_DYNAMIC) != F_ANN_DYNAMIC)
		return;

	pt_getaddr(re->prefix, &addr);
	prefixlen = re->prefix->prefixlen;

	for (i = RIB_LOC_START; i < rib_size; i++) {
		struct rib *rib = rib_byid(i);
		if (rib == NULL)
			continue;
		if (prefix_withdraw(rib, peer, &addr, prefixlen) == 1)
			rde_update_log("flush announce", i, peer,
			    NULL, &addr, prefixlen);
	}

	if (prefix_withdraw(rib_byid(RIB_ADJ_IN), peer, &addr,
	    prefixlen) == 1)
		peer->prefix_cnt--;
}

/* clean up */
void
rde_shutdown(void)
{
	/*
	 * the decision process is turned off if rde_quit = 1 and
	 * rde_shutdown depends on this.
	 */

	/* First all peers go down */
	peer_foreach(peer_down, NULL);

	/* free filters */
	filterlist_free(out_rules);
	filterlist_free(out_rules_tmp);

	/* kill the VPN configs */
	free_l3vpns(&conf->l3vpns);

	/* now check everything */
	rib_shutdown();
	nexthop_shutdown();
	path_shutdown();
	aspath_shutdown();
	attr_shutdown();
	pt_shutdown();
	peer_shutdown();
}

struct rde_prefixset *
rde_find_prefixset(char *name, struct rde_prefixset_head *p)
{
	struct rde_prefixset *ps;

	SIMPLEQ_FOREACH(ps, p, entry) {
		if (!strcmp(ps->name, name))
			return (ps);
	}
	return (NULL);
}

void
rde_mark_prefixsets_dirty(struct rde_prefixset_head *psold,
    struct rde_prefixset_head *psnew)
{
	struct rde_prefixset *new, *old;

	SIMPLEQ_FOREACH(new, psnew, entry) {
		if ((psold == NULL) ||
		    (old = rde_find_prefixset(new->name, psold)) == NULL) {
			new->dirty = 1;
			new->lastchange = getmonotime();
		} else {
			if (trie_equal(&new->th, &old->th) == 0) {
				new->dirty = 1;
				new->lastchange = getmonotime();
			} else
				new->lastchange = old->lastchange;
		}
	}
}

u_int8_t
rde_roa_validity(struct rde_prefixset *ps, struct bgpd_addr *prefix,
    u_int8_t plen, u_int32_t as)
{
	int r;

	r = trie_roa_check(&ps->th, prefix, plen, as);
	return (r & ROA_MASK);
}

int
ovs_match(struct prefix *p, u_int32_t flag)
{
	if (flag & (F_CTL_OVS_VALID|F_CTL_OVS_INVALID|F_CTL_OVS_NOTFOUND)) {
		switch (prefix_vstate(p)) {
		case ROA_VALID:
			if (!(flag & F_CTL_OVS_VALID))
				return 0;
			break;
		case ROA_INVALID:
			if (!(flag & F_CTL_OVS_INVALID))
				return 0;
			break;
		case ROA_NOTFOUND:
			if (!(flag & F_CTL_OVS_NOTFOUND))
				return 0;
			break;
		default:
			break;
		}
	}

	return 1;
}
