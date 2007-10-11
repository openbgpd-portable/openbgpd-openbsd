/*	$OpenBSD: res_debug.c,v 1.22 2007/10/11 18:36:41 jakob Exp $	*/

/*
 * ++Copyright++ 1985, 1990, 1993
 * -
 * Copyright (c) 1985, 1990, 1993
 *    The Regents of the University of California.  All rights reserved.
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
 * -
 * Portions Copyright (c) 1993 by Digital Equipment Corporation.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies, and that
 * the name of Digital Equipment Corporation not be used in advertising or
 * publicity pertaining to distribution of the document or software without
 * specific, written prior permission.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND DIGITAL EQUIPMENT CORP. DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS.   IN NO EVENT SHALL DIGITAL EQUIPMENT
 * CORPORATION BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 * -
 * Portions Copyright (c) 1995 by International Business Machines, Inc.
 *
 * International Business Machines, Inc. (hereinafter called IBM) grants
 * permission under its copyrights to use, copy, modify, and distribute this
 * Software with or without fee, provided that the above copyright notice and
 * all paragraphs of this notice appear in all copies, and that the name of IBM
 * not be used in connection with the marketing of any product incorporating
 * the Software or modifications thereof, without specific, written prior
 * permission.
 *
 * To the extent it has a right to do so, IBM grants an immunity from suit
 * under its patents, if any, for the use, sale or manufacture of products to
 * the extent that such products are used for performing Domain Name System
 * dynamic updates in TCP/IP networks by means of the Software.  No immunity is
 * granted for any product per se or for any other function of any product.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", AND IBM DISCLAIMS ALL WARRANTIES,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE.  IN NO EVENT SHALL IBM BE LIABLE FOR ANY SPECIAL,
 * DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE, EVEN
 * IF IBM IS APPRISED OF THE POSSIBILITY OF SUCH DAMAGES.
 * --Copyright--
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>

#include <ctype.h>
#include <netdb.h>
#include <resolv.h>
#include <stdio.h>
#include <time.h>

#include <stdlib.h>
#include <string.h>

#include "thread_private.h"

extern const char *_res_opcodes[];
extern const char *_res_resultcodes[];

static const char *loc_ntoal(const u_char *binary, char *ascii, int ascii_len);

/* XXX: we should use getservbyport() instead. */
static const char *
dewks(int wks)
{
	static char nbuf[20];

	switch (wks) {
	case 5: return "rje";
	case 7: return "echo";
	case 9: return "discard";
	case 11: return "systat";
	case 13: return "daytime";
	case 15: return "netstat";
	case 17: return "qotd";
	case 19: return "chargen";
	case 20: return "ftp-data";
	case 21: return "ftp";
	case 23: return "telnet";
	case 25: return "smtp";
	case 37: return "time";
	case 39: return "rlp";
	case 42: return "name";
	case 43: return "whois";
	case 53: return "domain";
	case 57: return "apts";
	case 59: return "apfs";
	case 67: return "bootps";
	case 68: return "bootpc";
	case 69: return "tftp";
	case 77: return "rje";
	case 79: return "finger";
	case 87: return "link";
	case 95: return "supdup";
	case 100: return "newacct";
	case 101: return "hostnames";
	case 102: return "iso-tsap";
	case 103: return "x400";
	case 104: return "x400-snd";
	case 105: return "csnet-ns";
	case 109: return "pop-2";
	case 111: return "sunrpc";
	case 113: return "auth";
	case 115: return "sftp";
	case 117: return "uucp-path";
	case 119: return "nntp";
	case 121: return "erpc";
	case 123: return "ntp";
	case 133: return "statsrv";
	case 136: return "profile";
	case 144: return "NeWS";
	case 161: return "snmp";
	case 162: return "snmp-trap";
	case 170: return "print-srv";
	default:
		(void) snprintf(nbuf, sizeof nbuf, "%d", wks);
		return (nbuf);
	}
}

/* XXX: we should use getprotobynumber() instead. */
static const char *
deproto(int protonum)
{
	static char nbuf[20];

	switch (protonum) {
	case 1: return "icmp";
	case 2: return "igmp";
	case 3: return "ggp";
	case 5: return "st";
	case 6: return "tcp";
	case 7: return "ucl";
	case 8: return "egp";
	case 9: return "igp";
	case 11: return "nvp-II";
	case 12: return "pup";
	case 16: return "chaos";
	case 17: return "udp";
	default:
		(void) snprintf(nbuf, sizeof nbuf, "%d", protonum);
		return (nbuf);
	}
}

static const u_char *
do_rrset(const u_char *msg, int len, const u_char *cp, int cnt, int pflag,
    FILE *file, const char *hs)
{
	struct __res_state *_resp = _THREAD_PRIVATE(_res, _res, &_res);
	int n;
	int sflag;

	/*
	 * Print answer records.
	 */
	sflag = (_resp->pfcode & pflag);
	if ((n = ntohs(cnt))) {
		if ((!_resp->pfcode) ||
		    ((sflag) && (_resp->pfcode & RES_PRF_HEAD1)))
			fprintf(file, "%s", hs);
		while (--n >= 0) {
			if ((!_resp->pfcode) || sflag) {
				cp = p_rr(cp, msg, file);
			} else {
				unsigned int dlen;
				cp += __dn_skipname(cp, cp + MAXCDNAME);
				cp += INT16SZ;
				cp += INT16SZ;
				cp += INT32SZ;
				dlen = _getshort((u_char*)cp);
				cp += INT16SZ;
				cp += dlen;
			}
			if ((cp - msg) > len)
				return (NULL);
		}
		if ((!_resp->pfcode) ||
		    ((sflag) && (_resp->pfcode & RES_PRF_HEAD1)))
			putc('\n', file);
	}
	return (cp);
}

void
__p_query(const u_char *msg)
{
	__fp_query(msg, stdout);
}

/*
 * Print the current options.
 * This is intended to be primarily a debugging routine.
 */
void
__fp_resstat(struct __res_state *statp, FILE *file)
{
	u_long mask;

	fprintf(file, ";; res options:");
	if (!statp)
		statp = &_res;
	for (mask = 1;  mask != 0;  mask <<= 1)
		if (statp->options & mask)
			fprintf(file, " %s", p_option(mask));
	putc('\n', file);
}

/*
 * Print the contents of a query.
 * This is intended to be primarily a debugging routine.
 */
void
__fp_nquery(const u_char *msg, int len, FILE *file)
{
	struct __res_state *_resp = _THREAD_PRIVATE(_res, _res, &_res);
	const u_char *cp, *endMark;
	const HEADER *hp;
	int n;

	if (_res_init(0) == -1)
		return;

#define TruncTest(x) if (x > endMark) goto trunc
#define	ErrorTest(x) if (x == NULL) goto error

	/*
	 * Print header fields.
	 */
	hp = (HEADER *)msg;
	cp = msg + HFIXEDSZ;
	endMark = msg + len;
	if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_HEADX) || hp->rcode) {
		fprintf(file, ";; ->>HEADER<<- opcode: %s, status: %s, id: %u",
			_res_opcodes[hp->opcode],
			_res_resultcodes[hp->rcode],
			ntohs(hp->id));
		putc('\n', file);
	}
	if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_HEADX))
		putc(';', file);
	if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_HEAD2)) {
		fprintf(file, "; flags:");
		if (hp->qr)
			fprintf(file, " qr");
		if (hp->aa)
			fprintf(file, " aa");
		if (hp->tc)
			fprintf(file, " tc");
		if (hp->rd)
			fprintf(file, " rd");
		if (hp->ra)
			fprintf(file, " ra");
		if (hp->unused)
			fprintf(file, " UNUSED-BIT-ON");
		if (hp->ad)
			fprintf(file, " ad");
		if (hp->cd)
			fprintf(file, " cd");
	}
	if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_HEAD1)) {
		fprintf(file, "; Ques: %u", ntohs(hp->qdcount));
		fprintf(file, ", Ans: %u", ntohs(hp->ancount));
		fprintf(file, ", Auth: %u", ntohs(hp->nscount));
		fprintf(file, ", Addit: %u", ntohs(hp->arcount));
	}
	if ((!_resp->pfcode) || (_resp->pfcode & 
		(RES_PRF_HEADX | RES_PRF_HEAD2 | RES_PRF_HEAD1))) {
		putc('\n',file);
	}
	/*
	 * Print question records.
	 */
	if ((n = ntohs(hp->qdcount))) {
		if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_QUES))
			fprintf(file, ";; QUESTIONS:\n");
		while (--n >= 0) {
			if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_QUES))
				fprintf(file, ";;\t");
			TruncTest(cp);
			if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_QUES))
				cp = p_cdnname(cp, msg, len, file);
			else {
				int n;
				char name[MAXDNAME];

				if ((n = dn_expand(msg, msg+len, cp, name,
						sizeof name)) < 0)
					cp = NULL;
				else
					cp += n;
			}
			ErrorTest(cp);
			TruncTest(cp);
			if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_QUES))
				fprintf(file, ", type = %s",
					__p_type(_getshort((u_char*)cp)));
			cp += INT16SZ;
			TruncTest(cp);
			if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_QUES))
				fprintf(file, ", class = %s\n",
					__p_class(_getshort((u_char*)cp)));
			cp += INT16SZ;
			if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_QUES))
				putc('\n', file);
		}
	}
	/*
	 * Print authoritative answer records
	 */
	TruncTest(cp);
	cp = do_rrset(msg, len, cp, hp->ancount, RES_PRF_ANS, file,
		      ";; ANSWERS:\n");
	ErrorTest(cp);

	/*
	 * print name server records
	 */
	TruncTest(cp);
	cp = do_rrset(msg, len, cp, hp->nscount, RES_PRF_AUTH, file,
		      ";; AUTHORITY RECORDS:\n");
	ErrorTest(cp);

	TruncTest(cp);
	/*
	 * print additional records
	 */
	cp = do_rrset(msg, len, cp, hp->arcount, RES_PRF_ADD, file,
		      ";; ADDITIONAL RECORDS:\n");
	ErrorTest(cp);
	return;
 trunc:
	fprintf(file, "\n;; ...truncated\n");
	return;
 error:
	fprintf(file, "\n;; ...malformed\n");
}

void
__fp_query(const u_char *msg, FILE *file)
{
	fp_nquery(msg, PACKETSZ, file);
}

const u_char *
__p_cdnname(const u_char *cp, const u_char *msg, int len, FILE *file)
{
	char name[MAXDNAME];
	int n;

	if ((n = dn_expand(msg, msg + len, cp, name, sizeof name)) < 0)
		return (NULL);
	if (name[0] == '\0')
		putc('.', file);
	else
		fputs(name, file);
	return (cp + n);
}

const u_char *
__p_cdname(const u_char *cp, const u_char *msg, FILE *file)
{
	return (p_cdnname(cp, msg, PACKETSZ, file));
}


/* Return a fully-qualified domain name from a compressed name (with
   length supplied).  */

const u_char *
__p_fqnname(const u_char *cp, const u_char *msg, int msglen, char *name, int namelen)
{
	int n, newlen;

	if ((n = dn_expand(msg, cp + msglen, cp, name, namelen)) < 0)
		return (NULL);
	newlen = strlen(name);
	if (newlen == 0 || name[newlen - 1] != '.') {
		if (newlen + 1 >= namelen)	/* Lack space for final dot */
			return (NULL);
		else
			strlcpy(name + newlen, ".", namelen - newlen);
	}
	return (cp + n);
}

/* XXX:	the rest of these functions need to become length-limited, too. (vix)
 */

const u_char *
__p_fqname(const u_char *cp, const u_char *msg, FILE *file)
{
	char name[MAXDNAME];
	const u_char *n;

	n = __p_fqnname(cp, msg, MAXCDNAME, name, sizeof name);
	if (n == NULL)
		return (NULL);
	fputs(name, file);
	return (n);
}

/*
 * Print resource record fields in human readable form.
 */
const u_char *
__p_rr(const u_char *cp, const u_char *msg, FILE *file)
{
	struct __res_state *_resp = _THREAD_PRIVATE(_res, _res, &_res);
	int type, class, dlen, n, c;
	struct in_addr inaddr;
	const u_char *cp1, *cp2;
	u_int32_t tmpttl, t;
	int lcnt;
	u_int16_t keyflags;
	char rrname[MAXDNAME];		/* The fqdn of this RR */
	char base64_key[MAX_KEY_BASE64];

	if (_res_init(0) == -1) {
		h_errno = NETDB_INTERNAL;
		return (NULL);
	}
	cp = __p_fqnname(cp, msg, MAXCDNAME, rrname, sizeof rrname);
	if (!cp)
		return (NULL);			/* compression error */
	fputs(rrname, file);
	
	type = _getshort((u_char*)cp);
	cp += INT16SZ;
	class = _getshort((u_char*)cp);
	cp += INT16SZ;
	tmpttl = _getlong((u_char*)cp);
	cp += INT32SZ;
	dlen = _getshort((u_char*)cp);
	cp += INT16SZ;
	cp1 = cp;
	if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_TTLID))
		fprintf(file, "\t%lu", (u_long)tmpttl);
	if ((!_resp->pfcode) || (_resp->pfcode & RES_PRF_CLASS))
		fprintf(file, "\t%s", __p_class(class));
	fprintf(file, "\t%s", __p_type(type));
	/*
	 * Print type specific data, if appropriate
	 */
	switch (type) {
	case T_A:
		switch (class) {
		case C_IN:
		case C_HS:
			bcopy(cp, (char *)&inaddr, INADDRSZ);
			if (dlen == 4) {
				fprintf(file, "\t%s", inet_ntoa(inaddr));
				cp += dlen;
			} else if (dlen == 7) {
				char *address;
				u_char protocol;
				in_port_t port;

				address = inet_ntoa(inaddr);
				cp += INADDRSZ;
				protocol = *(u_char*)cp;
				cp += sizeof (u_char);
				port = _getshort((u_char*)cp);
				cp += INT16SZ;
				fprintf(file, "\t%s\t; proto %u, port %u",
					address, protocol, port);
			}
			break;
		default:
			cp += dlen;
		}
		break;
	case T_CNAME:
	case T_MB:
	case T_MG:
	case T_MR:
	case T_NS:
	case T_PTR:
		putc('\t', file);
		if ((cp = p_fqname(cp, msg, file)) == NULL)
			return (NULL);
		break;

	case T_HINFO:
	case T_ISDN:
		cp2 = cp + dlen;
		(void) fputs("\t\"", file);
		if ((n = (unsigned char) *cp++) != 0) {
			for (c = n; c > 0 && cp < cp2; c--) {
				if (strchr("\n\"\\", *cp))
					(void) putc('\\', file);
				(void) putc(*cp++, file);
			}
		}
		putc('"', file);
		if (cp < cp2 && (n = (unsigned char) *cp++) != 0) {
			(void) fputs ("\t\"", file);
			for (c = n; c > 0 && cp < cp2; c--) {
				if (strchr("\n\"\\", *cp))
					(void) putc('\\', file);
				(void) putc(*cp++, file);
			}
			putc('"', file);
		} else if (type == T_HINFO) {
			(void) fputs("\"?\"", file);
			fprintf(file, "\n;; *** Warning *** OS-type missing");
		}
		break;

	case T_SOA:
		putc('\t', file);
		if ((cp = p_fqname(cp, msg, file)) == NULL)
			return (NULL);
		putc(' ', file);
		if ((cp = p_fqname(cp, msg, file)) == NULL)
			return (NULL);
		fputs(" (\n", file);
		t = _getlong((u_char*)cp);  cp += INT32SZ;
		fprintf(file, "\t\t\t%lu\t; serial\n", (u_long)t);
		t = _getlong((u_char*)cp);  cp += INT32SZ;
		fprintf(file, "\t\t\t%lu\t; refresh (%s)\n",
			(u_long)t, __p_time(t));
		t = _getlong((u_char*)cp);  cp += INT32SZ;
		fprintf(file, "\t\t\t%lu\t; retry (%s)\n",
			(u_long)t, __p_time(t));
		t = _getlong((u_char*)cp);  cp += INT32SZ;
		fprintf(file, "\t\t\t%lu\t; expire (%s)\n",
			(u_long)t, __p_time(t));
		t = _getlong((u_char*)cp);  cp += INT32SZ;
		fprintf(file, "\t\t\t%lu )\t; minimum (%s)",
			(u_long)t, __p_time(t));
		break;

	case T_MX:
	case T_AFSDB:
	case T_RT:
		fprintf(file, "\t%u ", _getshort((u_char*)cp));
		cp += INT16SZ;
		if ((cp = p_fqname(cp, msg, file)) == NULL)
			return (NULL);
		break;

	case T_PX:
		fprintf(file, "\t%u ", _getshort((u_char*)cp));
		cp += INT16SZ;
		if ((cp = p_fqname(cp, msg, file)) == NULL)
			return (NULL);
		putc(' ', file);
		if ((cp = p_fqname(cp, msg, file)) == NULL)
			return (NULL);
		break;

	case T_X25:
		cp2 = cp + dlen;
		(void) fputs("\t\"", file);
		if ((n = (unsigned char) *cp++) != 0) {
			for (c = n; c > 0 && cp < cp2; c--) {
				if (strchr("\n\"\\", *cp))
					(void) putc('\\', file);
				(void) putc(*cp++, file);
			}
		}
		putc('"', file);
		break;

	case T_TXT:
		(void) putc('\t', file);
		cp2 = cp1 + dlen;
		while (cp < cp2) {
			putc('"', file);
			if ((n = (unsigned char) *cp++)) {
				for (c = n; c > 0 && cp < cp2; c--) {
					if (strchr("\n\"\\", *cp))
						(void) putc('\\', file);
					(void) putc(*cp++, file);
				}
			}
			putc('"', file);
			if (cp < cp2)
				putc(' ', file);
		}
		break;

	case T_NSAP:
		(void) fprintf(file, "\t%s", inet_nsap_ntoa(dlen, cp, NULL));
		cp += dlen;
		break;

	case T_AAAA: {
		char t[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"];

		fprintf(file, "\t%s", inet_ntop(AF_INET6, cp, t, sizeof t));
		cp += dlen;
		break;
	    }

	case T_LOC: {
		char t[255];

		fprintf(file, "\t%s", loc_ntoal(cp, t, sizeof t));
		cp += dlen;
		break;
	    }

	case T_NAPTR: {
		u_int order, preference;

		order = _getshort(cp);  cp += INT16SZ;
		preference   = _getshort(cp);  cp += INT16SZ;
		fprintf(file, "\t%u %u ",order, preference);
		/* Flags */
		n = *cp++;
		fprintf(file,"\"%.*s\" ", (int)n, cp);
		cp += n;
		/* Service */
		n = *cp++;
		fprintf(file,"\"%.*s\" ", (int)n, cp);
		cp += n;
		/* Regexp */
		n = *cp++;
		fprintf(file,"\"%.*s\" ", (int)n, cp);
		cp += n;
		if ((cp = p_fqname(cp, msg, file)) == NULL)
			return (NULL);
		break;
	    }

	case T_SRV: {
		u_int priority, weight, port;

		priority = _getshort(cp);  cp += INT16SZ;
		weight   = _getshort(cp);  cp += INT16SZ;
		port     = _getshort(cp);  cp += INT16SZ;
		fprintf(file, "\t%u %u %u ", priority, weight, port);
		if ((cp = p_fqname(cp, msg, file)) == NULL)
			return (NULL);
		break;
	    }

	case T_MINFO:
	case T_RP:
		putc('\t', file);
		if ((cp = p_fqname(cp, msg, file)) == NULL)
			return (NULL);
		putc(' ', file);
		if ((cp = p_fqname(cp, msg, file)) == NULL)
			return (NULL);
		break;

	case T_UINFO:
		putc('\t', file);
		fputs((char *)cp, file);
		cp += dlen;
		break;

	case T_UID:
	case T_GID:
		if (dlen == 4) {
			fprintf(file, "\t%u", _getlong((u_char*)cp));
			cp += INT32SZ;
		}
		break;

	case T_WKS:
		if (dlen < INT32SZ + 1)
			break;
		bcopy(cp, (char *)&inaddr, INADDRSZ);
		cp += INT32SZ;
		fprintf(file, "\t%s %s ( ",
			inet_ntoa(inaddr),
			deproto((int) *cp));
		cp += sizeof (u_char);
		n = 0;
		lcnt = 0;
		while (cp < cp1 + dlen) {
			c = *cp++;
			do {
				if (c & 0200) {
					if (lcnt == 0) {
						fputs("\n\t\t\t", file);
						lcnt = 5;
					}
					fputs(dewks(n), file);
					putc(' ', file);
					lcnt--;
				}
				c <<= 1;
			} while (++n & 07);
		}
		putc(')', file);
		break;

	case T_KEY:
		putc('\t', file);
		keyflags = _getshort(cp);
		cp += 2;
		fprintf(file,"0x%04x", keyflags );	/* flags */
		fprintf(file," %u", *cp++);	/* protocol */
		fprintf(file," %u (", *cp++);	/* algorithm */

		n = b64_ntop(cp, (cp1 + dlen) - cp,
			     base64_key, sizeof base64_key);
		for (c = 0; c < n; ++c) {
			if (0 == (c & 0x3F))
				fprintf(file, "\n\t");
			putc(base64_key[c], file);  /* public key data */
		}

		fprintf(file, " )");
		if (n < 0)
			fprintf(file, "\t; BAD BASE64");
		fflush(file);
		cp = cp1 + dlen;
		break;

	case T_SIG:
	case T_RRSIG:
	        type = _getshort((u_char*)cp);
		cp += INT16SZ;
		fprintf(file, " %s", p_type(type));
		fprintf(file, "\t%u", *cp++);	/* algorithm */
		/* Check label value and print error if wrong. */
		n = *cp++;
		c = dn_count_labels (rrname);
		if (n != c)
			fprintf(file, "\t; LABELS WRONG (%d should be %d)\n\t",
				n, c);
		/* orig ttl */
		n = _getlong((u_char*)cp);
		if (n != tmpttl)
			fprintf(file, " %u", n);
		cp += INT32SZ;
		/* sig expire */
		fprintf(file, " (\n\t%s",
				     __p_secstodate(_getlong((u_char*)cp)));
		cp += INT32SZ;
		/* time signed */
		fprintf(file, " %s", __p_secstodate(_getlong((u_char*)cp)));
		cp += INT32SZ;
		/* sig footprint */
		fprintf(file," %u ", _getshort((u_char*)cp));
		cp += INT16SZ;
		/* signer's name */
		cp = p_fqname(cp, msg, file);
		n = b64_ntop(cp, (cp1 + dlen) - cp,
			     base64_key, sizeof base64_key);
		for (c = 0; c < n; c++) {
			if (0 == (c & 0x3F))
				fprintf (file, "\n\t");
			putc(base64_key[c], file);		/* signature */
		}
		/* Clean up... */
		fprintf(file, " )");
		if (n < 0)
			fprintf(file, "\t; BAD BASE64");
		fflush(file);
		cp = cp1+dlen;
		break;

#ifdef ALLOW_T_UNSPEC
	case T_UNSPEC:
		{
			int NumBytes = 8;
			u_char *DataPtr;
			int i;

			if (dlen < NumBytes) NumBytes = dlen;
			fprintf(file, "\tFirst %d bytes of hex data:",
				NumBytes);
			for (i = 0, DataPtr = cp; i < NumBytes; i++, DataPtr++)
				fprintf(file, " %x", *DataPtr);
			cp += dlen;
		}
		break;
#endif /* ALLOW_T_UNSPEC */

	default:
		fprintf(file, "\t?%d?", type);
		cp += dlen;
	}
#if 0
	fprintf(file, "\t; dlen=%d, ttl %s\n", dlen, __p_time(tmpttl));
#else
	putc('\n', file);
#endif
	if (cp - cp1 != dlen) {
		fprintf(file, ";; packet size error (found %ld, dlen was %d)\n",
			(long)(cp - cp1), dlen);
		cp = NULL;
	}
	return (cp);
}

int
__sym_ston(const struct res_sym *syms, char *name, int *success)
{
	for (; syms->name != 0; syms++) {
		if (strcasecmp (name, syms->name) == 0) {
			if (success)
				*success = 1;
			return (syms->number);
		}
	}
	if (success)
		*success = 0;
	return (syms->number);		/* The default value. */
}


const char *
__sym_ntop(const struct res_sym *syms, int number, int *success)
{
	static char unname[20];

	for (; syms->name != 0; syms++) {
		if (number == syms->number) {
			if (success)
				*success = 1;
			return (syms->humanname);
		}
	}
	snprintf(unname, sizeof unname, "%d", number);
	if (success)
		*success = 0;
	return (unname);
}

/*
 * Return a mnemonic for an option
 */
const char *
__p_option(u_long option)
{
	static char nbuf[40];

	switch (option) {
	case RES_INIT:		return "init";
	case RES_DEBUG:		return "debug";
	case RES_AAONLY:	return "aaonly(unimpl)";
	case RES_USEVC:		return "usevc";
	case RES_PRIMARY:	return "primry(unimpl)";
	case RES_IGNTC:		return "igntc";
	case RES_RECURSE:	return "recurs";
	case RES_DEFNAMES:	return "defnam";
	case RES_STAYOPEN:	return "styopn";
	case RES_DNSRCH:	return "dnsrch";
	case RES_INSECURE1:	return "insecure1";
	case RES_INSECURE2:	return "insecure2";
	case RES_USE_INET6:	return "inet6";
	case RES_USE_EDNS0:	return "edns0";
	default:
		snprintf(nbuf, sizeof nbuf, "?0x%lx?", (u_long)option);
		return (nbuf);
	}
}

/*
 * Return a mnemonic for a time to live
 */
const char *
p_time(u_int32_t value)
{
	static char nbuf[40];
	char *ebuf;
	int secs, mins, hours, days;
	char *p;
	int tmp;

	if (value == 0) {
		strlcpy(nbuf, "0 secs", sizeof nbuf);
		return (nbuf);
	}

	secs = value % 60;
	value /= 60;
	mins = value % 60;
	value /= 60;
	hours = value % 24;
	value /= 24;
	days = value;
	value = 0;

#define	PLURALIZE(x)	x, (x == 1) ? "" : "s"
	p = nbuf;
	ebuf = nbuf + sizeof(nbuf);
	if (days) {
		if ((tmp = snprintf(p, ebuf - p, "%d day%s",
		    PLURALIZE(days))) >= ebuf - p || tmp < 0)
			goto full;
		p += tmp;
	}
	if (hours) {
		if (days)
			*p++ = ' ';
		if (p >= ebuf)
			goto full;
		if ((tmp = snprintf(p, ebuf - p, "%d hour%s",
		    PLURALIZE(hours))) >= ebuf - p || tmp < 0)
			goto full;
		p += tmp;
	}
	if (mins) {
		if (days || hours)
			*p++ = ' ';
		if (p >= ebuf)
			goto full;
		if ((tmp = snprintf(p, ebuf - p, "%d min%s",
		    PLURALIZE(mins))) >= ebuf - p || tmp < 0)
			goto full;
		p += tmp;
	}
	if (secs || ! (days || hours || mins)) {
		if (days || hours || mins)
			*p++ = ' ';
		if (p >= ebuf)
			goto full;
		if ((tmp = snprintf(p, ebuf - p, "%d sec%s",
		    PLURALIZE(secs))) >= ebuf - p || tmp < 0)
			goto full;
	}
	return (nbuf);
full:
	p = nbuf + sizeof(nbuf) - 4;
	*p++ = '.';
	*p++ = '.';
	*p++ = '.';
	*p++ = '\0';
	return (nbuf);
}

/*
 * routines to convert between on-the-wire RR format and zone file format.
 * Does not contain conversion to/from decimal degrees; divide or multiply
 * by 60*60*1000 for that.
 */

static unsigned int poweroften[10] = {1, 10, 100, 1000, 10000, 100000,
				      1000000,10000000,100000000,1000000000};

/* takes an XeY precision/size value, returns a string representation. */
static const char *
precsize_ntoa(u_int8_t prec)
{
	static char retbuf[sizeof "90000000.00"];
	unsigned long val;
	int mantissa, exponent;

	mantissa = (int)((prec >> 4) & 0x0f) % 10;
	exponent = (int)((prec >> 0) & 0x0f) % 10;

	val = mantissa * poweroften[exponent];

	(void) snprintf(retbuf, sizeof retbuf, "%ld.%.2ld", val/100, val%100);
	return (retbuf);
}

/* converts ascii size/precision X * 10**Y(cm) to 0xXY.  moves pointer. */
static u_int8_t
precsize_aton(char **strptr)
{
	unsigned int mval = 0, cmval = 0;
	u_int8_t retval = 0;
	char *cp;
	int exponent;
	int mantissa;

	cp = *strptr;

	while (isdigit(*cp))
		mval = mval * 10 + (*cp++ - '0');

	if (*cp == '.') {		/* centimeters */
		cp++;
		if (isdigit(*cp)) {
			cmval = (*cp++ - '0') * 10;
			if (isdigit(*cp)) {
				cmval += (*cp++ - '0');
			}
		}
	}
	cmval = (mval * 100) + cmval;

	for (exponent = 0; exponent < 9; exponent++)
		if (cmval < poweroften[exponent+1])
			break;

	mantissa = cmval / poweroften[exponent];
	if (mantissa > 9)
		mantissa = 9;

	retval = (mantissa << 4) | exponent;

	*strptr = cp;

	return (retval);
}

/* converts ascii lat/lon to unsigned encoded 32-bit number.  moves pointer. */
static u_int32_t
latlon2ul(char **latlonstrptr, int *which)
{
	char *cp;
	u_int32_t retval;
	int deg = 0, min = 0, secs = 0, secsfrac = 0;

	cp = *latlonstrptr;

	while (isdigit(*cp))
		deg = deg * 10 + (*cp++ - '0');

	while (isspace(*cp))
		cp++;

	if (!(isdigit(*cp)))
		goto fndhemi;

	while (isdigit(*cp))
		min = min * 10 + (*cp++ - '0');

	while (isspace(*cp))
		cp++;

	if (!(isdigit(*cp)))
		goto fndhemi;

	while (isdigit(*cp))
		secs = secs * 10 + (*cp++ - '0');

	if (*cp == '.') {		/* decimal seconds */
		cp++;
		if (isdigit(*cp)) {
			secsfrac = (*cp++ - '0') * 100;
			if (isdigit(*cp)) {
				secsfrac += (*cp++ - '0') * 10;
				if (isdigit(*cp)) {
					secsfrac += (*cp++ - '0');
				}
			}
		}
	}

	while (!isspace(*cp))	/* if any trailing garbage */
		cp++;

	while (isspace(*cp))
		cp++;

 fndhemi:
	switch (*cp) {
	case 'N': case 'n':
	case 'E': case 'e':
		retval = ((unsigned)1<<31)
			+ (((((deg * 60) + min) * 60) + secs) * 1000)
			+ secsfrac;
		break;
	case 'S': case 's':
	case 'W': case 'w':
		retval = ((unsigned)1<<31)
			- (((((deg * 60) + min) * 60) + secs) * 1000)
			- secsfrac;
		break;
	default:
		retval = 0;	/* invalid value -- indicates error */
		break;
	}

	switch (*cp) {
	case 'N': case 'n':
	case 'S': case 's':
		*which = 1;	/* latitude */
		break;
	case 'E': case 'e':
	case 'W': case 'w':
		*which = 2;	/* longitude */
		break;
	default:
		*which = 0;	/* error */
		break;
	}

	cp++;			/* skip the hemisphere */

	while (!isspace(*cp))	/* if any trailing garbage */
		cp++;

	while (isspace(*cp))	/* move to next field */
		cp++;

	*latlonstrptr = cp;

	return (retval);
}

/* converts a zone file representation in a string to an RDATA on-the-wire
 * representation. */
int
loc_aton(const char *ascii, u_char *binary)
{
	const char *maxcp;
	u_char *bcp;
	char *cp;

	u_int32_t latit = 0, longit = 0, alt = 0;
	u_int32_t lltemp1 = 0, lltemp2 = 0;
	int altmeters = 0, altfrac = 0, altsign = 1;
	u_int8_t hp = 0x16;	/* default = 1e6 cm = 10000.00m = 10km */
	u_int8_t vp = 0x13;	/* default = 1e3 cm = 10.00m */
	u_int8_t siz = 0x12;	/* default = 1e2 cm = 1.00m */
	int which1 = 0, which2 = 0;

	cp = (char *)ascii;
	maxcp = cp + strlen(ascii);

	lltemp1 = latlon2ul(&cp, &which1);

	lltemp2 = latlon2ul(&cp, &which2);

	switch (which1 + which2) {
	case 3:			/* 1 + 2, the only valid combination */
		if ((which1 == 1) && (which2 == 2)) { /* normal case */
			latit = lltemp1;
			longit = lltemp2;
		} else if ((which1 == 2) && (which2 == 1)) { /* reversed */
			longit = lltemp1;
			latit = lltemp2;
		} else {	/* some kind of brokenness */
			return (0);
		}
		break;
	default:		/* we didn't get one of each */
		return (0);
	}

	/* altitude */
	if (*cp == '-') {
		altsign = -1;
		cp++;
	}
    
	if (*cp == '+')
		cp++;

	while (isdigit(*cp))
		altmeters = altmeters * 10 + (*cp++ - '0');

	if (*cp == '.') {		/* decimal meters */
		cp++;
		if (isdigit(*cp)) {
			altfrac = (*cp++ - '0') * 10;
			if (isdigit(*cp)) {
				altfrac += (*cp++ - '0');
			}
		}
	}

	alt = (10000000 + (altsign * (altmeters * 100 + altfrac)));

	while (!isspace(*cp) && (cp < maxcp)) /* if trailing garbage or m */
		cp++;

	while (isspace(*cp) && (cp < maxcp))
		cp++;

	if (cp >= maxcp)
		goto defaults;

	siz = precsize_aton(&cp);
	
	while (!isspace(*cp) && (cp < maxcp))	/* if trailing garbage or m */
		cp++;

	while (isspace(*cp) && (cp < maxcp))
		cp++;

	if (cp >= maxcp)
		goto defaults;

	hp = precsize_aton(&cp);

	while (!isspace(*cp) && (cp < maxcp))	/* if trailing garbage or m */
		cp++;

	while (isspace(*cp) && (cp < maxcp))
		cp++;

	if (cp >= maxcp)
		goto defaults;

	vp = precsize_aton(&cp);

 defaults:

	bcp = binary;
	*bcp++ = (u_int8_t) 0;	/* version byte */
	*bcp++ = siz;
	*bcp++ = hp;
	*bcp++ = vp;
	PUTLONG(latit,bcp);
	PUTLONG(longit,bcp);
	PUTLONG(alt,bcp);
    
	return (16);		/* size of RR in octets */
}

const char *
loc_ntoa(const u_char *binary, char *ascii)
{
	return loc_ntoal(binary, ascii, 255);
}

/* takes an on-the-wire LOC RR and formats it in a human readable format. */
static const char *
loc_ntoal(const u_char *binary, char *ascii, int ascii_len)
{
	static char *error = "?";
	const u_char *cp = binary;

	int latdeg, latmin, latsec, latsecfrac;
	int longdeg, longmin, longsec, longsecfrac;
	char northsouth, eastwest;
	int altmeters, altfrac, altsign;

	const int referencealt = 100000 * 100;

	int32_t latval, longval, altval;
	u_int32_t templ;
	u_int8_t sizeval, hpval, vpval, versionval;
    
	char *sizestr, *hpstr, *vpstr;

	versionval = *cp++;

	if (versionval) {
		snprintf(ascii, ascii_len, "; error: unknown LOC RR version");
		return (ascii);
	}

	sizeval = *cp++;

	hpval = *cp++;
	vpval = *cp++;

	GETLONG(templ, cp);
	latval = (templ - ((unsigned)1<<31));

	GETLONG(templ, cp);
	longval = (templ - ((unsigned)1<<31));

	GETLONG(templ, cp);
	if (templ < referencealt) { /* below WGS 84 spheroid */
		altval = referencealt - templ;
		altsign = -1;
	} else {
		altval = templ - referencealt;
		altsign = 1;
	}

	if (latval < 0) {
		northsouth = 'S';
		latval = -latval;
	} else
		northsouth = 'N';

	latsecfrac = latval % 1000;
	latval = latval / 1000;
	latsec = latval % 60;
	latval = latval / 60;
	latmin = latval % 60;
	latval = latval / 60;
	latdeg = latval;

	if (longval < 0) {
		eastwest = 'W';
		longval = -longval;
	} else
		eastwest = 'E';

	longsecfrac = longval % 1000;
	longval = longval / 1000;
	longsec = longval % 60;
	longval = longval / 60;
	longmin = longval % 60;
	longval = longval / 60;
	longdeg = longval;

	altfrac = altval % 100;
	altmeters = (altval / 100) * altsign;

	if ((sizestr = strdup(precsize_ntoa(sizeval))) == NULL)
		sizestr = error;
	if ((hpstr = strdup(precsize_ntoa(hpval))) == NULL)
		hpstr = error;
	if ((vpstr = strdup(precsize_ntoa(vpval))) == NULL)
		vpstr = error;

	snprintf(ascii, ascii_len,
	      "%d %.2d %.2d.%.3d %c %d %.2d %.2d.%.3d %c %d.%.2dm %sm %sm %sm",
		latdeg, latmin, latsec, latsecfrac, northsouth,
		longdeg, longmin, longsec, longsecfrac, eastwest,
		altmeters, altfrac, sizestr, hpstr, vpstr);

	if (sizestr != error)
		free(sizestr);
	if (hpstr != error)
		free(hpstr);
	if (vpstr != error)
		free(vpstr);

	return (ascii);
}


/* Return the number of DNS hierarchy levels in the name. */
int
__dn_count_labels(char *name)
{
	int i, len, count;

	len = strlen(name);

	for(i = 0, count = 0; i < len; i++) {
		if (name[i] == '.')
			count++;
	}

	/* don't count initial wildcard */
	if (name[0] == '*')
		if (count)
			count--;

	/* don't count the null label for root. */
	/* if terminating '.' not found, must adjust */
	/* count to include last label */
	if (len > 0 && name[len-1] != '.')
		count++;
	return (count);
}


/* 
 * Make dates expressed in seconds-since-Jan-1-1970 easy to read.  
 * SIG records are required to be printed like this, by the Secure DNS RFC.
 */
char *
__p_secstodate (long unsigned int secs)
{
	static char output[15];		/* YYYYMMDDHHMMSS and null */
	time_t clock = secs;
	struct tm *time;
	
	time = gmtime(&clock);
	time->tm_year += 1900;
	time->tm_mon += 1;
	snprintf(output, sizeof output, "%04d%02d%02d%02d%02d%02d",
		time->tm_year, time->tm_mon, time->tm_mday,
		time->tm_hour, time->tm_min, time->tm_sec);
	return (output);
}
