/*	$OpenBSD: control.c,v 1.37 2004/08/24 12:43:34 claudio Exp $ */

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
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bgpd.h"
#include "session.h"

#define	CONTROL_BACKLOG	5

struct {
	int	fd;
} control_state;

struct ctl_conn	*control_connbyfd(int);
struct ctl_conn	*control_connbypid(pid_t);
int		 control_close(int);

int
control_init(void)
{
	struct sockaddr_un	 sun;
	int			 fd;
	mode_t			 old_umask;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		log_warn("control_init: socket");
		return (-1);
	}

	old_umask = umask(S_IXUSR|S_IXGRP|S_IWOTH|S_IROTH|S_IXOTH);
	bzero(&sun, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, SOCKET_NAME, sizeof(sun.sun_path));

	if (unlink(SOCKET_NAME) == -1)
		if (errno != ENOENT) {
			log_warn("unlink %s", SOCKET_NAME);
			close(fd);
			return (-1);
		}

	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
		log_warn("control_init: bind: %s", SOCKET_NAME);
		close(fd);
		return (-1);
	}

	if (chmod(SOCKET_NAME, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) == -1) {
		log_warn("control_init chmod");
		close(fd);
		return (-1);
	}

	umask(old_umask);

	session_socket_blockmode(fd, BM_NONBLOCK);
	control_state.fd = fd;

	return (fd);
}

int
control_listen(void)
{
	if (listen(control_state.fd, CONTROL_BACKLOG) == -1) {
		log_warn("control_listen: listen");
		return (-1);
	}

	return (control_state.fd);
}

void
control_shutdown(void)
{
	close(control_state.fd);
}

void
control_cleanup(void)
{
	unlink(SOCKET_NAME);
}

int
control_accept(int listenfd)
{
	int			 connfd;
	socklen_t		 len;
	struct sockaddr_un	 sun;
	struct ctl_conn		*ctl_conn;

	len = sizeof(sun);
	if ((connfd = accept(listenfd,
	    (struct sockaddr *)&sun, &len)) == -1) {
		if (errno != EWOULDBLOCK && errno != EINTR)
			log_warn("session_control_accept");
		return (0);
	}

	session_socket_blockmode(connfd, BM_NONBLOCK);

	if ((ctl_conn = malloc(sizeof(struct ctl_conn))) == NULL) {
		log_warn("session_control_accept");
		return (0);
	}

	imsg_init(&ctl_conn->ibuf, connfd);

	TAILQ_INSERT_TAIL(&ctl_conns, ctl_conn, entry);

	return (1);
}

struct ctl_conn *
control_connbyfd(int fd)
{
	struct ctl_conn	*c;

	for (c = TAILQ_FIRST(&ctl_conns); c != NULL && c->ibuf.fd != fd;
	    c = TAILQ_NEXT(c, entry))
		;	/* nothing */

	return (c);
}

struct ctl_conn *
control_connbypid(pid_t pid)
{
	struct ctl_conn	*c;

	for (c = TAILQ_FIRST(&ctl_conns); c != NULL && c->ibuf.pid != pid;
	    c = TAILQ_NEXT(c, entry))
		;	/* nothing */

	return (c);
}

int
control_close(int fd)
{
	struct ctl_conn	*c;

	if ((c = control_connbyfd(fd)) == NULL) {
		log_warn("control_close: fd %d: not found", fd);
		return (0);
	}

	msgbuf_clear(&c->ibuf.w);
	TAILQ_REMOVE(&ctl_conns, c, entry);

	close(c->ibuf.fd);
	free(c);

	return (1);
}

int
control_dispatch_msg(struct pollfd *pfd, u_int *ctl_cnt)
{
	struct imsg		 imsg;
	struct ctl_conn		*c;
	int			 n;
	struct peer		*p;
	struct bgpd_addr	*addr;

	if ((c = control_connbyfd(pfd->fd)) == NULL) {
		log_warn("control_dispatch_msg: fd %d: not found", pfd->fd);
		return (0);
	}

	if (pfd->revents & POLLOUT)
		if (msgbuf_write(&c->ibuf.w) < 0) {
			*ctl_cnt -= control_close(pfd->fd);
			return (1);
		}

	if (!(pfd->revents & POLLIN))
		return (0);

	if (imsg_read(&c->ibuf) <= 0) {
		*ctl_cnt -= control_close(pfd->fd);
		return (1);
	}

	for (;;) {
		if ((n = imsg_get(&c->ibuf, &imsg)) == -1) {
			*ctl_cnt -= control_close(pfd->fd);
			return (1);
		}

		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_CTL_SHOW_NEIGHBOR:
			c->ibuf.pid = imsg.hdr.pid;
			if (imsg.hdr.len == IMSG_HEADER_SIZE +
			    sizeof(struct bgpd_addr)) {
				addr = imsg.data;
				p = getpeerbyaddr(addr);
				if (p != NULL)
					imsg_compose_rde(imsg.hdr.type,
					    imsg.hdr.pid,
					    p, sizeof(struct peer));
			} else
				for (p = peers; p != NULL; p = p->next)
					imsg_compose_rde(imsg.hdr.type,
					    imsg.hdr.pid,
					    p, sizeof(struct peer));
			imsg_compose_rde(IMSG_CTL_END, imsg.hdr.pid, NULL, 0);
			break;
		case IMSG_CTL_RELOAD:
		case IMSG_CTL_FIB_COUPLE:
		case IMSG_CTL_FIB_DECOUPLE:
			imsg_compose_parent(imsg.hdr.type, 0, NULL, 0);
			break;
		case IMSG_CTL_NEIGHBOR_UP:
		case IMSG_CTL_NEIGHBOR_DOWN:
		case IMSG_CTL_NEIGHBOR_CLEAR:
			if (imsg.hdr.len == IMSG_HEADER_SIZE +
			    sizeof(struct bgpd_addr)) {
				addr = imsg.data;
				p = getpeerbyaddr(addr);
				if (p == NULL) {
					log_warnx("IMSG_CTL_NEIGHBOR_ "
					    "with unknown neighbor");
					break;
				}
				switch (imsg.hdr.type) {
				case IMSG_CTL_NEIGHBOR_UP:
					bgp_fsm(p, EVNT_START);
					break;
				case IMSG_CTL_NEIGHBOR_DOWN:
					bgp_fsm(p, EVNT_STOP);
					break;
				case IMSG_CTL_NEIGHBOR_CLEAR:
					bgp_fsm(p, EVNT_STOP);
					bgp_fsm(p, EVNT_START);
					break;
				default:
					fatal("king bula wants more humppa");
				}
			} else
				log_warnx("got IMSG_CTL_NEIGHBOR_ with "
				    "wrong length");
			break;
		case IMSG_CTL_KROUTE:
		case IMSG_CTL_KROUTE_ADDR:
		case IMSG_CTL_SHOW_NEXTHOP:
		case IMSG_CTL_SHOW_INTERFACE:
			c->ibuf.pid = imsg.hdr.pid;
			imsg_compose_parent(imsg.hdr.type, imsg.hdr.pid,
			    imsg.data, imsg.hdr.len - IMSG_HEADER_SIZE);
			break;
		case IMSG_CTL_SHOW_RIB:
		case IMSG_CTL_SHOW_RIB_AS:
		case IMSG_CTL_SHOW_RIB_PREFIX:
		case IMSG_CTL_SHOW_NETWORK:
			c->ibuf.pid = imsg.hdr.pid;
			imsg_compose_rde(imsg.hdr.type, imsg.hdr.pid,
			    imsg.data, imsg.hdr.len - IMSG_HEADER_SIZE);
			break;
		case IMSG_NETWORK_ADD:
		case IMSG_NETWORK_REMOVE:
		case IMSG_NETWORK_FLUSH:
			imsg_compose_rde(imsg.hdr.type, 0,
			    imsg.data, imsg.hdr.len - IMSG_HEADER_SIZE);
			break;
		default:
			break;
		}
		imsg_free(&imsg);
	}

	return (0);
}

int
control_imsg_relay(struct imsg *imsg)
{
	struct ctl_conn	*c;

	if ((c = control_connbypid(imsg->hdr.pid)) == NULL)
		return (0);

	return (imsg_compose_pid(&c->ibuf, imsg->hdr.type, imsg->hdr.pid,
	    imsg->data, imsg->hdr.len - IMSG_HEADER_SIZE));
}
