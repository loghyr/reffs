/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * Callback timeout tracking.
 *
 * Maintains a doubly-linked list of pending callbacks. A timer thread
 * periodically scans the list and expires entries that have been
 * pending longer than the lease period.
 *
 * Race safety: both cb_reply_handler and the timeout thread can try
 * to complete the same cb_pending.  The atomic CAS on cp_status from
 * -EINPROGRESS to the final value ensures only the winner calls
 * task_resume.
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/task.h"
#include "nfs4/cb.h"

/* Default timeout: 90 seconds. */
#define CB_TIMEOUT_SEC 90

/* Scan interval: 5 seconds. */
#define CB_SCAN_INTERVAL_SEC 5

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static pthread_mutex_t cb_timeout_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cb_timeout_cv = PTHREAD_COND_INITIALIZER;
static struct cb_pending cb_timeout_head; /* sentinel node */
static pthread_t cb_timeout_thread;
static _Atomic uint32_t cb_timeout_running;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* Register / unregister                                               */
/* ------------------------------------------------------------------ */

void cb_timeout_register(struct cb_pending *cp)
{
	cp->cp_deadline_ns =
		now_ns() + (uint64_t)CB_TIMEOUT_SEC * 1000000000ULL;

	pthread_mutex_lock(&cb_timeout_mutex);
	/* Insert at tail (before sentinel). */
	cp->cp_prev = cb_timeout_head.cp_prev;
	cp->cp_next = &cb_timeout_head;
	cb_timeout_head.cp_prev->cp_next = cp;
	cb_timeout_head.cp_prev = cp;
	pthread_mutex_unlock(&cb_timeout_mutex);
}

void cb_timeout_unregister(struct cb_pending *cp)
{
	pthread_mutex_lock(&cb_timeout_mutex);
	if (cp->cp_next) {
		cp->cp_prev->cp_next = cp->cp_next;
		cp->cp_next->cp_prev = cp->cp_prev;
		cp->cp_next = NULL;
		cp->cp_prev = NULL;
	}
	pthread_mutex_unlock(&cb_timeout_mutex);
}

/* ------------------------------------------------------------------ */
/* Timer thread                                                        */
/* ------------------------------------------------------------------ */

static void *cb_timeout_thread_fn(void *arg __attribute__((unused)))
{
	while (atomic_load_explicit(&cb_timeout_running,
				    memory_order_relaxed)) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += CB_SCAN_INTERVAL_SEC;

		pthread_mutex_lock(&cb_timeout_mutex);
		pthread_cond_timedwait(&cb_timeout_cv, &cb_timeout_mutex, &ts);
		pthread_mutex_unlock(&cb_timeout_mutex);

		if (!atomic_load_explicit(&cb_timeout_running,
					  memory_order_relaxed))
			break;

		uint64_t now = now_ns();

		pthread_mutex_lock(&cb_timeout_mutex);
		struct cb_pending *cp = cb_timeout_head.cp_next;

		while (cp != &cb_timeout_head) {
			struct cb_pending *next = cp->cp_next;

			if (now >= cp->cp_deadline_ns) {
				/* Unlink from list. */
				cp->cp_prev->cp_next = cp->cp_next;
				cp->cp_next->cp_prev = cp->cp_prev;
				cp->cp_next = NULL;
				cp->cp_prev = NULL;

				TRACE("CB timeout xid=0x%08x op=%u", cp->cp_xid,
				      cp->cp_op);

				/* Remove from RPC pending table. */
				io_unregister_request(cp->cp_xid);

				/*
				 * CAS ensures only one of reply handler /
				 * timeout calls task_resume.
				 */
				if (cb_pending_try_complete(cp, -ETIMEDOUT))
					task_resume(cp->cp_task);
			}

			cp = next;
		}
		pthread_mutex_unlock(&cb_timeout_mutex);
	}

	return NULL;
}

/* ------------------------------------------------------------------ */
/* Init / fini                                                         */
/* ------------------------------------------------------------------ */

int cb_timeout_init(void)
{
	/* Initialize sentinel (empty circular list). */
	cb_timeout_head.cp_next = &cb_timeout_head;
	cb_timeout_head.cp_prev = &cb_timeout_head;

	atomic_store_explicit(&cb_timeout_running, 1, memory_order_relaxed);

	return pthread_create(&cb_timeout_thread, NULL, cb_timeout_thread_fn,
			      NULL);
}

void cb_timeout_fini(void)
{
	/*
	 * Hold the mutex while clearing the flag and signalling so the
	 * wakeup is never lost: if the thread is between its running-flag
	 * check and pthread_cond_timedwait, it still holds the mutex, so
	 * our lock blocks until the thread has entered the wait -- and our
	 * signal is then guaranteed to reach it.  Without the lock the
	 * signal fires into the void and the thread sleeps for the full
	 * CB_SCAN_INTERVAL_SEC (5 s), causing intermittent test timeouts.
	 */
	pthread_mutex_lock(&cb_timeout_mutex);
	atomic_store_explicit(&cb_timeout_running, 0, memory_order_relaxed);
	pthread_cond_signal(&cb_timeout_cv);
	pthread_mutex_unlock(&cb_timeout_mutex);
	pthread_join(cb_timeout_thread, NULL);
}
