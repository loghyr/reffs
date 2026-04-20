/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Per-client export policy matching.
 *
 * Implements exports(5)-style client matching:
 *   single host  -- priority 1
 *   CIDR         -- priority 2
 *   hostname wildcard -- priority 3
 *   anonymous *  -- priority 4
 *
 * client_rule_match() does a two-pass scan: first pass records the
 * best match (lowest priority number) seen so far; second pass is not
 * needed because the match is found inline.  Within the same priority
 * the first listed rule wins, so the array is scanned in order and
 * we replace the best match only when a strictly better priority is
 * found.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <arpa/inet.h>
#include <fnmatch.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "reffs/client_match.h"

/* Priority constants -- lower number = higher priority. */
#define PRIO_HOST 1
#define PRIO_CIDR 2
#define PRIO_WILD 3
#define PRIO_STAR 4
#define PRIO_NONE 5 /* sentinel: no match */

/*
 * Determine whether `spec` is an anonymous wildcard, i.e. exactly "*".
 */
static bool is_star(const char *spec)
{
	return spec[0] == '*' && spec[1] == '\0';
}

/*
 * Determine whether `spec` contains a '/' (CIDR notation).
 */
static bool is_cidr(const char *spec)
{
	return strchr(spec, '/') != NULL;
}

/*
 * Determine whether `spec` is a hostname wildcard.
 * Hostname wildcards start with '*' but are not the bare "*".
 */
static bool is_hostname_wildcard(const char *spec)
{
	return spec[0] == '*' && spec[1] != '\0';
}

/*
 * Return the priority class for spec.
 */
static int spec_priority(const char *spec)
{
	if (is_star(spec))
		return PRIO_STAR;
	if (is_hostname_wildcard(spec))
		return PRIO_WILD;
	if (is_cidr(spec))
		return PRIO_CIDR;
	/* Anything else: attempt single-host parse */
	return PRIO_HOST;
}

/*
 * Match peer against an exact host spec (IPv4 or IPv6 address string).
 * Returns true on match.
 */
static bool match_host(const char *spec, const struct sockaddr_storage *peer)
{
	struct in_addr a4;
	struct in6_addr a6;

	if (peer->ss_family == AF_INET) {
		const struct sockaddr_in *sin =
			(const struct sockaddr_in *)peer;

		if (inet_pton(AF_INET, spec, &a4) == 1)
			return memcmp(&sin->sin_addr, &a4, sizeof(a4)) == 0;
	} else if (peer->ss_family == AF_INET6) {
		const struct sockaddr_in6 *sin6 =
			(const struct sockaddr_in6 *)peer;

		if (inet_pton(AF_INET6, spec, &a6) == 1)
			return memcmp(&sin6->sin6_addr, &a6, sizeof(a6)) == 0;
	}
	return false;
}

/*
 * Match peer against a CIDR spec "addr/prefix".
 * Returns true on match.
 */
static bool match_cidr(const char *spec, const struct sockaddr_storage *peer)
{
	char addr_buf[INET6_ADDRSTRLEN + 4]; /* enough for addr + "/" + prefix */
	char *slash;
	int prefix_len;

	strncpy(addr_buf, spec, sizeof(addr_buf) - 1);
	addr_buf[sizeof(addr_buf) - 1] = '\0';

	slash = strchr(addr_buf, '/');
	if (!slash)
		return false;
	*slash = '\0';
	prefix_len = atoi(slash + 1);

	if (peer->ss_family == AF_INET) {
		struct in_addr net_addr;

		if (inet_pton(AF_INET, addr_buf, &net_addr) != 1)
			return false;
		if (prefix_len < 0 || prefix_len > 32)
			return false;

		const struct sockaddr_in *sin =
			(const struct sockaddr_in *)peer;
		uint32_t mask = prefix_len == 0 ?
					0 :
					htonl(~((1u << (32 - prefix_len)) - 1));
		return (sin->sin_addr.s_addr & mask) ==
		       (net_addr.s_addr & mask);

	} else if (peer->ss_family == AF_INET6) {
		struct in6_addr net_addr;

		if (inet_pton(AF_INET6, addr_buf, &net_addr) != 1)
			return false;
		if (prefix_len < 0 || prefix_len > 128)
			return false;

		const struct sockaddr_in6 *sin6 =
			(const struct sockaddr_in6 *)peer;

		/*
		 * Compare byte-by-byte; the prefix_len determines how many
		 * full bytes and what mask to apply to the boundary byte.
		 */
		int full_bytes = prefix_len / 8;
		int rem_bits = prefix_len % 8;

		if (memcmp(sin6->sin6_addr.s6_addr, net_addr.s6_addr,
			   (size_t)full_bytes) != 0)
			return false;
		if (rem_bits > 0) {
			unsigned char mask =
				(unsigned char)(0xff << (8 - rem_bits));
			if ((sin6->sin6_addr.s6_addr[full_bytes] & mask) !=
			    (net_addr.s6_addr[full_bytes] & mask))
				return false;
		}
		return true;
	}
	return false;
}

/*
 * Reverse-DNS cache for hostname-wildcard matching.  A single slow
 * getnameinfo call would otherwise stall whichever worker picks up
 * the request; the cache caps that blocking to once per peer per
 * NAME_CACHE_TTL seconds.  Fixed-size direct-mapped table with one
 * entry per bucket; collisions evict the older entry (acceptable
 * since each miss only costs one extra DNS roundtrip and the eviction
 * victim will retry on its next request).
 */
#define NAME_CACHE_SIZE 64
#define NAME_CACHE_TTL 300 /* 5 minutes */

struct name_cache_entry {
	bool valid;
	bool resolved; /* getnameinfo succeeded */
	socklen_t addrlen;
	struct sockaddr_storage addr;
	time_t expires_at;
	char host[NI_MAXHOST];
};

static struct name_cache_entry name_cache[NAME_CACHE_SIZE];
static pthread_mutex_t name_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t sockaddr_hash(const struct sockaddr_storage *peer,
			      socklen_t addrlen)
{
	const unsigned char *p = (const unsigned char *)peer;
	uint32_t h = 2166136261u; /* FNV-1a 32-bit offset basis */
	for (socklen_t i = 0; i < addrlen; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

/*
 * Resolve peer to a hostname using a TTL cache.  Writes the hostname
 * into host_out (of size host_out_len) on success and returns 0;
 * returns -1 on DNS failure.  Thread-safe.
 */
static int resolve_peer_cached(const struct sockaddr_storage *peer,
			       socklen_t addrlen, char *host_out,
			       size_t host_out_len)
{
	time_t now = time(NULL);
	uint32_t idx = sockaddr_hash(peer, addrlen) % NAME_CACHE_SIZE;

	pthread_mutex_lock(&name_cache_mutex);
	struct name_cache_entry *e = &name_cache[idx];
	if (e->valid && e->addrlen == addrlen &&
	    memcmp(&e->addr, peer, addrlen) == 0 && e->expires_at > now) {
		bool resolved = e->resolved;
		if (resolved) {
			strncpy(host_out, e->host, host_out_len - 1);
			host_out[host_out_len - 1] = '\0';
		}
		pthread_mutex_unlock(&name_cache_mutex);
		return resolved ? 0 : -1;
	}
	pthread_mutex_unlock(&name_cache_mutex);

	/* Cache miss or expired: pay the sync lookup cost once. */
	char host[NI_MAXHOST];
	int rc = getnameinfo((const struct sockaddr *)peer, addrlen, host,
			     sizeof(host), NULL, 0, NI_NAMEREQD);

	pthread_mutex_lock(&name_cache_mutex);
	memset(e, 0, sizeof(*e));
	memcpy(&e->addr, peer, addrlen);
	e->addrlen = addrlen;
	e->valid = true;
	e->expires_at = now + NAME_CACHE_TTL;
	if (rc == 0) {
		e->resolved = true;
		strncpy(e->host, host, sizeof(e->host) - 1);
		strncpy(host_out, host, host_out_len - 1);
		host_out[host_out_len - 1] = '\0';
	} else {
		e->resolved = false;
		e->host[0] = '\0';
	}
	pthread_mutex_unlock(&name_cache_mutex);

	return rc == 0 ? 0 : -1;
}

/*
 * Match peer against a hostname wildcard spec (e.g. "*.lab.example.com").
 * Reverse-DNS lookup is TTL-cached so a slow resolver only blocks once per
 * peer per NAME_CACHE_TTL seconds.  On DNS failure, never fail open -- the
 * rule is skipped.
 */
static bool match_hostname_wildcard(const char *spec,
				    const struct sockaddr_storage *peer)
{
	char host[NI_MAXHOST];
	socklen_t addrlen;

	if (peer->ss_family == AF_INET)
		addrlen = sizeof(struct sockaddr_in);
	else if (peer->ss_family == AF_INET6)
		addrlen = sizeof(struct sockaddr_in6);
	else
		return false;

	if (resolve_peer_cached(peer, addrlen, host, sizeof(host)) != 0)
		return false;

	return fnmatch(spec, host, FNM_CASEFOLD) == 0;
}

/*
 * client_rule_match -- see client_match.h for API contract.
 */
const struct sb_client_rule *
client_rule_match(const struct sb_client_rule *rules, unsigned int nrules,
		  const struct sockaddr_storage *peer)
{
	if (!rules || nrules == 0 || !peer)
		return NULL;

	const struct sb_client_rule *best = NULL;
	int best_prio = PRIO_NONE;

	for (unsigned int i = 0; i < nrules; i++) {
		const struct sb_client_rule *r = &rules[i];
		const char *spec = r->scr_match;
		int prio = spec_priority(spec);

		/* Skip if this class can't beat the current best. */
		if (prio >= best_prio)
			continue;

		bool matched = false;

		switch (prio) {
		case PRIO_HOST:
			matched = match_host(spec, peer);
			break;
		case PRIO_CIDR:
			matched = match_cidr(spec, peer);
			break;
		case PRIO_WILD:
			matched = match_hostname_wildcard(spec, peer);
			break;
		case PRIO_STAR:
			matched = true;
			break;
		default:
			break;
		}

		if (matched) {
			best = r;
			best_prio = prio;
			if (best_prio == PRIO_HOST)
				break; /* can't do better */
		}
	}

	return best;
}

/*
 * Apply root_squash / all_squash from the matched export rule to the
 * AUTH_SYS credential.
 *
 *   all_squash  -- always map to nobody (uid=65534, gid=65534)
 *   root_squash -- map uid/gid 0 to nobody
 *
 * Squashing also zeroes the supplementary group list so that no
 * group membership from the wire credential survives.
 *
 * No-op when rule is NULL (no matching rule found -- caller should
 * have already returned an access error in that case).
 */
void rpc_cred_squash(struct authunix_parms *ap,
		     const struct sb_client_rule *rule)
{
	if (!rule)
		return;

	if (rule->scr_all_squash ||
	    (rule->scr_root_squash && ap->aup_uid == 0)) {
		ap->aup_uid = 65534;
		ap->aup_gid = 65534;
		ap->aup_len = 0;
		ap->aup_gids = NULL;
	}
}
