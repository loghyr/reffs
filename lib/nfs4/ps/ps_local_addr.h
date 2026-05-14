/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_LOCAL_ADDR_H
#define _REFFS_PS_LOCAL_ADDR_H

#include <stdbool.h>

#include "ps_state.h"

/*
 * Populate pls->pls_local_addrs[] from getifaddrs(3).  Walks every
 * AF_INET / AF_INET6 unicast interface address on the host and
 * copies it into the table, capping pls_nlocal_addrs at
 * PS_MAX_LOCAL_ADDRS.  Loopback is always included on Unix; the
 * cap rarely bites (host typically has <= 4 routable addresses).
 *
 * Idempotent: subsequent calls overwrite the table.  Intended for
 * register-time seeding under single-writer discipline; the
 * publish edge on ps_nlisteners fences the table for readers.
 *
 * Returns 0 on success, -errno on getifaddrs(3) failure.  An
 * empty table (no AF_INET / AF_INET6 entries found) is a success;
 * the match primitive returns false for every probe and the
 * short-circuit decision falls back to the RPC path.
 */
int ps_local_addr_seed(struct ps_listener_state *pls);

/*
 * Return true if `host` is a numeric IPv4 / IPv6 string that
 * matches any entry in pls->pls_local_addrs.  Uses
 * AI_NUMERICHOST: hostnames are NOT resolved -- the match decision
 * is on the per-mirror fanout setup hot path and must not block
 * on DNS.  A non-numeric `host` returns false.
 *
 * NULL or empty `host`, or NULL `pls`, returns false (callers may
 * pass uninitialized fields from a partially-resolved
 * deviceinfo; an early false is the safe default).
 */
bool ps_local_addr_match(const struct ps_listener_state *pls, const char *host);

#endif /* _REFFS_PS_LOCAL_ADDR_H */
