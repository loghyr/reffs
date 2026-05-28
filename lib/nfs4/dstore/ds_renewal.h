/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_DS_RENEWAL_H
#define _REFFS_DS_RENEWAL_H

#include <stdint.h>

struct dstore;

/*
 * MDS-to-DS session keepalive.
 *
 * Each NFSv4.2 dstore holds a long-lived MDS-to-DS session for
 * control-plane fan-out (CREATE / REMOVE / GETATTR / SETATTR /
 * fence) and InBand I/O proxy.  The DS expires the session after
 * one lease period of inactivity -- RFC 8881 S2.10.6 -- and the
 * lib/io socket reaper additionally drops idle TCP after 601s
 * (CONNECTION_TIMEOUT_SECONDS, lib/include/reffs/io.h).  Without
 * a keepalive, periods of low fan-out traffic let the session
 * drift past either threshold; the next control-plane RPC fails
 * with NFS4ERR_BADSESSION or -EREMOTEIO, and the symptom on the
 * client side is the chunk-collision Track 2 Criterion 1
 * cascade.
 *
 * Mirrors the PS-side keepalive at lib/nfs4/ps/ps_renewal.h.  A
 * single worker thread sweeps every NFSv4 dstore every
 * `interval_seconds` (default = server_lease_time(ss) / 3, floor
 * 30s; 0 disables), borrows the session via dstore_session_borrow,
 * sends a SEQUENCE-only compound (mds_session_renew_lease_ex),
 * classifies the result via mds_session_is_dead, and on dead
 * sessions parks the pointer + reconnects with capped exponential
 * backoff (0s, 1s, 2s, 4s, 8s, 16s, 32s, 60s, 60s, ...).
 *
 * See .claude/design/mds-ds-session-keepalive.md for the full
 * design including the BLOCKER B1 rwlock fix that this thread
 * (and the call-site sweep in dstore_ops_nfsv4.c) depend on.
 */

/*
 * Start the keepalive thread.  Idempotent: a second call returns 0
 * without spawning a new thread.  interval_seconds == 0 means
 * "keepalive disabled" -- the thread is not started.
 *
 * Returns 0 on success, -errno on pthread_create / cond init failure.
 */
int ds_renewal_start(uint32_t interval_seconds);

/*
 * Stop the keepalive thread.  Blocks until the worker has joined.
 * Safe to call before ds_renewal_start (no-op).  Must run BEFORE
 * dstore_fini() because the thread iterates the global dstore table.
 */
void ds_renewal_stop(void);

/*
 * Wake the renewal thread early AND reset this dstore's reconnect
 * schedule so the next sweep attempts immediately.  Idempotent and
 * safe from any thread; safe before ds_renewal_start() and after
 * ds_renewal_stop().
 *
 * Called by send_and_check_ds when a control-plane compound observes
 * a dead-session classification.  The forwarder returns -EIO to its
 * caller; this kick shrinks worst-case recovery from one renewal
 * interval down to one TLS+EXCHANGE_ID+CREATE_SESSION round-trip.
 */
void ds_renewal_kick(struct dstore *ds);

#endif /* _REFFS_DS_RENEWAL_H */
