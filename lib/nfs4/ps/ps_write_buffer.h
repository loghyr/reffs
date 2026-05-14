/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_WRITE_BUFFER_H
#define _REFFS_PS_WRITE_BUFFER_H

/*
 * Per-(stateid, upstream FH) write-buffer state used by the PS Phase
 * 4a whole-file COMMIT-deferred WRITE path.  See
 * .claude/design/proxy-server-phase4a.md.
 *
 * This header is the public PS-internal surface (ps_state.c, the
 * pipeline shim in ps_proxy_ops.c, op handlers).  The opaque
 * struct ps_write_buffer lives in ps_write_buffer_internal.h so
 * tests and the byte-copy shim can touch fields directly without
 * a forest of accessors -- the same shape as ps_proxy_ops_internal.h.
 *
 * Concurrency model:
 *   - Lookup goes through enter_quiesce_or_bail() FIRST.  That
 *     increments pls_active_buffer_refs and gates on pls_state.
 *     Every "OK to proceed" return is matched by exactly one
 *     leave_quiesce() somewhere on the unwind.
 *   - Buffer ref discipline is urcu_ref (Rule 6 in
 *     patterns/ref-counting.md): one table ref taken at
 *     insertion; per-op find refs taken via lookup_or_alloc.
 *   - pwb_mutex is leaf-most: nothing else is acquired while it
 *     is held.  See Reviewer checklist rule 4 in the design doc.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ps_state.h" /* struct ps_listener_state, PS_MAX_FH_SIZE */

struct ps_write_buffer; /* opaque to most callers; full def in
				 * ps_write_buffer_internal.h.            */

/*
 * Erasure-coding geometry snapshot for a single buffer (Phase 4b).
 * Cached on first WRITE via ps_write_buffer_set_geom so the dirty-
 * stripe bitmap and RMW path see a consistent (k, m, shard_size)
 * tuple even if the underlying layout reissues with different
 * parameters mid-buffer (a geometry mismatch at flush time forces
 * a buffer drop + NFS4ERR_STALE; see proxy-server-phase4b.md).
 *
 * Field semantics:
 *   pwbg_k          number of data shards per stripe
 *   pwbg_m          number of parity shards per stripe (informational
 *                   for the buffer; the encode path consumes m, the
 *                   dirty bitmap only tracks data shards)
 *   pwbg_shard_size bytes per shard.  Stripe size derived as
 *                   pwbg_k * pwbg_shard_size.
 */
struct ps_write_buffer_geom {
	uint32_t pwbg_k;
	uint32_t pwbg_m;
	uint32_t pwbg_shard_size;
};

/* ------------------------------------------------------------------ */
/* Per-listener buffer-table lifecycle                                 */
/* ------------------------------------------------------------------ */

/*
 * Initialise the per-listener write-buffer table.  Called from
 * ps_state_register before the listener slot is published.  On
 * success the listener's pls_write_buffer_ht is non-NULL and
 * pls_state == PS_LISTENER_RUNNING.  Returns 0 on success,
 * -ENOMEM if cds_lfht_new fails, -errno from pthread mutex/cv init.
 */
int ps_write_buffer_table_init(struct ps_listener_state *pls);

/*
 * Drain + destroy the per-listener write-buffer table.  Idempotent;
 * a second call on an already-STOPPED listener is a no-op.  Used
 * by ps_listener_stop after the active-refs counter reaches zero.
 *
 * Caller MUST have already transitioned pls_state to DRAINING and
 * waited for pls_active_buffer_refs == 0 -- this function does not
 * itself wait; it just walks the table dropping every table ref
 * and then cds_lfht_destroy's the table.
 */
void ps_write_buffer_table_destroy(struct ps_listener_state *pls);

/* ------------------------------------------------------------------ */
/* Quiesce protocol primitives                                         */
/* ------------------------------------------------------------------ */

/*
 * Bump pls_active_buffer_refs and load pls_state with acquire
 * ordering.  Returns true if the op may proceed (state is RUNNING).
 * On false the active-refs decrement has already happened via
 * leave_quiesce; the caller bails (typically NFS4ERR_DELAY).
 *
 * TOCTOU-safe: the fetch_add happens BEFORE the state load so a
 * concurrent ps_listener_stop sets pls_state = DRAINING and either
 * (a) sees us in the counter (waits), or (b) we observe DRAINING
 * on the post-add load and decrement out without taking any
 * downstream refs.  See "Quiesce protocol" in the design doc.
 */
bool ps_write_buffer_enter_quiesce_or_bail(struct ps_listener_state *pls);

/*
 * Decrement pls_active_buffer_refs; on the last decrement, take
 * pls_drain_mutex around pthread_cond_broadcast to avoid the
 * lost-wakeup race with ps_listener_stop's cv_wait predicate
 * check.  Always paired with an earlier enter_quiesce_or_bail()
 * returning true (or with a successful internal counter bump
 * inside lookup_or_alloc).
 */
void ps_write_buffer_leave_quiesce(struct ps_listener_state *pls);

/* ------------------------------------------------------------------ */
/* Buffer lookup / alloc                                               */
/* ------------------------------------------------------------------ */

/*
 * Find or allocate the write buffer for (stateid_other, upstream_fh)
 * on this listener.  Caller MUST have already passed
 * enter_quiesce_or_bail (so pls_active_buffer_refs is held).
 *
 * SYMMETRIC NULL CONTRACT.  On non-NULL return the caller owns TWO
 * refs: the long-lived table ref (released by ps_write_buffer_drop
 * or by ps_write_buffer_table_destroy) and a per-op find ref
 * (released by ps_write_buffer_release_find_ref / drop).  On NULL
 * return the caller's enter_quiesce reservation has been released
 * by this function -- the caller must NOT call leave_quiesce again
 * on a NULL return.  Failure cases that return NULL:
 *   - invalid args (NULL pls / stateid / fh, fh_len == 0 or too big)
 *   - -ENOMEM during allocation
 *   - pthread_mutex_init failure on the buffer's leaf mutex
 *   - lost the cds_lfht_add_unique race AND the winning entry was
 *     itself already in teardown (urcu_ref_get_unless_zero failed)
 */
struct ps_write_buffer *ps_write_buffer_lookup_or_alloc(
	struct ps_listener_state *pls,
	const uint8_t stateid_other[12], /* PS_STATEID_OTHER_SIZE */
	const uint8_t *upstream_fh, uint32_t upstream_fh_len);

/*
 * Find an existing buffer for (stateid_other, upstream_fh) WITHOUT
 * allocating.  Returns NULL if no such buffer exists.  Used by the
 * CLOSE shim: CLOSE carries the stateid (unlike COMMIT) and we
 * must not accidentally allocate a buffer on a file that had no
 * writes.
 *
 * Caller MUST have already passed enter_quiesce_or_bail.  On
 * non-NULL return the caller owns a per-op find ref (release via
 * release_find_ref / drop).  On NULL return the enter_quiesce
 * reservation has been released (symmetric NULL contract, same
 * as lookup_or_alloc).
 */
struct ps_write_buffer *ps_write_buffer_lookup(
	struct ps_listener_state *pls,
	const uint8_t stateid_other[12], /* PS_STATEID_OTHER_SIZE */
	const uint8_t *upstream_fh, uint32_t upstream_fh_len);

/*
 * Find a buffer by upstream FH alone (no stateid).  Used by the
 * COMMIT shim, which doesn't have a stateid to key on.  Linear
 * scan of the per-listener table; Phase 4a's single-writer-per-FH
 * model means this is at-most-one match in the happy path.
 *
 * Caller MUST have already passed enter_quiesce_or_bail.  On
 * non-NULL return the caller owns a per-op find ref; release via
 * ps_write_buffer_release_find_ref or drop.  On NULL return the
 * enter_quiesce reservation has been released (symmetric NULL
 * contract, same as lookup_or_alloc).
 */
struct ps_write_buffer *
ps_write_buffer_find_by_fh(struct ps_listener_state *pls,
			   const uint8_t *upstream_fh,
			   uint32_t upstream_fh_len);

/*
 * Drop only the per-op find ref taken by lookup_or_alloc.  Does NOT
 * remove the buffer from the table; subsequent lookups on the same
 * key still find the buffer.  Calls leave_quiesce internally so the
 * caller's outer enter_quiesce reservation is released too.
 */
void ps_write_buffer_release_find_ref(struct ps_write_buffer *buffer,
				      struct ps_listener_state *pls);

/*
 * Full drop: cds_lfht_del + drop both refs (table + find).  Used by
 * the COMMIT success path and by the stale-gen rejection path.  The
 * release callback call_rcu's the eventual free.  Calls leave_quiesce
 * internally.
 */
void ps_write_buffer_drop(struct ps_write_buffer *buffer,
			  struct ps_listener_state *pls);

/* ------------------------------------------------------------------ */
/* Per-stripe dirty bitmap (Phase 4b)                                  */
/* ------------------------------------------------------------------ */

/*
 * Snapshot encoding geometry on first WRITE.  Idempotent: a second
 * call with identical fields is a no-op; a call with different
 * fields after the geometry is already set returns -EINVAL.
 * Geometry is required before ps_write_buffer_mark_dirty.
 *
 * Caller MUST hold buf->pwb_mutex.  Returns 0 on success,
 * -EINVAL on geometry mismatch or invalid fields (k == 0,
 * shard_size == 0, k * shard_size overflow).
 */
int ps_write_buffer_set_geom(struct ps_write_buffer *buf,
			     const struct ps_write_buffer_geom *geom);

/*
 * Mark the byte range [offset, offset+count) dirty.  Allocates per-
 * stripe entries lazily; each entry tracks which of the k data
 * shards are touched.  A write covering all k shards of a stripe
 * (or a later widening write that completes the coverage) collapses
 * the entry's partial mask to "fully dirty" (no RMW read needed at
 * flush time).
 *
 * Caller MUST hold buf->pwb_mutex and the buffer's geometry MUST
 * be set (ps_write_buffer_set_geom returned 0).  Returns 0 on
 * success.  Returns -ENOMEM if the dirty hash table or a stripe
 * entry cannot be allocated; in this partial-failure case any
 * stripes that succeeded stay marked, and the caller treats the
 * failure as an NFS4ERR_DELAY (the client retries the WRITE).
 * Returns -EINVAL if geometry is not set or count == 0.
 */
int ps_write_buffer_mark_dirty(struct ps_write_buffer *buf, uint64_t offset,
			       uint32_t count);

/*
 * Count of dirty stripe entries in this buffer.  Caller MUST hold
 * buf->pwb_mutex.  Walks the dirty hash table; cost is O(N) in the
 * number of dirty stripes.  Used by tests and by the forthcoming
 * ps-write-buffer-stats probe extension (slice 4b.7).
 */
size_t ps_write_buffer_dirty_count(struct ps_write_buffer *buf);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/*
 * Count entries currently in the per-listener buffer table.  Walks
 * under rcu_read_lock; safe to call any time the listener is
 * registered.  Used by the ps-write-buffer-stats probe handler and
 * by drain-correctness tests.  Lazy count -- no maintained counter
 * to avoid extra work on the hot path.
 */
size_t ps_write_buffer_table_count(struct ps_listener_state *pls);

/*
 * Sum of dirty-stripe entries across every live buffer on this
 * listener.  Walks under rcu_read_lock + per-buffer Rule 6 find
 * ref (the inner pwb_dirty_ht is cds_lfht_destroy'd synchronously
 * inside pwb_release before the rcu callback, so a ref-less walk
 * would UAF the inner table on a concurrent last-put).  Cost is
 * O(buffers * dirty stripes per buffer).  Used by the
 * ps-write-buffer-stats probe extension (slice 4b.7) as an
 * operator signal that "stuff is buffered and waiting to flush".
 * Best-effort snapshot: a buffer in mid-teardown (refcount zero)
 * is skipped, and the inner walk is unsynchronized w.r.t.
 * pwb_mutex so concurrent mark_dirty / dirty_remove may add or
 * subtract from the running total -- acceptable for observability.
 */
size_t ps_write_buffer_dirty_total(struct ps_listener_state *pls);

/* ------------------------------------------------------------------ */
/* Composed write verifier (Phase 4b slice 4b.4)                       */
/* ------------------------------------------------------------------ */

/*
 * NFSv4 verifier4 width in bytes (RFC 8881 S3.1).  Mirrored from
 * PS_PROXY_VERIFIER_SIZE in ps_proxy_ops.h so callers that only
 * include this header (the buffer + tests) do not need to pull in
 * the wider op-handler surface for one constant.
 */
#define PS_WRITE_VERIFIER_SIZE 8

/*
 * Compose the 8-byte write verifier returned in WRITE / COMMIT
 * replies.  Folds two halves: the per-listener boot generation
 * (changes on PS restart) XOR'd with an optional MDS-supplied
 * verifier byte-string (changes on upstream DS-boot-epoch change).
 *
 * `mds_verf_set == false` -> output equals the listener verifier
 * unmodified; this is the "buffer has never flushed yet" path.  The
 * client's subsequent COMMIT still sees the same listener verifier
 * and the WRITE/COMMIT comparison succeeds when no MDS restart has
 * happened.
 *
 * `mds_verf_set == true` -> output is `listener_verf XOR
 * mds_verf[0..PS_WRITE_VERIFIER_SIZE]`.  An upstream MDS/DS reboot
 * between WRITE and COMMIT changes `mds_verf`, the composed
 * verifier changes, and the client sees a mismatch on COMMIT and
 * rewrites -- closing Risk #3a from Phase 4a (silent data loss).
 *
 * Composition is XOR (non-cryptographic) because verifier compare
 * is equality and collisions only cause a missed-mismatch, which
 * degrades to "wait for the next COMMIT" (not a correctness
 * violation).  See proxy-server-phase4b.md "Composed write
 * verifier" and Risk #4.
 *
 * The buffer's `pwb_mds_verf` / `pwb_mds_verf_set` are mutated
 * under pwb_mutex; the reply paths snapshot those into local
 * stack copies before unlocking and call this helper with the
 * snapshot.  The function itself takes no locks and is pure.
 */
void ps_compose_write_verf(const struct ps_listener_state *pls,
			   bool mds_verf_set,
			   const uint8_t mds_verf[PS_WRITE_VERIFIER_SIZE],
			   uint8_t out[PS_WRITE_VERIFIER_SIZE]);

#endif /* _REFFS_PS_WRITE_BUFFER_H */
