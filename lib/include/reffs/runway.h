/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * File runway — pre-created pool of data file FHs per dstore.
 *
 * The MDS pre-populates each dstore with empty data files at startup
 * so that LAYOUTGET never blocks on NFSv3 CREATE.  FHs are popped
 * from the runway and assigned to layout segments.
 *
 * File creation is the bottleneck (from production experience).
 */

#ifndef _REFFS_RUNWAY_H
#define _REFFS_RUNWAY_H

#include <pthread.h>
#include <stdint.h>

#include "reffs/dstore.h"

#define RUNWAY_DEFAULT_SIZE 256
#define RUNWAY_MAX_FH 64

struct runway_entry {
	uint8_t re_fh[RUNWAY_MAX_FH];
	uint32_t re_fh_len;
};

struct runway {
	struct dstore *rw_ds; /* owning dstore (ref held) */
	struct runway_entry *rw_entries; /* circular buffer */
	uint32_t rw_capacity; /* total slots */
	uint32_t rw_head; /* next to pop */
	uint32_t rw_count; /* entries available */
	uint32_t rw_next_seq; /* next filename sequence number */
	pthread_mutex_t rw_mutex;
};

/*
 * runway_create -- allocate a runway and pre-create files on the dstore.
 *
 * Creates `count` empty data files on the dstore using the root FH
 * as the parent directory.  Files are named pool_NNNNNN.dat.
 *
 * Returns the runway on success (caller must runway_destroy), or
 * NULL on failure.  Partial success is OK — the runway may have
 * fewer entries than requested if some creates fail.
 */
struct runway *runway_create(struct dstore *ds, uint32_t count);

/*
 * runway_destroy -- free the runway.  Does NOT remove files from the DS.
 */
void runway_destroy(struct runway *rw);

/*
 * runway_pop -- pop a pre-created file FH from the runway.
 *
 * On success, copies the FH into out_fh/out_fh_len and returns 0.
 * Returns -EAGAIN if the runway is empty.
 */
int runway_pop(struct runway *rw, uint8_t *out_fh, uint32_t *out_fh_len);

/*
 * runway_available -- number of FHs available in the runway.
 */
static inline uint32_t runway_available(struct runway *rw)
{
	uint32_t n;

	pthread_mutex_lock(&rw->rw_mutex);
	n = rw->rw_count;
	pthread_mutex_unlock(&rw->rw_mutex);
	return n;
}

#endif /* _REFFS_RUNWAY_H */
