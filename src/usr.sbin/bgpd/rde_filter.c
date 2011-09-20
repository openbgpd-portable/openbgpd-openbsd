/*	$OpenBSD: rde_filter.c,v 1.67 2011/09/20 21:19:06 claudio Exp $ */

/*
 * Copyright (c) 2004 Claudio Jeker <claudio@openbsd.org>
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
#include <sys/queue.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "bgpd.h"
#include "rde.h"

int	rde_filter_match(struct filter_rule *, struct rde_aspath *,
	    struct bgpd_addr *, u_int8_t, struct rde_peer *);
int	filterset_equal(struct filter_set_head *, struct filter_set_head *);

enum filter_actions
rde_filter(u_int16_t ribid, struct rde_aspath **new, struct filter_head *rules,
    struct rde_peer *peer, struct rde_aspath *asp, struct bgpd_addr *prefix,
    u_int8_t prefixlen, struct rde_peer *from, enum directions dir)
{
	struct filter_rule	*f;
	enum filter_actions	 action = ACTION_ALLOW; /* default allow */

	if (new != NULL)
		*new = NULL;

	if (asp->flags & F_ATTR_PARSE_ERR)
		/*
	 	 * don't try to filter bad updates just deny them
		 * so they act as implicit withdraws
		 */
		return (ACTION_DENY);

	TAILQ_FOREACH(f, rules, entry) {
		if (dir != f->dir)
			continue;
		if (dir == DIR_IN && f->peer.ribid != ribid)
			continue;
		if (f->peer.groupid != 0 &&
		    f->peer.groupid != peer->conf.groupid)
			continue;
		if (f->peer.peerid != 0 &&
		    f->peer.peerid != peer->conf.id)
			continue;
		if (rde_filter_match(f, asp, prefix, prefixlen, peer)) {
			if (asp != NULL && new != NULL) {
				/* asp may get modified so create a copy */
				if (*new == NULL) {
					*new = path_copy(asp);
					/* ... and use the copy from now on */
					asp = *new;
				}
				rde_apply_set(asp, &f->set, prefix->aid,
				    from, peer);
			}
			if (f->action != ACTION_NONE)
				action = f->action;
			if (f->quick)
				return (action);
		}
	}
	return (action);
}

void
rde_apply_set(struct rde_aspath *asp, struct filter_set_head *sh,
    u_int8_t aid, struct rde_peer *from, struct rde_peer *peer)
{
	struct filter_set	*set;
	u_char			*np;
	int			 as, type;
	u_int32_t		 prep_as;
	u_int16_t		 nl;
	u_int8_t		 prepend;

	if (asp == NULL)
		return;

	TAILQ_FOREACH(set, sh, entry) {
		switch (set->type) {
		case ACTION_SET_LOCALPREF:
			asp->lpref = set->action.metric;
			break;
		case ACTION_SET_RELATIVE_LOCALPREF:
			if (set->action.relative > 0) {
				if (set->action.relative + asp->lpref <
				    asp->lpref)
					asp->lpref = UINT_MAX;
				else
					asp->lpref += set->action.relative;
			} else {
				if ((u_int32_t)-set->action.relative >
				    asp->lpref)
					asp->lpref = 0;
				else
					asp->lpref += set->action.relative;
			}
			break;
		case ACTION_SET_MED:
			asp->flags |= F_ATTR_MED | F_ATTR_MED_ANNOUNCE;
			asp->med = set->action.metric;
			break;
		case ACTION_SET_RELATIVE_MED:
			asp->flags |= F_ATTR_MED | F_ATTR_MED_ANNOUNCE;
			if (set->action.relative > 0) {
				if (set->action.relative + asp->med <
				    asp->med)
					asp->med = UINT_MAX;
				else
					asp->med += set->action.relative;
			} else {
				if ((u_int32_t)-set->action.relative >
				    asp->med)
					asp->med = 0;
				else
					asp->med += set->action.relative;
			}
			break;
		case ACTION_SET_WEIGHT:
			asp->weight = set->action.metric;
			break;
		case ACTION_SET_RELATIVE_WEIGHT:
			if (set->action.relative > 0) {
				if (set->action.relative + asp->weight <
				    asp->weight)
					asp->weight = UINT_MAX;
				else
					asp->weight += set->action.relative;
			} else {
				if ((u_int32_t)-set->action.relative >
				    asp->weight)
					asp->weight = 0;
				else
					asp->weight += set->action.relative;
			}
			break;
		case ACTION_SET_PREPEND_SELF:
			prep_as = rde_local_as();
			prepend = set->action.prepend;
			np = aspath_prepend(asp->aspath, prep_as, prepend, &nl);
			aspath_put(asp->aspath);
			asp->aspath = aspath_get(np, nl);
			free(np);
			break;
		case ACTION_SET_PREPEND_PEER:
			if (from == NULL)
				break;
			prep_as = from->conf.remote_as;
			prepend = set->action.prepend;
			np = aspath_prepend(asp->aspath, prep_as, prepend, &nl);
			aspath_put(asp->aspath);
			asp->aspath = aspath_get(np, nl);
			free(np);
			break;
		case ACTION_SET_NEXTHOP:
		case ACTION_SET_NEXTHOP_REJECT:
		case ACTION_SET_NEXTHOP_BLACKHOLE:
		case ACTION_SET_NEXTHOP_NOMODIFY:
		case ACTION_SET_NEXTHOP_SELF:
			nexthop_modify(asp, &set->action.nexthop, set->type,
			    aid);
			break;
		case ACTION_SET_COMMUNITY:
			switch (set->action.community.as) {
			case COMMUNITY_ERROR:
			case COMMUNITY_ANY:
				fatalx("rde_apply_set bad community string");
			case COMMUNITY_NEIGHBOR_AS:
				as = peer->conf.remote_as;
				break;
			default:
				as = set->action.community.as;
				break;
			}

			switch (set->action.community.type) {
			case COMMUNITY_ERROR:
			case COMMUNITY_ANY:
				fatalx("rde_apply_set bad community string");
			case COMMUNITY_NEIGHBOR_AS:
				type = peer->conf.remote_as;
				break;
			default:
				type = set->action.community.type;
				break;
			}

			community_set(asp, as, type);
			break;
		case ACTION_DEL_COMMUNITY:
			switch (set->action.community.as) {
			case COMMUNITY_ERROR:
				fatalx("rde_apply_set bad community string");
			case COMMUNITY_NEIGHBOR_AS:
				as = peer->conf.remote_as;
				break;
			case COMMUNITY_ANY:
			default:
				as = set->action.community.as;
				break;
			}

			switch (set->action.community.type) {
			case COMMUNITY_ERROR:
				fatalx("rde_apply_set bad community string");
			case COMMUNITY_NEIGHBOR_AS:
				type = peer->conf.remote_as;
				break;
			case COMMUNITY_ANY:
			default:
				type = set->action.community.type;
				break;
			}

			community_delete(asp, as, type);
			break;
		case ACTION_PFTABLE:
			/* convert pftable name to an id */
			set->action.id = pftable_name2id(set->action.pftable);
			set->type = ACTION_PFTABLE_ID;
			/* FALLTHROUGH */
		case ACTION_PFTABLE_ID:
			pftable_unref(asp->pftableid);
			asp->pftableid = set->action.id;
			pftable_ref(asp->pftableid);
			break;
		case ACTION_RTLABEL:
			/* convert the route label to an id for faster access */
			set->action.id = rtlabel_name2id(set->action.rtlabel);
			set->type = ACTION_RTLABEL_ID;
			/* FALLTHROUGH */
		case ACTION_RTLABEL_ID:
			rtlabel_unref(asp->rtlabelid);
			asp->rtlabelid = set->action.id;
			rtlabel_ref(asp->rtlabelid);
			break;
		case ACTION_SET_ORIGIN:
			asp->origin = set->action.origin;
			break;
		case ACTION_SET_EXT_COMMUNITY:
			community_ext_set(asp, &set->action.ext_community,
			    peer->conf.remote_as);
			break;
		case ACTION_DEL_EXT_COMMUNITY:
			community_ext_delete(asp, &set->action.ext_community,
			    peer->conf.remote_as);
			break;
		}
	}
}

int
rde_filter_match(struct filter_rule *f, struct rde_aspath *asp,
    struct bgpd_addr *prefix, u_int8_t plen, struct rde_peer *peer)
{
	u_int32_t	pas;
	int		cas, type;

	if (asp != NULL && f->match.as.type != AS_NONE) {
		if (f->match.as.flags & AS_FLAG_NEIGHBORAS)
			pas = peer->conf.remote_as;
		else
			pas = f->match.as.as;
		if (aspath_match(asp->aspath->data, asp->aspath->len,
		    f->match.as.type, pas) == 0)
			return (0);
	}

	if (asp != NULL && f->match.aslen.type != ASLEN_NONE)
		if (aspath_lenmatch(asp->aspath, f->match.aslen.type,
		    f->match.aslen.aslen) == 0)
			return (0);

	if (asp != NULL && f->match.community.as != COMMUNITY_UNSET) {
		switch (f->match.community.as) {
		case COMMUNITY_ERROR:
			fatalx("rde_apply_set bad community string");
		case COMMUNITY_NEIGHBOR_AS:
			cas = peer->conf.remote_as;
			break;
		default:
			cas = f->match.community.as;
			break;
		}

		switch (f->match.community.type) {
		case COMMUNITY_ERROR:
			fatalx("rde_apply_set bad community string");
		case COMMUNITY_NEIGHBOR_AS:
			type = peer->conf.remote_as;
			break;
		default:
			type = f->match.community.type;
			break;
		}

		if (community_match(asp, cas, type) == 0)
			return (0);
	}
	if (asp != NULL &&
	    (f->match.ext_community.flags & EXT_COMMUNITY_FLAG_VALID))
		if (community_ext_match(asp, &f->match.ext_community,
		    peer->conf.remote_as) == 0)
			return (0);

	if (f->match.prefix.addr.aid != 0) {
		if (f->match.prefix.addr.aid != prefix->aid)
			/* don't use IPv4 rules for IPv6 and vice versa */
			return (0);

		if (prefix_compare(prefix, &f->match.prefix.addr,
		    f->match.prefix.len))
			return (0);

		/* test prefixlen stuff too */
		switch (f->match.prefixlen.op) {
		case OP_NONE:
			/* perfect match */
			return (plen == f->match.prefix.len);
		case OP_RANGE:
			return ((plen >= f->match.prefixlen.len_min) &&
			    (plen <= f->match.prefixlen.len_max));
		case OP_XRANGE:
			return ((plen < f->match.prefixlen.len_min) ||
			    (plen > f->match.prefixlen.len_max));
		case OP_EQ:
			return (plen == f->match.prefixlen.len_min);
		case OP_NE:
			return (plen != f->match.prefixlen.len_min);
		case OP_LE:
			return (plen <= f->match.prefixlen.len_min);
		case OP_LT:
			return (plen < f->match.prefixlen.len_min);
		case OP_GE:
			return (plen >= f->match.prefixlen.len_min);
		case OP_GT:
			return (plen > f->match.prefixlen.len_min);
		}
		/* NOTREACHED */
	} else if (f->match.prefixlen.op != OP_NONE) {
		/* only prefixlen without a prefix */

		if (f->match.prefixlen.aid != prefix->aid)
			/* don't use IPv4 rules for IPv6 and vice versa */
			return (0);

		switch (f->match.prefixlen.op) {
		case OP_NONE:
			fatalx("internal filter bug");
		case OP_RANGE:
			return ((plen >= f->match.prefixlen.len_min) &&
			    (plen <= f->match.prefixlen.len_max));
		case OP_XRANGE:
			return ((plen < f->match.prefixlen.len_min) ||
			    (plen > f->match.prefixlen.len_max));
		case OP_EQ:
			return (plen == f->match.prefixlen.len_min);
		case OP_NE:
			return (plen != f->match.prefixlen.len_min);
		case OP_LE:
			return (plen <= f->match.prefixlen.len_min);
		case OP_LT:
			return (plen < f->match.prefixlen.len_min);
		case OP_GE:
			return (plen >= f->match.prefixlen.len_min);
		case OP_GT:
			return (plen > f->match.prefixlen.len_min);
		}
		/* NOTREACHED */
	}

	/* matched somewhen or is anymatch rule  */
	return (1);
}

int
rde_filter_equal(struct filter_head *a, struct filter_head *b,
    struct rde_peer *peer, enum directions dir)
{
	struct filter_rule	*fa, *fb;

	fa = TAILQ_FIRST(a);
	fb = TAILQ_FIRST(b);

	while (fa != NULL || fb != NULL) {
		/* skip all rules with wrong direction */
		if (fa != NULL && dir != fa->dir) {
			fa = TAILQ_NEXT(fa, entry);
			continue;
		}
		if (fb != NULL && dir != fb->dir) {
			fb = TAILQ_NEXT(fb, entry);
			continue;
		}

		/* skip all rules with wrong peer */
		if (fa != NULL && fa->peer.groupid != 0 &&
		    fa->peer.groupid != peer->conf.groupid) {
			fa = TAILQ_NEXT(fa, entry);
			continue;
		}
		if (fa != NULL && fa->peer.peerid != 0 &&
		    fa->peer.peerid != peer->conf.id) {
			fa = TAILQ_NEXT(fa, entry);
			continue;
		}

		if (fb != NULL && fb->peer.groupid != 0 &&
		    fb->peer.groupid != peer->conf.groupid) {
			fb = TAILQ_NEXT(fb, entry);
			continue;
		}
		if (fb != NULL && fb->peer.peerid != 0 &&
		    fb->peer.peerid != peer->conf.id) {
			fb = TAILQ_NEXT(fb, entry);
			continue;
		}

		/* compare the two rules */
		if ((fa == NULL && fb != NULL) || (fa != NULL && fb == NULL))
			/* new rule added or removed */
			return (0);

		if (fa->action != fb->action || fa->quick != fb->quick)
			return (0);
		if (memcmp(&fa->peer, &fb->peer, sizeof(fa->peer)))
			return (0);
		if (memcmp(&fa->match, &fb->match, sizeof(fa->match)))
			return (0);
		if (!filterset_equal(&fa->set, &fb->set))
			return (0);

		fa = TAILQ_NEXT(fa, entry);
		fb = TAILQ_NEXT(fb, entry);
	}
	return (1);
}

/* free a filterset and take care of possible name2id references */
void
filterset_free(struct filter_set_head *sh)
{
	struct filter_set	*s;
	struct nexthop		*nh;

	while ((s = TAILQ_FIRST(sh)) != NULL) {
		TAILQ_REMOVE(sh, s, entry);
		if (s->type == ACTION_RTLABEL_ID)
			rtlabel_unref(s->action.id);
		else if (s->type == ACTION_PFTABLE_ID)
			pftable_unref(s->action.id);
		else if (s->type == ACTION_SET_NEXTHOP &&
		    bgpd_process == PROC_RDE) {
			nh = nexthop_get(&s->action.nexthop);
			--nh->refcnt;
			(void)nexthop_delete(nh);
		}
		free(s);
	}
}

/*
 * this function is a bit more complicated than a memcmp() because there are
 * types that need to be considered equal e.g. ACTION_SET_MED and
 * ACTION_SET_RELATIVE_MED. Also ACTION_SET_COMMUNITY and ACTION_SET_NEXTHOP
 * need some special care. It only checks the types and not the values so
 * it does not do a real compare.
 */
int
filterset_cmp(struct filter_set *a, struct filter_set *b)
{
	if (strcmp(filterset_name(a->type), filterset_name(b->type)))
		return (a->type - b->type);

	if (a->type == ACTION_SET_COMMUNITY ||
	    a->type == ACTION_DEL_COMMUNITY) {	/* a->type == b->type */
		/* compare community */
		if (a->action.community.as - b->action.community.as != 0)
			return (a->action.community.as -
			    b->action.community.as);
		return (a->action.community.type - b->action.community.type);
	}

	if (a->type == ACTION_SET_EXT_COMMUNITY ||
	    a->type == ACTION_DEL_EXT_COMMUNITY) {	/* a->type == b->type */
		return (memcmp(&a->action.ext_community,
		    &b->action.ext_community, sizeof(a->action.ext_community)));
	}

	if (a->type == ACTION_SET_NEXTHOP && b->type == ACTION_SET_NEXTHOP) {
		/*
		 * This is the only interesting case, all others are considered
		 * equal. It does not make sense to e.g. set a nexthop and
		 * reject it at the same time. Allow one IPv4 and one IPv6
		 * per filter set or only one of the other nexthop modifiers.
		 */
		return (a->action.nexthop.aid - b->action.nexthop.aid);
	}

	/* equal */
	return (0);
}

void
filterset_move(struct filter_set_head *source, struct filter_set_head *dest)
{
	struct filter_set	*s;

	TAILQ_INIT(dest);

	if (source == NULL)
		return;

	while ((s = TAILQ_FIRST(source)) != NULL) {
		TAILQ_REMOVE(source, s, entry);
		TAILQ_INSERT_TAIL(dest, s, entry);
	}
}

int
filterset_equal(struct filter_set_head *ah, struct filter_set_head *bh)
{
	struct filter_set	*a, *b;
	const char		*as, *bs;

	for (a = TAILQ_FIRST(ah), b = TAILQ_FIRST(bh);
	    a != NULL && b != NULL;
	    a = TAILQ_NEXT(a, entry), b = TAILQ_NEXT(b, entry)) {
		switch (a->type) {
		case ACTION_SET_PREPEND_SELF:
		case ACTION_SET_PREPEND_PEER:
			if (a->type == b->type &&
			    a->action.prepend == b->action.prepend)
				continue;
			break;
		case ACTION_SET_LOCALPREF:
		case ACTION_SET_MED:
		case ACTION_SET_WEIGHT:
			if (a->type == b->type &&
			    a->action.metric == b->action.metric)
				continue;
			break;
		case ACTION_SET_RELATIVE_LOCALPREF:
		case ACTION_SET_RELATIVE_MED:
		case ACTION_SET_RELATIVE_WEIGHT:
			if (a->type == b->type &&
			    a->action.relative == b->action.relative)
				continue;
			break;
		case ACTION_SET_NEXTHOP:
			if (a->type == b->type &&
			    memcmp(&a->action.nexthop, &b->action.nexthop,
			    sizeof(a->action.nexthop)) == 0)
				continue;
			break;
		case ACTION_SET_NEXTHOP_BLACKHOLE:
		case ACTION_SET_NEXTHOP_REJECT:
		case ACTION_SET_NEXTHOP_NOMODIFY:
		case ACTION_SET_NEXTHOP_SELF:
			if (a->type == b->type)
				continue;
			break;
		case ACTION_DEL_COMMUNITY:
		case ACTION_SET_COMMUNITY:
			if (a->type == b->type &&
			    memcmp(&a->action.community, &b->action.community,
			    sizeof(a->action.community)) == 0)
				continue;
			break;
		case ACTION_PFTABLE:
		case ACTION_PFTABLE_ID:
			if (b->type == ACTION_PFTABLE)
				bs = b->action.pftable;
			else if (b->type == ACTION_PFTABLE_ID)
				bs = pftable_id2name(b->action.id);
			else
				break;

			if (a->type == ACTION_PFTABLE)
				as = a->action.pftable;
			else
				as = pftable_id2name(a->action.id);

			if (strcmp(as, bs) == 0)
				continue;
			break;
		case ACTION_RTLABEL:
		case ACTION_RTLABEL_ID:
			if (b->type == ACTION_RTLABEL)
				bs = b->action.rtlabel;
			else if (b->type == ACTION_RTLABEL_ID)
				bs = rtlabel_id2name(b->action.id);
			else
				break;

			if (a->type == ACTION_RTLABEL)
				as = a->action.rtlabel;
			else
				as = rtlabel_id2name(a->action.id);

			if (strcmp(as, bs) == 0)
				continue;
			break;
		case ACTION_SET_ORIGIN:
			if (a->type == b->type &&
			    a->action.origin == b->action.origin)
				continue;
			break;
		case ACTION_SET_EXT_COMMUNITY:
		case ACTION_DEL_EXT_COMMUNITY:
			if (a->type == b->type && memcmp(
			    &a->action.ext_community,
			    &b->action.ext_community,
			    sizeof(a->action.ext_community)) == 0)
				continue;
			break;
		}
		/* compare failed */
		return (0);
	}
	if (a != NULL || b != NULL)
		return (0);
	return (1);
}

const char *
filterset_name(enum action_types type)
{
	switch (type) {
	case ACTION_SET_LOCALPREF:
	case ACTION_SET_RELATIVE_LOCALPREF:
		return ("localpref");
	case ACTION_SET_MED:
	case ACTION_SET_RELATIVE_MED:
		return ("metric");
	case ACTION_SET_WEIGHT:
	case ACTION_SET_RELATIVE_WEIGHT:
		return ("weight");
	case ACTION_SET_PREPEND_SELF:
		return ("prepend-self");
	case ACTION_SET_PREPEND_PEER:
		return ("prepend-peer");
	case ACTION_SET_NEXTHOP:
	case ACTION_SET_NEXTHOP_REJECT:
	case ACTION_SET_NEXTHOP_BLACKHOLE:
	case ACTION_SET_NEXTHOP_NOMODIFY:
	case ACTION_SET_NEXTHOP_SELF:
		return ("nexthop");
	case ACTION_SET_COMMUNITY:
		return ("community");
	case ACTION_DEL_COMMUNITY:
		return ("community delete");
	case ACTION_PFTABLE:
	case ACTION_PFTABLE_ID:
		return ("pftable");
	case ACTION_RTLABEL:
	case ACTION_RTLABEL_ID:
		return ("rtlabel");
	case ACTION_SET_ORIGIN:
		return ("origin");
	case ACTION_SET_EXT_COMMUNITY:
		return ("ext-community");
	case ACTION_DEL_EXT_COMMUNITY:
		return ("ext-community delete");
	}

	fatalx("filterset_name: got lost");
}
