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

#include <urcu.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>

#include "ps_state.h" /* PS_MAX_FH_SIZE */

#define PS_STATEID_OTHER_SIZE 12

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

	/* Bytes */
	uint8_t *pwb_data; /* malloc, grows as needed */
	size_t pwb_capacity; /* allocated bytes */
	size_t pwb_high_water; /* highest (offset+count) seen */

	/* Bookkeeping */
	pthread_mutex_t pwb_mutex; /* leaf; serialises buffer mutation */
	struct urcu_ref pwb_ref; /* table + per-op find refs */
	uint64_t pwb_listener_gen; /* listener boot generation snapshot */

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
 * Single-WRITE / total-buffered cap (default 1 GiB).  Set at the
 * listener level so each listener can have its own cap; today every
 * listener uses the same default.
 *
 * Configurable via [[ps]] write_buffer_max_bytes in reffs.toml
 * (parser plumbing is Phase 4a step 9; 4a.2a ships the constant
 * with the default and the cap-check site reads the constant).
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

/* ------------------------------------------------------------------ */
/* Whitebox helpers used by tests                                      */
/* ------------------------------------------------------------------ */

/*
 * Count entries currently in the buffer table.  Used by drain tests
 * to assert the table is empty after teardown.  Walks the table
 * under rcu_read_lock; safe to call any time the listener is
 * registered.
 */
size_t ps_write_buffer_table_count(struct ps_listener_state *pls);

#endif /* PS_WRITE_BUFFER_INTERNAL_H */
