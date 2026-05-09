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
 * Reconnect on session-killer wire codes:
 *   When the renewal returns NFS4ERR_BADSESSION / NFS4ERR_DEADSESSION
 *   / NFS4ERR_STALE_CLIENTID -- or a connection-killer errno like
 *   -EPIPE / -ECONNRESET -- the thread tears down the dead session
 *   and replays the bring-up: TLS handshake + EXCHANGE_ID +
 *   CREATE_SESSION + PROXY_REGISTRATION (with the same
 *   registration_id reffsd's boot path generated, so the upstream
 *   MDS treats it as a renewal not a squat).  Failed reconnect
 *   attempts apply a capped exponential backoff (0s, 1s, 2s, 4s,
 *   8s, 16s, 32s, 60s, 60s, ...) per listener; success resets the
 *   backoff to 0.  Cached upstream FHs on proxy SBs survive the
 *   reconnect -- they identify upstream inodes which are stable
 *   across the upstream session's lifetime.  See
 *   .claude/design/ps-reconnect.md.
 *
 *   Worker forwarders that observe a session-killer in their own
 *   compound do NOT trigger reconnect -- they return NFS4ERR_DELAY
 *   (or NFS4ERR_IO) to the end client and rely on the next renewal
 *   tick to detect.  Worst-case recovery is one renewal interval
 *   (default 30s for a 90s lease).  A worker-path "kick the
 *   renewal early" path is NOT_NOW_BROWN_COW for the next slice.
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
