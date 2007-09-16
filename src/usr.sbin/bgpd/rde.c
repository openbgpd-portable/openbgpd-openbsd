/*	$OpenBSD: rde.c,v 1.228 2007/09/16 15:20:50 claudio Exp $ */

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

#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <pwd.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bgpd.h"
#include "mrt.h"
#include "rde.h"
#include "session.h"

#define PFD_PIPE_MAIN		0
#define PFD_PIPE_SESSION	1
#define PFD_PIPE_SESSION_CTL	2
#define PFD_MRT_FILE		3

void		 rde_sighdlr(int);
void		 rde_dispatch_imsg_session(struct imsgbuf *);
void		 rde_dispatch_imsg_parent(struct imsgbuf *);
int		 rde_update_dispatch(struct imsg *);
int		 rde_attr_parse(u_char *, u_int16_t, struct rde_peer *,
		     struct rde_aspath *, struct mpattr *);
u_int8_t	 rde_attr_missing(struct rde_aspath *, int, u_int16_t);
int		 rde_get_mp_nexthop(u_char *, u_int16_t, u_int16_t,
		     struct rde_aspath *);
int		 rde_update_get_prefix(u_char *, u_int16_t, struct bgpd_addr *,
		     u_int8_t *);
int		 rde_update_get_prefix6(u_char *, u_int16_t, struct bgpd_addr *,
		     u_int8_t *);
void		 rde_update_err(struct rde_peer *, u_int8_t , u_int8_t,
		     void *, u_int16_t);
void		 rde_update_log(const char *,
		     const struct rde_peer *, const struct bgpd_addr *,
		     const struct bgpd_addr *, u_int8_t);
void		 rde_as4byte_fixup(struct rde_peer *, struct rde_aspath *);
int		 rde_reflector(struct rde_peer *, struct rde_aspath *);

void		 rde_dump_rib_as(struct prefix *, struct rde_aspath *,pid_t,
		     int);
void		 rde_dump_filter(struct prefix *,
		     struct ctl_show_rib_request *);
void		 rde_dump_filterout(struct rde_peer *, struct prefix *,
		     struct ctl_show_rib_request *);
void		 rde_dump_upcall(struct pt_entry *, void *);
void		 rde_dump_as(struct ctl_show_rib_request *);
void		 rde_dump_prefix_upcall(struct pt_entry *, void *);
void		 rde_dump_prefix(struct ctl_show_rib_request *);
void		 rde_dump_community(struct ctl_show_rib_request *);
void		 rde_dump_ctx_new(struct ctl_show_rib_request *, pid_t,
		     enum imsg_type);
void		 rde_dump_runner(void);
int		 rde_dump_pending(void);

void		 rde_up_dump_upcall(struct pt_entry *, void *);
void		 rde_softreconfig_out(struct pt_entry *, void *);
void		 rde_softreconfig_in(struct pt_entry *, void *);
void		 rde_update_queue_runner(void);
void		 rde_update6_queue_runner(void);

void		 peer_init(u_int32_t);
void		 peer_shutdown(void);
void		 peer_localaddrs(struct rde_peer *, struct bgpd_addr *);
struct rde_peer	*peer_add(u_int32_t, struct peer_config *);
struct rde_peer	*peer_get(u_int32_t);
void		 peer_up(u_int32_t, struct session_up *);
void		 peer_down(u_int32_t);
void		 peer_dump(u_int32_t, u_int16_t, u_int8_t);
void		 peer_send_eor(struct rde_peer *, u_int16_t, u_int16_t);

void		 network_init(struct network_head *);
void		 network_add(struct network_config *, int);
void		 network_delete(struct network_config *, int);
void		 network_dump_upcall(struct pt_entry *, void *);
void		 network_flush(int);

void		 rde_shutdown(void);
int		 sa_cmp(struct bgpd_addr *, struct sockaddr *);

volatile sig_atomic_t	 rde_quit = 0;
struct bgpd_config	*conf, *nconf;
time_t			 reloadtime;
struct rde_peer_head	 peerlist;
struct rde_peer		 peerself;
struct rde_peer		 peerdynamic;
struct filter_head	*rules_l, *newrules;
struct imsgbuf		*ibuf_se;
struct imsgbuf		*ibuf_se_ctl;
struct imsgbuf		*ibuf_main;
struct mrt		*mrt;
struct rde_memstats	 rdemem;

struct rde_dump_ctx {
	TAILQ_ENTRY(rde_dump_ctx)	entry;
	struct pt_context		ptc;
	struct ctl_show_rib_request	req;
	sa_family_t			af;
};

TAILQ_HEAD(, rde_dump_ctx) rde_dump_h = TAILQ_HEAD_INITIALIZER(rde_dump_h);

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

u_int32_t	peerhashsize = 64;
u_int32_t	pathhashsize = 1024;
u_int32_t	attrhashsize = 512;
u_int32_t	nexthophashsize = 64;

pid_t
rde_main(struct bgpd_config *config, struct peer *peer_l,
    struct network_head *net_l, struct filter_head *rules,
    struct mrt_head *mrt_l, int pipe_m2r[2], int pipe_s2r[2], int pipe_m2s[2],
    int pipe_s2rctl[2], int debug)
{
	pid_t			 pid;
	struct passwd		*pw;
	struct peer		*p;
	struct listen_addr	*la;
	struct pollfd		 pfd[4];
	struct filter_rule	*f;
	struct filter_set	*set;
	struct nexthop		*nh;
	int			 i, timeout;

	switch (pid = fork()) {
	case -1:
		fatal("cannot fork");
	case 0:
		break;
	default:
		return (pid);
	}

	conf = config;

	if ((pw = getpwnam(BGPD_USER)) == NULL)
		fatal("getpwnam");

	if (chroot(pw->pw_dir) == -1)
		fatal("chroot");
	if (chdir("/") == -1)
		fatal("chdir(\"/\")");

	setproctitle("route decision engine");
	bgpd_process = PROC_RDE;

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		fatal("can't drop privileges");

	signal(SIGTERM, rde_sighdlr);
	signal(SIGINT, rde_sighdlr);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	close(pipe_s2r[0]);
	close(pipe_s2rctl[0]);
	close(pipe_m2r[0]);
	close(pipe_m2s[0]);
	close(pipe_m2s[1]);

	/* initialize the RIB structures */
	if ((ibuf_se = malloc(sizeof(struct imsgbuf))) == NULL ||
	    (ibuf_se_ctl = malloc(sizeof(struct imsgbuf))) == NULL ||
	    (ibuf_main = malloc(sizeof(struct imsgbuf))) == NULL)
		fatal(NULL);
	imsg_init(ibuf_se, pipe_s2r[1]);
	imsg_init(ibuf_se_ctl, pipe_s2rctl[1]);
	imsg_init(ibuf_main, pipe_m2r[1]);

	/* peer list, mrt list and listener list are not used in the RDE */
	while ((p = peer_l) != NULL) {
		peer_l = p->next;
		free(p);
	}

	while ((mrt = LIST_FIRST(mrt_l)) != NULL) {
		LIST_REMOVE(mrt, entry);
		free(mrt);
	}
	mrt = NULL;

	while ((la = TAILQ_FIRST(config->listen_addrs)) != NULL) {
		TAILQ_REMOVE(config->listen_addrs, la, entry);
		close(la->fd);
		free(la);
	}
	free(config->listen_addrs);

	pt_init();
	path_init(pathhashsize);
	aspath_init(pathhashsize);
	attr_init(attrhashsize);
	nexthop_init(nexthophashsize);
	peer_init(peerhashsize);
	rules_l = rules;
	network_init(net_l);

	log_info("route decision engine ready");

	TAILQ_FOREACH(f, rules, entry) {
		TAILQ_FOREACH(set, &f->set, entry) {
			if (set->type == ACTION_SET_NEXTHOP) {
				nh = nexthop_get(&set->action.nexthop);
				nh->refcnt++;
			}
		}
	}

	while (rde_quit == 0) {
		timeout = INFTIM;
		bzero(pfd, sizeof(pfd));
		pfd[PFD_PIPE_MAIN].fd = ibuf_main->fd;
		pfd[PFD_PIPE_MAIN].events = POLLIN;
		if (ibuf_main->w.queued > 0)
			pfd[PFD_PIPE_MAIN].events |= POLLOUT;

		pfd[PFD_PIPE_SESSION].fd = ibuf_se->fd;
		pfd[PFD_PIPE_SESSION].events = POLLIN;
		if (ibuf_se->w.queued > 0)
			pfd[PFD_PIPE_SESSION].events |= POLLOUT;

		pfd[PFD_PIPE_SESSION_CTL].fd = ibuf_se_ctl->fd;
		pfd[PFD_PIPE_SESSION_CTL].events = POLLIN;
		if (ibuf_se_ctl->w.queued > 0)
			pfd[PFD_PIPE_SESSION_CTL].events |= POLLOUT;
		else if (rde_dump_pending())
			timeout = 0;

		i = 3;
		if (mrt && mrt->queued) {
			pfd[PFD_MRT_FILE].fd = mrt->fd;
			pfd[PFD_MRT_FILE].events = POLLOUT;
			i++;
		}

		if (poll(pfd, i, timeout) == -1) {
			if (errno != EINTR)
				fatal("poll error");
			continue;
		}

		if ((pfd[PFD_PIPE_MAIN].revents & POLLOUT) &&
		    ibuf_main->w.queued)
			if (msgbuf_write(&ibuf_main->w) < 0)
				fatal("pipe write error");

		if (pfd[PFD_PIPE_MAIN].revents & POLLIN)
			rde_dispatch_imsg_parent(ibuf_main);

		if ((pfd[PFD_PIPE_SESSION].revents & POLLOUT) &&
		    ibuf_se->w.queued)
			if (msgbuf_write(&ibuf_se->w) < 0)
				fatal("pipe write error");

		if (pfd[PFD_PIPE_SESSION].revents & POLLIN)
			rde_dispatch_imsg_session(ibuf_se);

		if ((pfd[PFD_PIPE_SESSION_CTL].revents & POLLOUT) &&
		    ibuf_se_ctl->w.queued)
			if (msgbuf_write(&ibuf_se_ctl->w) < 0)
				fatal("pipe write error");

		if (pfd[PFD_PIPE_SESSION_CTL].revents & POLLIN)
			rde_dispatch_imsg_session(ibuf_se_ctl);

		if (pfd[PFD_MRT_FILE].revents & POLLOUT) {
			if (mrt_write(mrt) == -1) {
				free(mrt);
				mrt = NULL;
			} else if (mrt->queued == 0)
				close(mrt->fd);
		}

		rde_update_queue_runner();
		rde_update6_queue_runner();
		if (ibuf_se_ctl->w.queued <= 0)
			rde_dump_runner();
	}

	/* do not clean up on shutdown on production, it takes ages. */
	if (debug)
		rde_shutdown();

	msgbuf_clear(&ibuf_se->w);
	free(ibuf_se);
	msgbuf_clear(&ibuf_se_ctl->w);
	free(ibuf_se_ctl);
	msgbuf_clear(&ibuf_main->w);
	free(ibuf_main);

	log_info("route decision engine exiting");
	_exit(0);
}

struct network_config	 netconf_s, netconf_p;
struct filter_set_head	*session_set, *parent_set;

void
rde_dispatch_imsg_session(struct imsgbuf *ibuf)
{
	struct imsg		 imsg;
	struct peer		 p;
	struct peer_config	 pconf;
	struct rrefresh		 r;
	struct rde_peer		*peer;
	struct session_up	 sup;
	struct ctl_show_rib_request	req;
	struct filter_set	*s;
	struct nexthop		*nh;
	int			 n;
	sa_family_t		 af = AF_UNSPEC;

	if ((n = imsg_read(ibuf)) == -1)
		fatal("rde_dispatch_imsg_session: imsg_read error");
	if (n == 0)	/* connection closed */
		fatalx("rde_dispatch_imsg_session: pipe closed");

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("rde_dispatch_imsg_session: imsg_read error");
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_UPDATE:
			rde_update_dispatch(&imsg);
			break;
		case IMSG_SESSION_ADD:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(pconf))
				fatalx("incorrect size of session request");
			memcpy(&pconf, imsg.data, sizeof(pconf));
			peer = peer_add(imsg.hdr.peerid, &pconf);
			if (peer == NULL) {
				log_warnx("session add: "
				    "peer id %d already exists",
				    imsg.hdr.peerid);
				break;
			}
			break;
		case IMSG_SESSION_UP:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(sup))
				fatalx("incorrect size of session request");
			memcpy(&sup, imsg.data, sizeof(sup));
			peer_up(imsg.hdr.peerid, &sup);
			break;
		case IMSG_SESSION_DOWN:
			peer_down(imsg.hdr.peerid);
			break;
		case IMSG_REFRESH:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(r)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&r, imsg.data, sizeof(r));
			peer_dump(imsg.hdr.peerid, r.afi, r.safi);
			break;
		case IMSG_NETWORK_ADD:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct network_config)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&netconf_s, imsg.data, sizeof(netconf_s));
			TAILQ_INIT(&netconf_s.attrset);
			session_set = &netconf_s.attrset;
			break;
		case IMSG_NETWORK_DONE:
			if (imsg.hdr.len != IMSG_HEADER_SIZE) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			session_set = NULL;
			switch (netconf_s.prefix.af) {
			case AF_INET:
				if (netconf_s.prefixlen > 32)
					goto badnet;
				network_add(&netconf_s, 0);
				break;
			case AF_INET6:
				if (netconf_s.prefixlen > 128)
					goto badnet;
				network_add(&netconf_s, 0);
				break;
			default:
badnet:
				log_warnx("rde_dispatch: bad network");
				break;
			}
			break;
		case IMSG_NETWORK_REMOVE:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct network_config)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&netconf_s, imsg.data, sizeof(netconf_s));
			TAILQ_INIT(&netconf_s.attrset);
			network_delete(&netconf_s, 0);
			break;
		case IMSG_NETWORK_FLUSH:
			if (imsg.hdr.len != IMSG_HEADER_SIZE) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			network_flush(0);
			break;
		case IMSG_FILTER_SET:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct filter_set)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			if (session_set == NULL) {
				log_warnx("rde_dispatch: "
				    "IMSG_FILTER_SET unexpected");
				break;
			}
			if ((s = malloc(sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			memcpy(s, imsg.data, sizeof(struct filter_set));
			TAILQ_INSERT_TAIL(session_set, s, entry);

			if (s->type == ACTION_SET_NEXTHOP) {
				nh = nexthop_get(&s->action.nexthop);
				nh->refcnt++;
			}
			break;
		case IMSG_CTL_SHOW_NETWORK:
			if (imsg.hdr.len != IMSG_HEADER_SIZE + sizeof(af)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			bzero(&req, sizeof(req));
			memcpy(&req.af, imsg.data, sizeof(af));
			rde_dump_ctx_new(&req, imsg.hdr.pid, imsg.hdr.type);
			break;
		case IMSG_CTL_SHOW_RIB:
			if (imsg.hdr.len != IMSG_HEADER_SIZE + sizeof(req)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&req, imsg.data, sizeof(req));
			rde_dump_ctx_new(&req, imsg.hdr.pid, imsg.hdr.type);
			break;
		case IMSG_CTL_SHOW_RIB_AS:
			if (imsg.hdr.len != IMSG_HEADER_SIZE + sizeof(req)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&req, imsg.data, sizeof(req));
			req.pid = imsg.hdr.pid;
			rde_dump_as(&req);
			imsg_compose(ibuf_se_ctl, IMSG_CTL_END, 0, req.pid, -1,
			    NULL, 0);
			break;
		case IMSG_CTL_SHOW_RIB_PREFIX:
			if (imsg.hdr.len != IMSG_HEADER_SIZE + sizeof(req)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&req, imsg.data, sizeof(req));
			req.pid = imsg.hdr.pid;
			rde_dump_prefix(&req);
			imsg_compose(ibuf_se_ctl, IMSG_CTL_END, 0, req.pid, -1,
			    NULL, 0);
			break;
		case IMSG_CTL_SHOW_RIB_COMMUNITY:
			if (imsg.hdr.len != IMSG_HEADER_SIZE + sizeof(req)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&req, imsg.data, sizeof(req));
			req.pid = imsg.hdr.pid;
			rde_dump_community(&req);
			imsg_compose(ibuf_se_ctl, IMSG_CTL_END, 0, req.pid, -1,
			    NULL, 0);
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
				p.stats.prefix_rcvd_update =
				    peer->prefix_rcvd_update;
				p.stats.prefix_rcvd_withdraw =
				    peer->prefix_rcvd_withdraw;
				p.stats.prefix_sent_update =
				    peer->prefix_sent_update;
				p.stats.prefix_sent_withdraw =
				    peer->prefix_sent_withdraw;
			}
			imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_NEIGHBOR, 0,
			    imsg.hdr.pid, -1, &p, sizeof(struct peer));
			break;
		case IMSG_CTL_END:
			imsg_compose(ibuf_se_ctl, IMSG_CTL_END, 0, imsg.hdr.pid,
			    -1, NULL, 0);
			break;
		case IMSG_CTL_SHOW_RIB_MEM:
			imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_RIB_MEM, 0,
			    imsg.hdr.pid, -1, &rdemem, sizeof(rdemem));
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
	struct imsg		 imsg;
	struct rde_peer		*peer;
	struct filter_rule	*r;
	struct filter_set	*s;
	struct mrt		*xmrt;
	struct nexthop		*nh;
	int			 n, reconf_in = 0, reconf_out = 0;

	if ((n = imsg_read(ibuf)) == -1)
		fatal("rde_dispatch_imsg_parent: imsg_read error");
	if (n == 0)	/* connection closed */
		fatalx("rde_dispatch_imsg_parent: pipe closed");

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("rde_dispatch_imsg_parent: imsg_read error");
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_RECONF_CONF:
			reloadtime = time(NULL);
			newrules = calloc(1, sizeof(struct filter_head));
			if (newrules == NULL)
				fatal(NULL);
			TAILQ_INIT(newrules);
			if ((nconf = malloc(sizeof(struct bgpd_config))) ==
			    NULL)
				fatal(NULL);
			memcpy(nconf, imsg.data, sizeof(struct bgpd_config));
			break;
		case IMSG_NETWORK_ADD:
			memcpy(&netconf_p, imsg.data, sizeof(netconf_p));
			TAILQ_INIT(&netconf_p.attrset);
			parent_set = &netconf_p.attrset;
			break;
		case IMSG_NETWORK_DONE:
			parent_set = NULL;
			network_add(&netconf_p, 1);
			break;
		case IMSG_NETWORK_REMOVE:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct network_config)) {
				log_warnx("rde_dispatch: wrong imsg len");
				break;
			}
			memcpy(&netconf_p, imsg.data, sizeof(netconf_p));
			TAILQ_INIT(&netconf_p.attrset);
			network_delete(&netconf_p, 1);
			break;
		case IMSG_RECONF_FILTER:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct filter_rule))
				fatalx("IMSG_RECONF_FILTER bad len");
			if ((r = malloc(sizeof(struct filter_rule))) == NULL)
				fatal(NULL);
			memcpy(r, imsg.data, sizeof(struct filter_rule));
			TAILQ_INIT(&r->set);
			parent_set = &r->set;
			TAILQ_INSERT_TAIL(newrules, r, entry);
			break;
		case IMSG_RECONF_DONE:
			if (nconf == NULL)
				fatalx("got IMSG_RECONF_DONE but no config");
			if ((nconf->flags & BGPD_FLAG_NO_EVALUATE)
			    != (conf->flags & BGPD_FLAG_NO_EVALUATE)) {
				log_warnx( "change to/from route-collector "
				    "mode ignored");
				if (conf->flags & BGPD_FLAG_NO_EVALUATE)
					nconf->flags |= BGPD_FLAG_NO_EVALUATE;
				else
					nconf->flags &= ~BGPD_FLAG_NO_EVALUATE;
			}
			memcpy(conf, nconf, sizeof(struct bgpd_config));
			free(nconf);
			nconf = NULL;
			parent_set = NULL;
			prefix_network_clean(&peerself, reloadtime);

			/* check if filter changed */
			LIST_FOREACH(peer, &peerlist, peer_l) {
				peer->reconf_out = 0;
				peer->reconf_in = 0;
				if (peer->conf.softreconfig_out &&
				    !rde_filter_equal(rules_l, newrules, peer,
				    DIR_OUT)) {
					peer->reconf_out = 1;
					reconf_out = 1;
				}
				if (peer->conf.softreconfig_in &&
				    !rde_filter_equal(rules_l, newrules, peer,
				    DIR_IN)) {
					peer->reconf_in = 1;
					reconf_in = 1;
				}
			}
			/* sync local-RIB first */
			if (reconf_in)
				pt_dump(rde_softreconfig_in, NULL, AF_UNSPEC);
			/* then sync peers */
			if (reconf_out)
				pt_dump(rde_softreconfig_out, NULL, AF_UNSPEC);

			while ((r = TAILQ_FIRST(rules_l)) != NULL) {
				TAILQ_REMOVE(rules_l, r, entry);
				filterset_free(&r->set);
				free(r);
			}
			free(rules_l);
			rules_l = newrules;
			log_info("RDE reconfigured");
			break;
		case IMSG_NEXTHOP_UPDATE:
			nexthop_update(imsg.data);
			break;
		case IMSG_FILTER_SET:
			if (parent_set == NULL) {
				log_warnx("rde_dispatch_imsg_parent: "
				    "IMSG_FILTER_SET unexpected");
				break;
			}
			if ((s = malloc(sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			memcpy(s, imsg.data, sizeof(struct filter_set));
			TAILQ_INSERT_TAIL(parent_set, s, entry);

			if (s->type == ACTION_SET_NEXTHOP) {
				nh = nexthop_get(&s->action.nexthop);
				nh->refcnt++;
			}
			break;
		case IMSG_MRT_OPEN:
		case IMSG_MRT_REOPEN:
			if (imsg.hdr.len > IMSG_HEADER_SIZE +
			    sizeof(struct mrt)) {
				log_warnx("wrong imsg len");
				break;
			}

			xmrt = calloc(1, sizeof(struct mrt));
			if (xmrt == NULL)
				fatal("rde_dispatch_imsg_parent");
			memcpy(xmrt, imsg.data, sizeof(struct mrt));
			TAILQ_INIT(&xmrt->bufs);

			if ((xmrt->fd = imsg_get_fd(ibuf)) == -1)
				log_warnx("expected to receive fd for mrt dump "
				    "but didn't receive any");

			if (xmrt->type == MRT_TABLE_DUMP) {
				/* do not dump if another is still running */
				if (mrt == NULL || mrt->queued == 0) {
					free(mrt);
					mrt = xmrt;
					mrt_clear_seq();
					pt_dump(mrt_dump_upcall, mrt,
					    AF_UNSPEC);
					break;
				}
			}
			close(xmrt->fd);
			free(xmrt);
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

/* handle routing updates from the session engine. */
int
rde_update_dispatch(struct imsg *imsg)
{
	struct rde_peer		*peer;
	struct rde_aspath	*asp = NULL, *fasp;
	u_char			*p, *mpp = NULL;
	int			 error = -1, pos = 0;
	u_int16_t		 afi, len, mplen;
	u_int16_t		 withdrawn_len;
	u_int16_t		 attrpath_len;
	u_int16_t		 nlri_len;
	u_int8_t		 prefixlen, safi, subtype;
	struct bgpd_addr	 prefix;
	struct mpattr		 mpa;

	peer = peer_get(imsg->hdr.peerid);
	if (peer == NULL)	/* unknown peer, cannot happen */
		return (-1);
	if (peer->state != PEER_UP)
		return (-1);	/* peer is not yet up, cannot happen */

	p = imsg->data;

	memcpy(&len, p, 2);
	withdrawn_len = ntohs(len);
	p += 2;
	if (imsg->hdr.len < IMSG_HEADER_SIZE + 2 + withdrawn_len + 2) {
		rde_update_err(peer, ERR_UPDATE, ERR_UPD_ATTRLIST, NULL, 0);
		return (-1);
	}

	p += withdrawn_len;
	memcpy(&len, p, 2);
	attrpath_len = len = ntohs(len);
	p += 2;
	if (imsg->hdr.len <
	    IMSG_HEADER_SIZE + 2 + withdrawn_len + 2 + attrpath_len) {
		rde_update_err(peer, ERR_UPDATE, ERR_UPD_ATTRLIST, NULL, 0);
		return (-1);
	}

	nlri_len =
	    imsg->hdr.len - IMSG_HEADER_SIZE - 4 - withdrawn_len - attrpath_len;
	bzero(&mpa, sizeof(mpa));

	if (attrpath_len != 0) { /* 0 = no NLRI information in this message */
		/* parse path attributes */
		asp = path_get();
		while (len > 0) {
			if ((pos = rde_attr_parse(p, len, peer, asp,
			    &mpa)) < 0)
				goto done;
			p += pos;
			len -= pos;
		}

		/* check for missing but necessary attributes */
		if ((subtype = rde_attr_missing(asp, peer->conf.ebgp,
		    nlri_len))) {
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_MISSNG_WK_ATTR,
			    &subtype, sizeof(u_int8_t));
			goto done;
		}

		/*
		 * if either ATTR_NEW_AGGREGATOR or ATTR_NEW_ASPATH is present
		 * try to fixup the attributes.
		 */
		if (asp->flags & F_ATTR_AS4BYTE_NEW)
			rde_as4byte_fixup(peer, asp);

		/* enforce remote AS if requested */
		if (asp->flags & F_ATTR_ASPATH &&
		    peer->conf.enforce_as == ENFORCE_AS_ON)
			if (peer->conf.remote_as !=
			    aspath_neighbor(asp->aspath)) {
				rde_update_err(peer, ERR_UPDATE, ERR_UPD_ASPATH,
				    NULL, 0);
				goto done;
			}

		if (rde_reflector(peer, asp) != 1) {
			error = 0;
			goto done;
		}
	}

	p = imsg->data;
	len = withdrawn_len;
	p += 2;
	/* withdraw prefix */
	while (len > 0) {
		if ((pos = rde_update_get_prefix(p, len, &prefix,
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
		if (prefixlen > 32) {
			log_peer_warnx(&peer->conf, "bad withdraw prefix");
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_NETWORK,
			    NULL, 0);
			goto done;
		}

		p += pos;
		len -= pos;

		if (peer->capa_received.mp_v4 == SAFI_NONE &&
		    peer->capa_received.mp_v6 != SAFI_NONE) {
			log_peer_warnx(&peer->conf, "bad AFI, IPv4 disabled");
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_OPTATTR,
			    NULL, 0);
			goto done;
		}

		peer->prefix_rcvd_withdraw++;
		rde_update_log("withdraw", peer, NULL, &prefix, prefixlen);
		prefix_remove(peer, &prefix, prefixlen, F_LOCAL);
		prefix_remove(peer, &prefix, prefixlen, F_ORIGINAL);
	}

	if (attrpath_len == 0) /* 0 = no NLRI information in this message */
		return (0);

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
		switch (afi) {
		case AFI_IPv6:
			if (peer->capa_received.mp_v6 == SAFI_NONE) {
				log_peer_warnx(&peer->conf, "bad AFI, "
				    "IPv6 disabled");
				rde_update_err(peer, ERR_UPDATE,
				    ERR_UPD_OPTATTR, NULL, 0);
				goto done;
			}
			while (mplen > 0) {
				if ((pos = rde_update_get_prefix6(mpp, mplen,
				    &prefix, &prefixlen)) == -1) {
					log_peer_warnx(&peer->conf,
					    "bad IPv6 withdraw prefix");
					rde_update_err(peer, ERR_UPDATE,
					    ERR_UPD_OPTATTR,
					    mpa.unreach, mpa.unreach_len);
					goto done;
				}
				if (prefixlen > 128) {
					log_peer_warnx(&peer->conf,
					    "bad IPv6 withdraw prefix");
					rde_update_err(peer, ERR_UPDATE,
					    ERR_UPD_OPTATTR,
					    mpa.unreach, mpa.unreach_len);
					goto done;
				}

				mpp += pos;
				mplen -= pos;

				peer->prefix_rcvd_withdraw++;
				rde_update_log("withdraw", peer, NULL,
				    &prefix, prefixlen);
				prefix_remove(peer, &prefix, prefixlen,
				    F_LOCAL);
				prefix_remove(peer, &prefix, prefixlen,
				    F_ORIGINAL);
			}
			break;
		default:
			/* silently ignore unsupported multiprotocol AF */
			break;
		}

		if ((asp->flags & ~F_ATTR_MP_UNREACH) == 0) {
			error = 0;
			goto done;
		}
	}

	/* shift to NLRI information */
	p += 2 + attrpath_len;

	/* aspath needs to be loop free nota bene this is not a hard error */
	if (peer->conf.ebgp && !aspath_loopfree(asp->aspath, conf->as)) {
		error = 0;
		goto done;
	}

	/* parse nlri prefix */
	while (nlri_len > 0) {
		if ((pos = rde_update_get_prefix(p, nlri_len, &prefix,
		    &prefixlen)) == -1) {
			log_peer_warnx(&peer->conf, "bad nlri prefix");
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_NETWORK,
			    NULL, 0);
			goto done;
		}
		if (prefixlen > 32) {
			log_peer_warnx(&peer->conf, "bad nlri prefix");
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_NETWORK,
			    NULL, 0);
			goto done;
		}

		p += pos;
		nlri_len -= pos;

		if (peer->capa_received.mp_v4 == SAFI_NONE &&
		    peer->capa_received.mp_v6 != SAFI_NONE) {
			log_peer_warnx(&peer->conf, "bad AFI, IPv4 disabled");
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_OPTATTR,
			    NULL, 0);
			goto done;
		}

		peer->prefix_rcvd_update++;
		/* add original path to the Adj-RIB-In */
		if (peer->conf.softreconfig_in)
			path_update(peer, asp, &prefix, prefixlen, F_ORIGINAL);

		/* input filter */
		if (rde_filter(&fasp, rules_l, peer, asp, &prefix, prefixlen,
		    peer, DIR_IN) == ACTION_DENY) {
			path_put(fasp);
			continue;
		}

		/* max prefix checker */
		if (peer->conf.max_prefix &&
		    peer->prefix_cnt >= peer->conf.max_prefix) {
			log_peer_warnx(&peer->conf, "prefix limit reached");
			rde_update_err(peer, ERR_CEASE, ERR_CEASE_MAX_PREFIX,
			    NULL, 0);
			path_put(fasp);
			goto done;
		}

		if (fasp == NULL)
			fasp = asp;

		rde_update_log("update", peer, &fasp->nexthop->exit_nexthop,
		    &prefix, prefixlen);
		path_update(peer, fasp, &prefix, prefixlen, F_LOCAL);

		/* free modified aspath */
		if (fasp != asp)
			path_put(fasp);
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

		/*
		 * this works because asp is not linked.
		 * But first unlock the previously locked nexthop.
		 */
		if (asp->nexthop) {
			asp->nexthop->refcnt--;
			(void)nexthop_delete(asp->nexthop);
			asp->nexthop = NULL;
		}
		if ((pos = rde_get_mp_nexthop(mpp, mplen, afi, asp)) == -1) {
			log_peer_warnx(&peer->conf, "bad IPv6 nlri prefix");
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_OPTATTR,
			    mpa.reach, mpa.reach_len);
			goto done;
		}
		mpp += pos;
		mplen -= pos;

		switch (afi) {
		case AFI_IPv6:
			if (peer->capa_received.mp_v6 == SAFI_NONE) {
				log_peer_warnx(&peer->conf, "bad AFI, "
				    "IPv6 disabled");
				rde_update_err(peer, ERR_UPDATE,
				    ERR_UPD_OPTATTR, NULL, 0);
				goto done;
			}

			while (mplen > 0) {
				if ((pos = rde_update_get_prefix6(mpp, mplen,
				    &prefix, &prefixlen)) == -1) {
					log_peer_warnx(&peer->conf,
					    "bad IPv6 nlri prefix");
					rde_update_err(peer, ERR_UPDATE,
					    ERR_UPD_OPTATTR,
					    mpa.reach, mpa.reach_len);
					goto done;
				}
				if (prefixlen > 128) {
					rde_update_err(peer, ERR_UPDATE,
					    ERR_UPD_OPTATTR,
					    mpa.reach, mpa.reach_len);
					goto done;
				}

				mpp += pos;
				mplen -= pos;

				peer->prefix_rcvd_update++;
				/* add original path to the Adj-RIB-In */
				if (peer->conf.softreconfig_in)
					path_update(peer, asp, &prefix,
					    prefixlen, F_ORIGINAL);

				/* input filter */
				if (rde_filter(&fasp, rules_l, peer, asp,
				    &prefix, prefixlen, peer, DIR_IN) ==
				    ACTION_DENY) {
					path_put(fasp);
					continue;
				}

				/* max prefix checker */
				if (peer->conf.max_prefix &&
				    peer->prefix_cnt >= peer->conf.max_prefix) {
					log_peer_warnx(&peer->conf,
					    "prefix limit reached");
					rde_update_err(peer, ERR_CEASE,
					    ERR_CEASE_MAX_PREFIX, NULL, 0);
					path_put(fasp);
					goto done;
				}

				if (fasp == NULL)
					fasp = asp;

				rde_update_log("update", peer,
				    &asp->nexthop->exit_nexthop,
				    &prefix, prefixlen);
				path_update(peer, fasp, &prefix, prefixlen,
				    F_LOCAL);

				/* free modified aspath */
				if (fasp != asp)
					path_put(fasp);
			}
			break;
		default:
			/* silently ignore unsupported multiprotocol AF */
			break;
		}
	}

done:
	if (attrpath_len != 0) {
		/* unlock the previously locked entry */
		if (asp->nexthop) {
			asp->nexthop->refcnt--;
			(void)nexthop_delete(asp->nexthop);
		}
		/* free allocated attribute memory that is no longer used */
		path_put(asp);
	}

	return (error);
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
	(((s) & ~(ATTR_EXTLEN | (m))) == (t))

int
rde_attr_parse(u_char *p, u_int16_t len, struct rde_peer *peer,
    struct rde_aspath *a, struct mpattr *mpa)
{
	struct bgpd_addr nexthop;
	u_char		*op = p, *npath;
	u_int32_t	 tmp32;
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
		rde_update_err(peer, ERR_UPDATE, ERR_UPD_UNSPECIFIC, NULL, 0);
		return (-1);
	case ATTR_ORIGIN:
		if (attr_len != 1)
			goto bad_len;

		if (!CHECK_FLAGS(flags, ATTR_WELL_KNOWN, 0)) {
bad_flags:
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_ATTRFLAGS,
			    op, attr_len);
			return (-1);
		}

		UPD_READ(&a->origin, p, plen, 1);
		if (a->origin > ORIGIN_INCOMPLETE) {
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_ORIGIN,
			    op, attr_len);
			return (-1);
		}
		if (a->flags & F_ATTR_ORIGIN)
			goto bad_list;
		a->flags |= F_ATTR_ORIGIN;
		break;
	case ATTR_ASPATH:
		if (!CHECK_FLAGS(flags, ATTR_WELL_KNOWN, 0))
			goto bad_flags;
		if (aspath_verify(p, attr_len, rde_as4byte(peer)) != 0) {
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_ASPATH,
			    NULL, 0);
			return (-1);
		}
		if (a->flags & F_ATTR_ASPATH)
			goto bad_list;
		if (rde_as4byte(peer)) {
			npath = p;
			nlen = attr_len;
		} else
			npath = aspath_inflate(p, attr_len, &nlen);
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
		nexthop.af = AF_INET;
		UPD_READ(&nexthop.v4.s_addr, p, plen, 4);
		/*
		 * Check if the nexthop is a valid IP address. We consider
		 * multicast and experimental addresses as invalid.
		 */
		tmp32 = ntohl(nexthop.v4.s_addr);
		if (IN_MULTICAST(tmp32) || IN_BADCLASS(tmp32)) {
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_NETWORK,
			    op, attr_len);
			return (-1);
		}
		a->nexthop = nexthop_get(&nexthop);
		/*
		 * lock the nexthop because it is not yet linked else
		 * withdraws may remove this nexthop which in turn would
		 * cause a use after free error.
		 */
		a->nexthop->refcnt++;
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
		    (rde_as4byte(peer) && attr_len != 8))
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL|ATTR_TRANSITIVE, 0))
			goto bad_flags;
		if (!rde_as4byte(peer)) {
			/* need to inflate aggregator AS to 4-byte */
			u_char	t[8];
			t[0] = t[1] = 0;
			UPD_READ(&t[2], p, plen, 2);
			UPD_READ(&t[4], p, plen, 4);
			if (attr_optadd(a, flags, type, t,
			    sizeof(t)) == -1)
				goto bad_list;
			break;
		}
		/* 4-byte ready server take the default route */
		goto optattr;
	case ATTR_COMMUNITIES:
		if ((attr_len & 0x3) != 0)
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL|ATTR_TRANSITIVE,
		    ATTR_PARTIAL))
			goto bad_flags;
		goto optattr;
	case ATTR_ORIGINATOR_ID:
		if (attr_len != 4)
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL, 0))
			goto bad_flags;
		goto optattr;
	case ATTR_CLUSTER_LIST:
		if ((attr_len & 0x3) != 0)
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
	case ATTR_NEW_AGGREGATOR:
		if (attr_len != 8)
			goto bad_len;
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL|ATTR_TRANSITIVE,
		    ATTR_PARTIAL))
			goto bad_flags;
		a->flags |= F_ATTR_AS4BYTE_NEW;
		goto optattr;
	case ATTR_NEW_ASPATH:
		if (!CHECK_FLAGS(flags, ATTR_OPTIONAL|ATTR_TRANSITIVE,
		    ATTR_PARTIAL))
			goto bad_flags;
		if (aspath_verify(p, attr_len, 1) != 0) {
			/* XXX draft does not specify how to handle errors */
			rde_update_err(peer, ERR_UPDATE, ERR_UPD_ASPATH,
			    NULL, 0);
			return (-1);
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
rde_get_mp_nexthop(u_char *data, u_int16_t len, u_int16_t afi,
    struct rde_aspath *asp)
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
	switch (afi) {
	case AFI_IPv6:
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
		nexthop.af = AF_INET6;
		memcpy(&nexthop.v6.s6_addr, data, 16);
		asp->nexthop = nexthop_get(&nexthop);
		/*
		 * lock the nexthop because it is not yet linked else
		 * withdraws may remove this nexthop which in turn would
		 * cause a use after free error.
		 */
		asp->nexthop->refcnt++;

		/* ignore reserved (old SNPA) field as per RFC 4760 */
		totlen += nhlen + 1;
		data += nhlen + 1;

		return (totlen);
	default:
		log_warnx("bad multiprotocol nexthop, bad AF");
		break;
	}

	return (-1);
}

int
rde_update_get_prefix(u_char *p, u_int16_t len, struct bgpd_addr *prefix,
    u_int8_t *prefixlen)
{
	int		i;
	u_int8_t	pfxlen;
	u_int16_t	plen;
	union {
		struct in_addr	a32;
		u_int8_t	a8[4];
	}		addr;

	if (len < 1)
		return (-1);

	memcpy(&pfxlen, p, 1);
	p += 1;
	plen = 1;

	bzero(prefix, sizeof(struct bgpd_addr));
	addr.a32.s_addr = 0;
	for (i = 0; i <= 3; i++) {
		if (pfxlen > i * 8) {
			if (len - plen < 1)
				return (-1);
			memcpy(&addr.a8[i], p++, 1);
			plen++;
		}
	}
	prefix->af = AF_INET;
	prefix->v4.s_addr = addr.a32.s_addr;
	*prefixlen = pfxlen;

	return (plen);
}

int
rde_update_get_prefix6(u_char *p, u_int16_t len, struct bgpd_addr *prefix,
    u_int8_t *prefixlen)
{
	int		i;
	u_int8_t	pfxlen;
	u_int16_t	plen;

	if (len < 1)
		return (-1);

	memcpy(&pfxlen, p, 1);
	p += 1;
	plen = 1;

	bzero(prefix, sizeof(struct bgpd_addr));
	for (i = 0; i <= 15; i++) {
		if (pfxlen > i * 8) {
			if (len - plen < 1)
				return (-1);
			memcpy(&prefix->v6.s6_addr[i], p++, 1);
			plen++;
		}
	}
	prefix->af = AF_INET6;
	*prefixlen = pfxlen;

	return (plen);
}

void
rde_update_err(struct rde_peer *peer, u_int8_t error, u_int8_t suberr,
    void *data, u_int16_t size)
{
	struct buf	*wbuf;

	if ((wbuf = imsg_create(ibuf_se, IMSG_UPDATE_ERR, peer->conf.id, 0,
	    size + sizeof(error) + sizeof(suberr))) == NULL)
		fatal("imsg_create error");
	if (imsg_add(wbuf, &error, sizeof(error)) == -1 ||
	    imsg_add(wbuf, &suberr, sizeof(suberr)) == -1 ||
	    imsg_add(wbuf, data, size) == -1)
		fatal("imsg_add error");
	if (imsg_close(ibuf_se, wbuf) == -1)
		fatal("imsg_close error");
	peer->state = PEER_ERR;
}

void
rde_update_log(const char *message,
    const struct rde_peer *peer, const struct bgpd_addr *next,
    const struct bgpd_addr *prefix, u_int8_t prefixlen)
{
	char		*l = NULL;
	char		*n = NULL;
	char		*p = NULL;

	if (!(conf->log & BGPD_LOG_UPDATES))
		return;

	if (next != NULL)
		if (asprintf(&n, " via %s", log_addr(next)) == -1)
			n = NULL;
	if (asprintf(&p, "%s/%u", log_addr(prefix), prefixlen) == -1)
		p = NULL;
	l = log_fmt_peer(&peer->conf);
	log_info("%s AS%s: %s %s%s",
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

	/* first get the attributes */
	nasp = attr_optget(a, ATTR_NEW_ASPATH);
	naggr = attr_optget(a, ATTR_NEW_AGGREGATOR);

	if (rde_as4byte(peer)) {
		/* NEW session using 4-byte ASNs */
		if (nasp)
			attr_free(a, nasp);
		if (naggr)
			attr_free(a, naggr);
		return;
	}
	/* OLD session using 2-byte ASNs */
	/* try to merge the new attributes into the old ones */
	if ((oaggr = attr_optget(a, ATTR_AGGREGATOR))) {
		memcpy(&as, oaggr->data, sizeof(as));
		if (ntohl(as) != AS_TRANS) {
			/* per RFC draft ignore NEW_ASPATH and NEW_AGGREGATOR */
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
	/* there is no need for NEW_AGGREGATOR any more */
	if (naggr)
		attr_free(a, naggr);

	/* merge NEW_ASPATH with ASPATH */
	if (nasp)
		aspath_merge(a, nasp);
}


/*
 * route reflector helper function
 */
int
rde_reflector(struct rde_peer *peer, struct rde_aspath *asp)
{
	struct attr	*a;
	u_int8_t	*p;
	u_int16_t	 len;
	u_int32_t	 id;

	/* check for originator id if eq router_id drop */
	if ((a = attr_optget(asp, ATTR_ORIGINATOR_ID)) != NULL) {
		if (memcmp(&conf->bgpid, a->data, sizeof(conf->bgpid)) == 0)
			/* this is coming from myself */
			return (0);
	} else if (conf->flags & BGPD_FLAG_REFLECTOR) {
		if (peer->conf.ebgp == 0)
			id = htonl(peer->remote_bgpid);
		else
			id = conf->bgpid;
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
				    sizeof(conf->clusterid)) == 0)
					return (0);

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
	return (1);
}

/*
 * control specific functions
 */
void
rde_dump_rib_as(struct prefix *p, struct rde_aspath *asp, pid_t pid, int flags)
{
	struct ctl_show_rib	 rib;
	struct buf		*wbuf;
	struct attr		*a;
	void			*bp;
	u_int8_t		 l;

	bzero(&rib, sizeof(rib));
	rib.lastchange = p->lastchange;
	rib.local_pref = asp->lpref;
	rib.med = asp->med;
	rib.prefix_cnt = asp->prefix_cnt;
	rib.active_cnt = asp->active_cnt;
	rib.adjrib_cnt = asp->adjrib_cnt;
	strlcpy(rib.descr, asp->peer->conf.descr, sizeof(rib.descr));
	memcpy(&rib.remote_addr, &asp->peer->remote_addr,
	    sizeof(rib.remote_addr));
	rib.remote_id = asp->peer->remote_bgpid;
	if (asp->nexthop != NULL) {
		memcpy(&rib.true_nexthop, &asp->nexthop->true_nexthop,
		    sizeof(rib.true_nexthop));
		memcpy(&rib.exit_nexthop, &asp->nexthop->exit_nexthop,
		    sizeof(rib.exit_nexthop));
	} else {
		/* announced network may have a NULL nexthop */
		bzero(&rib.true_nexthop, sizeof(rib.true_nexthop));
		bzero(&rib.exit_nexthop, sizeof(rib.exit_nexthop));
		rib.true_nexthop.af = p->prefix->af;
		rib.exit_nexthop.af = p->prefix->af;
	}
	pt_getaddr(p->prefix, &rib.prefix);
	rib.prefixlen = p->prefix->prefixlen;
	rib.origin = asp->origin;
	rib.flags = 0;
	if (p->prefix->active == p)
		rib.flags |= F_RIB_ACTIVE;
	if (asp->peer->conf.ebgp == 0)
		rib.flags |= F_RIB_INTERNAL;
	if (asp->flags & F_PREFIX_ANNOUNCED)
		rib.flags |= F_RIB_ANNOUNCE;
	if (asp->nexthop == NULL || asp->nexthop->state == NEXTHOP_REACH)
		rib.flags |= F_RIB_ELIGIBLE;
	rib.aspath_len = aspath_length(asp->aspath);

	if ((wbuf = imsg_create(ibuf_se_ctl, IMSG_CTL_SHOW_RIB, 0, pid,
	    sizeof(rib) + rib.aspath_len)) == NULL)
		return;
	if (imsg_add(wbuf, &rib, sizeof(rib)) == -1 ||
	    imsg_add(wbuf, aspath_dump(asp->aspath),
	    rib.aspath_len) == -1)
		return;
	if (imsg_close(ibuf_se_ctl, wbuf) == -1)
		return;

	if (flags & F_CTL_DETAIL)
		for (l = 0; l < asp->others_len; l++) {
			if ((a = asp->others[l]) == NULL)
				break;
			if ((wbuf = imsg_create(ibuf_se_ctl,
			    IMSG_CTL_SHOW_RIB_ATTR, 0, pid,
			    attr_optlen(a))) == NULL)
				return;
			if ((bp = buf_reserve(wbuf, attr_optlen(a))) == NULL) {
				buf_free(wbuf);
				return;
			}
			if (attr_write(bp, attr_optlen(a), a->flags,
			    a->type, a->data, a->len) == -1) {
				buf_free(wbuf);
				return;
			}
			if (imsg_close(ibuf_se_ctl, wbuf) == -1)
				return;
		}
}

void
rde_dump_filterout(struct rde_peer *peer, struct prefix *p,
    struct ctl_show_rib_request *req)
{
	struct bgpd_addr	 addr;
	struct rde_aspath	*asp;
	enum filter_actions	 a;

	if (up_test_update(peer, p) != 1)
		return;

	pt_getaddr(p->prefix, &addr);
	a = rde_filter(&asp, rules_l, peer, p->aspath, &addr,
	    p->prefix->prefixlen, p->aspath->peer, DIR_OUT);
	if (asp)
		asp->peer = p->aspath->peer;
	else
		asp = p->aspath;

	if (a == ACTION_ALLOW)
		rde_dump_rib_as(p, asp, req->pid, req->flags);

	if (asp != p->aspath)
		path_put(asp);
}

void
rde_dump_filter(struct prefix *p, struct ctl_show_rib_request *req)
{
	struct rde_peer		*peer;

	if ((req->flags & F_CTL_ADJ_IN && p->flags & F_ORIGINAL) ||
	    (!(req->flags & (F_CTL_ADJ_IN|F_CTL_ADJ_OUT)) &&
	    p->flags & F_LOCAL)) {
		if (req->peerid && req->peerid != p->aspath->peer->conf.id)
			return;
		rde_dump_rib_as(p, p->aspath, req->pid, req->flags);
	} else if (req->flags & F_CTL_ADJ_OUT && p->flags & F_LOCAL) {
		if (p->prefix->active != p)
			/* only consider active prefix */
			return;

		if (req->peerid) {
			if ((peer = peer_get(req->peerid)) != NULL)
				rde_dump_filterout(peer, p, req);
			return;
		}
		LIST_FOREACH(peer, &peerlist, peer_l)
			rde_dump_filterout(peer, p, req);
	}
}

void
rde_dump_upcall(struct pt_entry *pt, void *ptr)
{
	struct prefix		*p;
	struct ctl_show_rib_request	*req = ptr;

	LIST_FOREACH(p, &pt->prefix_h, prefix_l)
		rde_dump_filter(p, req);
}

void
rde_dump_as(struct ctl_show_rib_request *req)
{
	extern struct path_table	 pathtable;
	struct rde_aspath		*asp;
	struct prefix			*p;
	u_int32_t			 i;

	for (i = 0; i <= pathtable.path_hashmask; i++) {
		LIST_FOREACH(asp, &pathtable.path_hashtbl[i], path_l) {
			if (!aspath_match(asp->aspath, req->as.type,
			    req->as.as))
				continue;
			/* match found */
			LIST_FOREACH(p, &asp->prefix_h, path_l)
				rde_dump_filter(p, req);
		}
	}
}

void
rde_dump_prefix_upcall(struct pt_entry *pt, void *ptr)
{
	struct ctl_show_rib_request	*req = ptr;
	struct prefix			*p;
	struct bgpd_addr		 addr;

	pt_getaddr(pt, &addr);
	if (addr.af != req->prefix.af)
		return;
	if (req->prefixlen > pt->prefixlen)
		return;
	if (!prefix_compare(&req->prefix, &addr, req->prefixlen))
		LIST_FOREACH(p, &pt->prefix_h, prefix_l)
			rde_dump_filter(p, req);
}

void
rde_dump_prefix(struct ctl_show_rib_request *req)
{
	struct pt_entry	*pt;

	if (req->prefixlen == 32) {
		if ((pt = pt_lookup(&req->prefix)) != NULL)
			rde_dump_upcall(pt, req);
	} else if (req->flags & F_LONGER) {
		pt_dump(rde_dump_prefix_upcall, req, req->prefix.af);
	} else {
		if ((pt = pt_get(&req->prefix, req->prefixlen)) != NULL)
			rde_dump_upcall(pt, req);
	}
}

void
rde_dump_community(struct ctl_show_rib_request *req)
{
	extern struct path_table	 pathtable;
	struct rde_aspath		*asp;
	struct prefix			*p;
	u_int32_t			 i;

	for (i = 0; i <= pathtable.path_hashmask; i++) {
		LIST_FOREACH(asp, &pathtable.path_hashtbl[i], path_l) {
			if (!rde_filter_community(asp, req->community.as,
			    req->community.type))
				continue;
			/* match found */
			LIST_FOREACH(p, &asp->prefix_h, path_l)
				rde_dump_filter(p, req);
		}
	}
}

void
rde_dump_ctx_new(struct ctl_show_rib_request *req, pid_t pid,
    enum imsg_type type)
{
	struct rde_dump_ctx	*ctx;
	u_int			 error;

	if ((ctx = calloc(1, sizeof(*ctx))) == NULL) {
		log_warn("rde_dump_ctx_new");
		error = CTL_RES_NOMEM;
		imsg_compose(ibuf_se_ctl, IMSG_CTL_RESULT, 0, pid, -1, &error,
		    sizeof(error));
		return;
	}
	memcpy(&ctx->req, req, sizeof(struct ctl_show_rib_request));
	ctx->req.pid = pid;
	ctx->req.type = type;
	ctx->ptc.count = RDE_RUNNER_ROUNDS;
	ctx->af = ctx->req.af;
	if (ctx->af == AF_UNSPEC)
		ctx->af = AF_INET;

	TAILQ_INSERT_TAIL(&rde_dump_h, ctx, entry);
}

void
rde_dump_runner(void)
{
	struct rde_dump_ctx	*ctx, *next;

	for (ctx = TAILQ_FIRST(&rde_dump_h); ctx != NULL; ctx = next) {
		next = TAILQ_NEXT(ctx, entry);
		if (ctx->ptc.done) {
			imsg_compose(ibuf_se_ctl, IMSG_CTL_END, 0, ctx->req.pid,
			    -1, NULL, 0);
			TAILQ_REMOVE(&rde_dump_h, ctx, entry);
			free(ctx);
			continue;
		}
		switch (ctx->req.type) {
		case IMSG_CTL_SHOW_NETWORK:
			pt_dump_r(network_dump_upcall, &ctx->req.pid,
			    ctx->af, &ctx->ptc);
			break;
		case IMSG_CTL_SHOW_RIB:
			pt_dump_r(rde_dump_upcall, &ctx->req, ctx->af,
			    &ctx->ptc);
			break;
		default:
			fatalx("rde_dump_runner: unsupported imsg type");
		}
		if (ctx->ptc.done && ctx->req.af == AF_UNSPEC)
			ctx->af = AF_INET6;
	}
}

int
rde_dump_pending(void)
{
	return (!TAILQ_EMPTY(&rde_dump_h));
}

/*
 * kroute specific functions
 */
void
rde_send_kroute(struct prefix *new, struct prefix *old)
{
	struct kroute_label	 kl;
	struct kroute6_label	 kl6;
	struct bgpd_addr	 addr;
	struct prefix		*p;
	enum imsg_type		 type;

	/*
	 * Make sure that self announce prefixes are not commited to the
	 * FIB. If both prefixes are unreachable no update is needed.
	 */
	if ((old == NULL || old->aspath->flags & F_PREFIX_ANNOUNCED) &&
	    (new == NULL || new->aspath->flags & F_PREFIX_ANNOUNCED))
		return;

	if (new == NULL || new->aspath->flags & F_PREFIX_ANNOUNCED) {
		type = IMSG_KROUTE_DELETE;
		p = old;
	} else {
		type = IMSG_KROUTE_CHANGE;
		p = new;
	}

	pt_getaddr(p->prefix, &addr);
	switch (addr.af) {
	case AF_INET:
		bzero(&kl, sizeof(kl));
		kl.kr.prefix.s_addr = addr.v4.s_addr;
		kl.kr.prefixlen = p->prefix->prefixlen;
		if (p->aspath->flags & F_NEXTHOP_REJECT)
			kl.kr.flags |= F_REJECT;
		if (p->aspath->flags & F_NEXTHOP_BLACKHOLE)
			kl.kr.flags |= F_BLACKHOLE;
		if (type == IMSG_KROUTE_CHANGE)
			kl.kr.nexthop.s_addr =
			    p->aspath->nexthop->true_nexthop.v4.s_addr;
		strlcpy(kl.label, rtlabel_id2name(p->aspath->rtlabelid),
		    sizeof(kl.label));
		if (imsg_compose(ibuf_main, type, 0, 0, -1, &kl,
		    sizeof(kl)) == -1)
			fatal("imsg_compose error");
		break;
	case AF_INET6:
		bzero(&kl6, sizeof(kl6));
		memcpy(&kl6.kr.prefix, &addr.v6, sizeof(struct in6_addr));
		kl6.kr.prefixlen = p->prefix->prefixlen;
		if (p->aspath->flags & F_NEXTHOP_REJECT)
			kl6.kr.flags |= F_REJECT;
		if (p->aspath->flags & F_NEXTHOP_BLACKHOLE)
			kl6.kr.flags |= F_BLACKHOLE;
		if (type == IMSG_KROUTE_CHANGE) {
			type = IMSG_KROUTE6_CHANGE;
			memcpy(&kl6.kr.nexthop,
			    &p->aspath->nexthop->true_nexthop.v6,
			    sizeof(struct in6_addr));
		} else
			type = IMSG_KROUTE6_DELETE;
		strlcpy(kl6.label, rtlabel_id2name(p->aspath->rtlabelid),
		    sizeof(kl6.label));
		if (imsg_compose(ibuf_main, type, 0, 0, -1, &kl6,
		    sizeof(kl6)) == -1)
			fatal("imsg_compose error");
		break;
	}
}

/*
 * pf table specific functions
 */
void
rde_send_pftable(u_int16_t id, struct bgpd_addr *addr,
    u_int8_t len, int del)
{
	struct pftable_msg pfm;

	if (id == 0)
		return;

	/* do not run while cleaning up */
	if (rde_quit)
		return;

	bzero(&pfm, sizeof(pfm));
	strlcpy(pfm.pftable, pftable_id2name(id), sizeof(pfm.pftable));
	memcpy(&pfm.addr, addr, sizeof(pfm.addr));
	pfm.len = len;

	if (imsg_compose(ibuf_main,
	    del ? IMSG_PFTABLE_REMOVE : IMSG_PFTABLE_ADD,
	    0, 0, -1, &pfm, sizeof(pfm)) == -1)
		fatal("imsg_compose error");
}

void
rde_send_pftable_commit(void)
{
	/* do not run while cleaning up */
	if (rde_quit)
		return;

	if (imsg_compose(ibuf_main, IMSG_PFTABLE_COMMIT, 0, 0, -1, NULL, 0) ==
	    -1)
		fatal("imsg_compose error");
}

/*
 * nexthop specific functions
 */
void
rde_send_nexthop(struct bgpd_addr *next, int valid)
{
	size_t			 size;
	int			 type;

	if (valid)
		type = IMSG_NEXTHOP_ADD;
	else
		type = IMSG_NEXTHOP_REMOVE;

	size = sizeof(struct bgpd_addr);

	if (imsg_compose(ibuf_main, type, 0, 0, -1, next,
	    sizeof(struct bgpd_addr)) == -1)
		fatal("imsg_compose error");
}

/*
 * soft reconfig specific functions
 */
void
rde_softreconfig_out(struct pt_entry *pt, void *ptr)
{
	struct prefix		*p = pt->active;
	struct rde_peer		*peer;
	struct rde_aspath	*oasp, *nasp;
	enum filter_actions	 oa, na;
	struct bgpd_addr	 addr;

	if (p == NULL)
		return;

	pt_getaddr(pt, &addr);
	LIST_FOREACH(peer, &peerlist, peer_l) {
		if (peer->reconf_out == 0)
			continue;
		if (up_test_update(peer, p) != 1)
			continue;

		oa = rde_filter(&oasp, rules_l, peer, p->aspath, &addr,
		    pt->prefixlen, p->aspath->peer, DIR_OUT);
		na = rde_filter(&nasp, newrules, peer, p->aspath, &addr,
		    pt->prefixlen, p->aspath->peer, DIR_OUT);
		oasp = oasp != NULL ? oasp : p->aspath;
		nasp = nasp != NULL ? nasp : p->aspath;

		if (oa == ACTION_DENY && na == ACTION_DENY)
			/* nothing todo */
			goto done;
		if (oa == ACTION_DENY && na == ACTION_ALLOW) {
			/* send update */
			up_generate(peer, nasp, &addr, pt->prefixlen);
			goto done;
		}
		if (oa == ACTION_ALLOW && na == ACTION_DENY) {
			/* send withdraw */
			up_generate(peer, NULL, &addr, pt->prefixlen);
			goto done;
		}
		if (oa == ACTION_ALLOW && na == ACTION_ALLOW) {
			if (path_compare(nasp, oasp) == 0)
				goto done;
			/* send update */
			up_generate(peer, nasp, &addr, pt->prefixlen);
		}

done:
		if (oasp != p->aspath)
			path_put(oasp);
		if (nasp != p->aspath)
			path_put(nasp);
	}
}

void
rde_softreconfig_in(struct pt_entry *pt, void *ptr)
{
	struct prefix		*p, *np;
	struct rde_peer		*peer;
	struct rde_aspath	*asp, *oasp, *nasp;
	enum filter_actions	 oa, na;
	struct bgpd_addr	 addr;

	pt_getaddr(pt, &addr);
	for (p = LIST_FIRST(&pt->prefix_h); p != NULL; p = np) {
		np = LIST_NEXT(p, prefix_l);
		if (!(p->flags & F_ORIGINAL))
			continue;

		/* store aspath as prefix may change till we're done */
		asp = p->aspath;
		peer = asp->peer;

		if (peer->reconf_in == 0)
			continue;

		/* check if prefix changed */
		oa = rde_filter(&oasp, rules_l, peer, asp, &addr,
		    pt->prefixlen, peer, DIR_IN);
		na = rde_filter(&nasp, newrules, peer, asp, &addr,
		    pt->prefixlen, peer, DIR_IN);
		oasp = oasp != NULL ? oasp : asp;
		nasp = nasp != NULL ? nasp : asp;

		if (oa == ACTION_DENY && na == ACTION_DENY)
			/* nothing todo */
			goto done;
		if (oa == ACTION_DENY && na == ACTION_ALLOW) {
			/* update Local-RIB */
			path_update(peer, nasp, &addr, pt->prefixlen, F_LOCAL);
			goto done;
		}
		if (oa == ACTION_ALLOW && na == ACTION_DENY) {
			/* remove from Local-RIB */
			prefix_remove(peer, &addr, pt->prefixlen, F_LOCAL);
			goto done;
		}
		if (oa == ACTION_ALLOW && na == ACTION_ALLOW) {
			if (path_compare(nasp, oasp) == 0)
				goto done;
			/* send update */
			path_update(peer, nasp, &addr, pt->prefixlen, F_LOCAL);
		}

done:
		if (oasp != asp)
			path_put(oasp);
		if (nasp != asp)
			path_put(nasp);
	}
}

/*
 * update specific functions
 */
u_char	queue_buf[4096];

void
rde_up_dump_upcall(struct pt_entry *pt, void *ptr)
{
	struct rde_peer		*peer = ptr;

	if (pt->active == NULL)
		return;
	up_generate_updates(rules_l, peer, pt->active, NULL);
}

void
rde_generate_updates(struct prefix *new, struct prefix *old)
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
		if (peer->state != PEER_UP)
			continue;
		up_generate_updates(rules_l, peer, new, old);
	}
}

void
rde_update_queue_runner(void)
{
	struct rde_peer		*peer;
	int			 r, sent, max = RDE_RUNNER_ROUNDS;
	u_int16_t		 len, wd_len, wpos;

	len = sizeof(queue_buf) - MSGSIZE_HEADER;
	do {
		sent = 0;
		LIST_FOREACH(peer, &peerlist, peer_l) {
			if (peer->state != PEER_UP)
				continue;
			/* first withdraws */
			wpos = 2; /* reserve space for the length field */
			r = up_dump_prefix(queue_buf + wpos, len - wpos - 2,
			    &peer->withdraws, peer);
			wd_len = r;
			/* write withdraws length filed */
			wd_len = htons(wd_len);
			memcpy(queue_buf, &wd_len, 2);
			wpos += r;

			/* now bgp path attributes */
			r = up_dump_attrnlri(queue_buf + wpos, len - wpos,
			    peer);
			wpos += r;

			if (wpos == 4)
				/*
				 * No packet to send. The 4 bytes are the
				 * needed withdraw and path attribute length.
				 */
				continue;

			/* finally send message to SE */
			if (imsg_compose(ibuf_se, IMSG_UPDATE, peer->conf.id,
			    0, -1, queue_buf, wpos) == -1)
				fatal("imsg_compose error");
			sent++;
		}
		max -= sent;
	} while (sent != 0 && max > 0);
}

void
rde_update6_queue_runner(void)
{
	struct rde_peer		*peer;
	u_char			*b;
	int			 sent, max = RDE_RUNNER_ROUNDS / 2;
	u_int16_t		 len;

	/* first withdraws ... */
	do {
		sent = 0;
		LIST_FOREACH(peer, &peerlist, peer_l) {
			if (peer->state != PEER_UP)
				continue;
			len = sizeof(queue_buf) - MSGSIZE_HEADER;
			b = up_dump_mp_unreach(queue_buf, &len, peer);

			if (b == NULL)
				continue;
			/* finally send message to SE */
			if (imsg_compose(ibuf_se, IMSG_UPDATE, peer->conf.id,
			    0, -1, b, len) == -1)
				fatal("imsg_compose error");
			sent++;
		}
		max -= sent;
	} while (sent != 0 && max > 0);

	/* ... then updates */
	max = RDE_RUNNER_ROUNDS / 2;
	do {
		sent = 0;
		LIST_FOREACH(peer, &peerlist, peer_l) {
			if (peer->state != PEER_UP)
				continue;
			len = sizeof(queue_buf) - MSGSIZE_HEADER;
			b = up_dump_mp_reach(queue_buf, &len, peer);

			if (b == NULL)
				continue;
			/* finally send message to SE */
			if (imsg_compose(ibuf_se, IMSG_UPDATE, peer->conf.id,
			    0, -1, b, len) == -1)
				fatal("imsg_compose error");
			sent++;
		}
		max -= sent;
	} while (sent != 0 && max > 0);
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
rde_noevaluate(void)
{
	/* do not run while cleaning up */
	if (rde_quit)
		return (1);

	return (conf->flags & BGPD_FLAG_NO_EVALUATE);
}

int
rde_decisionflags(void)
{
	return (conf->flags & BGPD_FLAG_DECISION_MASK);
}

int
rde_as4byte(struct rde_peer *peer)
{
	return (peer->capa_announced.as4byte && peer->capa_received.as4byte);
}

/*
 * peer functions
 */
struct peer_table {
	struct rde_peer_head	*peer_hashtbl;
	u_int32_t		 peer_hashmask;
} peertable;

#define PEER_HASH(x)		\
	&peertable.peer_hashtbl[(x) & peertable.peer_hashmask]

void
peer_init(u_int32_t hashsize)
{
	u_int32_t	 hs, i;

	for (hs = 1; hs < hashsize; hs <<= 1)
		;
	peertable.peer_hashtbl = calloc(hs, sizeof(struct rde_peer_head));
	if (peertable.peer_hashtbl == NULL)
		fatal("peer_init");

	for (i = 0; i < hs; i++)
		LIST_INIT(&peertable.peer_hashtbl[i]);
	LIST_INIT(&peerlist);

	peertable.peer_hashmask = hs - 1;
}

void
peer_shutdown(void)
{
	u_int32_t	i;

	for (i = 0; i <= peertable.peer_hashmask; i++)
		if (!LIST_EMPTY(&peertable.peer_hashtbl[i]))
			log_warnx("peer_free: free non-free table");

	free(peertable.peer_hashtbl);
}

struct rde_peer *
peer_get(u_int32_t id)
{
	struct rde_peer_head	*head;
	struct rde_peer		*peer;

	head = PEER_HASH(id);

	LIST_FOREACH(peer, head, hash_l) {
		if (peer->conf.id == id)
			return (peer);
	}
	return (NULL);
}

struct rde_peer *
peer_add(u_int32_t id, struct peer_config *p_conf)
{
	struct rde_peer_head	*head;
	struct rde_peer		*peer;

	if (peer_get(id))
		return (NULL);

	peer = calloc(1, sizeof(struct rde_peer));
	if (peer == NULL)
		fatal("peer_add");

	LIST_INIT(&peer->path_h);
	memcpy(&peer->conf, p_conf, sizeof(struct peer_config));
	peer->remote_bgpid = 0;
	peer->state = PEER_NONE;
	up_init(peer);

	head = PEER_HASH(id);

	LIST_INSERT_HEAD(head, peer, hash_l);
	LIST_INSERT_HEAD(&peerlist, peer, peer_l);

	return (peer);
}

void
peer_localaddrs(struct rde_peer *peer, struct bgpd_addr *laddr)
{
	struct ifaddrs	*ifap, *ifa, *match;

	if (getifaddrs(&ifap) == -1)
		fatal("getifaddrs");

	for (match = ifap; match != NULL; match = match->ifa_next)
		if (sa_cmp(laddr, match->ifa_addr) == 0)
			break;

	if (match == NULL)
		fatalx("peer_localaddrs: local address not found");

	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr->sa_family == AF_INET &&
		    strcmp(ifa->ifa_name, match->ifa_name) == 0) {
			if (ifa->ifa_addr->sa_family ==
			    match->ifa_addr->sa_family)
				ifa = match;
			peer->local_v4_addr.af = AF_INET;
			peer->local_v4_addr.v4.s_addr =
			    ((struct sockaddr_in *)ifa->ifa_addr)->
			    sin_addr.s_addr;
			break;
		}
	}

	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr->sa_family == AF_INET6 &&
		    strcmp(ifa->ifa_name, match->ifa_name) == 0) {
			/*
			 * only accept global scope addresses except explicitly
			 * specified.
			 */
			if (ifa->ifa_addr->sa_family ==
			    match->ifa_addr->sa_family)
				ifa = match;
			else if (IN6_IS_ADDR_LINKLOCAL(
			    &((struct sockaddr_in6 *)ifa->
			    ifa_addr)->sin6_addr) ||
			    IN6_IS_ADDR_SITELOCAL(
			    &((struct sockaddr_in6 *)ifa->
			    ifa_addr)->sin6_addr))
				continue;
			peer->local_v6_addr.af = AF_INET6;
			memcpy(&peer->local_v6_addr.v6,
			    &((struct sockaddr_in6 *)ifa->ifa_addr)->
			    sin6_addr, sizeof(struct in6_addr));
			peer->local_v6_addr.scope_id =
			    ((struct sockaddr_in6 *)ifa->ifa_addr)->
			    sin6_scope_id;
			break;
		}
	}

	freeifaddrs(ifap);
}

void
peer_up(u_int32_t id, struct session_up *sup)
{
	struct rde_peer	*peer;

	peer = peer_get(id);
	if (peer == NULL) {
		log_warnx("peer_up: peer id %d already exists", id);
		return;
	}

	if (peer->state != PEER_DOWN && peer->state != PEER_NONE)
		fatalx("peer_up: bad state");
	peer->remote_bgpid = ntohl(sup->remote_bgpid);
	peer->short_as = sup->short_as;
	memcpy(&peer->remote_addr, &sup->remote_addr,
	    sizeof(peer->remote_addr));
	memcpy(&peer->capa_announced, &sup->capa_announced,
	    sizeof(peer->capa_announced));
	memcpy(&peer->capa_received, &sup->capa_received,
	    sizeof(peer->capa_received));

	peer_localaddrs(peer, &sup->local_addr);

	peer->state = PEER_UP;
	up_init(peer);

	if (rde_noevaluate())
		/*
		 * no need to dump the table to the peer, there are no active
		 * prefixes anyway. This is a speed up hack.
		 */
		return;

	peer_dump(id, AFI_ALL, SAFI_ALL);
}

void
peer_down(u_int32_t id)
{
	struct rde_peer		*peer;
	struct rde_aspath	*asp, *nasp;

	peer = peer_get(id);
	if (peer == NULL) {
		log_warnx("peer_down: unknown peer id %d", id);
		return;
	}
	peer->remote_bgpid = 0;
	peer->state = PEER_DOWN;
	up_down(peer);

	/* walk through per peer RIB list and remove all prefixes. */
	for (asp = LIST_FIRST(&peer->path_h); asp != NULL; asp = nasp) {
		nasp = LIST_NEXT(asp, peer_l);
		path_remove(asp);
	}
	LIST_INIT(&peer->path_h);

	/* Deletions are performed in path_remove() */
	rde_send_pftable_commit();

	LIST_REMOVE(peer, hash_l);
	LIST_REMOVE(peer, peer_l);
	free(peer);
}

void
peer_dump(u_int32_t id, u_int16_t afi, u_int8_t safi)
{
	struct rde_peer		*peer;

	peer = peer_get(id);
	if (peer == NULL) {
		log_warnx("peer_down: unknown peer id %d", id);
		return;
	}

	if (afi == AFI_ALL || afi == AFI_IPv4)
		if (safi == SAFI_ALL || safi == SAFI_UNICAST) {
			if (peer->conf.announce_type ==
			    ANNOUNCE_DEFAULT_ROUTE)
				up_generate_default(rules_l, peer, AF_INET);
			else
				pt_dump(rde_up_dump_upcall, peer, AF_INET);
		}
	if (afi == AFI_ALL || afi == AFI_IPv6)
		if (safi == SAFI_ALL || safi == SAFI_UNICAST) {
			if (peer->conf.announce_type ==
			    ANNOUNCE_DEFAULT_ROUTE)
				up_generate_default(rules_l, peer, AF_INET6);
			else
				pt_dump(rde_up_dump_upcall, peer, AF_INET6);
		}

	if (peer->capa_received.restart && peer->capa_announced.restart)
		peer_send_eor(peer, afi, safi);
}

/* End-of-RIB marker, draft-ietf-idr-restart-13.txt */
void
peer_send_eor(struct rde_peer *peer, u_int16_t afi, u_int16_t safi)
{
	if (afi == AFI_IPv4 && safi == SAFI_UNICAST) {
		u_char null[4];

		bzero(&null, 4);
		if (imsg_compose(ibuf_se, IMSG_UPDATE, peer->conf.id,
		    0, -1, &null, 4) == -1)
			fatal("imsg_compose error in peer_send_eor");
	} else {
		u_int16_t	i;
		u_char		buf[10];

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
			fatal("imsg_compose error in peer_send_eor");
	}
}

/*
 * network announcement stuff
 */
void
network_init(struct network_head *net_l)
{
	struct network	*n;
	struct in_addr   id;

	reloadtime = time(NULL);
	bzero(&peerself, sizeof(peerself));
	peerself.state = PEER_UP;
	peerself.remote_bgpid = ntohl(conf->bgpid);
	id.s_addr = conf->bgpid;
	peerself.conf.remote_as = conf->as;
	peerself.short_as = conf->short_as;
	snprintf(peerself.conf.descr, sizeof(peerself.conf.descr),
	    "LOCAL: ID %s", inet_ntoa(id));
	bzero(&peerdynamic, sizeof(peerdynamic));
	peerdynamic.state = PEER_UP;
	peerdynamic.remote_bgpid = ntohl(conf->bgpid);
	peerdynamic.conf.remote_as = conf->as;
	peerdynamic.short_as = conf->short_as;
	snprintf(peerdynamic.conf.descr, sizeof(peerdynamic.conf.descr),
	    "LOCAL: ID %s", inet_ntoa(id));

	while ((n = TAILQ_FIRST(net_l)) != NULL) {
		TAILQ_REMOVE(net_l, n, entry);
		network_add(&n->net, 1);
		free(n);
	}
}

void
network_add(struct network_config *nc, int flagstatic)
{
	struct rde_aspath	*asp;
	struct rde_peer		*p;

	asp = path_get();
	asp->aspath = aspath_get(NULL, 0);
	asp->origin = ORIGIN_IGP;
	asp->flags = F_ATTR_ORIGIN | F_ATTR_ASPATH |
	    F_ATTR_LOCALPREF | F_PREFIX_ANNOUNCED;
	/* the nexthop is unset unless a default set overrides it */

	if (flagstatic)
		p = &peerself;
	else
		p = &peerdynamic;

	rde_apply_set(asp, &nc->attrset, nc->prefix.af, p, p);
	path_update(p, asp, &nc->prefix, nc->prefixlen, F_ORIGINAL);
	path_update(p, asp, &nc->prefix, nc->prefixlen, F_LOCAL);

	path_put(asp);
	filterset_free(&nc->attrset);
}

void
network_delete(struct network_config *nc, int flagstatic)
{
	struct rde_peer	*p;

	if (flagstatic)
		p = &peerself;
	else
		p = &peerdynamic;

	prefix_remove(p, &nc->prefix, nc->prefixlen, F_LOCAL);
	prefix_remove(p, &nc->prefix, nc->prefixlen, F_ORIGINAL);
}

void
network_dump_upcall(struct pt_entry *pt, void *ptr)
{
	struct prefix		*p;
	struct kroute		 k;
	struct kroute6		 k6;
	struct bgpd_addr	 addr;
	pid_t			 pid;

	memcpy(&pid, ptr, sizeof(pid));

	LIST_FOREACH(p, &pt->prefix_h, prefix_l) {
		if (!(p->aspath->flags & F_PREFIX_ANNOUNCED))
			continue;
		if (p->prefix->af == AF_INET) {
			bzero(&k, sizeof(k));
			pt_getaddr(p->prefix, &addr);
			k.prefix.s_addr = addr.v4.s_addr;
			k.prefixlen = p->prefix->prefixlen;
			if (p->aspath->peer == &peerself)
				k.flags = F_KERNEL;
			if (imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_NETWORK, 0,
			    pid, -1, &k, sizeof(k)) == -1)
				log_warnx("network_dump_upcall: "
				    "imsg_compose error");
		}
		if (p->prefix->af == AF_INET6) {
			bzero(&k6, sizeof(k6));
			pt_getaddr(p->prefix, &addr);
			memcpy(&k6.prefix, &addr.v6, sizeof(k6.prefix));
			k6.prefixlen = p->prefix->prefixlen;
			if (p->aspath->peer == &peerself)
				k6.flags = F_KERNEL;
			if (imsg_compose(ibuf_se_ctl, IMSG_CTL_SHOW_NETWORK6, 0,
			    pid, -1, &k6, sizeof(k6)) == -1)
				log_warnx("network_dump_upcall: "
				    "imsg_compose error");
		}
	}
}

void
network_flush(int flagstatic)
{
	if (flagstatic)
		prefix_network_clean(&peerself, time(NULL));
	else
		prefix_network_clean(&peerdynamic, time(NULL));
}

/* clean up */
void
rde_shutdown(void)
{
	struct rde_peer		*p;
	struct rde_aspath	*asp, *nasp;
	struct filter_rule	*r;
	u_int32_t		 i;

	/*
	 * the decision process is turned off if rde_quit = 1 and
	 * rde_shutdown depends on this.
	 */

	/*
	 * All peers go down
	 */
	for (i = 0; i <= peertable.peer_hashmask; i++)
		while ((p = LIST_FIRST(&peertable.peer_hashtbl[i])) != NULL)
			peer_down(p->conf.id);

	/* free announced network prefixes */
	peerself.remote_bgpid = 0;
	peerself.state = PEER_DOWN;
	for (asp = LIST_FIRST(&peerself.path_h); asp != NULL; asp = nasp) {
		nasp = LIST_NEXT(asp, peer_l);
		path_remove(asp);
	}

	peerdynamic.remote_bgpid = 0;
	peerdynamic.state = PEER_DOWN;
	for (asp = LIST_FIRST(&peerdynamic.path_h); asp != NULL; asp = nasp) {
		nasp = LIST_NEXT(asp, peer_l);
		path_remove(asp);
	}

	/* free filters */
	while ((r = TAILQ_FIRST(rules_l)) != NULL) {
		TAILQ_REMOVE(rules_l, r, entry);
		filterset_free(&r->set);
		free(r);
	}
	free(rules_l);

	nexthop_shutdown();
	path_shutdown();
	aspath_shutdown();
	attr_shutdown();
	pt_shutdown();
	peer_shutdown();
	free(mrt);
}

int
sa_cmp(struct bgpd_addr *a, struct sockaddr *b)
{
	struct sockaddr_in	*in_b;
	struct sockaddr_in6	*in6_b;

	if (a->af != b->sa_family)
		return (1);

	switch (a->af) {
	case AF_INET:
		in_b = (struct sockaddr_in *)b;
		if (a->v4.s_addr != in_b->sin_addr.s_addr)
			return (1);
		break;
	case AF_INET6:
		in6_b = (struct sockaddr_in6 *)b;
#ifdef __KAME__
		/* directly stolen from sbin/ifconfig/ifconfig.c */
		if (IN6_IS_ADDR_LINKLOCAL(&in6_b->sin6_addr)) {
			in6_b->sin6_scope_id =
			    ntohs(*(u_int16_t *)&in6_b->sin6_addr.s6_addr[2]);
			in6_b->sin6_addr.s6_addr[2] =
			    in6_b->sin6_addr.s6_addr[3] = 0;
		}
#endif
		if (bcmp(&a->v6, &in6_b->sin6_addr,
		    sizeof(struct in6_addr)))
			return (1);
		break;
	default:
		fatal("king bula sez: unknown address family");
		/* NOTREACHED */
	}

	return (0);
}

