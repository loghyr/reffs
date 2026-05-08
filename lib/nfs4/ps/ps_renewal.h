/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_RENEWAL_H
#define _REFFS_PS_RENEWAL_H

#include <stdint.h>

/*
 * PS upstream session keepalive.
 *
 * Each [[proxy_mds]] listener holds a long-lived NFSv4.2 session to
 * its upstream MDS.  The MDS expires the session after one lease
 * period of inactivity (default 90s, RFC 8881 S2.10.6).  Without a
 * keepalive, periods of low client traffic let the upstream session
 * drift past the lease and the next forwarded op fails with
 * NFS4ERR_BADSESSION -- exactly the failure mode that blocked PS
 * Phase 3 e2e mount verification.
 *
 * This subsystem runs a single worker thread that ticks every
 * `interval_seconds` and sends a SEQUENCE-only compound on every
 * non-NULL pls_session.  SEQUENCE is the standard renewal vehicle
 * (RFC 8881 S18.46): the server's slot bookkeeping refreshes the
 * lease as a side effect.
 *
 * Cadence: pick `interval_seconds = lease / 3` so a single missed
 * tick (network blip, GC stall) still leaves time for two more
 * before the lease expires.
 *
 * Reconnect on BADSESSION:
 *   NOT_NOW_BROWN_COW.  When a renewal returns NFS4ERR_BADSESSION
 *   (lease already expired between ticks), the thread logs once and
 *   leaves pls_session in place.  Subsequent forwarded ops will hit
 *   the same status; reconnect logic that ties to the [[proxy_mds]]
 *   config to recreate the TLS session is its own slice.
 */

/*
 * Start the keepalive thread.  Idempotent: a second call returns 0
 * without spawning a new thread.  The thread terminates when
 * ps_renewal_stop() is called and the next tick wakes up.
 *
 * Returns 0 on success, -errno on pthread_create / cond init failure.
 */
int ps_renewal_start(uint32_t interval_seconds);

/*
 * Stop the keepalive thread.  Blocks until the worker has joined.
 * Safe to call before ps_renewal_start (no-op).  Must run before
 * ps_state_fini() because the thread reads ps_listeners[].
 */
void ps_renewal_stop(void);

#endif /* _REFFS_PS_RENEWAL_H */
