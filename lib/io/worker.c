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
			if (t->t_fd > 0) {
				t->t_cb = io_rpc_trans_cb;
				int rc = rpc_process_task(t);
				if (rc == ENOMEM) {
					add_task(t);
					continue;
				}
			}

			free(t->t_buffer);
			free(t);
		}
	}

	// Unregister this thread from userspace RCU
	rcu_unregister_thread();

	return NULL;
}

void add_task(struct task *t)
{
	pthread_mutex_lock(&task_queue_mutex);
	// Wait while the queue is full
	while (((task_queue_tail + 1) % QUEUE_DEPTH) == task_queue_head) {
		pthread_cond_wait(&task_queue_full_cond, &task_queue_mutex);
	}
	task_queue[task_queue_tail] = t;
	task_queue_tail = (task_queue_tail + 1) % QUEUE_DEPTH;
	pthread_cond_signal(&task_queue_cond);
	pthread_mutex_unlock(&task_queue_mutex);
}

int create_worker_threads(volatile sig_atomic_t *running)
{
	// Create worker threads
	for (int i = 0; i < MAX_WORKER_THREADS; i++) {
		struct thread_data *td = malloc(sizeof(*td));
		if (!td) {
			LOG("Failed to create worker thread %d", i);
			continue;
		}

		td->thread_id = i;
		td->running = running;

		if (pthread_create(&worker_threads[i], NULL, io_worker_thread,
				   td) == 0) {
			num_worker_threads++;
		} else {
			free(td);
			LOG("Failed to create worker thread %d", i);
		}
	}

	return 0;
}

void wait_for_worker_threads(void)
{
	// Wait for worker threads to finish
	LOG("Waiting for worker threads to exit...");
	for (int i = 0; i < num_worker_threads; i++) {
		pthread_join(worker_threads[i], NULL);
	}
}

void wake_worker_threads(void)
{
	pthread_cond_broadcast(&task_queue_cond);
}
