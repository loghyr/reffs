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

#endif /* _REFFS_PS_WRITE_BUFFER_H */
