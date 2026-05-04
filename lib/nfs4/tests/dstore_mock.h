/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * dstore_mock -- fake dstore vtable for reflected GETATTR unit tests.
 *
 * A mock dstore intercepts control-plane calls and records call counts.
 * It responds to getattr by writing caller-supplied size and mtime into
 * the layout_data_file, allowing tests to verify that the fan-out fired
 * and that the inode was updated with the DS-reported values.
 *
 * Usage:
 *   struct dstore_mock *dm = dstore_mock_alloc(DSTORE_ID);
 *   dstore_mock_set_reply(dm, 4096, &mtime, false);
 *   // ... build compound with layout pointing at DSTORE_ID ...
 *   // ... run the op (LAYOUTRETURN, CLOSE, DELEGRETURN) ...
 *   mock_drive_fanout(rt);
 *   ck_assert_uint_eq(dstore_mock_getattr_calls(dm), 1);
 *   dstore_mock_free(dm);
 *
 * The dstore subsystem (dstore_init/dstore_fini) must be initialised
 * before calling dstore_mock_alloc and torn down after dstore_mock_free.
 */

#ifndef _REFFS_DSTORE_MOCK_H
#define _REFFS_DSTORE_MOCK_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#include "reffs/dstore_ops.h"

struct dstore;
struct layout_segments;
struct rpc_trans;

/*
 * struct dstore_mock -- embedded-vtable mock for one dstore.
 *
 * dm_ops MUST be the first member so that a pointer to dm_ops can be
 * cast back to the enclosing struct dstore_mock via a plain pointer cast.
 * The dstore code stores ds_ops as a const pointer; the mock recovers
 * the dstore_mock via:
 *   (struct dstore_mock *)(void *)ds->ds_ops
 */
struct dstore_mock {
	struct dstore_ops dm_ops; /* embedded vtable -- MUST be first */
	struct dstore *dm_ds;
	uint32_t dm_id;

	/* per-call counters (C11 _Atomic) */
	_Atomic uint32_t dm_getattr_calls;
	_Atomic uint32_t dm_truncate_calls;
	_Atomic uint32_t dm_fence_calls;
	_Atomic uint32_t dm_chmod_calls;
	_Atomic uint32_t dm_revoke_calls;
	_Atomic uint32_t dm_trust_calls;

	/*
	 * Last revoke_stateid args received (for tests that assert which
	 * stateid was revoked at this DS).  When N priors are revoked
	 * against this mock, multiple fan-out worker threads write
	 * concurrently -- the seqid write is atomic; the 12-byte other
	 * field is protected by dm_last_revoke_lock.  Tests that need
	 * per-call recording can use dm_revoke_calls and a sequence of
	 * single-prior tests instead.
	 */
	_Atomic uint32_t dm_last_revoke_seqid;
	pthread_mutex_t dm_last_revoke_lock;
	uint8_t dm_last_revoke_other[12]; /* NFS4_OTHER_SIZE */

	/*
	 * Reply values written into the layout_data_file on getattr.
	 * Set via dstore_mock_set_reply() before the op under test.
	 */
	int64_t dm_reply_size;
	struct timespec dm_reply_mtime;
	bool dm_getattr_fail; /* return -EIO when true */
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */

/*
 * dstore_mock_alloc -- allocate a mock, register it in the dstore hash
 * table, and mark it as mounted so dstore_is_available() returns true.
 *
 * Requires dstore_init() to have been called.
 * Returns NULL on allocation failure.
 */
struct dstore_mock *dstore_mock_alloc(uint32_t id);

/*
 * dstore_mock_free -- remove from the hash table and free.
 * Drops the creation ref; safe only when no other refs are outstanding.
 */
void dstore_mock_free(struct dstore_mock *dm);

/* ------------------------------------------------------------------ */
/* Configuration                                                       */

/*
 * dstore_mock_set_reply -- set the size/mtime returned by getattr and
 * whether getattr should simulate an error.
 */
void dstore_mock_set_reply(struct dstore_mock *dm, int64_t size,
			   const struct timespec *mtime, bool fail);

/*
 * dstore_mock_reset -- zero all call counters and clear the reply values.
 */
void dstore_mock_reset(struct dstore_mock *dm);

/* ------------------------------------------------------------------ */
/* Accessors (return current counter values)                           */

static inline uint32_t dstore_mock_getattr_calls(const struct dstore_mock *dm)
{
	return atomic_load_explicit(&dm->dm_getattr_calls,
				    memory_order_relaxed);
}

static inline uint32_t dstore_mock_truncate_calls(const struct dstore_mock *dm)
{
	return atomic_load_explicit(&dm->dm_truncate_calls,
				    memory_order_relaxed);
}

static inline uint32_t dstore_mock_fence_calls(const struct dstore_mock *dm)
{
	return atomic_load_explicit(&dm->dm_fence_calls, memory_order_relaxed);
}

static inline uint32_t dstore_mock_chmod_calls(const struct dstore_mock *dm)
{
	return atomic_load_explicit(&dm->dm_chmod_calls, memory_order_relaxed);
}

static inline uint32_t dstore_mock_revoke_calls(const struct dstore_mock *dm)
{
	return atomic_load_explicit(&dm->dm_revoke_calls, memory_order_relaxed);
}

static inline uint32_t dstore_mock_trust_calls(const struct dstore_mock *dm)
{
	return atomic_load_explicit(&dm->dm_trust_calls, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* Layout helpers                                                      */

/*
 * mock_layout_segments_alloc -- build a layout_segments with one segment
 * and one data file pointing at dstore_id.  Sets ldf_size and ldf_mtime
 * to the supplied values so that the pre-fan-out cached state is known.
 *
 * The returned pointer is owned by the caller.  Assign it directly to
 * inode->i_layout_segments; layout_segments_free handles cleanup.
 */
struct layout_segments *
mock_layout_segments_alloc(uint32_t dstore_id, int64_t cached_size,
			   const struct timespec *mtime);

/* ------------------------------------------------------------------ */
/* Async fan-out driver                                                */

/*
 * mock_drive_fanout -- spin until the task is no longer paused (all fan-out
 * threads completed), then invoke rt->rt_next_action inline.
 *
 * Call this after the op under test returned NFS4_OP_FLAG_ASYNC.  The
 * fan-out pthreads are fast (mock getattr returns immediately), so this
 * reliably completes in microseconds.
 *
 * Fails the current check test if the task is still paused after a
 * short timeout (indicates a bug in the fan-out or test setup).
 */
void mock_drive_fanout(struct rpc_trans *rt);

#endif /* _REFFS_DSTORE_MOCK_H */
