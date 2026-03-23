/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * File runway — pre-created data file pool.
 *
 * At MDS startup, each dstore gets a runway of empty data files
 * created via the dstore ops vtable (NFSv3 CREATE or local VFS).
 * LAYOUTGET pops FHs from the runway instead of blocking on CREATE.
 *
 * Restart recovery: the sequence counter is persisted to pool_seq.dat
 * on the dstore.  On restart, the counter is read back and creation
 * resumes from where it left off — no orphaned files.
 *
 * Replenishment: when runway_pop drops below 25% capacity, a batch
 * of new files is created synchronously before returning.  For
 * production, this should move to a background thread.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/layout_segment.h"
#include "reffs/log.h"
#include "reffs/runway.h"

#define RUNWAY_LOW_WATER_PCT 25
#define RUNWAY_SEQ_FILE "pool_seq.dat"

/* ------------------------------------------------------------------ */
/* Sequence counter persistence                                        */
/* ------------------------------------------------------------------ */

static int runway_load_seq(struct dstore *ds, uint32_t *seq_out)
{
	struct layout_data_file ldf = { 0 };
	uint8_t fh[RUNWAY_MAX_FH];
	uint32_t fh_len = 0;
	int ret;

	/*
	 * Try to CREATE the seq file (UNCHECKED — succeeds if exists).
	 * Then GETATTR to read the size.  If size > 0, the file
	 * contains a 4-byte sequence counter.
	 *
	 * For simplicity, we use the file size as a proxy:
	 * the sequence counter is stored as the file size itself
	 * via SETATTR(size).  This avoids needing READ/WRITE RPCs.
	 */
	ret = dstore_data_file_create(ds, ds->ds_root_fh, ds->ds_root_fh_len,
				      RUNWAY_SEQ_FILE, fh, &fh_len);
	if (ret < 0)
		return ret;

	ret = dstore_data_file_getattr(ds, fh, fh_len, &ldf);
	if (ret < 0)
		return ret;

	*seq_out = (uint32_t)ldf.ldf_size;
	return 0;
}

static int runway_save_seq(struct dstore *ds, uint32_t seq)
{
	uint8_t fh[RUNWAY_MAX_FH];
	uint32_t fh_len = 0;
	int ret;

	ret = dstore_data_file_create(ds, ds->ds_root_fh, ds->ds_root_fh_len,
				      RUNWAY_SEQ_FILE, fh, &fh_len);
	if (ret < 0)
		return ret;

	return dstore_data_file_truncate(ds, fh, fh_len, (uint64_t)seq);
}

/* ------------------------------------------------------------------ */
/* Batch create                                                        */
/* ------------------------------------------------------------------ */

/*
 * Create up to `count` files starting from rw->rw_next_seq.
 * Appends them to the circular buffer.  Caller must hold rw_mutex.
 * Returns the number of files actually created.
 */
static uint32_t runway_batch_create(struct runway *rw, uint32_t count)
{
	struct dstore *ds = rw->rw_ds;
	uint32_t created = 0;

	for (uint32_t i = 0; i < count; i++) {
		char name[32];
		uint32_t slot;
		struct runway_entry *re;
		int ret;

		if (rw->rw_count >= rw->rw_capacity)
			break; /* buffer full */

		snprintf(name, sizeof(name), "pool_%06u.dat", rw->rw_next_seq);

		slot = (rw->rw_head + rw->rw_count) % rw->rw_capacity;
		re = &rw->rw_entries[slot];

		ret = dstore_data_file_create(ds, ds->ds_root_fh,
					      ds->ds_root_fh_len, name,
					      re->re_fh, &re->re_fh_len);
		if (ret < 0) {
			LOG("runway: dstore[%u] create %s failed: %s",
			    ds->ds_id, name, strerror(-ret));
			break;
		}

		rw->rw_count++;
		rw->rw_next_seq++;
		created++;
	}

	/* Persist the sequence counter after each batch. */
	if (created > 0)
		runway_save_seq(ds, rw->rw_next_seq);

	return created;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

struct runway *runway_create(struct dstore *ds, uint32_t count)
{
	struct runway *rw;
	uint32_t saved_seq = 0;

	if (!ds || count == 0)
		return NULL;

	rw = calloc(1, sizeof(*rw));
	if (!rw)
		return NULL;

	rw->rw_entries = calloc(count, sizeof(struct runway_entry));
	if (!rw->rw_entries) {
		free(rw);
		return NULL;
	}

	rw->rw_ds = dstore_get(ds);
	rw->rw_capacity = count;
	rw->rw_head = 0;
	rw->rw_count = 0;
	pthread_mutex_init(&rw->rw_mutex, NULL);

	/*
	 * Restart recovery: load the persisted sequence counter.
	 * If the file doesn't exist or is empty, start from 1.
	 */
	if (runway_load_seq(ds, &saved_seq) == 0 && saved_seq > 0)
		rw->rw_next_seq = saved_seq;
	else
		rw->rw_next_seq = 1;

	TRACE("runway: dstore[%u] starting from seq %u", ds->ds_id,
	      rw->rw_next_seq);

	/* Pre-create the initial batch. */
	uint32_t created = runway_batch_create(rw, count);

	TRACE("runway: dstore[%u] pre-created %u/%u files", ds->ds_id, created,
	      count);

	if (created == 0)
		LOG("runway: dstore[%u] no files created", ds->ds_id);

	return rw;
}

void runway_destroy(struct runway *rw)
{
	if (!rw)
		return;

	dstore_put(rw->rw_ds);
	pthread_mutex_destroy(&rw->rw_mutex);
	free(rw->rw_entries);
	free(rw);
}

int runway_pop(struct runway *rw, uint8_t *out_fh, uint32_t *out_fh_len)
{
	int ret = -EAGAIN;

	pthread_mutex_lock(&rw->rw_mutex);

	/*
	 * Replenish if below low-water mark.  This is synchronous —
	 * the caller blocks while files are created.  For production,
	 * this should be a background thread triggered by a signal.
	 */
	uint32_t low_water = rw->rw_capacity * RUNWAY_LOW_WATER_PCT / 100;

	if (rw->rw_count <= low_water) {
		uint32_t want = rw->rw_capacity - rw->rw_count;
		uint32_t got = runway_batch_create(rw, want);

		if (got > 0)
			TRACE("runway: dstore[%u] replenished %u files "
			      "(was %u/%u)",
			      rw->rw_ds->ds_id, got, rw->rw_count - got,
			      rw->rw_capacity);
	}

	if (rw->rw_count > 0) {
		struct runway_entry *re = &rw->rw_entries[rw->rw_head];

		memcpy(out_fh, re->re_fh, re->re_fh_len);
		*out_fh_len = re->re_fh_len;

		rw->rw_head = (rw->rw_head + 1) % rw->rw_capacity;
		rw->rw_count--;
		ret = 0;
	}

	pthread_mutex_unlock(&rw->rw_mutex);
	return ret;
}
