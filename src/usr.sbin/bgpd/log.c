/*	$OpenBSD: log.c,v 1.12 2003/12/30 13:03:27 henning Exp $ */

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
#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bgpd.h"
#include "session.h"

static const char *statenames[] = {
	"None",
	"Idle",
	"Connect",
	"Active",
	"OpenSent",
	"OpenConfirm",
	"Established"
};

static const char *eventnames[] = {
	"None",
	"Start",
	"Stop",
	"Connection opened",
	"Connection closed",
	"Connection open failed",
	"Fatal error",
	"ConnectRetryTimer expired",
	"HoldTimer expired",
	"KeepaliveTimer expired",
	"OPEN message received",
	"KEEPALIVE message received",
	"UPDATE message received",
	"NOTIFICATION received"
};

static const char *errnames[] = {
	"none",
	"Header error",
	"error in OPEN message",
	"error in UPDATE message",
	"HoldTimer expired",
	"Finite State Machine error",
	"Cease"
};

static const char *suberr_header_names[] = {
	"none",
	"synchronization error",
	"wrong length",
	"unknown message type"
};

static const char *suberr_open_names[] = {
	"none",
	"version mismatch",
	"AS unacceptable",
	"BGPID invalid",
	"optional parameter error",
	"Authentication error",
	"unacceptable holdtime"
};

static const char *suberr_update_names[] = {
	"none",
	"attribute list error",
	"unknown well-known attribute",
	"well-known attribute missing",
	"attribute flags error",
	"attribute length wrong",
	"origin unacceptable",
	"loop detected",
	"nexthop unacceptable",
	"optional attribute error",
	"network unacceptable",
	"AS-Path unacceptable"
};

static const char *procnames[] = {
	"parent",
	"SE",
	"RDE"
};

int	debug;

char	*log_fmt_peer(const struct peer *);

char *
log_fmt_peer(const struct peer *peer)
{
	char		*ip;
	char		*pfmt;

	ip = inet_ntoa(peer->conf.remote_addr.sin_addr);
	if (peer->conf.descr[0]) {
		if (asprintf(&pfmt, "neighbor %s (%s)", ip, peer->conf.descr) ==
		    -1)
			fatal(NULL);
	} else {
		if (asprintf(&pfmt, "neighbor %s", ip) == -1)
			fatal(NULL);
	}
	return (pfmt);
}

void
log_init(int n_debug)
{
	debug = n_debug;

	if (!debug)
		openlog("bgpd", LOG_PID | LOG_NDELAY, LOG_DAEMON);
}

void
logit(int pri, const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	vlog(pri, fmt, ap);
	va_end(ap);
}

void
vlog(int pri, const char *fmt, va_list ap)
{
	if (debug) {
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
	} else
		vsyslog(pri, fmt, ap);
}


void
log_peer_err(const struct peer *peer, const char *emsg, ...)
{
	char	*p, *nfmt;
	va_list	 ap;

	p = log_fmt_peer(peer);
	if (emsg == NULL) {
		if (asprintf(&nfmt, "%s: %s", p, strerror(errno)) == -1)
			fatal(NULL);
	} else {
		if (asprintf(&nfmt, "%s: %s: %s", p, emsg, strerror(errno)) ==
		    -1)
			fatal(NULL);
	}
	va_start(ap, emsg);
	vlog(LOG_CRIT, nfmt, ap);
	va_end(ap);
	free(p);
}

void
log_peer_errx(const struct peer *peer, const char *emsg, ...)
{
	char	*p, *nfmt;
	va_list	 ap;

	p = log_fmt_peer(peer);
	if (asprintf(&nfmt, "%s: %s", p, emsg) == -1)
		fatal(NULL);
	va_start(ap, emsg);
	vlog(LOG_CRIT, nfmt, ap);
	va_end(ap);
	free(p);
}

void
log_err(const char *emsg, ...)
{
	char	*nfmt;
	va_list	 ap;

	/* best effort to even work in out of memory situations */

	va_start(ap, emsg);

	if (emsg == NULL)
		logit(LOG_CRIT, "%s", strerror(errno));
	else {
		if (asprintf(&nfmt, "%s: %s", emsg, strerror(errno)) == -1) {
			/* we tried it... */
			vlog(LOG_CRIT, emsg, ap);
			logit(LOG_CRIT, "%s", strerror(errno));
		} else
			vlog(LOG_CRIT, nfmt, ap);
	}
	va_end(ap);
}

void
fatal(const char *emsg)
{
	if (emsg == NULL)
		logit(LOG_CRIT, "fatal in %s: %s", procnames[bgpd_process],
		    strerror(errno));
	else
		if (errno)
			logit(LOG_CRIT, "fatal in %s: %s: %s",
			    procnames[bgpd_process], emsg, strerror(errno));
		else
			logit(LOG_CRIT, "fatal in %s: %s",
			    procnames[bgpd_process], emsg);

	if (bgpd_process == PROC_MAIN)
		exit(1);
	else				/* parent copes via SIGCHLD */
		_exit(1);
}

void
fatalx(const char *emsg)
{
	errno = 0;
	fatal(emsg);
}

void
fatal_ensure(const char *file, int line, const char *cond)
{
	logit(LOG_CRIT, "ENSURE (%s) failed in file %s on line %d",
	    cond, file, line);

	/* XXX check which process we are and notify others! */
	sleep(10);
	_exit(1);
}

void
log_statechange(const struct peer *peer, enum session_state nstate,
    enum session_events event)
{
	char	*p;

	p = log_fmt_peer(peer);
	logit(LOG_INFO, "%s: state change %s -> %s, reason: %s",
	    p, statenames[peer->state], statenames[nstate], eventnames[event]);
	free(p);
}

void
log_notification(const struct peer *peer, u_int8_t errcode, u_int8_t subcode,
    u_char *data, u_int16_t datalen)
{
	char		*p;
	const char	*suberrname = NULL;
	int		 uk = 0;

	p = log_fmt_peer(peer);
	switch (errcode) {
	case ERR_HEADER:
		if (subcode > sizeof(suberr_header_names)/sizeof(char *))
			uk = 1;
		else
			suberrname = suberr_header_names[subcode];
		break;
	case ERR_OPEN:
		if (subcode > sizeof(suberr_open_names)/sizeof(char *))
			uk = 1;
		else
			suberrname = suberr_open_names[subcode];
		break;
	case ERR_UPDATE:
		if (subcode > sizeof(suberr_update_names)/sizeof(char *))
			uk = 1;
		else
			suberrname = suberr_update_names[subcode];
		break;
	case ERR_HOLDTIMEREXPIRED:
	case ERR_FSM:
	case ERR_CEASE:
		uk = 1;
		break;
	default:
		logit(LOG_CRIT, "%s: received notification, unknown errcode "
		    "%u, subcode %u", p, errcode, subcode);
		free(p);
		return;
	}

	if (uk)
		logit(LOG_CRIT,
		    "%s: received notification: %s, unknown subcode %u",
		    p, errnames[errcode], subcode);
	else {
		if (suberrname == NULL)
			logit(LOG_CRIT, "%s: received notification: %s",
			    p, errnames[errcode]);
		else
			logit(LOG_CRIT, "%s: received notification: %s, %s",
			    p, errnames[errcode], suberrname);
	}
	free(p);
}

void
log_conn_attempt(const struct peer *peer, struct in_addr remote)
{
	char *p;

	if (peer == NULL)	/* connection from non-peer, drop */
		logit(LOG_INFO, "connection from non-peer %s refused",
			    inet_ntoa(remote));
	else {
		p = log_fmt_peer(peer);
		logit(LOG_INFO, "Connection attempt from %s while session is "
		    "in state %s", p, statenames[peer->state]);
		free(p);
	}
}

char *
log_ntoa(in_addr_t ip)
{
	struct in_addr	ina;

	ina.s_addr = ip;
	return (inet_ntoa(ina));
}
