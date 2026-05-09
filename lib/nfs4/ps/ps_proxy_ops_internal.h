/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PS_PROXY_OPS_INTERNAL_H
#define PS_PROXY_OPS_INTERNAL_H

/*
 * Whitebox surface for ps_proxy_ops.c.  Lives in lib/nfs4/ps/ (NOT
 * in lib/include/) to signal that this is private to the PS subsystem
 * and consumed only by tests.  Mirrors the ps_renewal_internal.h
 * pattern from the renewal-thread tests.
 *
 * Production code does NOT include this header -- the symbols here
 * are the implementation details of ps_proxy_send_with_kick that
 * tests need to drive without setting up a real RPC stack.
 */

struct mds_compound;
struct mds_session;
struct authunix_parms;

/*
 * The worker-side send wrapper from ps_proxy_ops.c, exported here for
 * the kick-wiring test.  Same contract as the static helper:
 *  - wraps mds_compound_send_with_auth
 *  - if the result is a session-killer AND ms->ms_kick_listener_id
 *    is non-zero, fires ps_listener_kick_reconnect on that id
 *  - returns the underlying send_with_auth value verbatim
 */
int ps_proxy_send_with_kick(struct mds_compound *mc, struct mds_session *ms,
			    const struct authunix_parms *creds);

#endif /* PS_PROXY_OPS_INTERNAL_H */
