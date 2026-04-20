/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TASK_H
#define _REFFS_TASK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>
#include "reffs/ring.h"
#include "reffs/network.h"

#ifdef __APPLE__
/*
 * Darwin's <mach/task.h> (pulled in transitively by <rpc/rpc.h>
 * force-include, and by liburcu's arch headers via <mach/mach.h>)
 * declares task_resume / task_suspend / task_create as Mach kernel
 * APIs.  reffs's task_resume / task_suspend are unrelated worker-
 * queue operations.
 *
 * Pre-include <mach/task.h> HERE, with the real names, so Mach's
 * declarations are parsed first.  THEN #define the names to rename
 * reffs's own symbols.  After the #define, any later
 * (re-)inclusion of <mach/task.h> is a no-op thanks to its header
 * guard, so the macro can't rewrite Mach's prototype into
 * `kern_return_t reffs_task_resume(task_t)` and collide with
 * reffs's `void reffs_task_resume(struct task *)`.
 *
 * Linux and FreeBSD are unaffected -- they never include <mach/task.h>.
 */
#include <mach/task.h>
#define task_resume reffs_task_resume
#define task_suspend reffs_task_suspend
#endif

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
	/*
	 * Set by task_pause() before the SQE is submitted; atomically read
	 * and cleared by task_check_and_clear_went_async() in
	 * dispatch_compound()'s forward loop.  This gives the CURRENT
	 * worker a racy-free "op went async" signal that cannot be stolen
	 * by a second worker that picks up the task after a fast CQE.
	 */
	_Atomic bool t_went_async;
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
	bool ok = atomic_compare_exchange_strong_explicit(
		&t->t_state, &expected, TASK_PAUSED, memory_order_acq_rel,
		memory_order_relaxed);
	if (ok)
		atomic_store_explicit(&t->t_went_async, true,
				      memory_order_release);
	return ok;
}

/*
 * task_check_and_clear_went_async -- atomically read and clear the
 * t_went_async flag set by task_pause().
 *
 * Called by dispatch_compound() in its forward loop immediately after
 * an op handler returns.  Because t_went_async is set BEFORE task_pause
 * submits the SQE, and is only cleared HERE (on the same worker thread
 * that called the op handler), this check cannot be stolen by a second
 * worker that picks up the task after a fast CQE fires.
 */
static inline bool task_check_and_clear_went_async(struct task *t)
{
	return atomic_exchange_explicit(&t->t_went_async, false,
					memory_order_acq_rel);
}

/*
 * task_unpause -- undo task_pause() WITHOUT re-enqueuing.
 *
 * Used when async I/O submission fails AFTER task_pause().  The task
 * stays on the current worker thread -- the caller can safely continue
 * accessing rt, compound, and all associated data.
 *
 * Contrast with task_resume() which re-enqueues the task and makes it
 * visible to other workers -- after task_resume() the caller MUST NOT
 * touch rt or compound.
 */
static inline bool task_unpause(struct task *t)
{
	enum task_state expected = TASK_PAUSED;
	bool ok = atomic_compare_exchange_strong_explicit(
		&t->t_state, &expected, TASK_RUNNING, memory_order_acq_rel,
		memory_order_relaxed);
	if (ok)
		atomic_store_explicit(&t->t_went_async, false,
				      memory_order_release);
	return ok;
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
