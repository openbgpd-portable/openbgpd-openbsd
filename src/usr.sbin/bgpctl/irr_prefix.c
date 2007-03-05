/*	$OpenBSD: irr_prefix.c,v 1.6 2007/03/05 13:45:52 henning Exp $ */

/*
 * Copyright (c) 2007 Henning Brauer <henning@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "irrfilter.h"

void	 prefixset_aggregate(struct prefix_set *);
int	 prefix_aggregate(struct irr_prefix *, const struct irr_prefix *);
int	 prefix_compare(const void *, const void *);
int	 prefix_set_compare(struct prefix_set *, struct prefix_set *);
struct prefix_set
	*prefix_set_find(char *);

RB_HEAD(prefix_set_h, prefix_set)	prefix_set_h;
RB_PROTOTYPE(prefix_set_h, prefix_set, entry, prefix_set_compare)
RB_GENERATE(prefix_set_h, prefix_set, entry, prefix_set_compare)

struct prefix_set	*curpfxs = NULL;

struct prefix_set *
prefixset_get(char *as)
{
	struct prefix_set	*pfxs;
	int			 r;

	if ((pfxs = prefix_set_find(as)) != NULL)
		return (pfxs);

	/* nothing found, resolve and store */
	if ((pfxs = calloc(1, sizeof(*pfxs))) == NULL)
		err(1, "get_prefixset calloc");
	if ((pfxs->as = strdup(as)) == NULL)
		err(1, "get_prefixset strdup");
	RB_INSERT(prefix_set_h, &prefix_set_h, pfxs);

	curpfxs = pfxs;
	if ((r = whois(as, QTYPE_ROUTE)) == -1)
		errx(1, "whois error, prefixset_get %s", as);
	curpfxs = NULL;

	prefixset_aggregate(pfxs);

	return (pfxs);
}

int
prefixset_addmember(char *s)
{
	void			*p;
	u_int			 i;
	struct irr_prefix	*pfx;
	int			 len;

	if (strchr(s, '/') == NULL)
		errx(1, "prefix %s does not have the len specified", s);

	if ((pfx = calloc(1, sizeof(*pfx))) == NULL)
		err(1, "prefixset_addmember calloc");

	if ((len = inet_net_pton(AF_INET, s, &pfx->addr.in,
	    sizeof(pfx->addr.in))) == -1)
		err(1, "inet_net_pton %s", s);

	pfx->af = AF_INET;
	pfx->len = len;

	/* yes, there are dupes... e. g. from multiple sources */
	for (i = 0; i < curpfxs->prefixcnt; i++)
		if (prefix_compare(&curpfxs->prefix[i], &pfx) == 0) {
			free(pfx);
			return (0);
		}

	if ((p = realloc(curpfxs->prefix,
	    (curpfxs->prefixcnt + 1) * sizeof(void *))) == NULL)
		err(1, "prefixset_addmember realloc");
	curpfxs->prefix = p;
	curpfxs->prefixcnt++;
	curpfxs->prefix[curpfxs->prefixcnt - 1] = pfx;

	return (1);
}

void
prefixset_aggregate(struct prefix_set *pfxs)
{
	u_int			 i, newcnt;
	int			 res;
	struct irr_prefix	*cur, *last;
	void			*p;

	qsort(pfxs->prefix, pfxs->prefixcnt, sizeof(void *), prefix_compare);

	last = cur = NULL;
	for (i = 0, newcnt = 0; i < pfxs->prefixcnt; i++) {
		cur = pfxs->prefix[i];
		if (last != NULL && last->af == cur->af) {
			if (cur->af == AF_INET)
				res = prefix_aggregate(last, cur);
			else
				res = 0;

			if (res == 1) {	/* cur is covered by last */
				if (cur->len > last->maxlen)
					last->maxlen = cur->len;
				free(pfxs->prefix[i]);
				pfxs->prefix[i] = cur = NULL;
			}
		}

		if (cur != NULL) {
			pfxs->prefix[newcnt++] = cur;
			last = cur;
		}
	}

	if (newcnt == pfxs->prefixcnt)
		return;

	if (0)
		printf("%s: prefix aggregation: %u -> %u\n",
		    pfxs->as, pfxs->prefixcnt, newcnt);

	if ((p = realloc(pfxs->prefix, newcnt * sizeof(void *))) == NULL)
		err(1, "prefixset_aggregate realloc");
	pfxs->prefix = p;
	pfxs->prefixcnt = newcnt;
}

int
prefix_aggregate(struct irr_prefix *a, const struct irr_prefix *b)
{
	in_addr_t	mask;

	if (a->len == 0)
		return (1);

	mask = htonl(0xffffffff << (32 - a->len));

	if ((a->addr.in.s_addr & mask) == (b->addr.in.s_addr & mask))
		return (1);

	return (0);
}

int
prefix_compare(const void *a, const void *b)
{
	const struct irr_prefix	*pa;
	const struct irr_prefix	*pb;
	int			 r;

	pa = *((const struct irr_prefix	* const *)a);
	pb = *((const struct irr_prefix	* const *)b);

	if ((r = pa->af - pb->af) != 0)
		return (r);

	if (pa->af == AF_INET) {
		if (ntohl(pa->addr.in.s_addr) <
		    ntohl(pb->addr.in.s_addr))
			return (-1);
		if (ntohl(pa->addr.in.s_addr) >
		    ntohl(pb->addr.in.s_addr))
			return (1);
	} else
		errx(1, "prefix_compare unknown af %u", pa->af);

	if ((r = pa->len - pb->len) != 0)
		return (r);

	return (0);
}

/* RB helpers */
int
prefix_set_compare(struct prefix_set *a, struct prefix_set *b)
{
	return (strcmp(a->as, b->as));
}

struct prefix_set *
prefix_set_find(char *as)
{
	struct prefix_set	s;

	s.as = as;
	return (RB_FIND(prefix_set_h, &prefix_set_h, &s));
}
