/*	$OpenBSD: bgpd.c,v 1.71 2004/01/22 20:34:55 henning Exp $ */

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

void	sighdlr(int);
void	usage(void);
int	main(int, char *[]);
int	check_child(pid_t, const char *);
int	reconfigure(char *, struct bgpd_config *, struct mrt_head *,
	    struct peer *);
int	dispatch_imsg(struct imsgbuf *, int, struct mrt_head *);

int			rfd = -1;
volatile sig_atomic_t	mrtdump = 0;
volatile sig_atomic_t	quit = 0;
volatile sig_atomic_t	reconfig = 0;
volatile sig_atomic_t	sigchld = 0;
struct imsgbuf		ibuf_se;
struct imsgbuf		ibuf_rde;

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

void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-dnv] ", __progname);
	fprintf(stderr, "[-D macro=value] [-f file]\n");
	exit(1);
}

#define POLL_MAX		8
#define PFD_PIPE_SESSION	0
#define PFD_PIPE_ROUTE		1
#define PFD_SOCK_ROUTE		2
#define PFD_MRT_START		3

int
main(int argc, char *argv[])
{
	struct bgpd_config	 conf;
	struct peer		*peer_l, *p, *next;
	struct mrt_head		 mrt_l;
	struct network_head	 net_l;
	struct network		*net;
	struct mrt		*(mrt[POLL_MAX]);
	struct pollfd		 pfd[POLL_MAX];
	pid_t			 io_pid = 0, rde_pid = 0, pid;
	char			*conffile;
	int			 debug = 0;
	int			 ch, csock, i, j, n, nfds, timeout;
	int			 pipe_m2s[2];
	int			 pipe_m2r[2];
	int			 pipe_s2r[2];

	conffile = CONFFILE;
	bgpd_process = PROC_MAIN;

	log_init(1);		/* log to stderr until daemonized */

	bzero(&conf, sizeof(conf));
	LIST_INIT(&mrt_l);
	TAILQ_INIT(&net_l);
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

	if (parse_config(conffile, &conf, &mrt_l, &peer_l, &net_l))
		exit(1);

	if (conf.opts & BGPD_OPT_NOACTION) {
		fprintf(stderr, "configuration OK\n");
		exit(0);
	}

	if (geteuid())
		errx(1, "need root privileges");

	if (getpwnam(BGPD_USER) == NULL)
		errx(1, "unknown user %s", BGPD_USER);
	endpwent();

	log_init(debug);

	if (!debug)
		daemon(1, 0);

	log_info("startup");

	if (pipe(pipe_m2s) == -1)
		fatal("pipe");
	if (fcntl(pipe_m2s[0], F_SETFL, O_NONBLOCK) == -1 ||
	    fcntl(pipe_m2s[1], F_SETFL, O_NONBLOCK) == -1)
		fatal("fcntl");
	if (pipe(pipe_m2r) == -1)
		fatal("pipe");
	if (fcntl(pipe_m2r[0], F_SETFL, O_NONBLOCK) == -1 ||
	    fcntl(pipe_m2r[1], F_SETFL, O_NONBLOCK) == -1)
		fatal("fcntl");
	if (pipe(pipe_s2r) == -1)
		fatal("pipe");
	if (fcntl(pipe_s2r[0], F_SETFL, O_NONBLOCK) == -1 ||
	    fcntl(pipe_s2r[1], F_SETFL, O_NONBLOCK) == -1)
		fatal("fcntl");

	if ((csock = control_init()) == -1)
		fatalx("control socket setup failed");

	/* fork children */
	rde_pid = rde_main(&conf, peer_l, &net_l, pipe_m2r, pipe_s2r);
	io_pid = session_main(&conf, peer_l, pipe_m2s, pipe_s2r);

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
	close(csock);

	imsg_init(&ibuf_se, pipe_m2s[0]);
	imsg_init(&ibuf_rde, pipe_m2r[0]);
	mrt_init(&ibuf_rde, &ibuf_se);
	if ((rfd = kr_init(!(conf.flags & BGPD_FLAG_NO_FIB_UPDATE))) == -1)
		quit = 1;

	for (p = peer_l; p != NULL; p = next) {
		next = p->next;
		free(p);
	}
	for (net = TAILQ_FIRST(&net_l); net != TAILQ_END(&net_l);
	    net = TAILQ_FIRST(&net_l)) {
		TAILQ_REMOVE(&net_l, net, network_l);
		free(net);
	}

	while (quit == 0) {
		pfd[PFD_PIPE_SESSION].fd = ibuf_se.sock;
		pfd[PFD_PIPE_SESSION].events = POLLIN;
		if (ibuf_se.w.queued)
			pfd[PFD_PIPE_SESSION].events |= POLLOUT;
		pfd[PFD_PIPE_ROUTE].fd = ibuf_rde.sock;
		pfd[PFD_PIPE_ROUTE].events = POLLIN;
		if (ibuf_rde.w.queued)
			pfd[PFD_PIPE_ROUTE].events |= POLLOUT;
		pfd[PFD_SOCK_ROUTE].fd = rfd;
		pfd[PFD_SOCK_ROUTE].events = POLLIN;
		i = PFD_MRT_START;
		i = mrt_select(&mrt_l, pfd, mrt, i, POLL_MAX, &timeout);

		if ((nfds = poll(pfd, i, INFTIM)) == -1)
			if (errno != EINTR) {
				log_warn("poll error");
				quit = 1;
			}

		if (nfds > 0 && (pfd[PFD_PIPE_SESSION].revents & POLLOUT))
			if ((n = msgbuf_write(&ibuf_se.w)) < 0) {
				log_warn("pipe write error (to SE)");
				quit = 1;
			}

		if (nfds > 0 && (pfd[PFD_PIPE_ROUTE].revents & POLLOUT))
			if ((n = msgbuf_write(&ibuf_rde.w)) < 0) {
				log_warn("pipe write error (to RDE)");
				quit = 1;
			}

		if (nfds > 0 && pfd[PFD_PIPE_SESSION].revents & POLLIN) {
			nfds--;
			if (dispatch_imsg(&ibuf_se, PFD_PIPE_SESSION,
			    &mrt_l) == -1)
				quit = 1;
		}

		if (nfds > 0 && pfd[PFD_PIPE_ROUTE].revents & POLLIN) {
			nfds--;
			if (dispatch_imsg(&ibuf_rde, PFD_PIPE_ROUTE,
			    &mrt_l) == -1)
				quit = 1;
		}

		if (nfds > 0 && pfd[PFD_SOCK_ROUTE].revents & POLLIN) {
			nfds--;
			if (kr_dispatch_msg() == -1)
				quit = 1;
		}

		for (j = PFD_MRT_START; j < i && nfds > 0 ; j++) {
			if (pfd[j].revents & POLLOUT) {
				if ((n = mrt_write(mrt[j])) < 0) {
					log_warn("mrt write error");
				}
			}
		}

		if (reconfig) {
			log_info("rereading config");
			reconfigure(conffile, &conf, &mrt_l, peer_l);
			reconfig = 0;
		}

		if (sigchld) {
			if (check_child(io_pid, "session engine"))
				quit = 1;
			if (check_child(rde_pid, "route decision engine"))
				quit = 1;
			sigchld = 0;
		}

		if (mrtdump == 1) {
			mrt_handler(&mrt_l);
			mrtdump = 0;
		}
	}

	signal(SIGCHLD, SIG_IGN);

	if (io_pid)
		kill(io_pid, SIGTERM);

	if (rde_pid)
		kill(rde_pid, SIGTERM);

	do {
		pid = waitpid(-1, NULL, WNOHANG);
	} while (pid > 0 || (pid == -1 && errno == EINTR));

	control_cleanup();
	kr_shutdown();

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
reconfigure(char *conffile, struct bgpd_config *conf, struct mrt_head *mrt_l,
    struct peer *peer_l)
{
	struct network_head	 net_l;
	struct network		*n;
	struct peer		*p, *next;

	if (parse_config(conffile, conf, mrt_l, &peer_l, &net_l)) {
		log_warnx("config file %s has errors, not reloading",
		    conffile);
		return (-1);
	}

	if (imsg_compose(&ibuf_se, IMSG_RECONF_CONF, 0,
	    conf, sizeof(struct bgpd_config)) == -1)
		return (-1);
	if (imsg_compose(&ibuf_rde, IMSG_RECONF_CONF, 0,
	    conf, sizeof(struct bgpd_config)) == -1)
		return (-1);
	for (p = peer_l; p != NULL; p = next) {
		next = p->next;
		if (imsg_compose(&ibuf_se, IMSG_RECONF_PEER, p->conf.id,
		    &p->conf, sizeof(struct peer_config)) == -1)
			return (-1);
		if (imsg_compose(&ibuf_rde, IMSG_RECONF_PEER, p->conf.id,
		    &p->conf, sizeof(struct peer_config)) == -1)
			return (-1);
		free(p);
	}
	for (n = TAILQ_FIRST(&net_l); n != TAILQ_END(&net_l);
	    n = TAILQ_FIRST(&net_l)) {
		if (imsg_compose(&ibuf_rde, IMSG_RECONF_NETWORK, 0,
		    &n->net, sizeof(struct network_config)) == -1)
			return (-1);
		TAILQ_REMOVE(&net_l, n, network_l);
		free(n);
	}
	if (imsg_compose(&ibuf_se, IMSG_RECONF_DONE, 0, NULL, 0) == -1 ||
	    imsg_compose(&ibuf_rde, IMSG_RECONF_DONE, 0, NULL, 0) == -1)
		return (-1);

	return (0);
}

int
dispatch_imsg(struct imsgbuf *ibuf, int idx, struct mrt_head *mrt_l)
{
	struct imsg		 imsg;
	int			 n;

	if ((n = imsg_read(ibuf)) == -1)
		return (-1);

	if (n == 0) {	/* connection closed */
		log_warnx("dispatch_imsg in main: pipe closed");
		return (-1);
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			return (-1);

		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_MRT_MSG:
		case IMSG_MRT_END:
			if (mrt_queue(mrt_l, &imsg) == -1)
				log_warnx("mrt_queue failed.");
			break;
		case IMSG_KROUTE_CHANGE:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("route request not from RDE");
			else if (kr_change(imsg.data))
				return (-1);
			break;
		case IMSG_KROUTE_DELETE:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("route request not from RDE");
			else if (kr_delete(imsg.data))
				return (-1);
			break;
		case IMSG_NEXTHOP_ADD:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("nexthop request not from RDE");
			else
				if (imsg.hdr.len != IMSG_HEADER_SIZE +
				    sizeof(struct bgpd_addr))
					log_warnx("wrong imsg len");
				else if (kr_nexthop_add(imsg.data) == -1)
					return (-1);
			break;
		case IMSG_NEXTHOP_REMOVE:
			if (idx != PFD_PIPE_ROUTE)
				log_warnx("nexthop request not from RDE");
			else
				if (imsg.hdr.len != IMSG_HEADER_SIZE +
				    sizeof(struct bgpd_addr))
					log_warnx("wrong imsg len");
				else kr_nexthop_delete(imsg.data);
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
		default:
			break;
		}
		imsg_free(&imsg);
	}
	return (0);
}

void
send_nexthop_update(struct kroute_nexthop *msg)
{
	char	*gw = NULL, *nh = NULL;

	if (msg->gateway.af == AF_INET)
		if (asprintf(&gw, ": via %s",
		    log_ntoa(msg->gateway.v4.s_addr)) == -1) {
			log_warn("send_nexthop_update");
			quit = 1;
		}

	if (msg->nexthop.af == AF_INET)
		nh = log_ntoa(msg->nexthop.v4.s_addr);

	log_info("nexthop %s now %s%s%s", nh,
	    msg->valid ? "valid" : "invalid",
	    msg->connected ? ": directly connected" : "",
	    msg->gateway.af ? gw : "");

	free(gw);

	if (imsg_compose(&ibuf_rde, IMSG_NEXTHOP_UPDATE, 0,
	    msg, sizeof(struct kroute_nexthop)) == -1)
		quit = 1;
}

void
send_imsg_session(int type, pid_t pid, void *data, u_int16_t datalen)
{
	imsg_compose_pid(&ibuf_se, type, pid, data, datalen);
}


