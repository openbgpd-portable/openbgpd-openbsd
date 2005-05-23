/*	$OpenBSD: bgpd.c,v 1.118 2005/05/23 22:48:53 henning Exp $ */

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
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mrt.h"
#include "bgpd.h"
#include "session.h"

void		sighdlr(int);
__dead void	usage(void);
int		main(int, char *[]);
int		check_child(pid_t, const char *);
int		send_filterset(struct imsgbuf *, struct filter_set_head *,
		    int, int);
int		reconfigure(char *, struct bgpd_config *, struct mrt_head *,
		    struct peer **, struct filter_head *);
int		dispatch_imsg(struct imsgbuf *, int);

int			 rfd = -1;
int			 cflags = 0;
struct filter_set_head	*connectset;
struct filter_set_head	*staticset;
volatile sig_atomic_t	 mrtdump = 0;
volatile sig_atomic_t	 quit = 0;
volatile sig_atomic_t	 reconfig = 0;
volatile sig_atomic_t	 sigchld = 0;
struct imsgbuf		*ibuf_se;
struct imsgbuf		*ibuf_rde;

void
sighdlr(int sig)
{
	switch (sig) {
	case SIGTERM:
	case SIGINT:
		quit = 1;
		break;
	case SIGCHLD:
		sigchld = 1;
		break;
	case SIGHUP:
		reconfig = 1;
		break;
	case SIGALRM:
	case SIGUSR1:
		mrtdump = 1;
		break;
	}
}

__dead void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-dnv] ", __progname);
	fprintf(stderr, "[-D macro=value] [-f file]\n");
	exit(1);
}

#define PFD_PIPE_SESSION	0
#define PFD_PIPE_ROUTE		1
#define PFD_SOCK_ROUTE		2
#define POLL_MAX		3
#define MAX_TIMEOUT		3600

int
main(int argc, char *argv[])
{
	struct bgpd_config	 conf;
	struct peer		*peer_l, *p;
	struct mrt_head		 mrt_l;
	struct network_head	 net_l;
	struct filter_head	*rules_l;
	struct network		*net;
	struct filter_rule	*r;
	struct mrt		*m;
	struct listen_addr	*la;
	struct pollfd		 pfd[POLL_MAX];
	pid_t			 io_pid = 0, rde_pid = 0, pid;
	char			*conffile;
	int			 debug = 0;
	int			 ch, nfds, timeout;
	int			 pipe_m2s[2];
	int			 pipe_m2r[2];
	int			 pipe_s2r[2];

	conffile = CONFFILE;
	bgpd_process = PROC_MAIN;

	log_init(1);		/* log to stderr until daemonized */

	if ((rules_l = calloc(1, sizeof(struct filter_head))) == NULL)
		err(1, NULL);

	bzero(&conf, sizeof(conf));
	LIST_INIT(&mrt_l);
	TAILQ_INIT(&net_l);
	TAILQ_INIT(rules_l);
	peer_l = NULL;

	while ((ch = getopt(argc, argv, "dD:f:nv")) != -1) {
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'D':
			if (cmdline_symset(optarg) < 0)
				log_warnx("could not parse macro definition %s",
				    optarg);
			break;
		case 'f':
			conffile = optarg;
			break;
		case 'n':
			conf.opts |= BGPD_OPT_NOACTION;
			break;
		case 'v':
			if (conf.opts & BGPD_OPT_VERBOSE)
				conf.opts |= BGPD_OPT_VERBOSE2;
			conf.opts |= BGPD_OPT_VERBOSE;
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}

	if (parse_config(conffile, &conf, &mrt_l, &peer_l, &net_l, rules_l)) {
		free(rules_l);
		exit(1);
	}

	if (conf.opts & BGPD_OPT_NOACTION) {
		if (conf.opts & BGPD_OPT_VERBOSE)
			print_config(&conf, &net_l, peer_l, rules_l, &mrt_l);
		else
			fprintf(stderr, "configuration OK\n");
		exit(0);
	}
	cflags = conf.flags;
	connectset = &conf.connectset;
	staticset = &conf.staticset;

	if (geteuid())
		errx(1, "need root privileges");

	if (getpwnam(BGPD_USER) == NULL)
		errx(1, "unknown user %s", BGPD_USER);

	log_init(debug);

	if (!debug)
		daemon(1, 0);

	log_info("startup");

	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_m2s) == -1)
		fatal("socketpair");
	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_m2r) == -1)
		fatal("socketpair");
	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_s2r) == -1)
		fatal("socketpair");
	session_socket_blockmode(pipe_m2s[0], BM_NONBLOCK);
	session_socket_blockmode(pipe_m2s[1], BM_NONBLOCK);
	session_socket_blockmode(pipe_m2r[0], BM_NONBLOCK);
	session_socket_blockmode(pipe_m2r[1], BM_NONBLOCK);
	session_socket_blockmode(pipe_s2r[0], BM_NONBLOCK);
	session_socket_blockmode(pipe_s2r[1], BM_NONBLOCK);

	prepare_listeners(&conf);

	/* fork children */
	rde_pid = rde_main(&conf, peer_l, &net_l, rules_l, &mrt_l,
	    pipe_m2r, pipe_s2r, pipe_m2s);
	io_pid = session_main(&conf, peer_l, &net_l, rules_l, &mrt_l,
	    pipe_m2s, pipe_s2r, pipe_m2r);

	setproctitle("parent");

	signal(SIGTERM, sighdlr);
	signal(SIGINT, sighdlr);
	signal(SIGCHLD, sighdlr);
	signal(SIGHUP, sighdlr);
	signal(SIGALRM, sighdlr);
	signal(SIGUSR1, sighdlr);

	close(pipe_m2s[1]);
	close(pipe_m2r[1]);
	close(pipe_s2r[0]);
	close(pipe_s2r[1]);

	if ((ibuf_se = malloc(sizeof(struct imsgbuf))) == NULL ||
	    (ibuf_rde = malloc(sizeof(struct imsgbuf))) == NULL)
		fatal(NULL);
	imsg_init(ibuf_se, pipe_m2s[0]);
	imsg_init(ibuf_rde, pipe_m2r[0]);
	mrt_init(ibuf_rde, ibuf_se);
	if ((rfd = kr_init(!(conf.flags & BGPD_FLAG_NO_FIB_UPDATE))) == -1)
		quit = 1;
	if (pftable_clear_all() != 0)
		quit = 1;

	while ((net = TAILQ_FIRST(&net_l)) != NULL) {
		TAILQ_REMOVE(&net_l, net, entry);
		free(net);
	}

	while ((r = TAILQ_FIRST(rules_l)) != NULL) {
		TAILQ_REMOVE(rules_l, r, entry);
		free(r);
	}
	TAILQ_FOREACH(la, conf.listen_addrs, entry) {
		close(la->fd);
		la->fd = -1;
	}

	mrt_reconfigure(&mrt_l);

	while (quit == 0) {
		pfd[PFD_PIPE_SESSION].fd = ibuf_se->fd;
		pfd[PFD_PIPE_SESSION].events = POLLIN;
		if (ibuf_se->w.queued)
			pfd[PFD_PIPE_SESSION].events |= POLLOUT;
		pfd[PFD_PIPE_ROUTE].fd = ibuf_rde->fd;
		pfd[PFD_PIPE_ROUTE].events = POLLIN;
		if (ibuf_rde->w.queued)
			pfd[PFD_PIPE_ROUTE].events |= POLLOUT;
		pfd[PFD_SOCK_ROUTE].fd = rfd;
		pfd[PFD_SOCK_ROUTE].events = POLLIN;

		timeout = mrt_timeout(&mrt_l);
		if (timeout > MAX_TIMEOUT)
			timeout = MAX_TIMEOUT;

		if ((nfds = poll(pfd, POLL_MAX, timeout * 1000)) == -1)
			if (errno != EINTR) {
				log_warn("poll error");
				quit = 1;
			}

		if (nfds > 0 && (pfd[PFD_PIPE_SESSION].revents & POLLOUT))
			if (msgbuf_write(&ibuf_se->w) < 0) {
				log_warn("pipe write error (to SE)");
				quit = 1;
			}

		if (nfds > 0 && (pfd[PFD_PIPE_ROUTE].revents & POLLOUT))
			if (msgbuf_write(&ibuf_rde->w) < 0) {
				log_warn("pipe write error (to RDE)");
				quit = 1;
			}

		if (nfds > 0 && pfd[PFD_PIPE_SESSION].revents & POLLIN) {
			nfds--;
			if (dispatch_imsg(ibuf_se, PFD_PIPE_SESSION) == -1)
				quit = 1;
		}

		if (nfds > 0 && pfd[PFD_PIPE_ROUTE].revents & POLLIN) {
			nfds--;
			if (dispatch_imsg(ibuf_rde, PFD_PIPE_ROUTE) == -1)
				quit = 1;
		}

		if (nfds > 0 && pfd[PFD_SOCK_ROUTE].revents & POLLIN) {
			nfds--;
			if (kr_dispatch_msg() == -1)
				quit = 1;
		}

		if (reconfig) {
			reconfig = 0;
			log_info("rereading config");
			reconfigure(conffile, &conf, &mrt_l, &peer_l, rules_l);
		}

		if (sigchld) {
			sigchld = 0;
			if (check_child(io_pid, "session engine")) {
				quit = 1;
				io_pid = 0;
			}
			if (check_child(rde_pid, "route decision engine")) {
				quit = 1;
				rde_pid = 0;
			}
		}

		if (mrtdump == 1) {
			mrtdump = 0;
			mrt_handler(&mrt_l);
		}
	}

	signal(SIGCHLD, SIG_IGN);

	if (io_pid)
		kill(io_pid, SIGTERM);

	if (rde_pid)
		kill(rde_pid, SIGTERM);

	while ((p = peer_l) != NULL) {
		peer_l = p->next;
		free(p);
	}
	while ((m = LIST_FIRST(&mrt_l)) != NULL) {
		LIST_REMOVE(m, entry);
		free(m);
	}
	while ((la = TAILQ_FIRST(conf.listen_addrs)) != NULL) {
		TAILQ_REMOVE(conf.listen_addrs, la, entry);
		close(la->fd);
		free(la);
	}

	free(rules_l);
	control_cleanup();
	kr_shutdown();
	pftable_clear_all();
	free(conf.listen_addrs);

	do {
		if ((pid = wait(NULL)) == -1 &&
		    errno != EINTR && errno != ECHILD)
			fatal("wait");
	} while (pid != -1 || (pid == -1 && errno == EINTR));

	msgbuf_clear(&ibuf_se->w);
	free(ibuf_se);
	msgbuf_clear(&ibuf_rde->w);
	free(ibuf_rde);

	log_info("Terminating");
	return (0);
}

int
check_child(pid_t pid, const char *pname)
{
	int	status;

	if (waitpid(pid, &status, WNOHANG) > 0) {
		if (WIFEXITED(status)) {
			log_warnx("Lost child: %s exited", pname);
			return (1);
		}
		if (WIFSIGNALED(status)) {
			log_warnx("Lost child: %s terminated; signal %d",
			    pname, WTERMSIG(status));
			return (1);
		}
	}

	return (0);
}

int
send_filterset(struct imsgbuf *i, struct filter_set_head *set, int id, int f)
{
	struct filter_set	*s;

	for (s = SIMPLEQ_FIRST(set); s != NULL; ) {
		if (imsg_compose(i, IMSG_FILTER_SET, id, 0, -1, s,
		    sizeof(struct filter_set)) == -1)
			return (-1);
		if (f) {
			SIMPLEQ_REMOVE_HEAD(set, entry);
			free(s);
			s = SIMPLEQ_FIRST(set);
		} else
			s = SIMPLEQ_NEXT(s, entry);
	}
	return (0);
}

int
reconfigure(char *conffile, struct bgpd_config *conf, struct mrt_head *mrt_l,
    struct peer **peer_l, struct filter_head *rules_l)
{
	struct network_head	 net_l;
	struct network		*n;
	struct peer		*p;
	struct filter_rule	*r;
	struct listen_addr	*la;

	if (parse_config(conffile, conf, mrt_l, peer_l, &net_l, rules_l)) {
		log_warnx("config file %s has errors, not reloading",
		    conffile);
		return (-1);
	}

	cflags = conf->flags;
	connectset = &conf->connectset;
	staticset = &conf->staticset;

	prepare_listeners(conf);

	if (imsg_compose(ibuf_se, IMSG_RECONF_CONF, 0, 0, -1,
	    conf, sizeof(struct bgpd_config)) == -1)
		return (-1);
	if (imsg_compose(ibuf_rde, IMSG_RECONF_CONF, 0, 0, -1,
	    conf, sizeof(struct bgpd_config)) == -1)
		return (-1);
	for (p = *peer_l; p != NULL; p = p->next) {
		if (imsg_compose(ibuf_se, IMSG_RECONF_PEER, p->conf.id, 0, -1,
		    &p->conf, sizeof(struct peer_config)) == -1)
			return (-1);
		if (send_filterset(ibuf_se, &p->conf.attrset,
		    p->conf.id, 0) == -1)
			return (-1);
	}
	while ((n = TAILQ_FIRST(&net_l)) != NULL) {
		if (imsg_compose(ibuf_rde, IMSG_NETWORK_ADD, 0, 0, -1,
		    &n->net, sizeof(struct network_config)) == -1)
			return (-1);
		if (send_filterset(ibuf_rde, &n->net.attrset, 0, 1) == -1)
			return (-1);
		if (imsg_compose(ibuf_rde, IMSG_NETWORK_DONE, 0, 0, -1,
		    NULL, 0) == -1)
			return (-1);
		TAILQ_REMOVE(&net_l, n, entry);
		free(n);
	}
	/* redistribute list needs to be reloaded too */
	if (kr_redist_reload() == -1)
		return (-1);

	while ((r = TAILQ_FIRST(rules_l)) != NULL) {
		if (imsg_compose(ibuf_rde, IMSG_RECONF_FILTER, 0, 0, -1,
		    r, sizeof(struct filter_rule)) == -1)
			return (-1);
		if (send_filterset(ibuf_rde, &r->set, 0, 1) == -1)
			return (-1);
		TAILQ_REMOVE(rules_l, r, entry);
		free(r);
	}
	TAILQ_FOREACH(la, conf->listen_addrs, entry) {
		if (imsg_compose(ibuf_se, IMSG_RECONF_LISTENER, 0, 0, la->fd,
		    la, sizeof(struct listen_addr)) == -1)
			return (-1);
		la->fd = -1;
	}

	if (imsg_compose(ibuf_se, IMSG_RECONF_DONE, 0, 0, -1, NULL, 0) == -1 ||
	    imsg_compose(ibuf_rde, IMSG_RECONF_DONE, 0, 0, -1, NULL, 0) == -1)
		return (-1);

	/* mrt changes can be sent out of bound */
	mrt_reconfigure(mrt_l);
	return (0);
}

int
dispatch_imsg(struct imsgbuf *ibuf, int idx)
{
	struct imsg		 imsg;
	int			 n;
	int			 rv;

	if ((n = imsg_read(ibuf)) == -1)
		return (-1);

	if (n == 0) {	/* connection closed */
		log_warnx("dispatch_imsg in main: pipe closed");
		return (-1);
	}

	rv = 0;
	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			return (-1);

		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_KROUTE_CHANGE:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("route request not from RDE");
			else if (kr_change(imsg.data))
				rv = -1;
			break;
		case IMSG_KROUTE_DELETE:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("route request not from RDE");
			else if (kr_delete(imsg.data))
				rv = -1;
			break;
		case IMSG_NEXTHOP_ADD:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("nexthop request not from RDE");
			else
				if (imsg.hdr.len != IMSG_HEADER_SIZE +
				    sizeof(struct bgpd_addr))
					log_warnx("wrong imsg len");
				else if (kr_nexthop_add(imsg.data) == -1)
					rv = -1;
			break;
		case IMSG_NEXTHOP_REMOVE:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("nexthop request not from RDE");
			else
				if (imsg.hdr.len != IMSG_HEADER_SIZE +
				    sizeof(struct bgpd_addr))
					log_warnx("wrong imsg len");
				else
					kr_nexthop_delete(imsg.data);
			break;
		case IMSG_PFTABLE_ADD:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("pftable request not from RDE");
			else
				if (imsg.hdr.len != IMSG_HEADER_SIZE +
				    sizeof(struct pftable_msg))
					log_warnx("wrong imsg len");
				else if (pftable_addr_add(imsg.data) != 0)
					rv = -1;
			break;
		case IMSG_PFTABLE_REMOVE:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("pftable request not from RDE");
			else
				if (imsg.hdr.len != IMSG_HEADER_SIZE +
				    sizeof(struct pftable_msg))
					log_warnx("wrong imsg len");
				else if (pftable_addr_remove(imsg.data) != 0)
					rv = -1;
			break;
		case IMSG_PFTABLE_COMMIT:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("pftable request not from RDE");
			else
				if (imsg.hdr.len != IMSG_HEADER_SIZE)
					log_warnx("wrong imsg len");
				else if (pftable_commit() != 0)
					rv = -1;
			break;
		case IMSG_CTL_RELOAD:
			if (idx != PFD_PIPE_SESSION)
				log_warnx("reload request not from SE");
			else
				reconfig = 1;
			break;
		case IMSG_CTL_FIB_COUPLE:
			if (idx != PFD_PIPE_SESSION)
				log_warnx("couple request not from SE");
			else
				kr_fib_couple();
			break;
		case IMSG_CTL_FIB_DECOUPLE:
			if (idx != PFD_PIPE_SESSION)
				log_warnx("decouple request not from SE");
			else
				kr_fib_decouple();
			break;
		case IMSG_CTL_KROUTE:
		case IMSG_CTL_KROUTE_ADDR:
		case IMSG_CTL_SHOW_NEXTHOP:
		case IMSG_CTL_SHOW_INTERFACE:
			if (idx != PFD_PIPE_SESSION)
				log_warnx("kroute request not from SE");
			else
				kr_show_route(&imsg);
			break;
		case IMSG_IFINFO:
			if (idx != PFD_PIPE_SESSION)
				log_warnx("IFINFO request not from SE");
			else if (imsg.hdr.len != IMSG_HEADER_SIZE + IFNAMSIZ)
				log_warnx("IFINFO request with wrong len");
			else
				kr_ifinfo(imsg.data);
			break;
		default:
			break;
		}
		imsg_free(&imsg);
		if (rv != 0)
			return (rv);
	}
	return (0);
}

void
send_nexthop_update(struct kroute_nexthop *msg)
{
	char	*gw = NULL;

	if (msg->gateway.af)
		if (asprintf(&gw, ": via %s",
		    log_addr(&msg->gateway)) == -1) {
			log_warn("send_nexthop_update");
			quit = 1;
		}

	log_info("nexthop %s now %s%s%s", log_addr(&msg->nexthop),
	    msg->valid ? "valid" : "invalid",
	    msg->connected ? ": directly connected" : "",
	    msg->gateway.af ? gw : "");

	free(gw);

	if (imsg_compose(ibuf_rde, IMSG_NEXTHOP_UPDATE, 0, 0, -1,
	    msg, sizeof(struct kroute_nexthop)) == -1)
		quit = 1;
}

void
send_imsg_session(int type, pid_t pid, void *data, u_int16_t datalen)
{
	imsg_compose(ibuf_se, type, 0, pid, -1, data, datalen);
}

int
bgpd_redistribute(int type, struct kroute *kr)
{
	struct network_config	 net;
	struct filter_set_head	*h;

	if ((cflags & BGPD_FLAG_REDIST_CONNECTED) && (kr->flags & F_CONNECTED))
		h = connectset;
	else if ((cflags & BGPD_FLAG_REDIST_STATIC) && (kr->flags & F_STATIC))
		h = staticset;
	else
		return (0);

	bzero(&net, sizeof(net));
	net.prefix.af = AF_INET;
	net.prefix.v4.s_addr = kr->prefix.s_addr;
	net.prefixlen = kr->prefixlen;

	if (imsg_compose(ibuf_rde, type, 0, 0, -1, &net,
	    sizeof(struct network_config)) == -1)
		return (-1);

	/* networks that get deleted don't need to send the filter set */
	if (type == IMSG_NETWORK_REMOVE)
		return (1);
	
	if (send_filterset(ibuf_rde, h, 0, 0) == -1)
		return (-1);
	if (imsg_compose(ibuf_rde, IMSG_NETWORK_DONE, 0, 0, -1, NULL, 0) == -1)
		return (-1);

	return (1);
}

