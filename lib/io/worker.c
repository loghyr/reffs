/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/rpc.h"
#include "reffs/task.h"

// Queue for worker threads
pthread_mutex_t task_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t task_queue_cond = PTHREAD_COND_INITIALIZER;
struct task *task_queue[QUEUE_DEPTH];
int task_queue_head = 0;
int task_queue_tail = 0;

// Thread management
pthread_t worker_threads[MAX_WORKER_THREADS];
int num_worker_threads = 0;

/*
 * Set by io_mark_main_thread() so add_task() knows it must not block.
 * The io_uring main thread is the only one that processes CQEs; if it
 * blocks here waiting for queue space, no BACKEND_PWRITE completions
 * arrive, workers waiting for those completions never resume, and the
 * queue never drains -- a self-sustaining deadlock.
 */
static __thread bool is_io_main_thread = false;

void io_mark_main_thread(void)
{
	is_io_main_thread = true;
}

struct thread_data {
	int thread_id;
	volatile sig_atomic_t *running;
};

pthread_cond_t task_queue_full_cond = PTHREAD_COND_INITIALIZER;

// Worker thread function
void *io_worker_thread(void *vtd)
{
	struct thread_data *td = (struct thread_data *)vtd;

	volatile sig_atomic_t *running = td->running;

	free(vtd);

	// Register this thread with userspace RCU
	rcu_register_thread();

	while (*running) {
		struct task *t = NULL;

		// Use a timeout when checking for tasks during shutdown
		if (!running) {
			break;
		}

		pthread_mutex_lock(&task_queue_mutex);
		if (task_queue_head != task_queue_tail) {
			t = task_queue[task_queue_head];
			task_queue_head = (task_queue_head + 1) % QUEUE_DEPTH;
			pthread_cond_signal(&task_queue_full_cond);
			pthread_mutex_unlock(&task_queue_mutex);
		} else {
			// Wait with timeout during normal operation
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 1; // 1 second timeout

			int rc = pthread_cond_timedwait(&task_queue_cond,
							&task_queue_mutex, &ts);
			pthread_mutex_unlock(&task_queue_mutex);

			if (rc == ETIMEDOUT && !running) {
				break;
			}

			continue;
		}

		if (t) {
			/*
			 * If shutting down, discard queued tasks instead
			 * of processing them.  This prevents a long drain
			 * when the queue is full of RocksDB sync writes.
			 * In-flight async tasks complete naturally.
			 */
			if (!*running) {
				free(t->t_buffer);
				free(t);
				continue;
			}

			if (t->t_rt != NULL) {
				/*
				 * Resume path: task was paused by an async op
				 * and re-enqueued by the completer.  Skip RPC
				 * decode -- jump straight to the protocol op.
				 */
				struct rpc_trans *rt = t->t_rt;

				int rc = rpc_protocol_op_call(rt);
				if (rc == EINPROGRESS)
					continue; /* went async again */

				/*
				 * Compound fully complete.  Encode the RPC
				 * reply, send it to the client, and free all
				 * resources.
				 */
				rpc_complete_resumed_task(rt, t);
				free(t->t_buffer);
				free(t);
			} else if (t->t_fd > 0) {
				/*
				 * Fresh path: full RPC decode + dispatch.
				 */
				t->t_cb = io_rpc_trans_cb;
				int rc = rpc_process_task(t);
				if (rc == ENOMEM) {
					add_task(t);
					continue;
				}
				if (rc == EINPROGRESS)
					continue; /* owned by async path */

				free(t->t_buffer);
				free(t);
			} else {
				free(t->t_buffer);
				free(t);
			}
		}
	}

	// Unregister this thread from userspace RCU
	rcu_unregister_thread();

	return NULL;
}

void add_task(struct task *t)
{
	if (is_io_main_thread) {
		/*
		 * The io_uring main thread must never block here.  Spin
		 * briefly to give workers a chance to drain one slot, then
		 * drop the task so the main loop can continue processing
		 * CQEs (including the BACKEND_PWRITE completions that
		 * unblock paused workers).  The NFS client will retransmit
		 * after its RPC timeout.
		 */
		for (int i = 0; i < 16; i++) {
			pthread_mutex_lock(&task_queue_mutex);
			if (((task_queue_tail + 1) % QUEUE_DEPTH) !=
			    task_queue_head) {
				task_queue[task_queue_tail] = t;
				task_queue_tail =
					(task_queue_tail + 1) % QUEUE_DEPTH;
				pthread_cond_signal(&task_queue_cond);
				pthread_mutex_unlock(&task_queue_mutex);
				return;
			}
			pthread_mutex_unlock(&task_queue_mutex);
			sched_yield();
		}
		if (t->t_rt != NULL) {
			/*
			 * Resume task: the rpc_trans and compound are still
			 * live -- freeing t->t_buffer would corrupt them.
			 * Log the anomaly; the async completion path will
			 * eventually time out or the client will close.
			 */
			LOG("task queue full, dropping resume task fd=%d -- "
			    "in-flight RPC will stall",
			    t->t_fd);
		} else {
			LOG("task queue full, dropping RPC xid=0x%08x fd=%d -- "
			    "client will retransmit",
			    t->t_xid, t->t_fd);
			free(t->t_buffer);
		}
		free(t);
		return;
	}

	/* Worker threads can block -- they have nothing else to do. */
	pthread_mutex_lock(&task_queue_mutex);
	while (((task_queue_tail + 1) % QUEUE_DEPTH) == task_queue_head) {
		pthread_cond_wait(&task_queue_full_cond, &task_queue_mutex);
	}
	task_queue[task_queue_tail] = t;
	task_queue_tail = (task_queue_tail + 1) % QUEUE_DEPTH;
	pthread_cond_signal(&task_queue_cond);
	pthread_mutex_unlock(&task_queue_mutex);
}

int create_worker_threads(volatile sig_atomic_t *running, unsigned int nworkers)
{
	if (nworkers == 0 || nworkers > MAX_WORKER_THREADS)
		nworkers = MAX_WORKER_THREADS;

	TRACE("Creating %u worker threads", nworkers);

	for (unsigned int i = 0; i < nworkers; i++) {
		struct thread_data *td = malloc(sizeof(*td));
		if (!td) {
			LOG("Failed to create worker thread %u", i);
			continue;
		}

		td->thread_id = (int)i;
		td->running = running;

		if (pthread_create(&worker_threads[i], NULL, io_worker_thread,
				   td) == 0) {
			num_worker_threads++;
		} else {
			free(td);
			LOG("Failed to create worker thread %u", i);
		}
	}

	return 0;
}

void wait_for_worker_threads(void)
{
	// Wait for worker threads to finish
	TRACE("Waiting for worker threads to exit...");
	for (int i = 0; i < num_worker_threads; i++) {
		pthread_join(worker_threads[i], NULL);
	}
}

void wake_worker_threads(void)
{
	pthread_cond_broadcast(&task_queue_cond);
}
