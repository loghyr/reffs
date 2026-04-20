/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Dstore fan-out -- parallel async DS operations.
 *
 * Each fan-out spawns one detached pthread per DS.  The thread runs
 * the blocking dstore op (via the vtable -- NFSv3 RPC or local VFS),
 * stores the result in its slot, and atomically decrements df_pending.
 * The last thread to complete calls task_resume() to re-enqueue the
 * paused compound.
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/dstore.h"
#include "dstore_fanout.h"
#include "reffs/dstore_ops.h"
#include "reffs/log.h"
#include "reffs/task.h"
#include "reffs/time.h"

#include "nfs4/trust_stateid.h"

/*
 * Verify that the hardcoded sizes in dstore_fanout.h match the
 * constants defined in trust_stateid.h.  A mismatch would silently
 * truncate stateid other-field or principal copies.
 */
_Static_assert(sizeof(((struct dstore_fanout *)0)->df_ts_other) ==
		       NFS4_OTHER_SIZE,
	       "df_ts_other size mismatch with NFS4_OTHER_SIZE");
_Static_assert(sizeof(((struct dstore_fanout *)0)->df_ts_principal) ==
		       TRUST_PRINCIPAL_MAX,
	       "df_ts_principal size mismatch with TRUST_PRINCIPAL_MAX");

/* ------------------------------------------------------------------ */
/* Thread entry point                                                  */
/* ------------------------------------------------------------------ */

struct fanout_thread_arg {
	struct dstore_fanout *fta_df;
	uint32_t fta_slot_idx;
};

static void *fanout_thread(void *arg)
{
	struct fanout_thread_arg *fta = arg;
	struct dstore_fanout *df = fta->fta_df;
	struct fanout_slot *slot = &df->df_slots[fta->fta_slot_idx];
	int ret = 0;

	free(fta); /* arg was heap-allocated */

	if (slot->fs_ldf)
		slot->fs_ldf->ldf_last_op_sent_ns = reffs_now_ns();

	switch (df->df_op) {
	case FANOUT_TRUNCATE:
		ret = dstore_data_file_truncate(slot->fs_ds, slot->fs_fh,
						slot->fs_fh_len, df->df_size,
						&slot->fs_wcc);
		break;

	case FANOUT_GETATTR:
		ret = dstore_data_file_getattr(slot->fs_ds, slot->fs_fh,
					       slot->fs_fh_len, slot->fs_ldf);
		break;

	case FANOUT_FENCE:
		ret = dstore_data_file_fence(slot->fs_ds, slot->fs_fh,
					     slot->fs_fh_len, slot->fs_ldf,
					     df->df_fence_min, df->df_fence_max,
					     &slot->fs_wcc);
		break;

	case FANOUT_CHMOD:
		ret = dstore_data_file_chmod(slot->fs_ds, slot->fs_fh,
					     slot->fs_fh_len, &slot->fs_wcc);
		break;

	case FANOUT_TRUST_STATEID:
		ret = dstore_trust_stateid(
			slot->fs_ds, slot->fs_fh, slot->fs_fh_len,
			df->df_ts_seqid, df->df_ts_other, df->df_ts_iomode,
			df->df_ts_clientid, df->df_ts_expire_sec,
			df->df_ts_expire_nsec, df->df_ts_principal);
		break;
	}

	if (slot->fs_ldf)
		slot->fs_ldf->ldf_last_op_recv_ns = reffs_now_ns();

	slot->fs_result = ret;

	/*
	 * Atomically decrement pending.  The last thread to finish
	 * (pending goes to 0) resumes the paused compound.
	 */
	uint32_t remaining = atomic_fetch_sub_explicit(&df->df_pending, 1,
						       memory_order_acq_rel) -
			     1;

	if (remaining == 0)
		task_resume(df->df_task);

	return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

struct dstore_fanout *dstore_fanout_alloc(uint32_t nslots)
{
	struct dstore_fanout *df;
	size_t sz;

	if (nslots == 0)
		return NULL;

	sz = sizeof(*df) + nslots * sizeof(struct fanout_slot);
	df = calloc(1, sz);
	if (!df)
		return NULL;

	df->df_total = nslots;
	atomic_store_explicit(&df->df_pending, nslots, memory_order_relaxed);

	return df;
}

void dstore_fanout_launch(struct dstore_fanout *df, struct task *t)
{
	/*
	 * Snapshot df_total before spawning any threads.  Once the
	 * first thread completes and is the last pending, it calls
	 * task_resume --> worker runs resume callback --> dstore_fanout_free.
	 * After that, df is freed.  With a single dstore (combined mode),
	 * the thread can complete before pthread_create returns.
	 */
	uint32_t total = df->df_total;

	df->df_task = t;

	for (uint32_t i = 0; i < total; i++) {
		struct fanout_thread_arg *fta = malloc(sizeof(*fta));

		if (!fta) {
			/*
			 * OOM -- mark this slot as failed and decrement
			 * pending.  If this was the last slot, resume
			 * and stop -- df may be freed by the resumed
			 * worker.
			 */
			df->df_slots[i].fs_result = -ENOMEM;
			uint32_t rem = atomic_fetch_sub_explicit(
					       &df->df_pending, 1,
					       memory_order_acq_rel) -
				       1;
			if (rem == 0) {
				task_resume(df->df_task);
				return;
			}
			continue;
		}

		fta->fta_df = df;
		fta->fta_slot_idx = i;

		pthread_t tid;
		pthread_attr_t attr;

		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

		if (pthread_create(&tid, &attr, fanout_thread, fta) != 0) {
			free(fta);
			df->df_slots[i].fs_result = -ENOMEM;
			uint32_t rem = atomic_fetch_sub_explicit(
					       &df->df_pending, 1,
					       memory_order_acq_rel) -
				       1;
			pthread_attr_destroy(&attr);
			if (rem == 0) {
				task_resume(df->df_task);
				return;
			}
			continue;
		}

		pthread_attr_destroy(&attr);
	}
}

int dstore_fanout_result(struct dstore_fanout *df)
{
	for (uint32_t i = 0; i < df->df_total; i++) {
		if (df->df_slots[i].fs_result != 0)
			return df->df_slots[i].fs_result;
	}
	return 0;
}

void dstore_fanout_free(struct dstore_fanout *df)
{
	if (!df)
		return;

	for (uint32_t i = 0; i < df->df_total; i++)
		dstore_put(df->df_slots[i].fs_ds);
	free(df);
}
