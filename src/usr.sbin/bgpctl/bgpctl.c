/*	$OpenBSD: bgpctl.c,v 1.34 2004/01/21 23:47:35 henning Exp $ */

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
#include <sys/socket.h>
#include <sys/un.h>
#include <net/if.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bgpd.h"
#include "session.h"
#include "log.h"
#include "parser.h"

enum neighbor_views {
	NV_DEFAULT,
	NV_TIMERS
};


void		 usage(void);
int		 main(int, char *[]);
void		 show_summary_head(void);
int		 show_summary_msg(struct imsg *);
int		 show_neighbor_msg(struct imsg *, enum neighbor_views);
void		 print_neighbor_msgstats(struct peer *);
void		 print_neighbor_timers(struct peer *);
void		 print_timer(const char *, time_t, u_int);
static char	*fmt_timeframe(time_t t);
static char	*fmt_timeframe_core(time_t t);
void		 show_fib_head(void);
int		 show_fib_msg(struct imsg *);
void		 show_nexthop_head(void);
int		 show_nexthop_msg(struct imsg *);
void		 show_interface_head(void);
const char *	 get_media_descr(int);
const char *	 get_linkstate(int, int);
void		 print_baudrate(u_long);
int		 show_interface_msg(struct imsg *);

struct imsgbuf	ibuf;

void
usage(void)
{
	extern char	*__progname;

	fprintf(stderr, "usage: %s <command> [arg [...]]\n", __progname);
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct sockaddr_un	 sun;
	int			 fd, n, done;
	struct imsg		 imsg;
	struct parse_result	*res;

	if ((res = parse(argc, argv)) == NULL)
		exit(1);

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		err(1, "control_init: socket");

	bzero(&sun, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, SOCKET_NAME, sizeof(sun.sun_path));
	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) == -1)
		err(1, "connect: %s", SOCKET_NAME);

	imsg_init(&ibuf, fd);
	done = 0;

	switch (res->action) {
	case NONE:
		usage();
		/* not reached */
	case SHOW:
	case SHOW_SUMMARY:
		imsg_compose(&ibuf, IMSG_CTL_SHOW_NEIGHBOR, 0, NULL, 0);
		show_summary_head();
		break;
	case SHOW_FIB:
		if (!res->addr.af)
			imsg_compose(&ibuf, IMSG_CTL_KROUTE, 0, &res->flags,
			    sizeof(res->flags));
		else
			imsg_compose(&ibuf, IMSG_CTL_KROUTE_ADDR, 0, &res->addr,
			    sizeof(res->addr));
		show_fib_head();
		break;
	case SHOW_NEXTHOP:
		imsg_compose(&ibuf, IMSG_CTL_SHOW_NEXTHOP, 0, NULL, 0);
		show_nexthop_head();
		break;
	case SHOW_INTERFACE:
		imsg_compose(&ibuf, IMSG_CTL_SHOW_INTERFACE, 0, NULL, 0);
		show_interface_head();
		break;
	case SHOW_NEIGHBOR:
	case SHOW_NEIGHBOR_TIMERS:
		if (res->addr.af)
			imsg_compose(&ibuf, IMSG_CTL_SHOW_NEIGHBOR, 0,
			    &res->addr, sizeof(res->addr));
		else
			imsg_compose(&ibuf, IMSG_CTL_SHOW_NEIGHBOR, 0, NULL, 0);
		break;
	case RELOAD:
		imsg_compose(&ibuf, IMSG_CTL_RELOAD, 0, NULL, 0);
		printf("reload request sent.\n");
		done = 1;
		break;
	case FIB:
		errx(1, "action==FIB");
		break;
	case FIB_COUPLE:
		imsg_compose(&ibuf, IMSG_CTL_FIB_COUPLE, 0, NULL, 0);
		printf("couple request sent.\n");
		done = 1;
		break;
	case FIB_DECOUPLE:
		imsg_compose(&ibuf, IMSG_CTL_FIB_DECOUPLE, 0, NULL, 0);
		printf("decouple request sent.\n");
		done = 1;
		break;
	case NEIGHBOR:
		errx(1, "action==NEIGHBOR");
		break;
	case NEIGHBOR_UP:
		imsg_compose(&ibuf, IMSG_CTL_NEIGHBOR_UP, 0,
		    &res->addr, sizeof(res->addr));
		printf("request sent.\n");
		done = 1;
		break;
	case NEIGHBOR_DOWN:
		imsg_compose(&ibuf, IMSG_CTL_NEIGHBOR_DOWN, 0,
		    &res->addr, sizeof(res->addr));
		printf("request sent.\n");
		done = 1;
		break;
	}

	while (ibuf.w.queued)
		if (msgbuf_write(&ibuf.w) < 0)
			err(1, "write error");

	while (!done) {
		if ((n = imsg_read(&ibuf)) == -1)
			errx(1, "imsg_read error");
		if (n == 0)
			errx(1, "pipe closed");

		while (!done) {
			if ((n = imsg_get(&ibuf, &imsg)) == -1)
				errx(1, "imsg_get error");
			if (n == 0)
				break;
			switch (res->action) {
			case SHOW:
			case SHOW_SUMMARY:
				done = show_summary_msg(&imsg);
				break;
			case SHOW_FIB:
				done = show_fib_msg(&imsg);
				break;
			case SHOW_NEXTHOP:
				done = show_nexthop_msg(&imsg);
				break;
			case SHOW_INTERFACE:
				done = show_interface_msg(&imsg);
				break;
			case SHOW_NEIGHBOR:
				done = show_neighbor_msg(&imsg, NV_DEFAULT);
				break;
			case SHOW_NEIGHBOR_TIMERS:
				done = show_neighbor_msg(&imsg, NV_TIMERS);
				break;
			case NONE:
			case RELOAD:
			case FIB:
			case FIB_COUPLE:
			case FIB_DECOUPLE:
			case NEIGHBOR:
			case NEIGHBOR_UP:
			case NEIGHBOR_DOWN:
				break;
			}
			imsg_free(&imsg);
		}
	}
	close(fd);
}

void
show_summary_head(void)
{
	printf("%-15s %-5s %-10s %-10s %-8s %s\n", "Neighbor", "AS", "MsgRcvd",
	    "MsgSent", "Up/Down", "State");
}

int
show_summary_msg(struct imsg *imsg)
{
	struct peer		*p;

	switch (imsg->hdr.type) {
	case IMSG_CTL_SHOW_NEIGHBOR:
		p = imsg->data;
		printf("%-15s %5u %10llu %10llu %-8s %s\n",
		    inet_ntoa(p->conf.remote_addr.sin_addr),
		    p->conf.remote_as,
		    p->stats.msg_rcvd_open + p->stats.msg_rcvd_notification +
		    p->stats.msg_rcvd_update + p->stats.msg_rcvd_keepalive,
		    p->stats.msg_sent_open + p->stats.msg_sent_notification +
		    p->stats.msg_sent_update + p->stats.msg_sent_keepalive,
		    fmt_timeframe(p->stats.last_updown),
		    statenames[p->state]);
		break;
	case IMSG_CTL_END:
		return (1);
		break;
	default:
		break;
	}

	return (0);
}

int
show_neighbor_msg(struct imsg *imsg, enum neighbor_views nv)
{
	struct peer		*p;
	struct sockaddr_in	*sa_in;

	switch (imsg->hdr.type) {
	case IMSG_CTL_SHOW_NEIGHBOR:
		p = imsg->data;
		printf("BGP neighbor is %s, remote AS %u\n",
		    inet_ntoa(p->conf.remote_addr.sin_addr),
		    p->conf.remote_as);
		if (p->conf.descr[0])
			printf(" Description: %s\n", p->conf.descr);
		printf("  BGP version 4, remote router-id %s\n",
		    log_ntoa(p->remote_bgpid));
		printf("  BGP state = %s", statenames[p->state]);
		if (p->stats.last_updown != 0)
			printf(", %s for %s",
			    p->state == STATE_ESTABLISHED ? "up" : "down",
			    fmt_timeframe(p->stats.last_updown));
		printf("\n");
		printf("  Last read %s, holdtime %us, keepalive interval %us\n",
		    fmt_timeframe(p->stats.last_read),
		    p->holdtime, p->holdtime/3);
		printf("\n");
		switch (nv) {
		case NV_DEFAULT:
			print_neighbor_msgstats(p);
			break;
		case NV_TIMERS:
			print_neighbor_timers(p);
			break;
		}
		printf("\n");
		if (p->sa_local.ss_family == AF_INET) {
			sa_in = (struct sockaddr_in *)&p->sa_local;
			printf("  Local host:   %20s, Local port:   %5u\n",
			    log_ntoa(sa_in->sin_addr.s_addr),
			    ntohs(sa_in->sin_port));
		}
		if (p->sa_remote.ss_family == AF_INET) {
			sa_in = (struct sockaddr_in *)&p->sa_remote;
			printf("  Foreign host: %20s, Foreign port: %5u\n",
			    log_ntoa(sa_in->sin_addr.s_addr),
			    ntohs(sa_in->sin_port));
		}
		printf("\n");
		break;
	case IMSG_CTL_END:
		return (1);
		break;
	default:
		break;
	}

	return (0);
}

void
print_neighbor_msgstats(struct peer *p)
{
	printf("  Message statistics:\n");
	printf("  %-15s %-10s %-10s\n", "", "Sent", "Received");
	printf("  %-15s %10llu %10llu\n", "Opens",
	    p->stats.msg_sent_open, p->stats.msg_rcvd_open);
	printf("  %-15s %10llu %10llu\n", "Notifications",
	    p->stats.msg_sent_notification, p->stats.msg_rcvd_notification);
	printf("  %-15s %10llu %10llu\n", "Updates",
	    p->stats.msg_sent_update, p->stats.msg_rcvd_update);
	printf("  %-15s %10llu %10llu\n", "Keepalives",
	    p->stats.msg_sent_keepalive, p->stats.msg_rcvd_keepalive);
	printf("  %-15s %10llu %10llu\n", "Total",
	    p->stats.msg_sent_open + p->stats.msg_sent_notification +
	    p->stats.msg_sent_update + p->stats.msg_sent_keepalive,
	    p->stats.msg_rcvd_open + p->stats.msg_rcvd_notification +
	    p->stats.msg_rcvd_update + p->stats.msg_rcvd_keepalive);
}

void
print_neighbor_timers(struct peer *p)
{
	print_timer("IdleHoldTimer:", p->IdleHoldTimer, p->IdleHoldTime);
	print_timer("ConnectRetryTimer:", p->ConnectRetryTimer,
	    INTERVAL_CONNECTRETRY);
	print_timer("HoldTimer:", p->HoldTimer, p->holdtime);
	print_timer("KeepaliveTimer:", p->KeepaliveTimer, p->holdtime/3);
}

void
print_timer(const char *name, time_t val, u_int interval)
{
	int	d;

	d = val - time(NULL);
	printf("  %-20s ", name);

	if (val == 0)
		printf("%-20s", "not running");
	else if (d <= 0)
		printf("%-20s", "due");
	else
		printf("due in %-13s", fmt_timeframe_core(d));

	printf("Interval: %5us\n", interval);
}

#define TF_BUFS	8
#define TF_LEN	9

static char *
fmt_timeframe(time_t t)
{
	if (t == 0)
		return ("Never");
	else
		return (fmt_timeframe_core(time(NULL) - t));
}

static char *
fmt_timeframe_core(time_t t)
{
	char		*buf;
	static char	 tfbuf[TF_BUFS][TF_LEN];	/* ring buffer */
	static int	 idx = 0;
	unsigned	 sec, min, hrs, day, week;

	buf = tfbuf[idx++];
	if (idx == TF_BUFS)
		idx = 0;

	week = t;

	sec = week % 60;
	week /= 60;
	min = week % 60;
	week /= 60;
	hrs = week % 24;
	week /= 24;
	day = week % 7;
	week /= 7;

	if (week > 0)
		snprintf(buf, TF_LEN, "%02uw%01ud%02uh", week, day, hrs);
	else if (day > 0)
		snprintf(buf, TF_LEN, "%01ud%02uh%02um", day, hrs, min);
	else
		snprintf(buf, TF_LEN, "%02u:%02u:%02u", hrs, min, sec);

	return (buf);
}

void
show_fib_head(void)
{
	printf("flags: * = valid, B = BGP, C = Connected, S = Static\n");
	printf("       N = BGP Nexthop reachable via this route\n\n");
	printf("flags destination          gateway\n");
}

int
show_fib_msg(struct imsg *imsg)
{
	struct kroute		*k;
	char			*p;

	switch (imsg->hdr.type) {
	case IMSG_CTL_KROUTE:
		if (imsg->hdr.len < IMSG_HEADER_SIZE + sizeof(struct kroute))
			errx(1, "wrong imsg len");
		k = imsg->data;

		if (k->flags & F_DOWN)
			printf(" ");
		else
			printf("*");

		if (k->flags & F_BGPD_INSERTED)
			printf("B");
		else if (k->flags & F_CONNECTED)
			printf("C");
		else if (k->flags & F_KERNEL)
			printf("S");
		else
			printf(" ");

		if (k->flags & F_NEXTHOP)
			printf("N");
		else
			printf(" ");

		printf("   ");
		if (asprintf(&p, "%s/%u", log_ntoa(k->prefix), k->prefixlen) ==
		    -1)
			err(1, NULL);
		printf("%-20s ", p);
		free(p);

		if (k->nexthop)
			printf("%s", log_ntoa(k->nexthop));
		else if (k->flags & F_CONNECTED)
			printf("link#%u", k->ifindex);
		printf("\n");

		break;
	case IMSG_CTL_END:
		return (1);
		break;
	default:
		break;
	}

	return (0);
}

void
show_nexthop_head(void)
{
	printf("%-20s %s\n", "Nexthop", "State");
}

int
show_nexthop_msg(struct imsg *imsg)
{
	struct ctl_show_nexthop	*p;

	switch (imsg->hdr.type) {
	case IMSG_CTL_SHOW_NEXTHOP:
		p = imsg->data;
		printf("%-20s %s\n", inet_ntoa(p->addr.v4),
		    p->valid ? "valid" : "invalid");
		break;
	case IMSG_CTL_END:
		return (1);
		break;
	default:
		break;
	}

	return (0);
}


void
show_interface_head(void)
{
	printf("%-20s%-20s%-20s\n", "Interface", "Flags", "Link state");
}

const int	ifm_status_valid_list[] = IFM_STATUS_VALID_LIST;
const struct ifmedia_status_description
		ifm_status_descriptions[] = IFM_STATUS_DESCRIPTIONS;
const struct ifmedia_description
		ifm_type_descriptions[] = IFM_TYPE_DESCRIPTIONS;

const char *
get_media_descr(int media_type)
{
	const struct ifmedia_description	*p;

	for (p = ifm_type_descriptions; p->ifmt_string != NULL; p++)
		if (media_type == p->ifmt_word)
			return (p->ifmt_string);

	return ("unknown media");
}

const char *
get_linkstate(int media_type, int link_state)
{
	const struct ifmedia_status_description	*p;
	int					 i;

	if (link_state == LINK_STATE_UNKNOWN)
		return ("unknown");

	for (i = 0; ifm_status_valid_list[i] != 0; i++)
		for (p = ifm_status_descriptions; p->ifms_valid != 0; p++) {
			if (p->ifms_type != media_type ||
			    p->ifms_valid != ifm_status_valid_list[i])
				continue;
			return (p->ifms_string[link_state == LINK_STATE_UP]);
		}

	return ("unknown link state");
}

void
print_baudrate(u_long baudrate)
{
	if (baudrate > IF_Gbps(1))
		printf("%lu GBit/s", baudrate / IF_Gbps(1));
	else if (baudrate > IF_Mbps(1))
		printf("%lu MBit/s", baudrate / IF_Mbps(1));
	else if (baudrate > IF_Kbps(1))
		printf("%lu KBit/s", baudrate / IF_Kbps(1));
	else
		printf("%lu Bit/s", baudrate);
}


int
show_interface_msg(struct imsg *imsg)
{
	struct kif	*k;
	int		 ifms_type;

	switch (imsg->hdr.type) {
	case IMSG_CTL_SHOW_INTERFACE:
		k = imsg->data;
		printf("%-20s", k->ifname);
		printf("%-20s", k->flags & IFF_UP ? "UP" : "");
		switch (k->media_type) {
		case IFT_ETHER:
			ifms_type = IFM_ETHER;
			break;
		case IFT_FDDI:
			ifms_type = IFM_FDDI;
			break;
		case IFT_ISO88025:
			ifms_type = IFM_TOKEN;
			break;
		default:
			ifms_type = 0;
			break;
		}

		if (ifms_type)
			printf("%s, %s", get_media_descr(ifms_type),
			    get_linkstate(ifms_type, k->link_state));
		else if (k->link_state == LINK_STATE_UNKNOWN)
			printf("unknown");
		else
			printf("link state %u", k->link_state);

		if (k->link_state != LINK_STATE_DOWN && k->baudrate > 0) {
			printf(", ");
			print_baudrate(k->baudrate);
		}
		printf("\n");
		break;
	case IMSG_CTL_END:
		return (1);
		break;
	default:
		break;
	}

	return (0);
}
