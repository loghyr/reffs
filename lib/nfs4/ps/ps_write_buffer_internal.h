/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PS_WRITE_BUFFER_INTERNAL_H
#define PS_WRITE_BUFFER_INTERNAL_H

/*
 * Whitebox surface for ps_write_buffer.c (PS Phase 4a).  Tests
 * include this header to inspect / mutate buffer-table state
 * directly without going through the public surface.  The
 * Phase 4a pipeline shim (4a.2b) also includes this header for
 * the byte-copy critical section that needs pwb_data /
 * pwb_high_water / pwb_mutex access -- the alternative would be
 * a forest of accessors, and per the patterns/ps_proxy_ops_internal.h
 * precedent that's not worth the boilerplate when the consumer
 * is in the same library.
 *
 * Production code outside ps_write_buffer.c and the immediate
 * shim does NOT include this header.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include <stdbool.h>
#include <urcu.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>

#include "ps_state.h" /* PS_MAX_FH_SIZE */
#include "ps_write_buffer.h" /* struct ps_write_buffer_geom */

#define PS_STATEID_OTHER_SIZE 12

/*
 * Per-stripe dirty-tracking entry (PS Phase 4b).  Lives in
 * pwb_dirty_ht; one entry per stripe that has received any WRITE
 * bytes since the last successful flush.  Mutated and read under
 * pwb_mutex -- no concurrent access discipline beyond the buffer's
 * leaf mutex.
 *
 * pds_partial_mask == NULL means "fully dirty" -- every data shard
 * in this stripe is in the buffer; flush can encode directly from
 * the buffer with no RMW read.  Non-NULL means the mask has one
 * bit per data shard (pwbg_k bits, rounded up to bytes); a bit
 * set means that shard has been written.  The mask is promoted to
 * NULL ("fully dirty") when all bits become set.
 */
struct ps_dirty_stripe {
	struct cds_lfht_node pds_ht_node;
	uint32_t pds_stripe_no;
	uint32_t pds_partial_mask_bits; /* == geom.pwbg_k when allocated */
	uint8_t *pds_partial_mask; /* NULL => fully dirty */
};

struct ps_write_buffer {
	struct cds_lfht_node pwb_ht_node;

	/*
	 * Back-pointer to the listener's table.  Captured at insert
	 * time and read by the release callback so it can call
	 * cds_lfht_del unconditionally (idempotent) before queueing
	 * the call_rcu free -- defends against the "ref dropped via
	 * a path that forgot to del first" leak vector.  The lfht is
	 * destroyed only after every entry is removed and a
	 * synchronize_rcu drains pending frees, so this pointer is
	 * always valid for the duration of any release we run.
	 */
	struct cds_lfht *pwb_ht;

	/* Key */
	uint8_t pwb_stateid_other[PS_STATEID_OTHER_SIZE];
	uint8_t pwb_upstream_fh[PS_MAX_FH_SIZE];
	uint32_t pwb_upstream_fh_len;
	uint32_t pwb_listener_id;

	/*
	 * Latest stateid_seqid seen on a WRITE into this buffer.
	 * COMMIT op doesn't carry a stateid; pipeline_commit uses
	 * this stashed value when constructing the mds_file for
	 * ec_write_codec_with_file's LAYOUTGET.  Updated under
	 * pwb_mutex by every WRITE; a stale seqid would surface as
	 * NFS4ERR_BAD_STATEID on the upstream LAYOUTGET, which the
	 * codec returns to pipeline_commit verbatim.
	 */
	uint32_t pwb_stateid_seqid;

	/* Bytes */
	uint8_t *pwb_data; /* malloc, grows as needed */
	size_t pwb_capacity; /* allocated bytes */
	size_t pwb_high_water; /* highest (offset+count) seen */

	/* Bookkeeping */
	pthread_mutex_t pwb_mutex; /* leaf; serialises buffer mutation */
	struct urcu_ref pwb_ref; /* table + per-op find refs */
	uint64_t pwb_listener_gen; /* listener boot generation snapshot */

	/*
	 * Per-stripe RMW state (Phase 4b).  Geometry is snapshot on
	 * first WRITE via ps_write_buffer_set_geom; pwb_geom_set
	 * gates subsequent set_geom calls (idempotent for matching
	 * fields, -EINVAL on mismatch).  pwb_dirty_ht is allocated
	 * lazily on first dirty mark.  Both fields are mutated and
	 * read under pwb_mutex.
	 */
	struct ps_write_buffer_geom pwb_geom;
	bool pwb_geom_set;
	struct cds_lfht *pwb_dirty_ht;

	/*
	 * Composed-verifier state (PS Phase 4b slice 4b.4).  pwb_mds_verf
	 * captures the most recent writeverf observed in a successful
	 * per-stripe flush's CHUNK_COMMIT response (currently the first
	 * mirror's ccr_writeverf -- a per-DS-boot-epoch token).
	 * pwb_mds_verf_set is false until the first successful flush;
	 * ps_compose_write_verf folds the captured bytes into the
	 * listener verifier so a DS reboot between WRITE and COMMIT
	 * surfaces to the client as a verifier mismatch (RFC 8881
	 * S18.32.4 semantics).  Mutated and read under pwb_mutex;
	 * snapshotted by the reply paths before any drop/unref.
	 */
	uint8_t pwb_mds_verf[8]; /* == NFS4_VERIFIER_SIZE */
	bool pwb_mds_verf_set;

	/* RCU callback head for deferred free. */
	struct rcu_head pwb_rcu_head;
};

/*
 * Hash-table sizing.  Files in flight through a single PS listener
 * at any moment are bounded -- a typical pipeline test has <16
 * concurrent open files.  64 buckets is comfortably oversized; we
 * use auto-resize so growth is free if a workload demands it.
 */
#define PS_WRITE_BUFFER_HASH_BUCKETS_INIT 64

/*
 * Single-WRITE / total-buffered cap (default 1 GiB).
 *
 * NOT_NOW_BROWN_COW: the design's [[ps]] write_buffer_max_bytes
 * TOML field (Phase 4a step 9) is a follow-on slice.  Today this
 * is a compile-time constant; the pipeline shim is structured so
 * a later slice can swap a per-listener `pls_write_buffer_max`
 * field in without surface changes.
 */
#define REFFS_PS_WRITE_BUFFER_MAX (1024UL * 1024UL * 1024UL)

/* ------------------------------------------------------------------ */
/* Test hooks                                                          */
/* ------------------------------------------------------------------ */

/*
 * Function-pointer test hooks.  Default NULL; production code reads
 * with atomic_load_explicit(memory_order_relaxed) and skips the
 * call if NULL.  Tests atomic_store_explicit a callback before
 * exercising the path and clear it on teardown.  Atomic typing
 * defends against C11 data races (and TSAN false positives) on
 * concurrent assignment-vs-read; production reads happen on the
 * hot path so we keep the relaxed-load cost minimal.
 *
 * See "Test-hook injection" in
 * .claude/design/proxy-server-phase4a.md.
 */
extern _Atomic(void (*)(void)) ps_test_hook_pre_state_load;
extern _Atomic(void (*)(void)) ps_test_hook_in_codec_flush;
extern _Atomic(uint64_t (*)(void)) ps_test_hook_clock_now_ns;

/*
 * ps_write_buffer_table_count was moved to the public header
 * (ps_write_buffer.h) since the ps-write-buffer-stats probe handler
 * is the primary caller; tests pick it up from the same header.
 */

/* ------------------------------------------------------------------ */
/* Dirty-bitmap whitebox surface (Phase 4b)                            */
/* ------------------------------------------------------------------ */

/*
 * Look up a single stripe's dirty entry by stripe number.  Returns
 * NULL if the stripe has not been written.  Caller MUST hold
 * buf->pwb_mutex; the returned pointer is valid only while the
 * mutex is held.
 *
 * Whitebox-only: production code walks the dirty table via the
 * forthcoming flush iterator in slice 4b.2.  Tests use this for
 * point-inspection of the bitmap state.
 */
struct ps_dirty_stripe *
ps_write_buffer_dirty_lookup(struct ps_write_buffer *buf, uint32_t stripe_no);

/*
 * True iff every data shard in this stripe is dirty (no RMW read
 * needed at flush).  Equivalent to pds_partial_mask == NULL.
 * Caller MUST hold the owning buffer's pwb_mutex.
 */
bool ps_dirty_stripe_is_fully_dirty(const struct ps_dirty_stripe *ds);

/*
 * True iff shard `shard` (0..k-1) is dirty in this stripe.  A
 * fully-dirty stripe (NULL pds_partial_mask) reports every shard
 * as dirty.  Out-of-range shard returns false.  Caller MUST hold
 * the owning buffer's pwb_mutex.
 */
bool ps_dirty_stripe_shard_is_dirty(const struct ps_dirty_stripe *ds,
				    uint32_t shard);

/*
 * Remove a single stripe entry from the buffer's dirty hash
 * table.  No-op if `stripe_no` is not currently marked dirty.
 * Caller MUST hold buf->pwb_mutex; the helper is invoked by
 * ps_proxy_pipeline_commit (PS Phase 4b slice 4b.2) after a
 * successful per-stripe flush.
 *
 * The dirty table has no readers outside pwb_mutex (lookup_or_alloc
 * and the table-drain path are the only other touchers, both
 * mutex-bound), so the entry is freed directly without call_rcu --
 * same pattern as pwb_dirty_table_drain.
 */
void ps_write_buffer_dirty_remove(struct ps_write_buffer *buf,
				  uint32_t stripe_no);

#endif /* PS_WRITE_BUFFER_INTERNAL_H */
