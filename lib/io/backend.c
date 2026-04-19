/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Backend file-I/O ring.
 *
 * A dedicated io_uring instance separate from the network ring handles all
 * asynchronous file reads and writes (pread/pwrite on data-block file
 * descriptors).  Keeping the two rings separate prevents file I/O latency
 * from starving network completions and vice versa.
 *
 * Protocol:
 *   1. The NFSv4 op handler sets rt->rt_next_action = <resume_cb>.
 *   2. It calls task_pause(rt->rt_task)  <-- point of no return.
 *   3. It calls io_request_backend_pread/pwrite() to submit the SQE.
 *   4. When the CQE fires here, io_handle_backend_p{read,write}() stores
 *      cqe->res in rt->rt_io_result and calls task_resume(rt->rt_task).
 *   5. A worker dequeues the task; dispatch_compound() calls rt_next_action
 *      which reads rt->rt_io_result to build the response.
 *
 * The buffer pointer is owned by the compound/caller throughout -- we do NOT
 * store it in ic_buffer and do NOT free it on context destruction.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <liburing.h>

#include "tsan_uring.h"
#include <liburing/io_uring.h>
#include <linux/time_types.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/ring.h"
#include "ring_internal.h"
#include "reffs/rpc.h"
#include "reffs/task.h"

/* ------------------------------------------------------------------ */
/* Global backend ring pointer (set once at startup)                  */
/* ------------------------------------------------------------------ */

static struct ring_context *g_backend_rc;

void io_backend_set_global(struct ring_context *rc)
{
	g_backend_rc = rc;
}

struct ring_context *io_backend_get_global(void)
{
	return g_backend_rc;
}

/* ------------------------------------------------------------------ */
/* Ring setup / teardown                                              */
/* ------------------------------------------------------------------ */

int io_backend_init(struct ring_context *rc)
{
	struct io_uring_params params = { 0 };

	if (pthread_mutex_init(&rc->rc_mutex, NULL) != 0) {
		LOG("io_backend_init: failed to initialize ring mutex");
		return -1;
	}

	params.flags = IORING_SETUP_CQSIZE;
	params.cq_entries = 4 * BACKEND_QUEUE_DEPTH;

	if (io_uring_queue_init_params(BACKEND_QUEUE_DEPTH, &rc->rc_ring,
				       &params) < 0) {
		LOG("io_backend_init: io_uring_queue_init_params: %s",
		    strerror(errno));
		pthread_mutex_destroy(&rc->rc_mutex);
		return -1;
	}

	TRACE("io_backend_init: backend ring ready (SQ=%u CQ=%u)",
	      params.sq_entries, params.cq_entries);
	return 0;
}

void io_backend_fini(struct ring_context *rc)
{
	io_uring_queue_exit(&rc->rc_ring);
	pthread_mutex_destroy(&rc->rc_mutex);
}

/* ------------------------------------------------------------------ */
/* Completion handlers                                                */
/* ------------------------------------------------------------------ */

static void io_handle_backend_pread(struct io_context *ic, int res)
{
	struct rpc_trans *rt = ic->ic_rt;

	if (!rt) {
		LOG("io_handle_backend_pread: NULL rt in context id=%u",
		    ic->ic_id);
		io_context_destroy(ic);
		return;
	}

	if (res < 0)
		LOG("backend_pread: fd=%d expected=%zu error=%s", ic->ic_fd,
		    ic->ic_expected_len, strerror(-res));

	rt->rt_io_result = (ssize_t)res;
	io_context_destroy(ic);

	if (rt->rt_task)
		task_resume(rt->rt_task);

	rpc_protocol_free(rt);
}

static void io_handle_backend_pwrite(struct io_context *ic, int res)
{
	struct rpc_trans *rt = ic->ic_rt;

	if (!rt) {
		LOG("io_handle_backend_pwrite: NULL rt in context id=%u",
		    ic->ic_id);
		io_context_destroy(ic);
		return;
	}

	if (res < 0 || (size_t)res < ic->ic_expected_len)
		LOG("backend_pwrite: fd=%d expected=%zu got=%d", ic->ic_fd,
		    ic->ic_expected_len, res);

	rt->rt_io_result = (ssize_t)res;
	io_context_destroy(ic);

	if (rt->rt_task)
		task_resume(rt->rt_task);

	rpc_protocol_free(rt);
}

/* ------------------------------------------------------------------ */
/* Event loop                                                         */
/* ------------------------------------------------------------------ */

void io_backend_main_loop(volatile sig_atomic_t *running_flag,
			  struct ring_context *rc)
{
	struct io_uring_cqe *cqe;

	TRACE("io_backend_main_loop: started");

	while (1) {
		struct __kernel_timespec ts = { .tv_sec = IO_URING_WAIT_SEC,
						.tv_nsec = IO_URING_WAIT_NSEC };

		int running_local;
		__atomic_load(running_flag, &running_local, __ATOMIC_SEQ_CST);
		if (!running_local)
			break;

		int ret = io_uring_wait_cqe_timeout(&rc->rc_ring, &cqe, &ts);

		if (ret == -ETIME || ret == -EINTR)
			continue;

		if (ret < 0) {
			LOG("io_backend_main_loop: wait error: %s",
			    strerror(-ret));
			usleep(10000);
			continue;
		}

		struct io_context *ic =
			(struct io_context *)(uintptr_t)cqe->user_data;

		if (!ic) {
			LOG("io_backend_main_loop: NULL io_context in CQE");
			io_uring_cqe_seen(&rc->rc_ring, cqe);
			continue;
		}

		TSAN_ACQUIRE(ic);

		if (cqe->res < 0) {
			LOG("io_backend_main_loop: op=%s fd=%d error: %s",
			    io_op_type_to_str(ic->ic_op_type), ic->ic_fd,
			    strerror(-cqe->res));
		}

		switch (ic->ic_op_type) {
		case OP_TYPE_BACKEND_PREAD:
			io_handle_backend_pread(ic, cqe->res);
			break;
		case OP_TYPE_BACKEND_PWRITE:
			io_handle_backend_pwrite(ic, cqe->res);
			break;
		default:
			LOG("io_backend_main_loop: unexpected op_type=%d",
			    ic->ic_op_type);
			io_context_destroy(ic);
			break;
		}

		io_uring_cqe_seen(&rc->rc_ring, cqe);
	}

	TRACE("io_backend_main_loop: exiting");
}

/* ------------------------------------------------------------------ */
/* Submission helpers                                                 */
/* ------------------------------------------------------------------ */

/*
 * submit_backend_op -- shared SQE acquisition and submission logic.
 *
 * The caller has already prepared the SQE fields; this function handles
 * the retry loop, user_data assignment, and submit under the ring mutex.
 */
static int submit_backend_op(struct io_context *ic, struct io_uring_sqe *sqe,
			     struct ring_context *rc)
{
	io_uring_sqe_set_data(sqe, ic);

	bool submitted = false;
	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		int ret = io_uring_submit(&rc->rc_ring);
		if (ret >= 0) {
			TSAN_RELEASE(ic);
			submitted = true;
			break;
		}
		if (ret == -EAGAIN) {
			pthread_mutex_unlock(&rc->rc_mutex);
			sched_yield();
			pthread_mutex_lock(&rc->rc_mutex);
			continue;
		}
		LOG("submit_backend_op: io_uring_submit: %s", strerror(-ret));
		break;
	}

	pthread_mutex_unlock(&rc->rc_mutex);

	if (!submitted) {
		io_context_destroy(ic);
		return -EBUSY;
	}

	return 0;
}

int io_request_backend_pread(int fd, void *buf, size_t len, off_t offset,
			     struct rpc_trans *rt, struct ring_context *rc)
{
	/*
	 * ic_buffer is NULL -- the buffer is owned by the compound and must
	 * not be freed when the context is destroyed.
	 */
	struct io_context *ic =
		io_context_create(OP_TYPE_BACKEND_PREAD, fd, NULL, 0);
	if (!ic)
		return -ENOMEM;

	ic->ic_rt = rpc_trans_get(rt);

	struct io_uring_sqe *sqe = NULL;
	for (int i = 0; i < REFFS_IO_RING_RETRIES; i++) {
		pthread_mutex_lock(&rc->rc_mutex);
		sqe = io_uring_get_sqe(&rc->rc_ring);
		if (sqe)
			break;
		pthread_mutex_unlock(&rc->rc_mutex);
		sched_yield();
	}

	if (!sqe) {
		LOG("io_request_backend_pread: ring full (fd=%d len=%zu off=%lld)",
		    fd, len, (long long)offset);
		io_context_destroy(ic);
		return -EBUSY;
	}

	ic->ic_expected_len = len;
	io_uring_prep_read(sqe, fd, buf, (unsigned)len, (uint64_t)offset);

	return submit_backend_op(ic, sqe, rc);
}

int io_request_backend_pwrite(int fd, const void *buf, size_t len, off_t offset,
			      struct rpc_trans *rt, struct ring_context *rc)
{
	struct io_context *ic =
		io_context_create(OP_TYPE_BACKEND_PWRITE, fd, NULL, 0);
	if (!ic)
		return -ENOMEM;

	ic->ic_rt = rpc_trans_get(rt);

	struct io_uring_sqe *sqe = NULL;
	for (int i = 0; i < REFFS_IO_RING_RETRIES; i++) {
		pthread_mutex_lock(&rc->rc_mutex);
		sqe = io_uring_get_sqe(&rc->rc_ring);
		if (sqe)
			break;
		pthread_mutex_unlock(&rc->rc_mutex);
		sched_yield();
	}

	if (!sqe) {
		LOG("io_request_backend_pwrite: ring full (fd=%d len=%zu off=%lld)",
		    fd, len, (long long)offset);
		io_context_destroy(ic);
		return -EBUSY;
	}

	ic->ic_expected_len = len;
	/* Cast away const -- io_uring_prep_write takes void *, not const void * */
	io_uring_prep_write(sqe, fd, (void *)buf, (unsigned)len,
			    (uint64_t)offset);

	return submit_backend_op(ic, sqe, rc);
}

/* ------------------------------------------------------------------ */
/* ring_context lifecycle                                             */
/* ------------------------------------------------------------------ */

/*
 * Allocator for the opaque ring_context.  Callers outside lib/io/
 * cannot stack-allocate because the struct is incomplete in the
 * public header (see lib/include/reffs/ring.h).  The caller follows
 * with io_handler_init() or io_backend_init() to populate the struct;
 * ring_context_free() expects the caller to have already torn down
 * the struct via the matching fini.
 */
struct ring_context *ring_context_alloc(void)
{
	return calloc(1, sizeof(struct ring_context));
}

void ring_context_free(struct ring_context *rc)
{
	free(rc);
}
