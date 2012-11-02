/*	$OpenBSD: bgpd.c,v 1.170 2012/11/02 18:02:45 florian Exp $ */

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

#include "bgpd.h"
#include "mrt.h"
#include "session.h"

void		sighdlr(int);
__dead void	usage(void);
int		main(int, char *[]);
int		check_child(pid_t, const char *);
int		send_filterset(struct imsgbuf *, struct filter_set_head *);
int		reconfigure(char *, struct bgpd_config *, struct mrt_head *,
		    struct peer **);
int		dispatch_imsg(struct imsgbuf *, int);
int		control_setup(struct bgpd_config *);

int			 rfd = -1;
int			 cflags;
volatile sig_atomic_t	 mrtdump;
volatile sig_atomic_t	 quit;
volatile sig_atomic_t	 sigchld;
volatile sig_atomic_t	 reconfig;
pid_t			 reconfpid;
int			 reconfpending;
struct imsgbuf		*ibuf_se;
struct imsgbuf		*ibuf_rde;
struct rib_names	 ribnames = SIMPLEQ_HEAD_INITIALIZER(ribnames);
char			*cname;
char			*rcname;

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

	fprintf(stderr, "usage: %s [-cdnv] [-D macro=value] [-f file]\n",
	    __progname);
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
	struct mrt_head		 mrt_l;
	struct peer		*peer_l, *p;
	struct mrt		*m;
	struct listen_addr	*la;
	struct pollfd		 pfd[POLL_MAX];
	pid_t			 io_pid = 0, rde_pid = 0, pid;
	char			*conffile;
	int			 debug = 0;
	int			 ch, timeout, nfds;
	int			 pipe_m2s[2];
	int			 pipe_m2r[2];
	int			 pipe_s2r[2];
	int			 pipe_s2r_c[2];

	conffile = CONFFILE;
	bgpd_process = PROC_MAIN;

	log_init(1);		/* log to stderr until daemonized */
	log_verbose(1);

	bzero(&conf, sizeof(conf));
	LIST_INIT(&mrt_l);
	peer_l = NULL;

	while ((ch = getopt(argc, argv, "cdD:f:nv")) != -1) {
		switch (ch) {
		case 'c':
			conf.opts |= BGPD_OPT_FORCE_DEMOTE;
			break;
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
			log_verbose(1);
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;
	if (argc > 0)
		usage();

	if (conf.opts & BGPD_OPT_NOACTION) {
		struct network_head	net_l;
		struct rdomain_head	rdom_l;
		struct filter_head	rules_l;

		if (parse_config(conffile, &conf, &mrt_l, &peer_l, &net_l,
		    &rules_l, &rdom_l))
			exit(1);

		if (conf.opts & BGPD_OPT_VERBOSE)
			print_config(&conf, &ribnames, &net_l, peer_l, &rules_l,
			    &mrt_l, &rdom_l);
		else
			fprintf(stderr, "configuration OK\n");
		exit(0);
	}

	if (geteuid())
		errx(1, "need root privileges");

	if (getpwnam(BGPD_USER) == NULL)
		errx(1, "unknown user %s", BGPD_USER);

	log_init(debug);
	log_verbose(conf.opts & BGPD_OPT_VERBOSE);

	if (!debug)
		daemon(1, 0);

	log_info("startup");

	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_m2s) == -1)
		fatal("socketpair");
	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_m2r) == -1)
		fatal("socketpair");
	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_s2r) == -1)
		fatal("socketpair");
	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_s2r_c) == -1)
		fatal("socketpair");
	session_socket_blockmode(pipe_m2s[0], BM_NONBLOCK);
	session_socket_blockmode(pipe_m2s[1], BM_NONBLOCK);
	session_socket_blockmode(pipe_m2r[0], BM_NONBLOCK);
	session_socket_blockmode(pipe_m2r[1], BM_NONBLOCK);
	session_socket_blockmode(pipe_s2r[0], BM_NONBLOCK);
	session_socket_blockmode(pipe_s2r[1], BM_NONBLOCK);
	session_socket_blockmode(pipe_s2r_c[0], BM_NONBLOCK);
	session_socket_blockmode(pipe_s2r_c[1], BM_NONBLOCK);

	/* fork children */
	rde_pid = rde_main(pipe_m2r, pipe_s2r, pipe_m2s, pipe_s2r_c, debug);
	io_pid = session_main(pipe_m2s, pipe_s2r, pipe_m2r, pipe_s2r_c);

	setproctitle("parent");

	signal(SIGTERM, sighdlr);
	signal(SIGINT, sighdlr);
	signal(SIGCHLD, sighdlr);
	signal(SIGHUP, sighdlr);
	signal(SIGALRM, sighdlr);
	signal(SIGUSR1, sighdlr);
	signal(SIGPIPE, SIG_IGN);

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
	if ((rfd = kr_init()) == -1)
		quit = 1;
	quit = reconfigure(conffile, &conf, &mrt_l, &peer_l);
	if (pftable_clear_all() != 0)
		quit = 1;

	while (quit == 0) {
		bzero(pfd, sizeof(pfd));
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

		if (nfds > 0 && pfd[PFD_PIPE_SESSION].revents & POLLOUT)
			if (msgbuf_write(&ibuf_se->w) < 0) {
				log_warn("pipe write error (to SE)");
				quit = 1;
			}

		if (nfds > 0 && pfd[PFD_PIPE_ROUTE].revents & POLLOUT)
			if (msgbuf_write(&ibuf_rde->w) < 0) {
				log_warn("pipe write error (to RDE)");
				quit = 1;
			}

		if (nfds > 0 && pfd[PFD_PIPE_SESSION].revents & POLLIN) {
			if (dispatch_imsg(ibuf_se, PFD_PIPE_SESSION) == -1)
				quit = 1;
		}

		if (nfds > 0 && pfd[PFD_PIPE_ROUTE].revents & POLLIN) {
			if (dispatch_imsg(ibuf_rde, PFD_PIPE_ROUTE) == -1)
				quit = 1;
		}

		if (nfds > 0 && pfd[PFD_SOCK_ROUTE].revents & POLLIN) {
			if (kr_dispatch_msg() == -1)
				quit = 1;
		}

		if (reconfig) {
			u_int	error;

			reconfig = 0;
			switch (reconfigure(conffile, &conf, &mrt_l, &peer_l)) {
			case -1:	/* fatal error */
				quit = 1;
				break;
			case 0:		/* all OK */
				error = 0;
				break;
			case 2:
				error = CTL_RES_PENDING;
				break;
			default:	/* parse error */
				error = CTL_RES_PARSE_ERROR;
				break;
			}
			if (reconfpid != 0) {
				send_imsg_session(IMSG_CTL_RESULT, reconfpid,
				    &error, sizeof(error));
				reconfpid = 0;
			}
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

		if (mrtdump) {
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
	if (conf.listen_addrs)
		while ((la = TAILQ_FIRST(conf.listen_addrs)) != NULL) {
			TAILQ_REMOVE(conf.listen_addrs, la, entry);
			close(la->fd);
			free(la);
		}

	control_cleanup(conf.csock);
	control_cleanup(conf.rcsock);
	carp_demote_shutdown();
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
	free(rcname);
	free(cname);

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
send_filterset(struct imsgbuf *i, struct filter_set_head *set)
{
	struct filter_set	*s;

	TAILQ_FOREACH(s, set, entry)
		if (imsg_compose(i, IMSG_FILTER_SET, 0, 0, -1, s,
		    sizeof(struct filter_set)) == -1)
			return (-1);
	return (0);
}

int
reconfigure(char *conffile, struct bgpd_config *conf, struct mrt_head *mrt_l,
    struct peer **peer_l)
{
	struct network_head	 net_l;
	struct rdomain_head	 rdom_l;
	struct filter_head	 rules_l;
	struct peer		*p;
	struct filter_rule	*r;
	struct listen_addr	*la;
	struct rde_rib		*rr;
	struct rdomain		*rd;

	if (reconfpending) {
		log_info("previous reload still running");
		return (2);
	}
	reconfpending = 2;	/* one per child */

	log_info("rereading config");
	if (parse_config(conffile, conf, mrt_l, peer_l, &net_l, &rules_l,
	    &rdom_l)) {
		log_warnx("config file %s has errors, not reloading",
		    conffile);
		reconfpending = 0;
		return (1);
	}

	cflags = conf->flags;
	prepare_listeners(conf);

	/* start reconfiguration */
	if (imsg_compose(ibuf_se, IMSG_RECONF_CONF, 0, 0, -1,
	    conf, sizeof(struct bgpd_config)) == -1)
		return (-1);
	if (imsg_compose(ibuf_rde, IMSG_RECONF_CONF, 0, 0, -1,
	    conf, sizeof(struct bgpd_config)) == -1)
		return (-1);

	TAILQ_FOREACH(la, conf->listen_addrs, entry) {
		if (imsg_compose(ibuf_se, IMSG_RECONF_LISTENER, 0, 0, la->fd,
		    la, sizeof(struct listen_addr)) == -1)
			return (-1);
		la->fd = -1;
	}

	if (control_setup(conf) == -1)
		return (-1);

	/* adjust fib syncing on reload */
	ktable_preload();

	/* RIBs for the RDE */
	while ((rr = SIMPLEQ_FIRST(&ribnames))) {
		SIMPLEQ_REMOVE_HEAD(&ribnames, entry);
		if (ktable_update(rr->rtableid, rr->name, NULL,
		    rr->flags) == -1) {
			log_warnx("failed to load rdomain %d",
			    rr->rtableid);
			return (-1);
		}
		if (imsg_compose(ibuf_rde, IMSG_RECONF_RIB, 0, 0, -1,
		    rr, sizeof(struct rde_rib)) == -1)
			return (-1);
		free(rr);
	}

	/* send peer list and listeners to the SE and RDE */
	for (p = *peer_l; p != NULL; p = p->next) {
		if (imsg_compose(ibuf_se, IMSG_RECONF_PEER, p->conf.id, 0, -1,
		    &p->conf, sizeof(struct peer_config)) == -1)
			return (-1);
		if (imsg_compose(ibuf_rde, IMSG_RECONF_PEER, p->conf.id, 0, -1,
		    &p->conf, sizeof(struct peer_config)) == -1)
			return (-1);
	}

	/* networks go via kroute to the RDE */
	if (kr_net_reload(0, &net_l))
		return (-1);

	/* filters for the RDE */
	while ((r = TAILQ_FIRST(&rules_l)) != NULL) {
		TAILQ_REMOVE(&rules_l, r, entry);
		if (imsg_compose(ibuf_rde, IMSG_RECONF_FILTER, 0, 0, -1,
		    r, sizeof(struct filter_rule)) == -1)
			return (-1);
		if (send_filterset(ibuf_rde, &r->set) == -1)
			return (-1);
		filterset_free(&r->set);
		free(r);
	}

	while ((rd = SIMPLEQ_FIRST(&rdom_l)) != NULL) {
		SIMPLEQ_REMOVE_HEAD(&rdom_l, entry);
		if (ktable_update(rd->rtableid, rd->descr, rd->ifmpe,
		    rd->flags) == -1) {
			log_warnx("failed to load rdomain %d",
			    rd->rtableid);
			return (-1);
		}
		/* networks go via kroute to the RDE */
		if (kr_net_reload(rd->rtableid, &rd->net_l))
			return (-1);

		if (imsg_compose(ibuf_rde, IMSG_RECONF_RDOMAIN, 0, 0, -1,
		    rd, sizeof(*rd)) == -1)
			return (-1);

		/* export targets */
		if (imsg_compose(ibuf_rde, IMSG_RECONF_RDOMAIN_EXPORT, 0, 0,
		    -1, NULL, 0) == -1)
			return (-1);
		if (send_filterset(ibuf_rde, &rd->export) == -1)
			return (-1);
		filterset_free(&rd->export);

		/* import targets */
		if (imsg_compose(ibuf_rde, IMSG_RECONF_RDOMAIN_IMPORT, 0, 0,
		    -1, NULL, 0) == -1)
			return (-1);
		if (send_filterset(ibuf_rde, &rd->import) == -1)
			return (-1);
		filterset_free(&rd->import);

		if (imsg_compose(ibuf_rde, IMSG_RECONF_RDOMAIN_DONE, 0, 0,
		    -1, NULL, 0) == -1)
			return (-1);

		free(rd);
	}

	/* signal both childs to replace their config */
	if (imsg_compose(ibuf_se, IMSG_RECONF_DONE, 0, 0, -1, NULL, 0) == -1 ||
	    imsg_compose(ibuf_rde, IMSG_RECONF_DONE, 0, 0, -1, NULL, 0) == -1)
		return (-1);

	/* fix kroute information */
	ktable_postload();

	/* redistribute list needs to be reloaded too */
	if (kr_reload() == -1)
		return (-1);

	/* mrt changes can be sent out of bound */
	mrt_reconfigure(mrt_l);
	return (0);
}

int
dispatch_imsg(struct imsgbuf *ibuf, int idx)
{
	struct imsg		 imsg;
	ssize_t			 n;
	int			 rv, verbose;

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
			else if (imsg.hdr.len != IMSG_HEADER_SIZE +
			    sizeof(struct kroute_full))
				log_warnx("wrong imsg len");
			else if (kr_change(imsg.hdr.peerid, imsg.data))
				rv = -1;
			break;
		case IMSG_KROUTE_DELETE:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("route request not from RDE");
			else if (imsg.hdr.len != IMSG_HEADER_SIZE +
			    sizeof(struct kroute_full))
				log_warnx("wrong imsg len");
			else if (kr_delete(imsg.hdr.peerid, imsg.data))
				rv = -1;
			break;
		case IMSG_NEXTHOP_ADD:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("nexthop request not from RDE");
			else if (imsg.hdr.len != IMSG_HEADER_SIZE +
			    sizeof(struct bgpd_addr))
				log_warnx("wrong imsg len");
			else if (kr_nexthop_add(imsg.hdr.peerid, imsg.data) ==
			    -1)
				rv = -1;
			break;
		case IMSG_NEXTHOP_REMOVE:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("nexthop request not from RDE");
			else if (imsg.hdr.len != IMSG_HEADER_SIZE +
			    sizeof(struct bgpd_addr))
				log_warnx("wrong imsg len");
			else
				kr_nexthop_delete(imsg.hdr.peerid, imsg.data);
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
			else {
				reconfig = 1;
				reconfpid = imsg.hdr.pid;
			}
			break;
		case IMSG_CTL_FIB_COUPLE:
			if (idx != PFD_PIPE_SESSION)
				log_warnx("couple request not from SE");
			else
				kr_fib_couple(imsg.hdr.peerid);
			break;
		case IMSG_CTL_FIB_DECOUPLE:
			if (idx != PFD_PIPE_SESSION)
				log_warnx("decouple request not from SE");
			else
				kr_fib_decouple(imsg.hdr.peerid);
			break;
		case IMSG_CTL_KROUTE:
		case IMSG_CTL_KROUTE_ADDR:
		case IMSG_CTL_SHOW_NEXTHOP:
		case IMSG_CTL_SHOW_INTERFACE:
		case IMSG_CTL_SHOW_FIB_TABLES:
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
		case IMSG_DEMOTE:
			if (idx != PFD_PIPE_SESSION)
				log_warnx("demote request not from SE");
			else if (imsg.hdr.len != IMSG_HEADER_SIZE +
			    sizeof(struct demote_msg))
				log_warnx("DEMOTE request with wrong len");
			else {
				struct demote_msg	*msg;

				msg = imsg.data;
				carp_demote_set(msg->demote_group, msg->level);
			}
			break;
		case IMSG_CTL_LOG_VERBOSE:
			/* already checked by SE */
			memcpy(&verbose, imsg.data, sizeof(verbose));
			log_verbose(verbose);
			break;
		case IMSG_RECONF_DONE:
			if (reconfpending == 0)
				log_warnx("unexpected RECONF_DONE received");
			reconfpending--;
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

	if (msg->gateway.aid)
		if (asprintf(&gw, ": via %s",
		    log_addr(&msg->gateway)) == -1) {
			log_warn("send_nexthop_update");
			quit = 1;
		}

	log_info("nexthop %s now %s%s%s", log_addr(&msg->nexthop),
	    msg->valid ? "valid" : "invalid",
	    msg->connected ? ": directly connected" : "",
	    msg->gateway.aid ? gw : "");

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
send_network(int type, struct network_config *net, struct filter_set_head *h)
{
	if (imsg_compose(ibuf_rde, type, 0, 0, -1, net,
	    sizeof(struct network_config)) == -1)
		return (-1);
	/* networks that get deleted don't need to send the filter set */
	if (type == IMSG_NETWORK_REMOVE)
		return (0);
	if (send_filterset(ibuf_rde, h) == -1)
		return (-1);
	if (imsg_compose(ibuf_rde, IMSG_NETWORK_DONE, 0, 0, -1, NULL, 0) == -1)
		return (-1);

	return (0);
}

int
bgpd_filternexthop(struct kroute *kr, struct kroute6 *kr6)
{
	/* kernel routes are never filtered */
	if (kr && kr->flags & F_KERNEL && kr->prefixlen != 0)
		return (0);
	if (kr6 && kr6->flags & F_KERNEL && kr6->prefixlen != 0)
		return (0);

	if (cflags & BGPD_FLAG_NEXTHOP_BGP) {
		if (kr && kr->flags & F_BGPD_INSERTED)
			return (0);
		if (kr6 && kr6->flags & F_BGPD_INSERTED)
			return (0);
	}

	if (cflags & BGPD_FLAG_NEXTHOP_DEFAULT) {
		if (kr && kr->prefixlen == 0)
			return (0);
		if (kr6 && kr6->prefixlen == 0)
			return (0);
	}

	return (1);
}

int
control_setup(struct bgpd_config *conf)
{
	int fd, restricted;

	/* control socket is outside chroot */
	if (!cname || strcmp(cname, conf->csock)) {
		if (cname) {
			control_cleanup(cname);
			free(cname);
		}
		if ((cname = strdup(conf->csock)) == NULL)
			fatal("strdup");
		if ((fd = control_init(0, cname)) == -1)
			fatalx("control socket setup failed");
		restricted = 0;
		if (imsg_compose(ibuf_se, IMSG_RECONF_CTRL, 0, 0, fd,
		    &restricted, sizeof(restricted)) == -1)
			return (-1);
	}
	if (!conf->rcsock) {
		/* remove restricted socket */
		control_cleanup(rcname);
		free(rcname);
		rcname = NULL;
	} else if (!rcname || strcmp(rcname, conf->rcsock)) {
		if (rcname) {
			control_cleanup(rcname);
			free(rcname);
		}
		if ((rcname = strdup(conf->rcsock)) == NULL)
			fatal("strdup");
		if ((fd = control_init(1, rcname)) == -1)
			fatalx("control socket setup failed");
		restricted = 1;
		if (imsg_compose(ibuf_se, IMSG_RECONF_CTRL, 0, 0, fd,
		    &restricted, sizeof(restricted)) == -1)
			return (-1);
	}
	return (0);
}
