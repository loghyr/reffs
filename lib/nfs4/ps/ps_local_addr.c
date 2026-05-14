/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>

#include "ps_local_addr.h"

/*
 * Copy one sockaddr (AF_INET or AF_INET6) into the table at index
 * out_idx, advancing it on success.  Returns true if the address
 * was stored, false if the family is not one we track.  Truncation
 * (full table) is handled by the caller before invocation.
 */
static bool store_one(struct ps_listener_state *pls, uint32_t out_idx,
		      const struct sockaddr *sa)
{
	if (!sa)
		return false;

	switch (sa->sa_family) {
	case AF_INET: {
		struct sockaddr_in *dst =
			(struct sockaddr_in *)&pls->pls_local_addrs[out_idx]
				.la_ss;
		memcpy(dst, sa, sizeof(*dst));
		pls->pls_local_addrs[out_idx].la_len = sizeof(*dst);
		return true;
	}
	case AF_INET6: {
		struct sockaddr_in6 *dst =
			(struct sockaddr_in6 *)&pls->pls_local_addrs[out_idx]
				.la_ss;
		memcpy(dst, sa, sizeof(*dst));
		pls->pls_local_addrs[out_idx].la_len = sizeof(*dst);
		return true;
	}
	default:
		return false;
	}
}

int ps_local_addr_seed(struct ps_listener_state *pls)
{
	struct ifaddrs *list = NULL;

	if (!pls)
		return -EINVAL;

	memset(pls->pls_local_addrs, 0, sizeof(pls->pls_local_addrs));
	pls->pls_nlocal_addrs = 0;

	if (getifaddrs(&list) != 0)
		return -errno;

	uint32_t n = 0;

	for (struct ifaddrs *cur = list; cur != NULL && n < PS_MAX_LOCAL_ADDRS;
	     cur = cur->ifa_next) {
		if (store_one(pls, n, cur->ifa_addr))
			n++;
	}
	pls->pls_nlocal_addrs = n;

	freeifaddrs(list);
	return 0;
}

/*
 * Compare a parsed candidate sockaddr against one entry of the
 * table.  Returns true on byte-for-byte match of the family-
 * specific address field; ports and zone-ids are ignored (the
 * Phase 5 design declares zone-id matching out of scope).
 */
static bool one_match(const struct ps_local_addr *entry,
		      const struct sockaddr *cand)
{
	if (entry->la_len == 0 || !cand)
		return false;
	const struct sockaddr *stored = (const struct sockaddr *)&entry->la_ss;

	if (stored->sa_family != cand->sa_family)
		return false;

	if (cand->sa_family == AF_INET) {
		const struct sockaddr_in *a =
			(const struct sockaddr_in *)stored;
		const struct sockaddr_in *b = (const struct sockaddr_in *)cand;
		return a->sin_addr.s_addr == b->sin_addr.s_addr;
	}
	if (cand->sa_family == AF_INET6) {
		const struct sockaddr_in6 *a =
			(const struct sockaddr_in6 *)stored;
		const struct sockaddr_in6 *b =
			(const struct sockaddr_in6 *)cand;
		return memcmp(&a->sin6_addr, &b->sin6_addr,
			      sizeof(a->sin6_addr)) == 0;
	}
	return false;
}

bool ps_local_addr_match(const struct ps_listener_state *pls, const char *host)
{
	if (!pls || !host || host[0] == '\0')
		return false;

	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV,
	};
	struct addrinfo *res = NULL;

	if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
		return false;

	bool matched = false;

	for (struct addrinfo *ai = res; ai != NULL && !matched;
	     ai = ai->ai_next) {
		for (uint32_t i = 0; i < pls->pls_nlocal_addrs; i++) {
			if (one_match(&pls->pls_local_addrs[i], ai->ai_addr)) {
				matched = true;
				break;
			}
		}
	}

	freeaddrinfo(res);
	return matched;
}
