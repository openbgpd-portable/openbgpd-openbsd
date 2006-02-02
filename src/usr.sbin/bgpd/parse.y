/*	$OpenBSD: parse.y,v 1.179 2006/02/02 14:06:05 claudio Exp $ */

/*
 * Copyright (c) 2002, 2003, 2004 Henning Brauer <henning@openbsd.org>
 * Copyright (c) 2001 Markus Friedl.  All rights reserved.
 * Copyright (c) 2001 Daniel Hartmeier.  All rights reserved.
 * Copyright (c) 2001 Theo de Raadt.  All rights reserved.
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

%{
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "bgpd.h"
#include "mrt.h"
#include "session.h"

static struct bgpd_config	*conf;
static struct mrt_head		*mrtconf;
static struct network_head	*netconf;
static struct peer		*peer_l, *peer_l_old;
static struct peer		*curpeer;
static struct peer		*curgroup;
static struct filter_head	*filter_l;
static struct filter_head	*peerfilter_l;
static struct filter_head	*groupfilter_l;
static struct filter_rule	*curpeer_filter[2];
static struct filter_rule	*curgroup_filter[2];
static struct listen_addrs	*listen_addrs;
static FILE			*fin = NULL;
static int			 lineno = 1;
static int			 errors = 0;
static int			 pdebug = 1;
static u_int32_t		 id;
char				*infile;

int	 yyerror(const char *, ...);
int	 yyparse(void);
int	 kw_cmp(const void *, const void *);
int	 lookup(char *);
int	 lgetc(FILE *);
int	 lungetc(int);
int	 findeol(void);
int	 yylex(void);

struct filter_peers_l {
	struct filter_peers_l	*next;
	struct filter_peers	 p;
};

struct filter_prefix_l {
	struct filter_prefix_l	*next;
	struct filter_prefix	 p;
};

struct filter_as_l {
	struct filter_as_l	*next;
	struct filter_as	 a;
};

struct filter_match_l {
	struct filter_match	 m;
	struct filter_prefix_l	*prefix_l;
	struct filter_as_l	*as_l;
} fmopts;

struct peer	*alloc_peer(void);
struct peer	*new_peer(void);
struct peer	*new_group(void);
int		 add_mrtconfig(enum mrt_type, char *, time_t, struct peer *);
int		 get_id(struct peer *);
int		 expand_rule(struct filter_rule *, struct filter_peers_l *,
		    struct filter_match_l *, struct filter_set_head *);
int		 str2key(char *, char *, size_t);
int		 neighbor_consistent(struct peer *);
int		 merge_filterset(struct filter_set_head *, struct filter_set *);
void		 copy_filterset(struct filter_set_head *,
		    struct filter_set_head *);
void		 move_filterset(struct filter_set_head *,
		    struct filter_set_head *);
struct filter_rule	*get_rule(enum action_types);

TAILQ_HEAD(symhead, sym)	 symhead = TAILQ_HEAD_INITIALIZER(symhead);
struct sym {
	TAILQ_ENTRY(sym)	 entry;
	int			 used;
	int			 persist;
	char			*nam;
	char			*val;
};

int	 symset(const char *, const char *, int);
char	*symget(const char *);
int	 atoul(char *, u_long *);
int	 getcommunity(char *);
int	 parsecommunity(char *, int *, int *);

typedef struct {
	union {
		u_int32_t		 number;
		char			*string;
		struct bgpd_addr	 addr;
		u_int8_t		 u8;
		struct filter_peers_l	*filter_peers;
		struct filter_match_l	 filter_match;
		struct filter_prefix_l	*filter_prefix;
		struct filter_as_l	*filter_as;
		struct filter_prefixlen	 prefixlen;
		struct filter_set	*filter_set;
		struct filter_set_head	*filter_set_head;
		struct {
			struct bgpd_addr	prefix;
			u_int8_t		len;
		}			prefix;
		struct {
			u_int8_t		enc_alg;
			char			enc_key[IPSEC_ENC_KEY_LEN];
			u_int8_t		enc_key_len;
		}			encspec;
	} v;
	int lineno;
} YYSTYPE;

%}

%token	AS ROUTERID HOLDTIME YMIN LISTEN ON FIBUPDATE
%token	RDE EVALUATE IGNORE COMPARE
%token	GROUP NEIGHBOR NETWORK
%token	REMOTEAS DESCR LOCALADDR MULTIHOP PASSIVE MAXPREFIX ANNOUNCE
%token	ENFORCE NEIGHBORAS CAPABILITIES REFLECTOR DEPEND DOWN
%token	DUMP IN OUT
%token	LOG ROUTECOLL TRANSPARENT
%token	TCP MD5SIG PASSWORD KEY
%token	ALLOW DENY MATCH
%token	QUICK
%token	FROM TO ANY
%token	CONNECTED STATIC
%token	PREFIX PREFIXLEN SOURCEAS TRANSITAS COMMUNITY
%token	SET LOCALPREF MED METRIC NEXTHOP REJECT BLACKHOLE NOMODIFY
%token	PREPEND_SELF PREPEND_PEER PFTABLE WEIGHT RTLABEL
%token	ERROR
%token	IPSEC ESP AH SPI IKE
%token	IPV4 IPV6
%token	<v.string>		STRING
%type	<v.number>		number asnumber optnumber yesno inout espah
%type	<v.number>		family
%type	<v.string>		string
%type	<v.addr>		address
%type	<v.prefix>		prefix addrspec
%type	<v.u8>			action quick direction
%type	<v.filter_peers>	filter_peer filter_peer_l filter_peer_h
%type	<v.filter_match>	filter_match filter_elm filter_match_h
%type	<v.filter_as>		filter_as filter_as_l filter_as_h
%type	<v.filter_as>		filter_as_t filter_as_t_l filter_as_l_h
%type	<v.prefixlen>		prefixlenop
%type	<v.filter_set>		filter_set_opt
%type	<v.filter_set_head>	filter_set filter_set_l
%type	<v.filter_prefix>	filter_prefix filter_prefix_l
%type	<v.filter_prefix>	filter_prefix_h filter_prefix_m
%type	<v.u8>			unaryop binaryop filter_as_type
%type	<v.encspec>		encspec
%%

grammar		: /* empty */
		| grammar '\n'
		| grammar conf_main '\n'
		| grammar varset '\n'
		| grammar neighbor '\n'
		| grammar group '\n'
		| grammar filterrule '\n'
		| grammar error '\n'		{ errors++; }
		;

number		: STRING			{
			u_long	ulval;

			if (atoul($1, &ulval) == -1) {
				yyerror("\"%s\" is not a number", $1);
				free($1);
				YYERROR;
			} else
				$$ = ulval;
			free($1);
		}
		;

asnumber	: number			{
			if ($1 > USHRT_MAX) {
				yyerror("AS too big: max %u", USHRT_MAX);
				YYERROR;
			}
		}

string		: string STRING				{
			if (asprintf(&$$, "%s %s", $1, $2) == -1)
				fatal("string: asprintf");
			free($1);
			free($2);
		}
		| STRING
		;

yesno		:  STRING		{
			if (!strcmp($1, "yes"))
				$$ = 1;
			else if (!strcmp($1, "no"))
				$$ = 0;
			else {
				free($1);
				YYERROR;
			}
			free($1);
		}
		;

varset		: STRING '=' string		{
			if (conf->opts & BGPD_OPT_VERBOSE)
				printf("%s = \"%s\"\n", $1, $3);
			if (symset($1, $3, 0) == -1)
				fatal("cannot store variable");
			free($1);
			free($3);
		}
		;

conf_main	: AS asnumber		{
			conf->as = $2;
		}
		| ROUTERID address		{
			if ($2.af != AF_INET) {
				yyerror("router-id must be an IPv4 address");
				YYERROR;
			}
			conf->bgpid = $2.v4.s_addr;
		}
		| HOLDTIME number	{
			if ($2 < MIN_HOLDTIME) {
				yyerror("holdtime must be at least %u",
				    MIN_HOLDTIME);
				YYERROR;
			}
			conf->holdtime = $2;
		}
		| HOLDTIME YMIN number	{
			if ($3 < MIN_HOLDTIME) {
				yyerror("holdtime min must be at least %u",
				    MIN_HOLDTIME);
				YYERROR;
			}
			conf->min_holdtime = $3;
		}
		| LISTEN ON address	{
			struct listen_addr	*la;
			struct sockaddr_in	*in;
			struct sockaddr_in6	*in6;

			if ((la = calloc(1, sizeof(struct listen_addr))) ==
			    NULL)
				fatal("parse conf_main listen on calloc");

			la->fd = -1;
			la->sa.ss_family = $3.af;
			switch ($3.af) {
			case AF_INET:
				la->sa.ss_len = sizeof(struct sockaddr_in);
				in = (struct sockaddr_in *)&la->sa;
				in->sin_addr.s_addr = $3.v4.s_addr;
				in->sin_port = htons(BGP_PORT);
				break;
			case AF_INET6:
				la->sa.ss_len = sizeof(struct sockaddr_in6);
				in6 = (struct sockaddr_in6 *)&la->sa;
				memcpy(&in6->sin6_addr, &$3.v6,
				    sizeof(in6->sin6_addr));
				in6->sin6_port = htons(BGP_PORT);
				break;
			default:
				yyerror("king bula does not like family %u",
				    $3.af);
				YYERROR;
			}

			TAILQ_INSERT_TAIL(listen_addrs, la, entry);
		}
		| FIBUPDATE yesno		{
			if ($2 == 0)
				conf->flags |= BGPD_FLAG_NO_FIB_UPDATE;
			else
				conf->flags &= ~BGPD_FLAG_NO_FIB_UPDATE;
		}
		| ROUTECOLL yesno	{
			if ($2 == 1)
				conf->flags |= BGPD_FLAG_NO_EVALUATE;
			else
				conf->flags &= ~BGPD_FLAG_NO_EVALUATE;
		}
		| TRANSPARENT yesno	{
			if ($2 == 1)
				conf->flags |= BGPD_FLAG_DECISION_TRANS_AS;
			else
				conf->flags &= ~BGPD_FLAG_DECISION_TRANS_AS;
		}
		| LOG STRING		{
			if (!strcmp($2, "updates"))
				conf->log |= BGPD_LOG_UPDATES;
			else {
				free($2);
				YYERROR;
			}
			free($2);
		}
		| NETWORK prefix filter_set	{
			struct network	*n;

			if ((n = calloc(1, sizeof(struct network))) == NULL)
				fatal("new_network");
			memcpy(&n->net.prefix, &$2.prefix,
			    sizeof(n->net.prefix));
			n->net.prefixlen = $2.len;
			move_filterset($3, &n->net.attrset);
			free($3);

			TAILQ_INSERT_TAIL(netconf, n, entry);
		}
		| NETWORK STRING STATIC filter_set	{
			if (!strcmp($2, "inet")) {
				conf->flags |= BGPD_FLAG_REDIST_STATIC;
				move_filterset($4, &conf->staticset);
			} else if (!strcmp($2, "inet6")) {
				conf->flags |= BGPD_FLAG_REDIST6_STATIC;
				move_filterset($4, &conf->staticset6);
			}
			free($2);
			free($4);
		}
		| NETWORK STRING CONNECTED filter_set	{
			if (!strcmp($2, "inet")) {
				conf->flags |= BGPD_FLAG_REDIST_CONNECTED;
				move_filterset($4, &conf->connectset);
			} else if (!strcmp($2, "inet6")) {
				conf->flags |= BGPD_FLAG_REDIST6_CONNECTED;
				move_filterset($4, &conf->connectset6);
			}
			free($2);
			free($4);
		}
		| NETWORK STATIC filter_set	{
			/* keep for compatibility till after next release */
			conf->flags |= BGPD_FLAG_REDIST_STATIC;
			move_filterset($3, &conf->staticset);
			free($3);
		}
		| NETWORK CONNECTED filter_set	{
			/* keep for compatibility till after next release */
			conf->flags |= BGPD_FLAG_REDIST_CONNECTED;
			move_filterset($3, &conf->connectset);
			free($3);
		}
		| DUMP STRING STRING optnumber		{
			int action;

			if (!strcmp($2, "table"))
				action = MRT_TABLE_DUMP;
			else if (!strcmp($2, "table-mp"))
				action = MRT_TABLE_DUMP_MP;
			else {
				yyerror("unknown mrt dump type");
				free($2);
				free($3);
				YYERROR;
			}
			free($2);
			if (add_mrtconfig(action, $3, $4, NULL) == -1) {
				free($3);
				YYERROR;
			}
			free($3);
		}
		| mrtdump
		| RDE STRING EVALUATE		{
			if (!strcmp($2, "route-age"))
				conf->flags |= BGPD_FLAG_DECISION_ROUTEAGE;
			else {
				yyerror("unknown route decision type");
				free($2);
				YYERROR;
			}
			free($2);
		}
		| RDE STRING IGNORE		{
			if (!strcmp($2, "route-age"))
				conf->flags &= ~BGPD_FLAG_DECISION_ROUTEAGE;
			else {
				yyerror("unknown route decision type");
				free($2);
				YYERROR;
			}
			free($2);
		}
		| RDE MED COMPARE STRING	{
			if (!strcmp($4, "always"))
				conf->flags |= BGPD_FLAG_DECISION_MED_ALWAYS;
			else if (!strcmp($4, "strict"))
				conf->flags &= ~BGPD_FLAG_DECISION_MED_ALWAYS;
			else {
				yyerror("rde med compare: "
				    "unknown setting \"%s\"", $4);
				free($4);
				YYERROR;
			}
			free($4);
		}
		;

mrtdump		: DUMP STRING inout STRING optnumber	{
			int action;

			if (!strcmp($2, "all"))
				action = $3 ? MRT_ALL_IN : MRT_ALL_OUT;
			else if (!strcmp($2, "updates"))
				action = $3 ? MRT_UPDATE_IN : MRT_UPDATE_OUT;
			else {
				yyerror("unknown mrt msg dump type");
				free($2);
				free($4);
				YYERROR;
			}
			if (add_mrtconfig(action, $4, $5, curpeer) == -1) {
				free($2);
				free($4);
				YYERROR;
			}
			free($2);
			free($4);
		}
		;

inout		: IN		{ $$ = 1; }
		| OUT		{ $$ = 0; }
		;

address		: STRING		{
			u_int8_t	len;

			if (!host($1, &$$, &len)) {
				yyerror("could not parse address spec \"%s\"",
				    $1);
				free($1);
				YYERROR;
			}
			free($1);

			if (($$.af == AF_INET && len != 32) ||
			    ($$.af == AF_INET6 && len != 128)) {
				/* unreachable */
				yyerror("got prefixlen %u, expected %u",
				    len, $$.af == AF_INET ? 32 : 128);
				YYERROR;
			}
		}
		;

prefix		: STRING '/' number	{
			char	*s;

			if (asprintf(&s, "%s/%u", $1, $3) == -1)
				fatal(NULL);
			free($1);

			if (!host(s, &$$.prefix, &$$.len)) {
				yyerror("could not parse address \"%s\"", s);
				free(s);
				YYERROR;
			}
			free(s);
		}
		;

addrspec	: address	{
			memcpy(&$$.prefix, &$1, sizeof(struct bgpd_addr));
			if ($$.prefix.af == AF_INET)
				$$.len = 32;
			else
				$$.len = 128;
		}
		| prefix
		;

optnl		: '\n' optnl
		|
		;

nl		: '\n' optnl		/* one newline or more */
		;

optnumber	: /* empty */		{ $$ = 0; }
		| number
		;

neighbor	: {	curpeer = new_peer(); }
		    NEIGHBOR addrspec {
			memcpy(&curpeer->conf.remote_addr, &$3.prefix,
			    sizeof(curpeer->conf.remote_addr));
			curpeer->conf.remote_masklen = $3.len;
			if (($3.prefix.af == AF_INET && $3.len != 32) ||
			    ($3.prefix.af == AF_INET6 && $3.len != 128))
				curpeer->conf.template = 1;
			if (get_id(curpeer)) {
				yyerror("get_id failed");
				YYERROR;
			}
		}
		    peeropts_h {
			if (curpeer_filter[0] != NULL)
				TAILQ_INSERT_TAIL(peerfilter_l,
				    curpeer_filter[0], entry);
			if (curpeer_filter[1] != NULL)
				TAILQ_INSERT_TAIL(peerfilter_l,
				    curpeer_filter[1], entry);
			curpeer_filter[0] = NULL;
			curpeer_filter[1] = NULL;

			if (neighbor_consistent(curpeer) == -1)
				YYERROR;
			curpeer->next = peer_l;
			peer_l = curpeer;
			curpeer = curgroup;
		}
		;

group		: GROUP string optnl '{' optnl {
			curgroup = curpeer = new_group();
			if (strlcpy(curgroup->conf.group, $2,
			    sizeof(curgroup->conf.group)) >=
			    sizeof(curgroup->conf.group)) {
				yyerror("group name \"%s\" too long: max %u",
				    $2, sizeof(curgroup->conf.group) - 1);
				free($2);
				YYERROR;
			}
			free($2);
			if (get_id(curgroup)) {
				yyerror("get_id failed");
				YYERROR;
			}
		}
		    groupopts_l '}' {
			if (curgroup_filter[0] != NULL)
				TAILQ_INSERT_TAIL(groupfilter_l,
				    curgroup_filter[0], entry);
			if (curgroup_filter[1] != NULL)
				TAILQ_INSERT_TAIL(groupfilter_l,
				    curgroup_filter[1], entry);
			curgroup_filter[0] = NULL;
			curgroup_filter[1] = NULL;

			free(curgroup);
			curgroup = NULL;
		}
		;

groupopts_l	: groupopts_l groupoptsl
		| groupoptsl
		;

groupoptsl	: peeropts nl
		| neighbor nl
		| error nl
		;

peeropts_h	: '{' optnl peeropts_l '}'
		| /* empty */
		;

peeropts_l	: peeropts_l peeroptsl
		| peeroptsl
		;

peeroptsl	: peeropts nl
		| error nl
		;

peeropts	: REMOTEAS asnumber	{
			curpeer->conf.remote_as = $2;
		}
		| DESCR string		{
			if (strlcpy(curpeer->conf.descr, $2,
			    sizeof(curpeer->conf.descr)) >=
			    sizeof(curpeer->conf.descr)) {
				yyerror("descr \"%s\" too long: max %u",
				    $2, sizeof(curpeer->conf.descr) - 1);
				free($2);
				YYERROR;
			}
			free($2);
		}
		| LOCALADDR address	{
			memcpy(&curpeer->conf.local_addr, &$2,
			    sizeof(curpeer->conf.local_addr));
		}
		| MULTIHOP number	{
			if ($2 < 2 || $2 > 255) {
				yyerror("invalid multihop distance %d", $2);
				YYERROR;
			}
			curpeer->conf.distance = $2;
		}
		| PASSIVE		{
			curpeer->conf.passive = 1;
		}
		| DOWN		{
			curpeer->conf.down = 1;
		}
		| HOLDTIME number	{
			if ($2 < MIN_HOLDTIME) {
				yyerror("holdtime must be at least %u",
				    MIN_HOLDTIME);
				YYERROR;
			}
			curpeer->conf.holdtime = $2;
		}
		| HOLDTIME YMIN number	{
			if ($3 < MIN_HOLDTIME) {
				yyerror("holdtime min must be at least %u",
				    MIN_HOLDTIME);
				YYERROR;
			}
			curpeer->conf.min_holdtime = $3;
		}
		| ANNOUNCE family STRING {
			u_int8_t	safi;

			if (!strcmp($3, "none"))
				safi = SAFI_NONE;
			else if (!strcmp($3, "unicast"))
				safi = SAFI_UNICAST;
			else {
				yyerror("unknown/unsupported SAFI \"%s\"",
				    $3);
				free($3);
				YYERROR;
			}
			free($3);

			switch ($2) {
			case AFI_IPv4:
				curpeer->conf.capabilities.mp_v4 = safi;
				break;
			case AFI_IPv6:
				curpeer->conf.capabilities.mp_v6 = safi;
				break;
			default:
				fatal("king bula sees borked AFI");
			}
		}
		| ANNOUNCE STRING {
			if (!strcmp($2, "self"))
				curpeer->conf.announce_type = ANNOUNCE_SELF;
			else if (!strcmp($2, "none"))
				curpeer->conf.announce_type = ANNOUNCE_NONE;
			else if (!strcmp($2, "all"))
				curpeer->conf.announce_type = ANNOUNCE_ALL;
			else if (!strcmp($2, "default-route"))
				curpeer->conf.announce_type =
				    ANNOUNCE_DEFAULT_ROUTE;
			else {
				yyerror("invalid announce type");
				free($2);
				YYERROR;
			}
			free($2);
		}
		| ENFORCE NEIGHBORAS yesno {
			if ($3)
				curpeer->conf.enforce_as = ENFORCE_AS_ON;
			else
				curpeer->conf.enforce_as = ENFORCE_AS_OFF;
		}
		| MAXPREFIX number {
			curpeer->conf.max_prefix = $2;
		}
		| TCP MD5SIG PASSWORD string {
			if (curpeer->conf.auth.method) {
				yyerror("auth method cannot be redefined");
				free($4);
				YYERROR;
			}
			if (strlcpy(curpeer->conf.auth.md5key, $4,
			    sizeof(curpeer->conf.auth.md5key)) >=
			    sizeof(curpeer->conf.auth.md5key)) {
				yyerror("tcp md5sig password too long: max %u",
				    sizeof(curpeer->conf.auth.md5key) - 1);
				free($4);
				YYERROR;
			}
			curpeer->conf.auth.method = AUTH_MD5SIG;
			curpeer->conf.auth.md5key_len = strlen($4);
			free($4);
		}
		| TCP MD5SIG KEY string {
			if (curpeer->conf.auth.method) {
				yyerror("auth method cannot be redefined");
				free($4);
				YYERROR;
			}

			if (str2key($4, curpeer->conf.auth.md5key,
			    sizeof(curpeer->conf.auth.md5key)) == -1) {
				free($4);
				YYERROR;
			}
			curpeer->conf.auth.method = AUTH_MD5SIG;
			curpeer->conf.auth.md5key_len = strlen($4) / 2;
			free($4);
		}
		| IPSEC espah IKE {
			if (curpeer->conf.auth.method) {
				yyerror("auth method cannot be redefined");
				YYERROR;
			}
			if ($2)
				curpeer->conf.auth.method = AUTH_IPSEC_IKE_ESP;
			else
				curpeer->conf.auth.method = AUTH_IPSEC_IKE_AH;
		}
		| IPSEC espah inout SPI number STRING STRING encspec {
			u_int32_t	auth_alg;
			u_int8_t	keylen;

			if (curpeer->conf.auth.method &&
			    (((curpeer->conf.auth.spi_in && $3 == 1) ||
			    (curpeer->conf.auth.spi_out && $3 == 0)) ||
			    ($2 == 1 && curpeer->conf.auth.method !=
			    AUTH_IPSEC_MANUAL_ESP) ||
			    ($2 == 0 && curpeer->conf.auth.method !=
			    AUTH_IPSEC_MANUAL_AH))) {
				yyerror("auth method cannot be redefined");
				free($6);
				free($7);
				YYERROR;
			}

			if (!strcmp($6, "sha1")) {
				auth_alg = SADB_AALG_SHA1HMAC;
				keylen = 20;
			} else if (!strcmp($6, "md5")) {
				auth_alg = SADB_AALG_MD5HMAC;
				keylen = 16;
			} else {
				yyerror("unknown auth algorithm \"%s\"", $6);
				free($6);
				free($7);
				YYERROR;
			}
			free($6);

			if (strlen($7) / 2 != keylen) {
				yyerror("auth key len: must be %u bytes, "
				    "is %u bytes", keylen, strlen($7) / 2);
				free($7);
				YYERROR;
			}

			if ($2)
				curpeer->conf.auth.method =
				    AUTH_IPSEC_MANUAL_ESP;
			else {
				if ($8.enc_alg) {
					yyerror("\"ipsec ah\" doesn't take "
					    "encryption keys");
					free($7);
					YYERROR;
				}
				curpeer->conf.auth.method =
				    AUTH_IPSEC_MANUAL_AH;
			}

			if ($3 == 1) {
				if (str2key($7, curpeer->conf.auth.auth_key_in,
				    sizeof(curpeer->conf.auth.auth_key_in)) ==
				    -1) {
					free($7);
					YYERROR;
				}
				curpeer->conf.auth.spi_in = $5;
				curpeer->conf.auth.auth_alg_in = auth_alg;
				curpeer->conf.auth.enc_alg_in = $8.enc_alg;
				memcpy(&curpeer->conf.auth.enc_key_in,
				    &$8.enc_key,
				    sizeof(curpeer->conf.auth.enc_key_in));
				curpeer->conf.auth.enc_keylen_in =
				    $8.enc_key_len;
				curpeer->conf.auth.auth_keylen_in = keylen;
			} else {
				if (str2key($7, curpeer->conf.auth.auth_key_out,
				    sizeof(curpeer->conf.auth.auth_key_out)) ==
				    -1) {
					free($7);
					YYERROR;
				}
				curpeer->conf.auth.spi_out = $5;
				curpeer->conf.auth.auth_alg_out = auth_alg;
				curpeer->conf.auth.enc_alg_out = $8.enc_alg;
				memcpy(&curpeer->conf.auth.enc_key_out,
				    &$8.enc_key,
				    sizeof(curpeer->conf.auth.enc_key_out));
				curpeer->conf.auth.enc_keylen_out =
				    $8.enc_key_len;
				curpeer->conf.auth.auth_keylen_out = keylen;
			}
			free($7);
		}
		| ANNOUNCE CAPABILITIES yesno {
			curpeer->conf.announce_capa = $3;
		}
		| SET filter_set_opt	{
			struct filter_rule	*r;

			r = get_rule($2->type);
			if (merge_filterset(&r->set, $2) == -1)
				YYERROR;
		}
		| SET optnl "{" optnl filter_set_l optnl "}"	{
			struct filter_rule	*r;
			struct filter_set	*s;

			while ((s = TAILQ_FIRST($5)) != NULL) {
				TAILQ_REMOVE($5, s, entry);
				r = get_rule(s->type);
				if (merge_filterset(&r->set, s) == -1)
					YYERROR;
			}
			free($5);
		}
		| mrtdump
		| REFLECTOR		{
			if ((conf->flags & BGPD_FLAG_REFLECTOR) &&
			    conf->clusterid != 0) {
				yyerror("only one route reflector "
				    "cluster allowed");
				YYERROR;
			}
			conf->flags |= BGPD_FLAG_REFLECTOR;
			curpeer->conf.reflector_client = 1;
		}
		| REFLECTOR address	{
			if ($2.af != AF_INET) {
				yyerror("route reflector cluster-id must be "
				    "an IPv4 address");
				YYERROR;
			}
			if ((conf->flags & BGPD_FLAG_REFLECTOR) &&
			    conf->clusterid != $2.v4.s_addr) {
				yyerror("only one route reflector "
				    "cluster allowed");
				YYERROR;
			}
			conf->flags |= BGPD_FLAG_REFLECTOR;
			curpeer->conf.reflector_client = 1;
			conf->clusterid = $2.v4.s_addr;
		}
		| DEPEND ON STRING	{
			if (strlcpy(curpeer->conf.if_depend, $3,
			    sizeof(curpeer->conf.if_depend)) >=
			    sizeof(curpeer->conf.if_depend)) {
				yyerror("interface name \"%s\" too long: "
				    "max %u", $3,
				    sizeof(curpeer->conf.if_depend) - 1);
				free($3);
				YYERROR;
			}
			free($3);
		}
		;

family		: IPV4	{ $$ = AFI_IPv4; }
		| IPV6	{ $$ = AFI_IPv6; }
		;

espah		: ESP		{ $$ = 1; }
		| AH		{ $$ = 0; }
		;

encspec		: /* nada */	{
			bzero(&$$, sizeof($$));
		}
		| STRING STRING {
			bzero(&$$, sizeof($$));
			if (!strcmp($1, "3des") || !strcmp($1, "3des-cbc")) {
				$$.enc_alg = SADB_EALG_3DESCBC;
				$$.enc_key_len = 21; /* XXX verify */
			} else if (!strcmp($1, "aes") ||
			    !strcmp($1, "aes-128-cbc")) {
				$$.enc_alg = SADB_X_EALG_AES;
				$$.enc_key_len = 16;
			} else {
				yyerror("unknown enc algorithm \"%s\"", $1);
				free($1);
				free($2);
				YYERROR;
			}
			free($1);

			if (strlen($2) / 2 != $$.enc_key_len) {
				yyerror("enc key length wrong: should be %u "
				    "bytes, is %u bytes",
				    $$.enc_key_len * 2, strlen($2));
				free($2);
				YYERROR;
			}

			if (str2key($2, $$.enc_key, sizeof($$.enc_key)) == -1) {
				free($2);
				YYERROR;
			}
			free($2);
		}
		;

filterrule	: action quick direction filter_peer_h filter_match_h filter_set
		{
			struct filter_rule	 r;
			struct filter_prefix_l	*l;

			for (l = $5.prefix_l; l != NULL; l = l->next)
				if (l->p.addr.af && l->p.addr.af != AF_INET) {
					yyerror("king bula sez: AF_INET only");
					YYERROR;
				}

			bzero(&r, sizeof(r));
			r.action = $1;
			r.quick = $2;
			r.dir = $3;

			if (expand_rule(&r, $4, &$5, $6) == -1)
				YYERROR;
		}
		;

action		: ALLOW		{ $$ = ACTION_ALLOW; }
		| DENY		{ $$ = ACTION_DENY; }
		| MATCH		{ $$ = ACTION_NONE; }
		;

quick		: /* empty */	{ $$ = 0; }
		| QUICK		{ $$ = 1; }
		;

direction	: FROM		{ $$ = DIR_IN; }
		| TO		{ $$ = DIR_OUT; }
		;

filter_peer_h	: filter_peer
		| '{' filter_peer_l '}'		{ $$ = $2; }
		;

filter_peer_l	: filter_peer				{ $$ = $1; }
		| filter_peer_l comma filter_peer	{
			$3->next = $1;
			$$ = $3;
		}
		;

filter_peer	: ANY		{
			if (($$ = calloc(1, sizeof(struct filter_peers_l))) ==
			    NULL)
				fatal(NULL);
			$$->p.peerid = $$->p.groupid = 0;
			$$->next = NULL;
		}
		| address	{
			struct peer *p;

			if (($$ = calloc(1, sizeof(struct filter_peers_l))) ==
			    NULL)
				fatal(NULL);
			$$->p.groupid = $$->p.peerid = 0;
			$$->next = NULL;
			for (p = peer_l; p != NULL; p = p->next)
				if (!memcmp(&p->conf.remote_addr,
				    &$1, sizeof(p->conf.remote_addr))) {
					$$->p.peerid = p->conf.id;
					break;
				}
			if ($$->p.peerid == 0) {
				yyerror("no such peer: %s", log_addr(&$1));
				free($$);
				YYERROR;
			}
		}
		| GROUP STRING	{
			struct peer *p;

			if (($$ = calloc(1, sizeof(struct filter_peers_l))) ==
			    NULL)
				fatal(NULL);
			$$->p.peerid = 0;
			$$->next = NULL;
			for (p = peer_l; p != NULL; p = p->next)
				if (!strcmp(p->conf.group, $2)) {
					$$->p.groupid = p->conf.groupid;
					break;
				}
			if ($$->p.groupid == 0) {
				yyerror("no such group: \"%s\"", $2);
				free($2);
				free($$);
				YYERROR;
			}
			free($2);
		}
		;

filter_prefix_h	: PREFIX filter_prefix			{ $$ = $2; }
		| PREFIX '{' filter_prefix_m '}'	{ $$ = $3; }
		;

filter_prefix_m	: filter_prefix_l
		| '{' filter_prefix_l '}'		{ $$ = $2; }
		| '{' filter_prefix_l '}' filter_prefix_m
		{
			struct filter_prefix_l	*p;

			/* merge, both can be lists */
			for (p = $2; p != NULL && p->next != NULL; p = p->next)
				;	/* nothing */
			if (p != NULL)
				p->next = $4;
			$$ = $2;
		}
		;

filter_prefix_l	: filter_prefix				{ $$ = $1; }
		| filter_prefix_l comma filter_prefix	{
			$3->next = $1;
			$$ = $3;
		}
		;

filter_prefix	: prefix				{
			if (($$ = calloc(1, sizeof(struct filter_prefix_l))) ==
			    NULL)
				fatal(NULL);
			memcpy(&$$->p.addr, &$1.prefix,
			    sizeof($$->p.addr));
			$$->p.len = $1.len;
			$$->next = NULL;
		}
		;

filter_as_h	: filter_as_t
		| '{' filter_as_t_l '}'		{ $$ = $2; }
		;

filter_as_t_l	: filter_as_t
		| filter_as_t_l comma filter_as_t		{
			struct filter_as_l	*a;

			/* merge, both can be lists */
			for (a = $1; a != NULL && a->next != NULL; a = a->next)
				;	/* nothing */
			if (a != NULL)
				a->next = $3;
			$$ = $1;
		}
		;

filter_as_t	: filter_as_type filter_as			{
			$$ = $2;
			$$->a.type = $1;
		}
		| filter_as_type '{' filter_as_l_h '}'	{
			struct filter_as_l	*a;

			$$ = $3;
			for (a = $$; a != NULL; a = a->next)
				a->a.type = $1;
		}
		;

filter_as_l_h	: filter_as_l
		| '{' filter_as_l '}'			{ $$ = $2; }
		| '{' filter_as_l '}' filter_as_l_h
		{
			struct filter_as_l	*a;

			/* merge, both can be lists */
			for (a = $2; a != NULL && a->next != NULL; a = a->next)
				;	/* nothing */
			if (a != NULL)
				a->next = $4;
			$$ = $2;
		}
		;

filter_as_l	: filter_as
		| filter_as_l comma filter_as	{
			$3->next = $1;
			$$ = $3;
		}
		;

filter_as	: asnumber		{
			if (($$ = calloc(1, sizeof(struct filter_as_l))) ==
			    NULL)
				fatal(NULL);
			$$->a.as = $1;
		}
		;

filter_match_h	: /* empty */			{ bzero(&$$, sizeof($$)); }
		| { bzero(&fmopts, sizeof(fmopts)); }
		    filter_match		{
			memcpy(&$$, &fmopts, sizeof($$));
		}
		;

filter_match	: filter_elm
		| filter_match filter_elm
		;

filter_elm	: filter_prefix_h	{
			if (fmopts.prefix_l != NULL) {
				yyerror("\"prefix\" already specified");
				YYERROR;
			}
			fmopts.prefix_l = $1;
		}
		| PREFIXLEN prefixlenop		{
			if (fmopts.m.prefixlen.af) {
				yyerror("\"prefixlen\" already specified");
				YYERROR;
			}
			memcpy(&fmopts.m.prefixlen, &$2,
			    sizeof(fmopts.m.prefixlen));
			fmopts.m.prefixlen.af = AF_INET;
		}
		| filter_as_h		{
			if (fmopts.as_l != NULL) {
				yyerror("AS filters already specified");
				YYERROR;
			}
			fmopts.as_l = $1;
		}
		| COMMUNITY STRING	{
			if (fmopts.m.community.as) {
				yyerror("\"community\" already specified");
				free($2);
				YYERROR;
			}
			if (parsecommunity($2, &fmopts.m.community.as,
			    &fmopts.m.community.type) == -1) {
				free($2);
				YYERROR;
			}
			free($2);
		}
		;

prefixlenop	: unaryop number		{
			bzero(&$$, sizeof($$));
			if ($2 > 128) {
				yyerror("prefixlen must be < 128");
				YYERROR;
			}
			$$.op = $1;
			$$.len_min = $2;
		}
		| number binaryop number	{
			bzero(&$$, sizeof($$));
			if ($1 > 128 || $3 > 128) {
				yyerror("prefixlen must be < 128");
				YYERROR;
			}
			if ($1 >= $3) {
				yyerror("start prefixlen is bigger that end");
				YYERROR;
			}
			$$.op = $2;
			$$.len_min = $1;
			$$.len_max = $3;
		}
		;

filter_as_type	: AS		{ $$ = AS_ALL; }
		| SOURCEAS	{ $$ = AS_SOURCE; }
		| TRANSITAS	{ $$ = AS_TRANSIT; }
		;

filter_set	: /* empty */					{ $$ = NULL; }
		| SET filter_set_opt				{
			if (($$ = calloc(1, sizeof(struct filter_set_head))) ==
			    NULL)
				fatal(NULL);
			TAILQ_INIT($$);
			TAILQ_INSERT_TAIL($$, $2, entry);
		}
		| SET optnl "{" optnl filter_set_l optnl "}"	{ $$ = $5; }
		;

filter_set_l	: filter_set_l comma filter_set_opt	{
			$$ = $1;
			if (merge_filterset($$, $3) == 1)
				YYERROR;
		}
		| filter_set_opt {
			if (($$ = calloc(1, sizeof(struct filter_set_head))) ==
			    NULL)
				fatal(NULL);
			TAILQ_INIT($$);
			TAILQ_INSERT_TAIL($$, $1, entry);
		}
		;

filter_set_opt	: LOCALPREF number		{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_LOCALPREF;
			$$->action.metric = $2;
		}
		| LOCALPREF '+' number		{
			if ($3 > INT_MAX) {
				yyerror("localpref to big: max %u", INT_MAX);
				YYERROR;
			}
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_RELATIVE_LOCALPREF;
			$$->action.relative = $3;
		}
		| LOCALPREF '-' number		{
			if ($3 > INT_MAX) {
				yyerror("localpref to small: min -%u", INT_MAX);
				YYERROR;
			}
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_RELATIVE_LOCALPREF;
			$$->action.relative = -$3;
		}
		| MED number			{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_MED;
			$$->action.metric = $2;
		}
		| MED '+' number			{
			if ($3 > INT_MAX) {
				yyerror("metric to big: max %u", INT_MAX);
				YYERROR;
			}
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_RELATIVE_MED;
			$$->action.relative = $3;
		}
		| MED '-' number			{
			if ($3 > INT_MAX) {
				yyerror("metric to small: min -%u", INT_MAX);
				YYERROR;
			}
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_RELATIVE_MED;
			$$->action.relative = -$3;
		}
		| METRIC number			{	/* alias for MED */
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_MED;
			$$->action.metric = $2;
		}
		| METRIC '+' number			{
			if ($3 > INT_MAX) {
				yyerror("metric to big: max %u", INT_MAX);
				YYERROR;
			}
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_RELATIVE_MED;
			$$->action.metric = $3;
		}
		| METRIC '-' number			{
			if ($3 > INT_MAX) {
				yyerror("metric to small: min -%u", INT_MAX);
				YYERROR;
			}
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_RELATIVE_MED;
			$$->action.relative = -$3;
		}
		| WEIGHT number				{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_WEIGHT;
			$$->action.metric = $2;
		}
		| WEIGHT '+' number			{
			if ($3 > INT_MAX) {
				yyerror("weight to big: max %u", INT_MAX);
				YYERROR;
			}
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_RELATIVE_WEIGHT;
			$$->action.relative = $3;
		}
		| WEIGHT '-' number			{
			if ($3 > INT_MAX) {
				yyerror("weight to small: min -%u", INT_MAX);
				YYERROR;
			}
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_RELATIVE_WEIGHT;
			$$->action.relative = -$3;
		}
		| NEXTHOP address		{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_NEXTHOP;
			memcpy(&$$->action.nexthop, &$2,
			    sizeof($$->action.nexthop));
		}
		| NEXTHOP BLACKHOLE		{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_NEXTHOP_BLACKHOLE;
		}
		| NEXTHOP REJECT		{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_NEXTHOP_REJECT;
		}
		| NEXTHOP NOMODIFY		{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_NEXTHOP_NOMODIFY;
		}
		| PREPEND_SELF number		{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_PREPEND_SELF;
			if ($2 > 128) {
				yyerror("too many prepends");
				YYERROR;
			}
			$$->action.prepend = $2;
		}
		| PREPEND_PEER number		{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_PREPEND_PEER;
			if ($2 > 128) {
				yyerror("too many prepends");
				YYERROR;
			}
			$$->action.prepend = $2;
		}
		| PFTABLE STRING		{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_PFTABLE;
			if (!(conf->opts & BGPD_OPT_NOACTION) &&
			    pftable_exists($2) != 0) {
				yyerror("pftable name does not exist");
				free($2);
				YYERROR;
			}
			if (strlcpy($$->action.pftable, $2,
			    sizeof($$->action.pftable)) >=
			    sizeof($$->action.pftable)) {
				yyerror("pftable name too long");
				free($2);
				YYERROR;
			}
			if (pftable_add($2) != 0) {
				yyerror("Couldn't register table");
				free($2);
				YYERROR;
			}
			free($2);
		}
		| RTLABEL STRING		{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_RTLABEL;
			if (strlcpy($$->action.rtlabel, $2,
			    sizeof($$->action.rtlabel)) >=
			    sizeof($$->action.rtlabel)) {
				yyerror("rtlabel name too long");
				free($2);
				YYERROR;
			}
			free($2);
		}
		| COMMUNITY STRING		{
			if (($$ = calloc(1, sizeof(struct filter_set))) == NULL)
				fatal(NULL);
			$$->type = ACTION_SET_COMMUNITY;
			if (parsecommunity($2, &$$->action.community.as,
			    &$$->action.community.type) == -1) {
				free($2);
				YYERROR;
			}
			free($2);
			/* Don't allow setting of any match */
			if ($$->action.community.as == COMMUNITY_ANY ||
			    $$->action.community.type == COMMUNITY_ANY) {
				yyerror("'*' is not allowed in set community");
				YYERROR;
			}
			/* Don't allow setting of unknown well-known types */
			if ($$->action.community.as == COMMUNITY_WELLKNOWN) {
				switch ($$->action.community.type) {
				case COMMUNITY_NO_EXPORT:
				case COMMUNITY_NO_ADVERTISE:
				case COMMUNITY_NO_EXPSUBCONFED:
				case COMMUNITY_NO_PEER:
					/* valid */
					break;
				default:
					/* unknown */
					yyerror("Invalid well-known community");
					YYERROR;
					break;
				}
			}
		}
		;

comma		: ","
		| /* empty */
		;

unaryop		: '='		{ $$ = OP_EQ; }
		| '!' '='	{ $$ = OP_NE; }
		| '<' '='	{ $$ = OP_LE; }
		| '<'		{ $$ = OP_LT; }
		| '>' '='	{ $$ = OP_GE; }
		| '>'		{ $$ = OP_GT; }
		;

binaryop	: '-'		{ $$ = OP_RANGE; }
		| '>' '<'	{ $$ = OP_XRANGE; }
		;

%%

struct keywords {
	const char	*k_name;
	int		 k_val;
};

int
yyerror(const char *fmt, ...)
{
	va_list		 ap;
	char		*nfmt;

	errors = 1;
	va_start(ap, fmt);
	if (asprintf(&nfmt, "%s:%d: %s", infile, yylval.lineno, fmt) == -1)
		fatalx("yyerror asprintf");
	vlog(LOG_CRIT, nfmt, ap);
	va_end(ap);
	free(nfmt);
	return (0);
}

int
kw_cmp(const void *k, const void *e)
{
	return (strcmp(k, ((const struct keywords *)e)->k_name));
}

int
lookup(char *s)
{
	/* this has to be sorted always */
	static const struct keywords keywords[] = {
		{ "AS",			AS},
		{ "IPv4",		IPV4},
		{ "IPv6",		IPV6},
		{ "ah",			AH},
		{ "allow",		ALLOW},
		{ "announce",		ANNOUNCE},
		{ "any",		ANY},
		{ "blackhole",		BLACKHOLE},
		{ "capabilities",	CAPABILITIES},
		{ "community",		COMMUNITY},
		{ "compare",		COMPARE},
		{ "connected",		CONNECTED},
		{ "deny",		DENY},
		{ "depend",		DEPEND},
		{ "descr",		DESCR},
		{ "down",		DOWN},
		{ "dump",		DUMP},
		{ "enforce",		ENFORCE},
		{ "esp",		ESP},
		{ "evaluate",		EVALUATE},
		{ "fib-update",		FIBUPDATE},
		{ "from",		FROM},
		{ "group",		GROUP},
		{ "holdtime",		HOLDTIME},
		{ "ignore",		IGNORE},
		{ "ike",		IKE},
		{ "in",			IN},
		{ "ipsec",		IPSEC},
		{ "key",		KEY},
		{ "listen",		LISTEN},
		{ "local-address",	LOCALADDR},
		{ "localpref",		LOCALPREF},
		{ "log",		LOG},
		{ "match",		MATCH},
		{ "max-prefix",		MAXPREFIX},
		{ "md5sig",		MD5SIG},
		{ "med",		MED},
		{ "metric",		METRIC},
		{ "min",		YMIN},
		{ "multihop",		MULTIHOP},
		{ "neighbor",		NEIGHBOR},
		{ "neighbor-as",	NEIGHBORAS},
		{ "network",		NETWORK},
		{ "nexthop",		NEXTHOP},
		{ "no-modify",		NOMODIFY},
		{ "on",			ON},
		{ "out",		OUT},
		{ "passive",		PASSIVE},
		{ "password",		PASSWORD},
		{ "pftable",		PFTABLE},
		{ "prefix",		PREFIX},
		{ "prefixlen",		PREFIXLEN},
		{ "prepend-neighbor",	PREPEND_PEER},
		{ "prepend-self",	PREPEND_SELF},
		{ "quick",		QUICK},
		{ "rde",		RDE},
		{ "reject",		REJECT},
		{ "remote-as",		REMOTEAS},
		{ "route-collector",	ROUTECOLL},
		{ "route-reflector",	REFLECTOR},
		{ "router-id",		ROUTERID},
		{ "rtlabel",		RTLABEL},
		{ "set",		SET},
		{ "source-as",		SOURCEAS},
		{ "spi",		SPI},
		{ "static",		STATIC},
		{ "tcp",		TCP},
		{ "to",			TO},
		{ "transit-as",		TRANSITAS},
		{ "transparent-as",	TRANSPARENT},
		{ "weight",		WEIGHT}
	};
	const struct keywords	*p;

	p = bsearch(s, keywords, sizeof(keywords)/sizeof(keywords[0]),
	    sizeof(keywords[0]), kw_cmp);

	if (p) {
		if (pdebug > 1)
			fprintf(stderr, "%s: %d\n", s, p->k_val);
		return (p->k_val);
	} else {
		if (pdebug > 1)
			fprintf(stderr, "string: %s\n", s);
		return (STRING);
	}
}

#define MAXPUSHBACK	128

char	*parsebuf;
int	 parseindex;
char	 pushback_buffer[MAXPUSHBACK];
int	 pushback_index = 0;

int
lgetc(FILE *f)
{
	int	c, next;

	if (parsebuf) {
		/* Read character from the parsebuffer instead of input. */
		if (parseindex >= 0) {
			c = parsebuf[parseindex++];
			if (c != '\0')
				return (c);
			parsebuf = NULL;
		} else
			parseindex++;
	}

	if (pushback_index)
		return (pushback_buffer[--pushback_index]);

	while ((c = getc(f)) == '\\') {
		next = getc(f);
		if (next != '\n') {
			if (isspace(next))
				yyerror("whitespace after \\");
			ungetc(next, f);
			break;
		}
		yylval.lineno = lineno;
		lineno++;
	}
	if (c == '\t' || c == ' ') {
		/* Compress blanks to a single space. */
		do {
			c = getc(f);
		} while (c == '\t' || c == ' ');
		ungetc(c, f);
		c = ' ';
	}

	return (c);
}

int
lungetc(int c)
{
	if (c == EOF)
		return (EOF);
	if (parsebuf) {
		parseindex--;
		if (parseindex >= 0)
			return (c);
	}
	if (pushback_index < MAXPUSHBACK-1)
		return (pushback_buffer[pushback_index++] = c);
	else
		return (EOF);
}

int
findeol(void)
{
	int	c;

	parsebuf = NULL;
	pushback_index = 0;

	/* skip to either EOF or the first real EOL */
	while (1) {
		c = lgetc(fin);
		if (c == '\n') {
			lineno++;
			break;
		}
		if (c == EOF)
			break;
	}
	return (ERROR);
}

int
yylex(void)
{
	char	 buf[8096];
	char	*p, *val;
	int	 endc, c;
	int	 token;

top:
	p = buf;
	while ((c = lgetc(fin)) == ' ')
		; /* nothing */

	yylval.lineno = lineno;
	if (c == '#')
		while ((c = lgetc(fin)) != '\n' && c != EOF)
			; /* nothing */
	if (c == '$' && parsebuf == NULL) {
		while (1) {
			if ((c = lgetc(fin)) == EOF)
				return (0);

			if (p + 1 >= buf + sizeof(buf) - 1) {
				yyerror("string too long");
				return (findeol());
			}
			if (isalnum(c) || c == '_') {
				*p++ = (char)c;
				continue;
			}
			*p = '\0';
			lungetc(c);
			break;
		}
		val = symget(buf);
		if (val == NULL) {
			yyerror("macro \"%s\" not defined", buf);
			return (findeol());
		}
		parsebuf = val;
		parseindex = 0;
		goto top;
	}

	switch (c) {
	case '\'':
	case '"':
		endc = c;
		while (1) {
			if ((c = lgetc(fin)) == EOF)
				return (0);
			if (c == endc) {
				*p = '\0';
				break;
			}
			if (c == '\n') {
				lineno++;
				continue;
			}
			if (p + 1 >= buf + sizeof(buf) - 1) {
				yyerror("string too long");
				return (findeol());
			}
			*p++ = (char)c;
		}
		yylval.v.string = strdup(buf);
		if (yylval.v.string == NULL)
			fatal("yylex: strdup");
		return (STRING);
	}

#define allowed_in_string(x) \
	(isalnum(x) || (ispunct(x) && x != '(' && x != ')' && \
	x != '{' && x != '}' && x != '<' && x != '>' && \
	x != '!' && x != '=' && x != '/' && x != '#' && \
	x != ','))

	if (isalnum(c) || c == ':' || c == '_' || c == '*') {
		do {
			*p++ = c;
			if ((unsigned)(p-buf) >= sizeof(buf)) {
				yyerror("string too long");
				return (findeol());
			}
		} while ((c = lgetc(fin)) != EOF && (allowed_in_string(c)));
		lungetc(c);
		*p = '\0';
		if ((token = lookup(buf)) == STRING)
			if ((yylval.v.string = strdup(buf)) == NULL)
				fatal("yylex: strdup");
		return (token);
	}
	if (c == '\n') {
		yylval.lineno = lineno;
		lineno++;
	}
	if (c == EOF)
		return (0);
	return (c);
}

int
parse_config(char *filename, struct bgpd_config *xconf,
    struct mrt_head *xmconf, struct peer **xpeers, struct network_head *nc,
    struct filter_head *xfilter_l)
{
	struct sym		*sym, *next;
	struct peer		*p, *pnext;
	struct listen_addr	*la;
	struct network		*n;
	struct filter_rule	*r;

	if ((fin = fopen(filename, "r")) == NULL) {
		warn("%s", filename);
		return (-1);
	}
	infile = filename;

	if (check_file_secrecy(fileno(fin), filename)) {
		fclose(fin);
		return (-1);
	}

	if ((conf = calloc(1, sizeof(struct bgpd_config))) == NULL)
		fatal(NULL);
	if ((mrtconf = calloc(1, sizeof(struct mrt_head))) == NULL)
		fatal(NULL);
	if ((listen_addrs = calloc(1, sizeof(struct listen_addrs))) == NULL)
		fatal(NULL);
	if ((filter_l = calloc(1, sizeof(struct filter_head))) == NULL)
		fatal(NULL);
	if ((peerfilter_l = calloc(1, sizeof(struct filter_head))) == NULL)
		fatal(NULL);
	if ((groupfilter_l = calloc(1, sizeof(struct filter_head))) == NULL)
		fatal(NULL);
	LIST_INIT(mrtconf);
	TAILQ_INIT(listen_addrs);
	TAILQ_INIT(filter_l);
	TAILQ_INIT(peerfilter_l);
	TAILQ_INIT(groupfilter_l);

	peer_l = NULL;
	peer_l_old = *xpeers;
	curpeer = NULL;
	curgroup = NULL;
	lineno = 1;
	errors = 0;
	id = 1;
	conf->opts = xconf->opts;

	/* network list is always empty in the parent */
	netconf = nc;
	TAILQ_INIT(netconf);
	/* init the empty filter list for later */
	TAILQ_INIT(xfilter_l);

	yyparse();

	fclose(fin);

	/* Free macros and check which have not been used. */
	for (sym = TAILQ_FIRST(&symhead); sym != NULL; sym = next) {
		next = TAILQ_NEXT(sym, entry);
		if ((conf->opts & BGPD_OPT_VERBOSE2) && !sym->used)
			fprintf(stderr, "warning: macro \"%s\" not "
			    "used\n", sym->nam);
		if (!sym->persist) {
			free(sym->nam);
			free(sym->val);
			TAILQ_REMOVE(&symhead, sym, entry);
			free(sym);
		}
	}

	if (errors) {
		/* XXX more leaks in this case */
		while ((la = TAILQ_FIRST(listen_addrs)) != NULL) {
			TAILQ_REMOVE(listen_addrs, la, entry);
			free(la);
		}
		free(listen_addrs);

		for (p = peer_l; p != NULL; p = pnext) {
			pnext = p->next;
			free(p);
		}

		while ((n = TAILQ_FIRST(netconf)) != NULL) {
			TAILQ_REMOVE(netconf, n, entry);
			free(n);
		}

		while ((r = TAILQ_FIRST(filter_l)) != NULL) {
			TAILQ_REMOVE(filter_l, r, entry);
			free(r);
		}

		while ((r = TAILQ_FIRST(peerfilter_l)) != NULL) {
			TAILQ_REMOVE(peerfilter_l, r, entry);
			free(r);
		}

		while ((r = TAILQ_FIRST(groupfilter_l)) != NULL) {
			TAILQ_REMOVE(groupfilter_l, r, entry);
			free(r);
		}
	} else {
		errors += merge_config(xconf, conf, peer_l, listen_addrs);
		errors += mrt_mergeconfig(xmconf, mrtconf);
		*xpeers = peer_l;

		for (p = peer_l_old; p != NULL; p = pnext) {
			pnext = p->next;
			free(p);
		}

		/*
		 * Move filter list and static group and peer filtersets
		 * together. Static group sets come first then peer sets
		 * last normal filter rules.
		 */
		while ((r = TAILQ_FIRST(groupfilter_l)) != NULL) {
			TAILQ_REMOVE(groupfilter_l, r, entry);
			TAILQ_INSERT_TAIL(xfilter_l, r, entry);
		}
		while ((r = TAILQ_FIRST(peerfilter_l)) != NULL) {
			TAILQ_REMOVE(peerfilter_l, r, entry);
			TAILQ_INSERT_TAIL(xfilter_l, r, entry);
		}
		while ((r = TAILQ_FIRST(filter_l)) != NULL) {
			TAILQ_REMOVE(filter_l, r, entry);
			TAILQ_INSERT_TAIL(xfilter_l, r, entry);
		}
	}

	free(conf);
	free(mrtconf);
	free(filter_l);
	free(peerfilter_l);
	free(groupfilter_l);

	return (errors ? -1 : 0);
}

int
symset(const char *nam, const char *val, int persist)
{
	struct sym	*sym;

	for (sym = TAILQ_FIRST(&symhead); sym && strcmp(nam, sym->nam);
	    sym = TAILQ_NEXT(sym, entry))
		;	/* nothing */

	if (sym != NULL) {
		if (sym->persist == 1)
			return (0);
		else {
			free(sym->nam);
			free(sym->val);
			TAILQ_REMOVE(&symhead, sym, entry);
			free(sym);
		}
	}
	if ((sym = calloc(1, sizeof(*sym))) == NULL)
		return (-1);

	sym->nam = strdup(nam);
	if (sym->nam == NULL) {
		free(sym);
		return (-1);
	}
	sym->val = strdup(val);
	if (sym->val == NULL) {
		free(sym->nam);
		free(sym);
		return (-1);
	}
	sym->used = 0;
	sym->persist = persist;
	TAILQ_INSERT_TAIL(&symhead, sym, entry);
	return (0);
}

int
cmdline_symset(char *s)
{
	char	*sym, *val;
	int	ret;
	size_t	len;

	if ((val = strrchr(s, '=')) == NULL)
		return (-1);

	len = strlen(s) - strlen(val) + 1;
	if ((sym = malloc(len)) == NULL)
		fatal("cmdline_symset: malloc");

	strlcpy(sym, s, len);

	ret = symset(sym, val + 1, 1);
	free(sym);

	return (ret);
}

char *
symget(const char *nam)
{
	struct sym	*sym;

	TAILQ_FOREACH(sym, &symhead, entry)
		if (strcmp(nam, sym->nam) == 0) {
			sym->used = 1;
			return (sym->val);
		}
	return (NULL);
}

int
atoul(char *s, u_long *ulvalp)
{
	u_long	 ulval;
	char	*ep;

	errno = 0;
	ulval = strtoul(s, &ep, 0);
	if (s[0] == '\0' || *ep != '\0')
		return (-1);
	if (errno == ERANGE && ulval == ULONG_MAX)
		return (-1);
	*ulvalp = ulval;
	return (0);
}

int
getcommunity(char *s)
{
	u_long	ulval;

	if (strcmp(s, "*") == 0)
		return (COMMUNITY_ANY);
	if (strcmp(s, "neighbor-as") == 0)
		return (COMMUNITY_NEIGHBOR_AS);
	if (atoul(s, &ulval) == -1) {
		yyerror("\"%s\" is not a number", s);
		return (COMMUNITY_ERROR);
	}
	if (ulval > USHRT_MAX) {
		yyerror("Community too big: max %u", USHRT_MAX);
		return (COMMUNITY_ERROR);
	}
	return (ulval);
}

int
parsecommunity(char *s, int *as, int *type)
{
	char *p;
	int i;

	/* Well-known communities */
	if (strcasecmp(s, "NO_EXPORT") == 0) {
		*as = COMMUNITY_WELLKNOWN;
		*type = COMMUNITY_NO_EXPORT;
		return (0);
	} else if (strcasecmp(s, "NO_ADVERTISE") == 0) {
		*as = COMMUNITY_WELLKNOWN;
		*type = COMMUNITY_NO_ADVERTISE;
		return (0);
	} else if (strcasecmp(s, "NO_EXPORT_SUBCONFED") == 0) {
		*as = COMMUNITY_WELLKNOWN;
		*type = COMMUNITY_NO_EXPSUBCONFED;
		return (0);
	} else if (strcasecmp(s, "NO_PEER") == 0) {
		*as = COMMUNITY_WELLKNOWN;
		*type = COMMUNITY_NO_PEER;
		return (0);
	}

	if ((p = strchr(s, ':')) == NULL) {
		yyerror("Bad community syntax");
		return (-1);
	}
	*p++ = 0;

	if ((i = getcommunity(s)) == COMMUNITY_ERROR)
		return (-1);
	if (i == 0 || i == USHRT_MAX) {
		yyerror("Bad community AS number");
		return (-1);
	}
	*as = i;

	if ((i = getcommunity(p)) == COMMUNITY_ERROR)
		return (-1);
	*type = i;

	return (0);
}

struct peer *
alloc_peer(void)
{
	struct peer	*p;

	if ((p = calloc(1, sizeof(struct peer))) == NULL)
		fatal("new_peer");

	/* some sane defaults */
	p->state = STATE_NONE;
	p->next = NULL;
	p->conf.distance = 1;
	p->conf.announce_type = ANNOUNCE_UNDEF;
	p->conf.announce_capa = 1;
	p->conf.capabilities.mp_v4 = SAFI_UNICAST;
	p->conf.capabilities.mp_v6 = SAFI_NONE;
	p->conf.capabilities.refresh = 1;

	return (p);
}

struct peer *
new_peer(void)
{
	struct peer		*p;

	p = alloc_peer();

	if (curgroup != NULL) {
		memcpy(p, curgroup, sizeof(struct peer));
		if (strlcpy(p->conf.group, curgroup->conf.group,
		    sizeof(p->conf.group)) >= sizeof(p->conf.group))
			fatalx("new_peer group strlcpy");
		if (strlcpy(p->conf.descr, curgroup->conf.descr,
		    sizeof(p->conf.descr)) >= sizeof(p->conf.descr))
			fatalx("new_peer descr strlcpy");
		p->conf.groupid = curgroup->conf.id;
	}
	p->next = NULL;

	return (p);
}

struct peer *
new_group(void)
{
	return (alloc_peer());
}

int
add_mrtconfig(enum mrt_type type, char *name, time_t timeout, struct peer *p)
{
	struct mrt	*m, *n;

	LIST_FOREACH(m, mrtconf, entry) {
		if (p == NULL) {
			if (m->peer_id != 0 || m->group_id != 0)
				continue;
		} else {
			if (m->peer_id != p->conf.id ||
			    m->group_id != p->conf.groupid)
				continue;
		}
		if (m->type == type) {
			yyerror("only one mrtdump per type allowed.");
			return (-1);
		}
	}

	if ((n = calloc(1, sizeof(struct mrt_config))) == NULL)
		fatal("add_mrtconfig");

	n->type = type;
	if (strlcpy(MRT2MC(n)->name, name, sizeof(MRT2MC(n)->name)) >=
	    sizeof(MRT2MC(n)->name)) {
		yyerror("filename \"%s\" too long: max %u",
		    name, sizeof(MRT2MC(n)->name) - 1);
		free(n);
		return (-1);
	}
	MRT2MC(n)->ReopenTimerInterval = timeout;
	if (p != NULL) {
		if (curgroup == p) {
			n->peer_id = 0;
			n->group_id = p->conf.id;
		} else {
			n->peer_id = p->conf.id;
			n->group_id = 0;
		}
	}

	LIST_INSERT_HEAD(mrtconf, n, entry);

	return (0);
}

int
get_id(struct peer *newpeer)
{
	struct peer	*p;

	if (newpeer->conf.remote_addr.af)
		for (p = peer_l_old; p != NULL; p = p->next)
			if (!memcmp(&p->conf.remote_addr,
			    &newpeer->conf.remote_addr,
			    sizeof(p->conf.remote_addr))) {
				newpeer->conf.id = p->conf.id;
				return (0);
			}

	/* new one */
	for (; id < UINT_MAX / 2; id++) {
		for (p = peer_l_old; p != NULL && p->conf.id != id; p = p->next)
			;	/* nothing */
		if (p == NULL) {	/* we found a free id */
			newpeer->conf.id = id++;
			return (0);
		}
	}

	return (-1);
}

int
expand_rule(struct filter_rule *rule, struct filter_peers_l *peer,
    struct filter_match_l *match, struct filter_set_head *set)
{
	struct filter_rule	*r;
	struct filter_peers_l	*p, *pnext;
	struct filter_prefix_l	*prefix, *prefix_next;
	struct filter_as_l	*a, *anext;
	struct filter_set	*s;

	p = peer;
	do {
		prefix = match->prefix_l;
		do {
			a = match->as_l;
			do {
				if ((r = calloc(1,
				    sizeof(struct filter_rule))) == NULL) {
					log_warn("expand_rule");
					return (-1);
				}

				memcpy(r, rule, sizeof(struct filter_rule));
				memcpy(&r->match, match,
				    sizeof(struct filter_match));
				TAILQ_INIT(&r->set);
				copy_filterset(set, &r->set);

				if (p != NULL)
					memcpy(&r->peer, &p->p,
					    sizeof(struct filter_peers));

				if (prefix != NULL)
					memcpy(&r->match.prefix, &prefix->p,
					    sizeof(r->match.prefix));

				if (a != NULL)
					memcpy(&r->match.as, &a->a,
					    sizeof(struct filter_as));

				TAILQ_INSERT_TAIL(filter_l, r, entry);

				if (a != NULL)
					a = a->next;
			} while (a != NULL);

			if (prefix != NULL)
				prefix = prefix->next;
		} while (prefix != NULL);

		if (p != NULL)
			p = p->next;
	} while (p != NULL);

	for (p = peer; p != NULL; p = pnext) {
		pnext = p->next;
		free(p);
	}

	for (prefix = match->prefix_l; prefix != NULL; prefix = prefix_next) {
		prefix_next = prefix->next;
		free(prefix);
	}

	for (a = match->as_l; a != NULL; a = anext) {
		anext = a->next;
		free(a);
	}

	if (set != NULL) {
		while ((s = TAILQ_FIRST(set)) != NULL) {
			TAILQ_REMOVE(set, s, entry);
			free(s);
		}
		free(set);
	}

	return (0);
}

int
str2key(char *s, char *dest, size_t max_len)
{
	unsigned	i;
	char		t[3];

	if (strlen(s) / 2 > max_len) {
		yyerror("key too long");
		return (-1);
	}

	if (strlen(s) % 2) {
		yyerror("key must be of even length");
		return (-1);
	}

	for (i = 0; i < strlen(s) / 2; i++) {
		t[0] = s[2*i];
		t[1] = s[2*i + 1];
		t[2] = 0;
		if (!isxdigit(t[0]) || !isxdigit(t[1])) {
			yyerror("key must be specified in hex");
			return (-1);
		}
		dest[i] = strtoul(t, NULL, 16);
	}

	return (0);
}

int
neighbor_consistent(struct peer *p)
{
	/* local-address and peer's address: same address family */
	if (p->conf.local_addr.af &&
	    p->conf.local_addr.af != p->conf.remote_addr.af) {
		yyerror("local-address and neighbor address "
		    "must be of the same address family");
		return (-1);
	}

	/* with any form of ipsec local-address is required */
	if ((p->conf.auth.method == AUTH_IPSEC_IKE_ESP ||
	    p->conf.auth.method == AUTH_IPSEC_IKE_AH ||
	    p->conf.auth.method == AUTH_IPSEC_MANUAL_ESP ||
	    p->conf.auth.method == AUTH_IPSEC_MANUAL_AH) &&
	    !p->conf.local_addr.af) {
		yyerror("neighbors with any form of IPsec configured "
		    "need local-address to be specified");
		return (-1);
	}

	/* with static keying we need both directions */
	if ((p->conf.auth.method == AUTH_IPSEC_MANUAL_ESP ||
	    p->conf.auth.method == AUTH_IPSEC_MANUAL_AH) &&
	    (!p->conf.auth.spi_in || !p->conf.auth.spi_out)) {
		yyerror("with manual keyed IPsec, SPIs and keys "
		    "for both directions are required");
		return (-1);
	}

	if (!conf->as) {
		yyerror("AS needs to be given before neighbor definitions");
		return (-1);
	}

	/* set default values if they where undefined */
	p->conf.ebgp = (p->conf.remote_as != conf->as);
	if (p->conf.announce_type == ANNOUNCE_UNDEF)
		p->conf.announce_type = p->conf.ebgp == 0 ?
		    ANNOUNCE_ALL : ANNOUNCE_SELF;
	if (p->conf.enforce_as == ENFORCE_AS_UNDEF)
		p->conf.enforce_as = p->conf.ebgp == 0 ?
		    ENFORCE_AS_OFF : ENFORCE_AS_ON;

	/* EBGP neighbors are not allowed in route reflector clusters */
	if (p->conf.reflector_client && p->conf.ebgp) {
		yyerror("EBGP neighbors are not allowed in route "
		    "reflector clusters");
		return (-1);
	}

	return (0);
}

int
merge_filterset(struct filter_set_head *sh, struct filter_set *s)
{
	struct filter_set	*t;

	TAILQ_FOREACH(t, sh, entry) {
		/*
		 * need to cycle across the full list because even
		 * if types are not equal filterset_cmp() may return 0.
		 */
		if (filterset_cmp(s, t) == 0) {
			if (s->type == ACTION_SET_COMMUNITY)
				yyerror("community is already set");
			else
				yyerror("redefining set parameter %s",
				    filterset_names[s->type]);
			return (-1);
		}
	}

	TAILQ_FOREACH(t, sh, entry) {
		if (s->type < t->type) {
			TAILQ_INSERT_BEFORE(t, s, entry);
			return (0);
		}
		if (s->type == t->type)
			switch (s->type) {
			case ACTION_SET_COMMUNITY:
				if (s->action.community.as <
				    t->action.community.as ||
				    (s->action.community.as ==
				    t->action.community.as &&
				    s->action.community.type <
				    t->action.community.type)) {
					TAILQ_INSERT_BEFORE(t, s, entry);
					return (0);
				}
				break;
			case ACTION_SET_NEXTHOP:
				if (s->action.nexthop.af <
				    t->action.nexthop.af) {
					TAILQ_INSERT_BEFORE(t, s, entry);
					return (0);
				}
				break;
			default:
				break;
			}
	}

	TAILQ_INSERT_TAIL(sh, s, entry);
	return (0);
}

void
copy_filterset(struct filter_set_head *source, struct filter_set_head *dest)
{
	struct filter_set	*s, *t;

	if (source == NULL)
		return;

	TAILQ_FOREACH(s, source, entry) {
		if ((t = calloc(1, sizeof(struct filter_set))) == NULL)
			fatal(NULL);
		memcpy(t, s, sizeof(struct filter_set));
		TAILQ_INSERT_TAIL(dest, t, entry);
	}
}

void
move_filterset(struct filter_set_head *source, struct filter_set_head *dest)
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

struct filter_rule *
get_rule(enum action_types type)
{
	struct filter_rule	*r;
	int			 out;

	switch (type) {
	case ACTION_SET_PREPEND_SELF:
	case ACTION_SET_NEXTHOP_NOMODIFY:
		out = 1;
		break;
	default:
		out = 0;
		break;
	}
	r = (curpeer == curgroup) ? curgroup_filter[out] : curpeer_filter[out];
	if (r == NULL) {
		if ((r = calloc(1, sizeof(struct filter_rule))) == NULL)
			fatal(NULL);
		r->quick = 0;
		r->dir = out ? DIR_OUT : DIR_IN;
		r->action = ACTION_NONE;
		TAILQ_INIT(&r->set);
		if (curpeer == curgroup) {
			/* group */
			r->peer.groupid = curgroup->conf.id;
			curgroup_filter[out] = r;
		} else {
			/* peer */
			r->peer.peerid = curpeer->conf.id;
			curpeer_filter[out] = r;
		}
	}
	return (r);
}
