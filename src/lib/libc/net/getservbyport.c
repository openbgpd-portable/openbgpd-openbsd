/*
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if defined(LIBC_SCCS) && !defined(lint)
static char rcsid[] = "$OpenBSD: getservbyport.c,v 1.5 2004/10/17 20:24:23 millert Exp $";
#endif /* LIBC_SCCS and not lint */

#include <netdb.h>
#include <stdio.h>
#include <string.h>

struct servent *
getservbyport_r(int port, const char *proto, struct servent *se,
    struct servent_data *sd)
{
	struct servent *p;

	setservent_r(sd->stayopen, sd);
	while ((p = getservent_r(se, sd))) {
		if (p->s_port != port)
			continue;
		if (proto == 0 || strcmp(p->s_proto, proto) == 0)
			break;
	}
	if (!sd->stayopen && sd->fp != NULL) {
		fclose(sd->fp);
		sd->fp = NULL;
	}
	return (p);
}

struct servent *
getservbyport(int port, const char *proto)
{
	extern struct servent_data _servent_data;
	static struct servent serv;

        return (getservbyport_r(port, proto, &serv, &_servent_data));
}
