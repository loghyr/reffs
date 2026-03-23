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
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/log.h"
#include "reffs/runway.h"

struct runway *runway_create(struct dstore *ds, uint32_t count)
{
	struct runway *rw;

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
	rw->rw_next_seq = 1;
	pthread_mutex_init(&rw->rw_mutex, NULL);

	/*
	 * Pre-create files using the dstore's root FH as the parent.
	 * Files are named pool_NNNNNN.dat.  Partial success is fine —
	 * the runway just has fewer entries.
	 */
	for (uint32_t i = 0; i < count; i++) {
		char name[32];
		struct runway_entry *re;
		uint32_t idx;
		int ret;

		snprintf(name, sizeof(name), "pool_%06u.dat", rw->rw_next_seq);

		idx = rw->rw_count;
		re = &rw->rw_entries[idx];

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
	}

	TRACE("runway: dstore[%u] pre-created %u/%u files", ds->ds_id,
	      rw->rw_count, count);

	if (rw->rw_count == 0) {
		LOG("runway: dstore[%u] no files created", ds->ds_id);
		/* Return the empty runway — caller can check. */
	}

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
