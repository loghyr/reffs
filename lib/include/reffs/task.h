/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TASK_H
#define _REFFS_TASK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>
#include <liburing.h>
#include "reffs/ring.h"
#include "reffs/network.h"

struct rpc_trans;

enum task_state {
	TASK_IDLE = 0,
	TASK_RUNNING = 1,
	TASK_PAUSED = 2,
	TASK_DONE = 3,
};

/* Task struct for worker queue */
struct task {
	char *t_buffer;
	int t_bytes_read;
	uint32_t t_xid;
	int t_fd; /* For sending responses */
	struct connection_info t_ci;
	struct ring_context *t_rc;
	int (*t_cb)(struct rpc_trans *rt);
	_Atomic enum task_state t_state;
	struct rpc_trans *t_rt; /* non-NULL once rpc_trans is allocated */
};

/* Forward declaration to avoid circular include with reffs/io.h */
void add_task(struct task *task);

/*
 * task_pause -- transition RUNNING -> PAUSED atomically.
 *
 * Called by an op handler before starting async work.  After this returns
 * true the caller MUST NOT touch the task, rpc_trans, or compound again.
 * Returns false if the transition was not possible (programming error).
 */
static inline bool task_pause(struct task *t)
{
	enum task_state expected = TASK_RUNNING;
	return atomic_compare_exchange_strong_explicit(&t->t_state, &expected,
						       TASK_PAUSED,
						       memory_order_acq_rel,
						       memory_order_relaxed);
}

/*
 * task_resume -- transition PAUSED -> RUNNING and re-enqueue the task.
 *
 * Called by the async completer (io_uring CQE handler, DS response handler,
 * etc.).  Safe to call from any thread.  The worker that dequeues the task
 * owns it from that point.
 */
static inline void task_resume(struct task *t)
{
	enum task_state expected = TASK_PAUSED;
	bool ok = atomic_compare_exchange_strong_explicit(
		&t->t_state, &expected, TASK_RUNNING, memory_order_acq_rel,
		memory_order_relaxed);
	if (ok)
		add_task(t);
}

/*
 * task_is_paused -- true if the task is currently in the PAUSED state.
 *
 * Used by dispatch_compound after each op call to detect async yield.
 */
static inline bool task_is_paused(const struct task *t)
{
	return atomic_load_explicit(&t->t_state, memory_order_acquire) ==
	       TASK_PAUSED;
}

#endif /* _REFFS_TASK_H */
