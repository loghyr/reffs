/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Dstore fan-out — parallel async operations across mirror DSes.
 *
 * Used when the MDS needs to send the same operation to all DSes
 * in a layout segment and wait for all to complete before resuming
 * the paused NFSv4 compound.
 *
 * Each fan-out spawns one pthread per DS.  The last thread to
 * complete calls task_resume() to re-enqueue the compound.
 */

#ifndef _REFFS_DSTORE_FANOUT_H
#define _REFFS_DSTORE_FANOUT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/layout_segment.h"

struct rpc_trans;
struct task;

/* Per-DS operation slot. */
struct fanout_slot {
	struct dstore *fs_ds;
	uint8_t fs_fh[LAYOUT_SEG_MAX_FH];
	uint32_t fs_fh_len;
	struct layout_data_file *fs_ldf; /* back-pointer to layout data file */
	int fs_result; /* 0 or -errno */
	struct dstore_wcc fs_wcc; /* post-op WCC from SETATTR ops */
};

/* Operation type for the fan-out. */
enum fanout_op {
	FANOUT_TRUNCATE,
	FANOUT_GETATTR,
	FANOUT_FENCE,
	FANOUT_CHMOD,
};

/* Fan-out context — allocated per compound that needs DS fan-out. */
struct dstore_fanout {
	struct task *df_task; /* task to resume when all complete */
	_Atomic uint32_t df_pending; /* threads still running */
	uint32_t df_total; /* total DS operations */
	enum fanout_op df_op; /* which operation */

	/* Operation-specific parameters. */
	union {
		uint64_t df_size; /* FANOUT_TRUNCATE */
		struct {
			uint32_t df_fence_min;
			uint32_t df_fence_max;
		};
	};

	struct fanout_slot df_slots[]; /* flex array, df_total entries */
};

/*
 * dstore_fanout_alloc -- allocate a fan-out context for nslots DSes.
 *
 * The caller populates df_slots[].fs_ds, fs_fh, fs_fh_len, and
 * sets df_op + operation-specific params, then calls
 * dstore_fanout_launch().
 *
 * Returns NULL on OOM.
 */
struct dstore_fanout *dstore_fanout_alloc(uint32_t nslots);

/*
 * dstore_fanout_launch -- spawn threads and start the fan-out.
 *
 * The caller must have already called task_pause() on the task.
 * df->df_task is set to the task to resume.
 *
 * After this returns, the caller must NOT touch the fan-out context
 * or the task — ownership transfers to the spawned threads.
 */
void dstore_fanout_launch(struct dstore_fanout *df, struct task *t);

/*
 * dstore_fanout_result -- check if all operations succeeded.
 *
 * Called from the resume callback after the compound is re-dispatched.
 * Returns 0 if all DSes succeeded, or the first non-zero error.
 */
int dstore_fanout_result(struct dstore_fanout *df);

/*
 * dstore_fanout_free -- release the fan-out context.
 *
 * Drops dstore refs held by each slot.  Called from the resume
 * callback after reading results.
 */
void dstore_fanout_free(struct dstore_fanout *df);

#endif /* _REFFS_DSTORE_FANOUT_H */
