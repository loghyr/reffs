/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Mock dstore vtable for reflected GETATTR unit tests.
 *
 * See dstore_mock.h for the public API and usage notes.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <check.h>

#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/layout_segment.h"
#include "reffs/rpc.h"
#include "reffs/task.h"

#include "dstore_mock.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */

/*
 * Recover the enclosing dstore_mock from a dstore pointer.
 * The mock stores its vtable as the first member of dstore_mock, so
 * ds->ds_ops points into the middle of the dstore_mock allocation.
 * Cast back via a void * to silence strict-aliasing warnings.
 */
static struct dstore_mock *mock_from_ds(struct dstore *ds)
{
	return (struct dstore_mock *)(void *)ds->ds_ops;
}

/* ------------------------------------------------------------------ */
/* Vtable functions                                                    */

static int mock_getattr(struct dstore *ds,
			const uint8_t *fh __attribute__((unused)),
			uint32_t fh_len __attribute__((unused)),
			struct layout_data_file *ldf)
{
	struct dstore_mock *dm = mock_from_ds(ds);

	atomic_fetch_add_explicit(&dm->dm_getattr_calls, 1,
				  memory_order_relaxed);

	if (dm->dm_getattr_fail)
		return -EIO;

	/*
	 * Write the mock reply into the layout_data_file so the resume
	 * callback (nfs4_op_layoutreturn_resume) can compare against the
	 * inode's cached values and update i_size/i_mtime accordingly.
	 */
	ldf->ldf_size = dm->dm_reply_size;
	ldf->ldf_mtime = dm->dm_reply_mtime;
	return 0;
}

static int mock_truncate(struct dstore *ds,
			 const uint8_t *fh __attribute__((unused)),
			 uint32_t fh_len __attribute__((unused)),
			 uint64_t size __attribute__((unused)),
			 struct dstore_wcc *wcc __attribute__((unused)))
{
	struct dstore_mock *dm = mock_from_ds(ds);

	atomic_fetch_add_explicit(&dm->dm_truncate_calls, 1,
				  memory_order_relaxed);
	return 0;
}

static int mock_fence(struct dstore *ds,
		      const uint8_t *fh __attribute__((unused)),
		      uint32_t fh_len __attribute__((unused)),
		      struct layout_data_file *ldf __attribute__((unused)),
		      uint32_t fence_min __attribute__((unused)),
		      uint32_t fence_max __attribute__((unused)),
		      struct dstore_wcc *wcc __attribute__((unused)))
{
	struct dstore_mock *dm = mock_from_ds(ds);

	atomic_fetch_add_explicit(&dm->dm_fence_calls, 1, memory_order_relaxed);
	return 0;
}

static int mock_chmod(struct dstore *ds,
		      const uint8_t *fh __attribute__((unused)),
		      uint32_t fh_len __attribute__((unused)),
		      struct dstore_wcc *wcc __attribute__((unused)))
{
	struct dstore_mock *dm = mock_from_ds(ds);

	atomic_fetch_add_explicit(&dm->dm_chmod_calls, 1, memory_order_relaxed);
	return 0;
}

/* Stubs -- not needed for current tests */
static int mock_create(struct dstore *ds __attribute__((unused)),
		       const uint8_t *dir_fh __attribute__((unused)),
		       uint32_t dir_fh_len __attribute__((unused)),
		       const char *name __attribute__((unused)),
		       uint8_t *out_fh __attribute__((unused)),
		       uint32_t *out_fh_len __attribute__((unused)))
{
	return -ENOSYS;
}

static int mock_remove(struct dstore *ds __attribute__((unused)),
		       const uint8_t *dir_fh __attribute__((unused)),
		       uint32_t dir_fh_len __attribute__((unused)),
		       const char *name __attribute__((unused)))
{
	return -ENOSYS;
}

/*
 * Trust-stateid hooks: count only.  No mock trust table; tests that
 * need to assert "the prior client's trust entry is gone" use the
 * revoke-call counter instead.  The probe_tight_coupling stub
 * returns 0 so dstore_alloc considers the mock tight-coupled (which
 * the slice 1 path requires before issuing TRUST_STATEID).
 */
static int mock_probe_tight_coupling(struct dstore *ds __attribute__((unused)))
{
	return 0;
}

static int mock_trust_stateid(struct dstore *ds,
			      const uint8_t *fh __attribute__((unused)),
			      uint32_t fh_len __attribute__((unused)),
			      uint32_t stid_seqid __attribute__((unused)),
			      const uint8_t *stid_other __attribute__((unused)),
			      uint32_t iomode __attribute__((unused)),
			      uint64_t clientid __attribute__((unused)),
			      int64_t expire_sec __attribute__((unused)),
			      uint32_t expire_nsec __attribute__((unused)),
			      const char *principal __attribute__((unused)))
{
	struct dstore_mock *dm = mock_from_ds(ds);

	atomic_fetch_add_explicit(&dm->dm_trust_calls, 1, memory_order_relaxed);
	return 0;
}

static int mock_revoke_stateid(struct dstore *ds,
			       const uint8_t *fh __attribute__((unused)),
			       uint32_t fh_len __attribute__((unused)),
			       uint32_t stid_seqid, const uint8_t *stid_other)
{
	struct dstore_mock *dm = mock_from_ds(ds);

	atomic_fetch_add_explicit(&dm->dm_revoke_calls, 1,
				  memory_order_relaxed);
	atomic_store_explicit(&dm->dm_last_revoke_seqid, stid_seqid,
			      memory_order_release);
	pthread_mutex_lock(&dm->dm_last_revoke_lock);
	memcpy(dm->dm_last_revoke_other, stid_other,
	       sizeof(dm->dm_last_revoke_other));
	pthread_mutex_unlock(&dm->dm_last_revoke_lock);
	return 0;
}

static int mock_bulk_revoke_stateid(struct dstore *ds __attribute__((unused)),
				    uint64_t clientid __attribute__((unused)))
{
	return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */

struct dstore_mock *dstore_mock_alloc(uint32_t id)
{
	struct dstore_mock *dm = calloc(1, sizeof(*dm));

	if (!dm)
		return NULL;

	pthread_mutex_init(&dm->dm_last_revoke_lock, NULL);

	/* Initialize the embedded vtable. */
	dm->dm_ops.name = "mock";
	dm->dm_ops.create = mock_create;
	dm->dm_ops.remove = mock_remove;
	dm->dm_ops.chmod = mock_chmod;
	dm->dm_ops.truncate = mock_truncate;
	dm->dm_ops.fence = mock_fence;
	dm->dm_ops.getattr = mock_getattr;
	dm->dm_ops.probe_tight_coupling = mock_probe_tight_coupling;
	dm->dm_ops.trust_stateid = mock_trust_stateid;
	dm->dm_ops.revoke_stateid = mock_revoke_stateid;
	dm->dm_ops.bulk_revoke_stateid = mock_bulk_revoke_stateid;
	/* read/write/commit remain NULL -- not needed for fan-out tests */

	dm->dm_id = id;

	/*
	 * Register the dstore in the global hash table (mount=false so
	 * we don't actually try to connect to any NFS server).
	 */
	dm->dm_ds = dstore_alloc(id, "mock-ds", 0, "/mock",
				 REFFS_DS_PROTO_NFSV3, false, false);
	if (!dm->dm_ds) {
		free(dm);
		return NULL;
	}

	/*
	 * Override the vtable with our mock and mark the dstore as mounted
	 * so dstore_is_available() returns true and dstore_find() is
	 * willing to hand it out to the fan-out code.
	 */
	dm->dm_ds->ds_ops = (const struct dstore_ops *)&dm->dm_ops;
	__atomic_or_fetch(&dm->dm_ds->ds_state, DSTORE_IS_MOUNTED,
			  __ATOMIC_RELEASE);

	return dm;
}

void dstore_mock_free(struct dstore_mock *dm)
{
	if (!dm)
		return;

	/*
	 * Remove from hash table, then drop both refs:
	 *   - the alloc ref (ref 2 -> 1)
	 *   - the creation ref (ref 1 -> 0) -> triggers dstore_release -> call_rcu
	 * dstore_fini() -> rcu_barrier() drains the call_rcu before LSAN runs.
	 */
	if (dm->dm_ds) {
		struct dstore *ds = dm->dm_ds;

		dm->dm_ds = NULL;
		dstore_unhash(ds);
		dstore_put(ds); /* alloc ref */
		dstore_put(ds); /* creation ref */
	}

	pthread_mutex_destroy(&dm->dm_last_revoke_lock);
	free(dm);
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */

void dstore_mock_set_reply(struct dstore_mock *dm, int64_t size,
			   const struct timespec *mtime, bool fail)
{
	dm->dm_reply_size = size;
	dm->dm_reply_mtime = mtime ? *mtime : (struct timespec){ 0, 0 };
	dm->dm_getattr_fail = fail;
}

void dstore_mock_reset(struct dstore_mock *dm)
{
	atomic_store_explicit(&dm->dm_getattr_calls, 0, memory_order_relaxed);
	atomic_store_explicit(&dm->dm_truncate_calls, 0, memory_order_relaxed);
	atomic_store_explicit(&dm->dm_fence_calls, 0, memory_order_relaxed);
	atomic_store_explicit(&dm->dm_chmod_calls, 0, memory_order_relaxed);
	atomic_store_explicit(&dm->dm_revoke_calls, 0, memory_order_relaxed);
	atomic_store_explicit(&dm->dm_trust_calls, 0, memory_order_relaxed);
	dm->dm_reply_size = 0;
	dm->dm_reply_mtime = (struct timespec){ 0, 0 };
	dm->dm_getattr_fail = false;
	atomic_store_explicit(&dm->dm_last_revoke_seqid, 0,
			      memory_order_relaxed);
	pthread_mutex_lock(&dm->dm_last_revoke_lock);
	memset(dm->dm_last_revoke_other, 0, sizeof(dm->dm_last_revoke_other));
	pthread_mutex_unlock(&dm->dm_last_revoke_lock);
}

/* ------------------------------------------------------------------ */
/* Layout helper                                                       */

struct layout_segments *mock_layout_segments_alloc(uint32_t dstore_id,
						   int64_t cached_size,
						   const struct timespec *mtime)
{
	struct layout_segments *lss = layout_segments_alloc();

	if (!lss)
		return NULL;

	struct layout_data_file *ldf = calloc(1, sizeof(*ldf));

	if (!ldf) {
		layout_segments_free(lss);
		return NULL;
	}

	ldf->ldf_dstore_id = dstore_id;
	ldf->ldf_size = cached_size;
	if (mtime)
		ldf->ldf_mtime = *mtime;

	/*
	 * layout_segments_add takes ownership of ldf (the ls_files array).
	 * Use LAYOUT_IOMODE_RW so nfs4_layout_implicit_return_rw can find
	 * and clear the write iomode bit on the matching stateid.
	 */
	struct layout_segment seg = {
		.ls_nfiles = 1,
		.ls_files = ldf,
	};

	if (layout_segments_add(lss, &seg) != 0) {
		free(ldf);
		layout_segments_free(lss);
		return NULL;
	}

	return lss;
}

/* ------------------------------------------------------------------ */
/* Async fan-out driver                                                */

/*
 * Spin timeout: 50 ms in 2 us steps.  The mock getattr returns
 * immediately so the fan-out threads complete in microseconds;
 * 50 ms is an enormous safety margin.
 */
#define MOCK_FANOUT_ITERS 25000
#define MOCK_FANOUT_SLEEP_NS 2000

void mock_drive_fanout(struct rpc_trans *rt)
{
	struct task *t = rt->rt_task;

	for (int n = 0; n < MOCK_FANOUT_ITERS; n++) {
		if (!task_is_paused(t))
			break;
		struct timespec ts = { .tv_sec = 0,
				       .tv_nsec = MOCK_FANOUT_SLEEP_NS };
		nanosleep(&ts, NULL);
	}

	ck_assert_msg(!task_is_paused(t),
		      "mock_drive_fanout: fan-out timed out after 50 ms");

	/*
	 * Invoke the resume callback inline, exactly as rpc_process_task
	 * would after picking the task out of the run queue.
	 */
	if (rt->rt_next_action) {
		uint32_t (*fn)(struct rpc_trans *) = rt->rt_next_action;
		rt->rt_next_action = NULL;
		fn(rt);
	}
}
