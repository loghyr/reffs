/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * macOS (Darwin) I/O backend.  Thread-pool for file I/O +
 * kqueue EVFILT_READ/WRITE for sockets (shared via
 * lib/io/kqueue_socket.c and lib/io/kqueue_common.{c,h}).
 *
 * Architecture
 * ------------
 *   File I/O:    worker threads run blocking pread(2)/pwrite(2) and
 *                signal completion via a pipe registered with
 *                EVFILT_READ on the backend ring's kqueue.
 *
 *   Socket I/O:  kqueue readiness (EVFILT_READ/WRITE), identical
 *                to FreeBSD -- shared code in kqueue_socket.c.
 *
 *   Heartbeat:   EVFILT_TIMER -- shared.
 *
 *   Shutdown:    pipe + EVFILT_READ via kqueue_common's
 *                io_handler_signal_shutdown.
 *
 * Why a thread pool and not POSIX aio?
 *   Darwin's POSIX aio routes through a small fixed-size kernel
 *   thread pool that serializes badly under concurrent load --
 *   documented pathology since OS X 10.5.  A userspace pthread
 *   pool gives better throughput and is portable.
 *
 * Why not libdispatch (Grand Central Dispatch)?
 *   libdispatch is Apple-only; binding the backend to it would
 *   lock reffs to macOS.  Plain pthreads are portable.
 *
 * Worker thread discipline
 * ------------------------
 *   - Workers do NOT call rcu_register_thread.  They touch only
 *     pread/pwrite + their own dp_job struct; they never traverse
 *     RCU-managed NFS state.  Contrast lib/io/worker.c's NFS-op
 *     workers, which DO register with RCU.
 *   - Workers never hold conn_mutex or rc_mutex.  They hold the
 *     pool's job_mutex only over linked-list manipulation.
 *     Lock order:  job_mutex (leaf).
 *   - TSAN annotations: worker releases dp_job and ic before the
 *     pipe write; main loop acquires both after the pipe read.
 *     The pipe write/read pair is the synthetic happens-before
 *     edge that ThreadSanitizer cannot infer from pread/pipe
 *     itself.
 *
 * Completion pipe payload
 * -----------------------
 *   Each worker writes exactly sizeof(struct dp_job *) bytes --
 *   a pointer, 8 bytes on arm64.  POSIX guarantees writes <=
 *   PIPE_BUF (Darwin: 512) are atomic and never short; the pipe
 *   can hold 64 pointers.  If a worker finds the pipe full
 *   (EAGAIN), it blocks on pool->pipe_space_cond, which the main
 *   loop's drain path signals after reading at least one pointer.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#ifndef IO_BACKEND_DARWIN_SKELETON_OK
#ifndef __APPLE__
#error "backend_darwin.c requires __APPLE__"
#endif
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "reffs/io.h"
#include "reffs/log.h"
#include "reffs/ring.h"
#include "reffs/rpc.h"
#include "reffs/task.h"
#include "trace/io.h"

#include "kqueue_common.h"
#include "posix_shims.h"
#include "tsan_io.h"

/* ------------------------------------------------------------------ */
/* Thread pool types                                                   */
/* ------------------------------------------------------------------ */

#define DP_MAX_WORKERS 16
#define DP_DEFAULT_WORKERS 4

enum dp_op_kind {
	DP_OP_PREAD,
	DP_OP_PWRITE,
};

struct dp_job {
	struct io_context *ic;
	enum dp_op_kind op;
	int fd;
	void *buf;
	size_t len;
	off_t offset;
	ssize_t result; /* filled by worker */
	int saved_errno; /* filled by worker */
	struct dp_job *next;
};

struct darwin_file_pool {
	pthread_t workers[DP_MAX_WORKERS];
	int num_workers;

	pthread_mutex_t job_mutex;
	pthread_cond_t job_cond; /* signaled when a job is enqueued */
	pthread_cond_t pipe_space_cond; /* signaled when pipe has space */
	struct dp_job *job_head;
	struct dp_job *job_tail;

	int completion_pipe[2]; /* [0]=main-loop read, [1]=worker write */

	/*
	 * Queue of jobs whose completion pipe write failed with a
	 * non-EAGAIN/non-EINTR error (e.g. EPIPE on shutdown races).
	 * Workers CANNOT call handle_backend_{pread,pwrite} inline --
	 * those routines reach into RCU-managed rpc_trans refcounts via
	 * rpc_protocol_free, and workers are deliberately not
	 * rcu_register_thread'd (see file comment).  Instead workers
	 * append here and the main loop drains on each tick.  Worst-
	 * case latency: IO_URING_WAIT_SEC (~1s) before the stalled
	 * task gets its -EIO completion.
	 */
	struct dp_job *failed_head;
	struct dp_job *failed_tail;

	/*
	 * Set to 1 by pool_init before any worker starts; cleared to 0
	 * by pool_fini.  Read both under job_mutex (dequeue, pipe-full
	 * retry) and unlocked (worker outer loop).  Must be _Atomic so
	 * the unlocked read in pool_worker observes the pool_fini store
	 * without a data race, and so the release/acquire pair gives
	 * workers a happens-before edge on the final job_head drain.
	 */
	_Atomic int running;
};

/*
 * One global pool per process.  The backend ring_context keeps a
 * back-pointer for convenience, but the storage is static because
 * ring_context is opaque to other TUs and we do not want to expose
 * the pool layout.
 */
static struct darwin_file_pool g_pool;

/* ------------------------------------------------------------------ */
/* Job queue                                                          */
/* ------------------------------------------------------------------ */

static void pool_enqueue(struct darwin_file_pool *pool, struct dp_job *job)
{
	pthread_mutex_lock(&pool->job_mutex);
	job->next = NULL;
	if (pool->job_tail)
		pool->job_tail->next = job;
	else
		pool->job_head = job;
	pool->job_tail = job;
	pthread_cond_signal(&pool->job_cond);
	pthread_mutex_unlock(&pool->job_mutex);
}

static struct dp_job *pool_dequeue(struct darwin_file_pool *pool)
{
	pthread_mutex_lock(&pool->job_mutex);
	while (atomic_load_explicit(&pool->running, memory_order_acquire) &&
	       !pool->job_head)
		pthread_cond_wait(&pool->job_cond, &pool->job_mutex);

	struct dp_job *job = pool->job_head;
	if (job) {
		pool->job_head = job->next;
		if (!pool->job_head)
			pool->job_tail = NULL;
		job->next = NULL;
	}
	pthread_mutex_unlock(&pool->job_mutex);
	return job;
}

/* ------------------------------------------------------------------ */
/* Completion handlers (main-loop thread)                             */
/* ------------------------------------------------------------------ */

static void handle_backend_pread(struct dp_job *job)
{
	struct io_context *ic = job->ic;
	struct rpc_trans *rt = ic->ic_rt;

	if (!rt) {
		LOG("handle_backend_pread: NULL rt in context id=%u",
		    ic->ic_id);
		io_context_destroy(ic);
		return;
	}

	if (job->result < 0)
		LOG("backend_pread: fd=%d expected=%zu error=%s", ic->ic_fd,
		    ic->ic_expected_len, strerror(job->saved_errno));

	rt->rt_io_result = job->result;
	io_context_destroy(ic);

	if (rt->rt_task)
		task_resume(rt->rt_task);

	rpc_protocol_free(rt);
}

static void handle_backend_pwrite(struct dp_job *job)
{
	struct io_context *ic = job->ic;
	struct rpc_trans *rt = ic->ic_rt;

	if (!rt) {
		LOG("handle_backend_pwrite: NULL rt in context id=%u",
		    ic->ic_id);
		io_context_destroy(ic);
		return;
	}

	if (job->result < 0 || (size_t)job->result < ic->ic_expected_len)
		LOG("backend_pwrite: fd=%d expected=%zu got=%zd", ic->ic_fd,
		    ic->ic_expected_len, job->result);

	rt->rt_io_result = job->result;
	io_context_destroy(ic);

	if (rt->rt_task)
		task_resume(rt->rt_task);

	rpc_protocol_free(rt);
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                      */
/* ------------------------------------------------------------------ */

static void *pool_worker(void *arg)
{
	struct darwin_file_pool *pool = arg;

	while (atomic_load_explicit(&pool->running, memory_order_acquire)) {
		struct dp_job *job = pool_dequeue(pool);
		if (!job)
			break; /* running=0 and queue drained */

		switch (job->op) {
		case DP_OP_PREAD:
			job->result =
				pread(job->fd, job->buf, job->len, job->offset);
			break;
		case DP_OP_PWRITE:
			job->result = pwrite(job->fd, job->buf, job->len,
					     job->offset);
			break;
		}
		job->saved_errno = (job->result < 0) ? errno : 0;

		TSAN_RELEASE(job);
		TSAN_RELEASE(job->ic);

		/*
		 * Hand the job pointer to the main loop via the pipe.
		 * POSIX guarantees write(pipe, ptr, sizeof(ptr)) is
		 * atomic-or-EAGAIN for sizes <= PIPE_BUF.  Retry EAGAIN
		 * via pipe_space_cond (signaled by the main loop after
		 * it drains at least one pointer) and EINTR inline.
		 *
		 * Missed-wakeup guard: the main loop's drain+broadcast
		 * is atomic under job_mutex, but a naive worker that
		 * observes EAGAIN outside the mutex and only then locks
		 * can race with a drain that completes (and broadcasts
		 * to no waiter) in the window before the lock.  The
		 * worker would then cond_wait on an already-satisfied
		 * condition with no more broadcasts coming until the
		 * next pipe fills -- a latent deadlock at high fan-in.
		 *
		 * Fix: re-attempt the write *under* job_mutex before
		 * cond_wait.  If the main loop drained in the race
		 * window, the pipe is now writable and the retry wins
		 * without sleeping.  If the pipe is still full, the
		 * drain has not happened yet, so the subsequent
		 * cond_wait is guaranteed to see the next broadcast.
		 */
		for (;;) {
			ssize_t w = write(pool->completion_pipe[1], &job,
					  sizeof(job));
			if (w == sizeof(job))
				break;
			if (w < 0 && errno == EINTR)
				continue;
			if (w < 0 &&
			    (errno == EAGAIN || errno == EWOULDBLOCK)) {
				pthread_mutex_lock(&pool->job_mutex);
				if (!atomic_load_explicit(
					    &pool->running,
					    memory_order_acquire)) {
					pthread_mutex_unlock(&pool->job_mutex);
					free(job);
					goto done;
				}
				w = write(pool->completion_pipe[1], &job,
					  sizeof(job));
				if (w == sizeof(job)) {
					pthread_mutex_unlock(&pool->job_mutex);
					break;
				}
				if (w < 0 &&
				    (errno == EAGAIN || errno == EWOULDBLOCK)) {
					pthread_cond_wait(
						&pool->pipe_space_cond,
						&pool->job_mutex);
					pthread_mutex_unlock(&pool->job_mutex);
					continue;
				}
				pthread_mutex_unlock(&pool->job_mutex);
				if (w < 0 && errno == EINTR)
					continue;
			}
			LOG("backend_darwin worker: pipe write failed: %s",
			    strerror(errno));
			/*
			 * Hand the failed job off to the main loop for
			 * completion.  Cannot call handle_backend_{pread,
			 * pwrite} here -- workers are not rcu_register_
			 * thread'd, and handle_* -> rpc_protocol_free ->
			 * urcu_ref_put requires RCU registration.
			 */
			job->result = -EIO;
			job->saved_errno = EIO;
			pthread_mutex_lock(&pool->job_mutex);
			job->next = NULL;
			if (pool->failed_tail)
				pool->failed_tail->next = job;
			else
				pool->failed_head = job;
			pool->failed_tail = job;
			pthread_mutex_unlock(&pool->job_mutex);
			goto done;
		}
done:;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Backend ring init / fini                                           */
/* ------------------------------------------------------------------ */

static int pool_init(struct darwin_file_pool *pool, struct ring_context *rc)
{
	memset(pool, 0, sizeof(*pool));
	pool->num_workers = DP_DEFAULT_WORKERS;
	atomic_store_explicit(&pool->running, 1, memory_order_release);

	if (pthread_mutex_init(&pool->job_mutex, NULL) != 0) {
		LOG("backend_darwin: pool mutex init failed");
		return -1;
	}
	if (pthread_cond_init(&pool->job_cond, NULL) != 0) {
		LOG("backend_darwin: job cond init failed");
		goto err_mutex;
	}
	if (pthread_cond_init(&pool->pipe_space_cond, NULL) != 0) {
		LOG("backend_darwin: pipe_space cond init failed");
		goto err_jcond;
	}

	if (reffs_pipe_nb_cloexec(pool->completion_pipe) < 0) {
		LOG("backend_darwin: completion pipe: %s", strerror(errno));
		goto err_pcond;
	}

	/* Register the pipe's read end with the backend ring's kqueue
	 * so the main loop wakes on completion. */
	struct kevent ke;
	EV_SET(&ke, pool->completion_pipe[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0,
	       0, NULL);
	if (kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL) < 0) {
		LOG("backend_darwin: EVFILT_READ add: %s", strerror(errno));
		goto err_pipe;
	}

	for (int i = 0; i < pool->num_workers; i++) {
		int err = pthread_create(&pool->workers[i], NULL, pool_worker,
					 pool);
		if (err != 0) {
			LOG("backend_darwin: pthread_create worker %d: %s", i,
			    strerror(err));
			/* Roll back started workers. */
			atomic_store_explicit(&pool->running, 0,
					      memory_order_release);
			pthread_cond_broadcast(&pool->job_cond);
			for (int j = 0; j < i; j++)
				pthread_join(pool->workers[j], NULL);
			goto err_pipe;
		}
	}

	TRACE("backend_darwin: pool up with %d workers, completion_pipe=(%d,%d)",
	      pool->num_workers, pool->completion_pipe[0],
	      pool->completion_pipe[1]);
	return 0;

err_pipe:
	close(pool->completion_pipe[0]);
	close(pool->completion_pipe[1]);
err_pcond:
	pthread_cond_destroy(&pool->pipe_space_cond);
err_jcond:
	pthread_cond_destroy(&pool->job_cond);
err_mutex:
	pthread_mutex_destroy(&pool->job_mutex);
	return -1;
}

static void drain_failed_jobs(struct darwin_file_pool *pool);

static void pool_fini(struct darwin_file_pool *pool)
{
	atomic_store_explicit(&pool->running, 0, memory_order_release);

	/* Wake any workers blocked on job_cond (empty queue) or
	 * pipe_space_cond (pipe full). */
	pthread_mutex_lock(&pool->job_mutex);
	pthread_cond_broadcast(&pool->job_cond);
	pthread_cond_broadcast(&pool->pipe_space_cond);
	pthread_mutex_unlock(&pool->job_mutex);

	for (int i = 0; i < pool->num_workers; i++)
		pthread_join(pool->workers[i], NULL);

	/* Drain any jobs that workers completed but whose pointer is
	 * still in the pipe. */
	struct dp_job *job;
	while (read(pool->completion_pipe[0], &job, sizeof(job)) ==
	       sizeof(job)) {
		if (job->op == DP_OP_PREAD)
			handle_backend_pread(job);
		else
			handle_backend_pwrite(job);
		free(job);
	}

	/* Drain any jobs the workers routed to the failed list
	 * (fatal pipe-write error). */
	drain_failed_jobs(pool);

	close(pool->completion_pipe[0]);
	close(pool->completion_pipe[1]);
	pthread_cond_destroy(&pool->pipe_space_cond);
	pthread_cond_destroy(&pool->job_cond);
	pthread_mutex_destroy(&pool->job_mutex);
}

int io_backend_init(struct ring_context *rc)
{
	if (kq_setup(rc, "io_backend_init") != 0)
		return -1;
	if (pool_init(&g_pool, rc) != 0) {
		kq_teardown(rc);
		return -1;
	}
	return 0;
}

void io_backend_fini(struct ring_context *rc)
{
	pool_fini(&g_pool);
	kq_teardown(rc);
}

/* ------------------------------------------------------------------ */
/* Event loop                                                         */
/* ------------------------------------------------------------------ */

/*
 * Drain any jobs a worker queued to failed_head after a fatal
 * pipe-write error.  Completion runs here (on the main thread) where
 * RCU registration and conn_mutex are safe to touch.
 */
static void drain_failed_jobs(struct darwin_file_pool *pool)
{
	struct dp_job *failed;

	pthread_mutex_lock(&pool->job_mutex);
	failed = pool->failed_head;
	pool->failed_head = NULL;
	pool->failed_tail = NULL;
	pthread_mutex_unlock(&pool->job_mutex);

	while (failed) {
		struct dp_job *next = failed->next;
		failed->next = NULL;
		if (failed->op == DP_OP_PREAD)
			handle_backend_pread(failed);
		else
			handle_backend_pwrite(failed);
		free(failed);
		failed = next;
	}
}

void io_backend_main_loop(volatile sig_atomic_t *running_flag,
			  struct ring_context *rc)
{
	struct kevent events[KQUEUE_BATCH_SIZE];

	TRACE("io_backend_main_loop: started (kqueue fd=%d)", rc->rc_kq_fd);

	while (1) {
		struct timespec ts = { .tv_sec = IO_URING_WAIT_SEC,
				       .tv_nsec = IO_URING_WAIT_NSEC };
		sig_atomic_t running_local;

		__atomic_load(running_flag, &running_local, __ATOMIC_SEQ_CST);
		if (!running_local)
			break;

		drain_failed_jobs(&g_pool);

		int n = kevent(rc->rc_kq_fd, NULL, 0, events, KQUEUE_BATCH_SIZE,
			       &ts);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			LOG("io_backend_main_loop: kevent: %s",
			    strerror(errno));
			usleep(10000);
			continue;
		}

		for (int i = 0; i < n; i++) {
			struct kevent *ke = &events[i];

			if (ke->filter != EVFILT_READ)
				continue;

			if ((int)ke->ident == rc->rc_shutdown_pipe[0]) {
				char drain[64];
				while (read(rc->rc_shutdown_pipe[0], drain,
					    sizeof(drain)) > 0)
					;
				continue;
			}

			if ((int)ke->ident == g_pool.completion_pipe[0]) {
				struct dp_job *job;
				bool any_drained = false;
				while (read(g_pool.completion_pipe[0], &job,
					    sizeof(job)) == sizeof(job)) {
					TSAN_ACQUIRE(job);
					TSAN_ACQUIRE(job->ic);

					switch (job->op) {
					case DP_OP_PREAD:
						handle_backend_pread(job);
						break;
					case DP_OP_PWRITE:
						handle_backend_pwrite(job);
						break;
					}
					free(job);
					any_drained = true;
				}
				if (any_drained) {
					pthread_mutex_lock(&g_pool.job_mutex);
					pthread_cond_broadcast(
						&g_pool.pipe_space_cond);
					pthread_mutex_unlock(&g_pool.job_mutex);
				}
				continue;
			}

			LOG("io_backend_main_loop: unexpected EVFILT_READ ident=%lu",
			    (unsigned long)ke->ident);
		}
	}

	TRACE("io_backend_main_loop: exiting");
}

/* ------------------------------------------------------------------ */
/* Submission entry points                                            */
/* ------------------------------------------------------------------ */

static int submit_file_op(enum dp_op_kind op, int fd, void *buf, size_t len,
			  off_t offset, struct rpc_trans *rt)
{
	struct io_context *ic =
		io_context_create(op == DP_OP_PREAD ? OP_TYPE_BACKEND_PREAD :
						      OP_TYPE_BACKEND_PWRITE,
				  fd, NULL, 0);
	if (!ic)
		return -ENOMEM;
	/*
	 * Take a refcount on rt that the completion handler's
	 * rpc_protocol_free(rt) will drop.  Linux (backend.c:278,311) and
	 * FreeBSD (backend_kqueue.c:298,336) do the same.  Without this
	 * the completer drops a ref the backend never acquired, freeing
	 * rt while the resumed task is still using it -- the root cause
	 * of issue #44 (SIGSEGV in rpc_protocol_op_call at rpc.c:667).
	 */
	ic->ic_rt = rpc_trans_get(rt);
	ic->ic_expected_len = len;

	struct dp_job *job = calloc(1, sizeof(*job));
	if (!job) {
		rpc_protocol_free(ic->ic_rt);
		io_context_destroy(ic);
		return -ENOMEM;
	}
	job->ic = ic;
	job->op = op;
	job->fd = fd;
	job->buf = buf;
	job->len = len;
	job->offset = offset;

	TSAN_RELEASE(ic);
	TSAN_RELEASE(job);

	pool_enqueue(&g_pool, job);
	return 0;
}

int io_request_backend_pread(int fd, void *buf, size_t len, off_t offset,
			     struct rpc_trans *rt, struct ring_context *rc)
{
	(void)rc; /* Darwin thread-pool uses g_pool; rc unused here. */
	return submit_file_op(DP_OP_PREAD, fd, buf, len, offset, rt);
}

int io_request_backend_pwrite(int fd, const void *buf, size_t len, off_t offset,
			      struct rpc_trans *rt, struct ring_context *rc)
{
	(void)rc;
	return submit_file_op(DP_OP_PWRITE, fd, (void *)buf, len, offset, rt);
}
