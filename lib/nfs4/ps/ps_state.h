/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_STATE_H
#define _REFFS_PS_STATE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "nfsv42_xdr.h" /* nfsstat4 */
#include "reffs/settings.h"

struct mds_session; /* forward: from lib/nfs4/client/ec_client.h */
struct cds_lfht; /* forward: liburcu lock-free hash table */

/*
 * Per-listener lifecycle state machine -- Phase 4a quiesce protocol.
 *
 *   RUNNING   accepting new ops; buffer-table writes and the
 *             session-borrow path both gated on this state.
 *   DRAINING  ps_listener_stop has been called; no new ops, in-flight
 *             ops finish, then teardown walks the buffer table.
 *   STOPPED   teardown complete.  Slot stays in the registry (so
 *             ps_state_find still resolves the id) but all op-handler
 *             entry points return NFS4ERR_DELAY.  ps_state_fini turns
 *             every running listener through DRAINING -> STOPPED before
 *             destroying mutexes and rwlocks.
 *
 * See `.claude/design/proxy-server-phase4a.md` "Quiesce protocol".
 */
enum ps_listener_state_kind {
	PS_LISTENER_RUNNING = 0, /* zero so ps_state_register init clears to */
	PS_LISTENER_DRAINING = 1, /* it -- minus the explicit store below.  */
	PS_LISTENER_STOPPED = 2,
};

/*
 * Upper bound on filehandle length used by proxy-server storage.
 * NFSv4 caps at 128 bytes (RFC 8881).  Defining the limit locally
 * keeps ps_state.h free of the generated XDR header.
 */
#define PS_MAX_FH_SIZE 128

/*
 * Maximum exported paths the PS tracks per listener.  Matches
 * MNTPATHLEN-shaped upstream exports at reasonable scale; a real
 * deployment rarely exceeds a handful.  If an upstream advertises
 * more, the extras are logged and skipped.
 */
#define PS_MAX_EXPORTS_PER_LISTENER 32

/*
 * Per-discovered-export record: the upstream path and the FH we
 * obtained by walking it at discovery time.  Held inside
 * ps_listener_state.pls_exports[] so compound dispatch can look
 * up {listener_id, path} in O(n_exports) to find the upstream FH.
 *
 * ple_fh_len == 0 marks the slot empty.  ple_path is the absolute
 * path as the upstream announced it (NUL-terminated).
 */
struct ps_export {
	char ple_path[1025]; /* MNTPATHLEN + 1 for NUL */
	uint8_t ple_fh[PS_MAX_FH_SIZE];
	/*
	 * Atomic so the re-discovery path can retire a slot to 0,
	 * rewrite ple_fh, and republish the length without readers
	 * ever observing torn FH bytes against a stale-but-nonzero
	 * length.  See ps_state_add_export() for the protocol.
	 */
	_Atomic uint32_t ple_fh_len; /* 0 = slot empty */
};

/*
 * Per-listener proxy-server runtime state.
 *
 * Populated from cfg.proxy_mds[] entries at reffsd startup, one entry
 * per [[proxy_mds]] config block.  Looked up at compound dispatch
 * time via the compound's c_listener_id so op handlers can reach the
 * upstream-MDS binding for the listener the client connected on.
 *
 * pls_session is NULL until reffsd opens a connection to the
 * upstream (ps_state_set_session()).  A NULL session means any of:
 * empty pls_upstream (no upstream configured), connect failed at
 * startup, or session has been torn down.  Op handlers that need the
 * session MUST check for NULL and fail gracefully
 * (NFS4ERR_NOTSUPP or NFS4ERR_DELAY as appropriate).
 *
 * pls_mds_root_fh_len == 0 means the MDS root FH has not been
 * discovered yet.  Populated by ps_state_set_mds_root_fh() after a
 * successful PUTROOTFH+GETFH round-trip on the session.  Note that
 * this is distinct from "session not open": a listener can have a
 * valid pls_session but a zero pls_mds_root_fh_len if discovery
 * failed at startup.  Future op handlers / forwarding paths must
 * treat (session open && root FH empty) as an explicit error rather
 * than blindly dereferencing an empty FH.  A `ps_state_discovery_complete()`
 * helper will live here once the first consumer lands.
 * NOT_NOW_BROWN_COW: discovery-complete helper.
 *
 * See `.claude/design/proxy-server.md` phase 2.
 */
struct ps_listener_state {
	uint32_t pls_listener_id; /* matches compound->c_listener_id */
	char pls_upstream[REFFS_CONFIG_MAX_HOST];
	uint16_t pls_upstream_port;
	uint16_t pls_upstream_probe;
	struct mds_session *pls_session; /* NULL until session opens */
	uint8_t pls_mds_root_fh[PS_MAX_FH_SIZE];
	uint32_t pls_mds_root_fh_len; /* 0 = not yet discovered */

	/*
	 * Discovered upstream exports, indexed by insertion order.  Slots
	 * with ple_fh_len == 0 are empty.  Updates publish via release-
	 * store on pls_nexports; readers use acquire-load (same pattern
	 * as ps_nlisteners at the table level).  Single-writer discipline:
	 * only the discovery coordinator populates this; op handlers read.
	 */
	struct ps_export pls_exports[PS_MAX_EXPORTS_PER_LISTENER];
	_Atomic uint32_t pls_nexports;

	/*
	 * Serializes discovery runs for this listener.  Held across the
	 * whole body of ps_discovery_run() so two writers (reffsd startup
	 * + an on-demand LOOKUP-triggered re-discovery) cannot race on
	 * pls_exports[] / pls_nexports.  Initialized by ps_state_register
	 * and destroyed by ps_state_fini; callers go through
	 * ps_state_discovery_lock() / _unlock() rather than reaching
	 * into the mutex directly so the "no lookup miss on the id"
	 * invariant stays with the registry.
	 */
	pthread_mutex_t pls_discovery_mutex;

	/*
	 * Reconnect: protects pls_session lifetime.  Workers take a read
	 * lock for the duration of a forwarded RPC; the renewal thread
	 * takes a write lock when swapping in a freshly built session
	 * after the previous one died (NFS4ERR_BADSESSION /
	 * NFS4ERR_DEADSESSION / NFS4ERR_STALE_CLIENTID / connection drop).
	 * See .claude/design/ps-reconnect.md.
	 *
	 * Lock order rule (verified by TSAN soak):
	 *   pls_session_rwlock (read or write)
	 *     -> mds_session::ms_call_mutex (acquired only inside
	 *        mds_compound_send_with_auth and mds_session_destroy)
	 * No path acquires ms_call_mutex before the rwlock.
	 *
	 * pls_reconnect_backoff_sec is the next wait interval before the
	 * renewal thread re-attempts reconnect after a failed attempt;
	 * grows by doubling 1, 2, 4, 8, 16, 32, capped at 60.  Zero means
	 * "first attempt allowed immediately" (steady state and right
	 * after a successful reconnect).
	 *
	 * pls_reconnect_next_attempt_ns is a CLOCK_MONOTONIC deadline; the
	 * renewal thread skips the reconnect path until reffs_now_ns()
	 * crosses it.  Zero means "no wait scheduled".
	 *
	 * Both are _Atomic because the worker-path kick
	 * (ps_listener_kick_reconnect, see below) clears them concurrently
	 * with the renewal thread's own reads/writes.  Use
	 * atomic_load_explicit / atomic_store_explicit -- all schedule
	 * accesses are independent reads / unconditional writes; no
	 * compare-and-swap is needed because the schedule is advisory
	 * (the tick logic is idempotent if state changes between read
	 * and write).
	 */
	pthread_rwlock_t pls_session_rwlock;
	_Atomic uint32_t pls_reconnect_backoff_sec;
	_Atomic uint64_t pls_reconnect_next_attempt_ns;

	/*
	 * Cached bring-up parameters used by the renewal thread to
	 * replay TLS + EXCHANGE_ID + CREATE_SESSION + PROXY_REGISTRATION
	 * after a session-killer wire status.  Copied from the
	 * reffs_proxy_mds_config at register time; immutable after.
	 *
	 * pls_registration_id is set once by ps_state_set_registration_id
	 * after reffsd's bootstrap RAND_bytes; reusing the same id on
	 * reconnect lets the upstream MDS recognise the new
	 * PROXY_REGISTRATION as a renewal (not a squat) so it does not
	 * have to wait out the squat-guard window before accepting.
	 * pls_registration_id_len == 0 means the boot path has not yet
	 * populated the id; the renewal thread treats reconnect as
	 * impossible in that state and skips.
	 */
	char pls_tls_cert[REFFS_CONFIG_MAX_PATH];
	char pls_tls_key[REFFS_CONFIG_MAX_PATH];
	char pls_tls_ca[REFFS_CONFIG_MAX_PATH];
	int pls_tls_mode; /* mirrors enum reffs_proxy_tls_mode */
	bool pls_tls_insecure_no_verify;
	uint8_t pls_registration_id[16];
	uint32_t pls_registration_id_len;

	/*
	 * Phase 4a quiesce protocol (.claude/design/proxy-server-phase4a.md).
	 *
	 * pls_state           lifecycle gate; release-store on transition,
	 *                     acquire-load by op handlers after the
	 *                     pls_active_buffer_refs increment closes the
	 *                     TOCTOU window.
	 * pls_boot_gen        monotonic per-listener generation; bumped on
	 *                     each ps_state_register (today only at boot;
	 *                     re-register is NOT_NOW_BROWN_COW).  Each
	 *                     buffer carries the gen seen at alloc time
	 *                     so a listener restart invalidates buffers
	 *                     from the prior generation without scanning.
	 * pls_active_buffer_refs  active-op counter: number of op handlers
	 *                     that may still touch a buffer or look up
	 *                     pls_write_buffer_ht.  Incremented by
	 *                     enter_quiesce_or_bail BEFORE the state check;
	 *                     decremented + cv-broadcast by leave_quiesce.
	 *                     Teardown waits on this reaching zero.
	 * pls_drain_mutex /
	 * pls_drain_cv        wakeup primitive for the teardown wait.
	 *                     Standard cv-predicate discipline: teardown
	 *                     loads pls_active_buffer_refs UNDER the
	 *                     mutex; leave_quiesce takes the mutex around
	 *                     the broadcast to avoid lost-wakeup.
	 * pls_write_buffer_ht the buffer table (Rule 6 lifecycle in
	 *                     ps_write_buffer.c).  cds_lfht_new'd in
	 *                     register; iterated + destroyed in
	 *                     ps_listener_stop.
	 */
	_Atomic enum ps_listener_state_kind pls_state;
	_Atomic uint64_t pls_boot_gen;
	_Atomic uint64_t pls_active_buffer_refs;
	pthread_mutex_t pls_drain_mutex;
	pthread_cond_t pls_drain_cv;
	struct cds_lfht *pls_write_buffer_ht;

	/*
	 * Phase 4a observability counters exposed via the
	 * ps-write-buffer-stats probe op.  All-relaxed atomics: the
	 * probe handler reports a self-consistent snapshot of the
	 * counters at a moment in time, not a transactionally
	 * consistent picture across all of them; relaxed ordering
	 * keeps the increment sites off the hot path's memory order
	 * cost.  Forward-compat reserves two counters (peak bytes,
	 * close-flush timeouts) that stay zero until Phase 4b adds
	 * the bookkeeping that maintains them.
	 *
	 *   pls_cap_rejections_total  pipeline_write returned -EAGAIN
	 *                             because offset+count exceeded
	 *                             REFFS_PS_WRITE_BUFFER_MAX
	 *   pls_fbig_rejections_total pipeline_write returned -EFBIG
	 *                             because data_len alone exceeded
	 *                             REFFS_PS_WRITE_BUFFER_MAX
	 *   pls_close_flush_timeouts_total
	 *                             reserved -- close-flush timeout
	 *                             machinery (design Risk #7) is
	 *                             deferred; counter stays zero
	 *                             until that lands.
	 *   pls_rmw_reads_total       Phase 4b.7: CHUNK_READ issued as
	 *                             a partial-stripe RMW prefix.
	 *                             High on non-RMW-heavy workloads
	 *                             flags unexpected partial-stripe
	 *                             patterns from the client.
	 *   pls_rmw_read_failures_total Phase 4b.7: RMW prefix
	 *                             CHUNK_READ failure (DS
	 *                             unreachable / decode quorum
	 *                             lost).  Surfaces DS degradation
	 *                             the WRITE-side counters miss.
	 *
	 * The active_buffers count + total_bytes_buffered are NOT
	 * counters here; they are computed lazily by the probe
	 * handler walking the buffer table (cheap; tables are small).
	 * dirty_stripes_total is also lazy-computed by walking each
	 * buffer's pwb_dirty_ht (Phase 4b.7).
	 */
	_Atomic uint64_t pls_cap_rejections_total;
	_Atomic uint64_t pls_fbig_rejections_total;
	_Atomic uint64_t pls_close_flush_timeouts_total;
	_Atomic uint64_t pls_rmw_reads_total;
	_Atomic uint64_t pls_rmw_read_failures_total;
};

/*
 * Initialize the registry.  Call once during reffsd startup before
 * any ps_state_register() / ps_state_find() call.  Single-threaded.
 * Returns 0 on success, -errno on failure.
 */
int ps_state_init(void);

/*
 * Register one listener.  Called from reffsd main for each
 * cfg.proxy_mds[] entry, regardless of whether address is empty
 * (an empty-address entry still gets registered so a future
 * ps_state_find() doesn't return NULL for an unconfigured-upstream
 * listener -- callers can distinguish via pls_upstream[0] == '\0').
 *
 * Returns:
 *   0        success
 *   -EINVAL  cfg is NULL, or cfg->id == 0 (reserved for native)
 *   -EEXIST  a listener with this id is already registered
 *   -ENOSPC  the registry is full
 */
int ps_state_register(const struct reffs_proxy_mds_config *cfg);

/*
 * Look up a listener by id.  Returns NULL if not found.  The
 * returned pointer is valid until ps_state_fini().
 *
 * Safe to call from any thread after ps_state_init() has returned.
 * The registry is populated during startup (single-threaded) and is
 * read-only in steady state -- updates publish via release-store on
 * the count, readers use acquire-load.
 */
const struct ps_listener_state *ps_state_find(uint32_t listener_id);

/*
 * Attach an mds_session to an already-registered listener.  The
 * registry stores the pointer; ownership does not transfer -- the
 * caller is responsible for calling mds_session_destroy() + free()
 * before ps_state_fini() runs.  Passing NULL clears any stored
 * pointer (used at shutdown, after the caller has destroyed the
 * session).
 *
 * Returns:
 *   0        success
 *   -ENOENT  no listener with this id is registered
 */
int ps_state_set_session(uint32_t listener_id, struct mds_session *session);

/*
 * Record the MDS's root filehandle for this listener.  Fetched once
 * at startup via ps_discovery_fetch_root_fh() and stored so future
 * LOOKUP / GETATTR forwarding does not need to re-ask.  `fh_len == 0`
 * is legal (clears the stored FH -- useful for test cleanup).
 *
 * Returns:
 *   0        success
 *   -ENOENT  no listener with this id is registered
 *   -E2BIG   fh_len exceeds PS_MAX_FH_SIZE
 */
int ps_state_set_mds_root_fh(uint32_t listener_id, const uint8_t *fh,
			     uint32_t fh_len);

/*
 * Take (resp. release) the per-listener discovery mutex.  Used by
 * ps_discovery_run() and any future on-demand re-discovery path to
 * serialize writers on pls_exports[] while still letting readers
 * (op handlers calling ps_state_find_export) proceed via the
 * release/acquire atomics.  Returns:
 *
 *   0        success
 *   -ENOENT  no listener with this id is registered
 */
int ps_state_discovery_lock(uint32_t listener_id);
int ps_state_discovery_unlock(uint32_t listener_id);

/*
 * Walk the cached exports on a listener, invoking `cb` for each
 * non-empty slot.  Callers that need to act on every discovered
 * export (e.g. reffsd's startup SB allocator) use this instead of
 * reaching into pls_exports[] directly, so the release/acquire
 * contract on pls_nexports + ple_fh_len stays encapsulated and any
 * future layout changes land in one place.
 *
 * `cb` is invoked synchronously for each non-empty entry with the
 * slot pointer (valid for the duration of the call) and the
 * caller's `ctx`.  A writer racing with the walker through
 * ps_state_add_export will either be fully published (this call
 * sees the new entry) or retired-then-republished (this call sees
 * ple_fh_len==0 and skips the slot); there is no torn-FH window.
 *
 * Returns the number of entries the callback saw (>= 0),
 * -ENOENT if no listener with this id is registered, or
 * -EINVAL if cb is NULL.
 */
typedef void (*ps_state_export_cb)(const struct ps_export *ex, void *ctx);
int ps_state_exports_for_each(uint32_t listener_id, ps_state_export_cb cb,
			      void *ctx);

/*
 * Record a discovered upstream export on this listener.  Called by
 * the discovery coordinator after MOUNT3 EXPORT lists a path and
 * ps_discovery_walk_path() resolves it to an FH on the upstream.
 *
 * Appends to pls_exports[].  If `path` already has an entry the
 * existing slot is updated in place (lets on-demand re-discovery
 * refresh an FH after an upstream restart without growing the
 * table).  `fh_len == 0` is rejected -- a zero-length FH is
 * semantically meaningless and indistinguishable from the
 * empty-slot sentinel.
 *
 * Returns:
 *   0        success
 *   -ENOENT  no listener with this id is registered
 *   -EINVAL  path is NULL / empty, or fh is NULL, or fh_len == 0
 *   -E2BIG   path length >= sizeof(ple_path), or fh_len > PS_MAX_FH_SIZE
 *   -ENOSPC  pls_exports[] is full (PS_MAX_EXPORTS_PER_LISTENER)
 */
int ps_state_add_export(uint32_t listener_id, const char *path,
			const uint8_t *fh, uint32_t fh_len);

/*
 * Look up a discovered export by path.  Returns a pointer to the
 * export slot, or NULL if not found.  The returned pointer is valid
 * until ps_state_fini() and must not be mutated by the caller.
 */
const struct ps_export *ps_state_find_export(uint32_t listener_id,
					     const char *path);

/*
 * Iteration callback for ps_state_listeners_for_each.  Returning
 * non-zero short-circuits the iteration (the value is propagated to
 * the caller as the for_each return).  Returning 0 continues.
 */
typedef int (*ps_state_listener_cb)(const struct ps_listener_state *pls,
				    void *arg);

/*
 * Walk every registered listener in registration order, invoking
 * `cb(pls, arg)` for each.  Used by the PS renewal thread to
 * iterate active upstream sessions.
 *
 * Safe to call from any thread post-init.  The registry is treated
 * as read-only here; callers that mutate session pointers must use
 * ps_state_set_session() per-id.
 *
 * Returns:
 *   0     iteration completed
 *   N     `cb` returned non-zero N (iteration stopped early)
 */
int ps_state_listeners_for_each(ps_state_listener_cb cb, void *arg);

/*
 * Tear down the registry at shutdown.  Must run after all worker
 * threads have stopped, i.e. after io_handler_fini().  No
 * ps_state_find() call may be in flight.  Does NOT destroy any
 * attached mds_session -- the caller MUST drain sessions via
 * ps_state_set_session(id, NULL) + their own destroy first.
 */
void ps_state_fini(void);

/*
 * Stash the registration_id reffsd's boot path generated for this
 * listener so the renewal thread's reconnect path can re-issue
 * PROXY_REGISTRATION with the same id (which the MDS treats as a
 * renewal, not a squat).  Replaces any prior id; passing len == 0
 * clears the cached id (used by tests).  Returns 0 on success,
 * -ENOENT if the listener id is unknown, -EINVAL if len exceeds
 * the on-state buffer.
 */
int ps_state_set_registration_id(uint32_t listener_id, const uint8_t *id,
				 uint32_t len);

/*
 * Borrow the listener's current upstream session under a read lock.
 * Returns NULL if the listener id is unknown OR the session pointer
 * is NULL (boot before first connect, or the renewal thread is mid-
 * reconnect and has temporarily cleared the slot).  When NULL is
 * returned, no lock is held and the caller MUST NOT call
 * ps_listener_session_release().
 *
 * On non-NULL return the read lock is held until
 * ps_listener_session_release(listener_id) runs.  Callers MUST keep
 * the borrow region tight -- it brackets the entire forwarded RPC,
 * so a write-lock requester (the renewal thread mid-reconnect)
 * waits for every in-flight forwarder to finish before destroying
 * the old session.
 */
/*
 * Borrow a per-listener mds_session pointer for the duration of one
 * forwarded RPC.  Returns NULL when:
 *   - the listener id is not registered
 *   - the listener has no session attached yet (boot race / disconnect)
 *   - the listener's pls_state is not PS_LISTENER_RUNNING (Phase 4a
 *     quiesce protocol -- the session is being torn down)
 *
 * Callers MUST pair every non-NULL return with ps_listener_session_release.
 */
struct mds_session *ps_listener_session_borrow(uint32_t listener_id);

/*
 * Quiesce + tear down one listener's per-listener state (Phase 4a).
 * Synchronous; returns after every in-flight buffer-table op has
 * dropped its find ref and the write-buffer table has been destroyed.
 *
 * Sequence:
 *   1. release-store pls_state = DRAINING.  After this, every new op
 *      that calls enter_quiesce_or_bail() sees DRAINING and returns
 *      false (op handler maps to NFS4ERR_DELAY).
 *   2. wait on pls_drain_cv until pls_active_buffer_refs reaches 0.
 *      The mutex around the predicate check defends against a
 *      lost-wakeup race with leave_quiesce.
 *   3. walk pls_write_buffer_ht under rcu_read_lock dropping every
 *      table ref; synchronize_rcu; destroy the hash table.
 *   4. release-store pls_state = STOPPED.
 *
 * Idempotent: a second call on an already-STOPPED listener is a
 * no-op.  Returns -ENOENT if the listener id is not registered.
 *
 * ps_state_fini calls this internally on every registered listener
 * before destroying the mutexes / rwlocks / cv.  Direct callers can
 * use it to drain a single listener (e.g. tests, future admin-driven
 * teardown) without taking the whole registry down.
 */
int ps_listener_stop(uint32_t listener_id);
void ps_listener_session_release(uint32_t listener_id);

/*
 * Wake the PS renewal thread early and zero the reconnect schedule
 * for `listener_id` so the next tick attempts a reconnect immediately.
 *
 * Called by worker forwarders that observed a session-killer wire
 * status (see ps_session_is_dead) on their own compound -- shrinks
 * the worst-case recovery window from one renewal interval down to
 * one TLS handshake.  Without the kick, recovery waits for the next
 * scheduled tick.
 *
 * Idempotent and safe from any thread.  Unknown listener id is a
 * no-op.  Safe to call before ps_renewal_start() (the wake is
 * harmless when no thread is parked on the CV) and after
 * ps_renewal_stop() (same).
 *
 * Does NOT clear pls_session -- the renewal thread's own SEQUENCE
 * renewal will observe BADSESSION on the next tick and run the
 * already-tested classify + replace + reconnect path.  Workers
 * that already know the session is dead pay one extra round-trip
 * for the renewal thread to re-observe it; this preserves the
 * single-writer discipline on pls_session (only the renewal thread
 * writes it).
 */
void ps_listener_kick_reconnect(uint32_t listener_id);

/*
 * Replace the listener's session pointer under a write lock.
 * Acquires the write lock (waiting for all in-flight readers to
 * finish), stores `new_session` (which may be NULL to clear the
 * slot), then -- if there was a prior non-NULL session -- calls
 * `mds_session_destroy(old)` and `free(old)` after the lock has
 * been released to keep the destroy off the lock.
 *
 * Ownership: after a successful return the listener owns
 * `new_session`.  Callers building a fresh session via
 * mds_session_create_tls() pass the new session in and stop
 * tracking it themselves.
 *
 * Returns 0 on success, -ENOENT if the listener id is unknown.
 */
int ps_listener_session_replace(uint32_t listener_id,
				struct mds_session *new_session);

/*
 * Classify the result of an attempted upstream operation.  Returns
 * true if the (errno, sr_status) tuple indicates the upstream
 * session is dead and must be rebuilt; false for per-op transients
 * (e.g. NFS4ERR_DELAY) and successes.
 *
 * Session-killer wire codes:
 *   NFS4ERR_BADSESSION, NFS4ERR_DEADSESSION,
 *   NFS4ERR_STALE_CLIENTID, NFS4ERR_BAD_SESSION_DIGEST
 *
 * Connection-killer errno codes (sr_status irrelevant):
 *   -EIO, -EPIPE, -ECONNRESET, -ETIMEDOUT, -ENOTCONN, -ENETUNREACH
 *
 * Pure factory function -- no global state, no locking.  Used by the
 * renewal thread on each tick and unit-testable in isolation.
 */
bool ps_session_is_dead(int err, nfsstat4 sr_status);

/*
 * Backoff scheduler for reconnect attempts.  Pure functions on the
 * caller-owned counter so unit tests can step the schedule without
 * touching the global registry.
 *
 * ps_reconnect_backoff_next: returns the wait (in seconds) the
 * renewal thread MUST honour before its next reconnect attempt and
 * advances `*backoff_sec` along the schedule (0, 1, 2, 4, 8, 16, 32,
 * 60, 60, ...).  The first call (with *backoff_sec == 0) returns 0
 * (immediate retry permitted) and bumps to 1.
 *
 * ps_reconnect_backoff_reset: zeroes the counter; called after a
 * successful reconnect.
 */
uint32_t ps_reconnect_backoff_next(uint32_t *backoff_sec);
void ps_reconnect_backoff_reset(uint32_t *backoff_sec);

#endif /* _REFFS_PS_STATE_H */
