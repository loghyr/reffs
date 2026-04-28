/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Proxy assignment queue -- slice 6c-y.
 *
 * The MDS-side autopilot (mirror-lifecycle slice E) decides what
 * MOVE / REPAIR work needs to happen for which files; PROXY_PROGRESS
 * pulls those work items from this queue and ships them inline to
 * the polling PS in proxy_assignment4 form.
 *
 * Slice 6c-y scope: the queue mechanism + producer / consumer API
 * + PROXY_PROGRESS reply builder that consumes it.  The autopilot
 * ITSELF (the producer for normal operation) is mirror-lifecycle
 * slice E and lands separately; for now an admin probe op
 * (NOT_NOW_BROWN_COW for slice 6c-y -- slice 6c-y ships the
 * in-process API only) and the unit tests are the producers.
 *
 * Wire shape this maps to: proxy_assignment4 in nfsv42_xdr.x:
 *   { proxy_op_kind4 pa_kind;
 *     proxy_stateid4 pa_stateid;
 *     nfs_fh4        pa_file_fh;
 *     uint64_t       pa_source_dstore_id;
 *     uint64_t       pa_target_dstore_id;
 *     opaque         pa_descriptor<>; }
 *
 * proxy_stateid is minted at PROXY_PROGRESS reply time, NOT at
 * enqueue time -- the queue holds work descriptions; the migration
 * record (and its proxy_stateid) is created when the work is
 * actually delivered to a PS.  Per-inode invariant
 * (one in-flight migration per inode) is enforced at delivery time
 * by migration_record_create's existing -EBUSY check.
 *
 * Queue ordering: FIFO.  Concurrent producers + a single consumer
 * are safe (mutex-guarded).  A future scheduler can replace the
 * FIFO with priority + per-PS affinity; the API stays the same.
 */

#ifndef _REFFS_NFS4_PROXY_ASSIGNMENT_QUEUE_H
#define _REFFS_NFS4_PROXY_ASSIGNMENT_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "nfsv42_xdr.h"

/*
 * Single queued work item.  Mirrors proxy_assignment4 with the
 * proxy_stateid omitted -- that field is populated by the consumer
 * when it mints the stateid + creates the migration record.
 *
 * pa_file_fh is held as a network_file_handle (sb_id + ino) inline
 * to keep enqueue allocation-free.  The wire encoding wraps it as a
 * variable-length opaque at delivery time.
 */
struct proxy_assignment_item {
	uint32_t paq_kind; /* proxy_op_kind4: MOVE | REPAIR | CANCEL_PRIOR */
	uint64_t paq_sb_id;
	uint64_t paq_ino;
	uint64_t paq_source_dstore_id;
	uint64_t paq_target_dstore_id;
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/*
 * proxy_assignment_queue_init -- initialize the global queue.
 * Idempotent on already-initialized.  Returns 0 on success.
 */
int proxy_assignment_queue_init(void);

/*
 * proxy_assignment_queue_fini -- drain and free the queue.
 * Idempotent on already-fini'd / never-init'd.
 */
void proxy_assignment_queue_fini(void);

/* ------------------------------------------------------------------ */
/* Producer                                                            */
/* ------------------------------------------------------------------ */

/*
 * proxy_assignment_queue_push -- enqueue one work item.  Caller-side
 * helper: pass a fully-populated `item` (the function copies the
 * bytes; caller may free its local copy).
 *
 * Returns 0 on success, -ENOMEM on allocation failure, -EINVAL if
 * the queue is not initialized or `item` is NULL.
 *
 * The queue has no max length in this slice; a future scheduler
 * may add per-PS or global caps.  The autopilot is expected to
 * self-throttle based on the in-flight migration record count.
 */
int proxy_assignment_queue_push(const struct proxy_assignment_item *item);

/* ------------------------------------------------------------------ */
/* Consumer                                                            */
/* ------------------------------------------------------------------ */

/*
 * proxy_assignment_queue_pop -- dequeue up to `max` items into
 * caller-provided storage (no allocation).
 *
 * Returns the number of items actually dequeued (0 if queue empty,
 * never > max).  The caller's `out` array must have room for at
 * least `max` entries.
 *
 * FIFO order: the oldest enqueued item is returned first.
 *
 * Used by PROXY_PROGRESS reply builder: pop up to PROXY_PROGRESS_MAX_BATCH
 * items per poll; for each, mint a proxy_stateid, create a migration
 * record, and emit a proxy_assignment4 in the reply.
 */
size_t proxy_assignment_queue_pop(struct proxy_assignment_item *out,
				  size_t max);

/*
 * proxy_assignment_queue_len -- snapshot the current queue length.
 * Advisory only; the value can change between this call and any
 * subsequent pop.  Used by tests.
 */
size_t proxy_assignment_queue_len(void);

#endif /* _REFFS_NFS4_PROXY_ASSIGNMENT_QUEUE_H */
