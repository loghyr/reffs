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
 *   compound return NFS4ERR_DELAY (or NFS4ERR_IO) to the end client.
 *   They MAY also call ps_listener_kick_reconnect(listener_id) to
 *   wake the renewal thread early -- this shrinks worst-case
 *   recovery from one renewal interval (default 30s for a 90s
 *   lease) down to one TLS handshake.  See
 *   ps_listener_kick_reconnect in ps_state.h for the worker API and
 *   ps_renewal_kick below for the bare wake primitive.
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

/*
 * Wake the renewal thread early without touching any per-listener
 * schedule state.  The next iteration of the renewal loop runs
 * immediately rather than waiting out the remaining cond_timedwait
 * interval.  Idempotent and safe from any thread; safe before
 * ps_renewal_start() and after ps_renewal_stop().
 *
 * Most callers want ps_listener_kick_reconnect(listener_id) (in
 * ps_state.h) which combines the wake with a per-listener
 * schedule reset.  This bare primitive is exposed for tests and
 * for any future caller that wants to wake the thread without
 * advancing the schedule of any specific listener.
 */
void ps_renewal_kick(void);

#endif /* _REFFS_PS_RENEWAL_H */
