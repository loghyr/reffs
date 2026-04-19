/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * FreeBSD aio + kqueue I/O backend.
 *
 * Parallel to lib/io/backend.c + handler.c + read.c + write.c + accept.c +
 * connect.c + heartbeat.c which together implement the Linux io_uring
 * backend.  Exactly one backend is compiled per target; see configure.ac
 * and lib/io/Makefile.am for the HAVE_FREEBSD_KQUEUE gate.
 *
 * Completion model
 * ----------------
 *   - Every aio_* submission embeds a sigevent of type SIGEV_KEVENT
 *     targeting this backend's kqueue fd.  Completion drops an
 *     EVFILT_AIO event with udata pointing back at our aio_op.
 *   - accept(2) and connect(2) are non-blocking; completion is an
 *     EVFILT_READ / EVFILT_WRITE readiness notification from kqueue,
 *     at which point the backend calls accept(4)/connects-result and
 *     dispatches to io_handle_{accept,connect}.
 *   - poll/timeout/cancel use EVFILT_USER, EVFILT_TIMER, and
 *     aio_cancel+EV_DELETE respectively (see stubs below).
 *
 * This commit implements the file-I/O subset (io_backend_init/fini,
 * io_request_backend_pread/pwrite, io_backend_main_loop).  Network
 * ops are stubbed; subsequent commits fill them in.
 *
 * Not compiled by Makefile.am yet; awaiting commit 7 (compile-time
 * backend selection).  Also depends on a libtirpc substitute for
 * FreeBSD (RPC is in base libc), which is a separate PR.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef HAVE_FREEBSD_KQUEUE
#error "backend_kqueue.c requires HAVE_FREEBSD_KQUEUE"
#endif

#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
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
#include "tsan_io.h"

/* ------------------------------------------------------------------ */
/* struct ring_context -- kqueue variant                              */
/* ------------------------------------------------------------------ */

/*
 * On Linux, struct ring_context wraps a struct io_uring.  On FreeBSD,
 * it wraps a kqueue fd.  Callers outside lib/io/ treat it as opaque
 * (ring.h forward-declares it), so the two backends can have entirely
 * different struct layouts.
 *
 * rc_shutdown_user_ident is the ident for an EVFILT_USER event that
 * wakes the main loop from kevent() on shutdown.  Linux uses an
 * eventfd for the same purpose; FreeBSD has kqueue-native user events.
 */
struct ring_context {
	int rc_kq_fd;
	pthread_mutex_t rc_mutex;
	uintptr_t rc_shutdown_user_ident;
};

/*
 * Per-op bookkeeping.  aio_read/aio_write need a struct aiocb that
 * stays valid until completion; we pair it with a back-pointer to the
 * io_context so the completion handler can dispatch correctly.
 */
struct aio_op {
	struct aiocb ao_cb;
	struct io_context *ao_ic;
	struct ring_context *ao_rc; /* which backend this submitted to */
};

static struct ring_context *g_backend_rc;
static struct ring_context *g_network_rc;

void io_backend_set_global(struct ring_context *rc)
{
	g_backend_rc = rc;
}

struct ring_context *io_backend_get_global(void)
{
	return g_backend_rc;
}

struct ring_context *io_network_get_global(void)
{
	return g_network_rc;
}

/* ------------------------------------------------------------------ */
/* ring_context lifecycle                                             */
/* ------------------------------------------------------------------ */

struct ring_context *ring_context_alloc(void)
{
	struct ring_context *rc = calloc(1, sizeof(*rc));

	if (!rc)
		return NULL;
	rc->rc_kq_fd = -1;
	return rc;
}

void ring_context_free(struct ring_context *rc)
{
	free(rc);
}

/* ------------------------------------------------------------------ */
/* Backend ring setup / teardown                                      */
/* ------------------------------------------------------------------ */

int io_backend_init(struct ring_context *rc)
{
	if (pthread_mutex_init(&rc->rc_mutex, NULL) != 0) {
		LOG("io_backend_init: mutex init failed");
		return -1;
	}

	rc->rc_kq_fd = kqueue();
	if (rc->rc_kq_fd < 0) {
		LOG("io_backend_init: kqueue: %s", strerror(errno));
		pthread_mutex_destroy(&rc->rc_mutex);
		return -1;
	}

	/*
	 * Register an EVFILT_USER event with the ident set to this
	 * ring_context pointer; io_handler_signal_shutdown triggers it
	 * via kevent() to wake the main loop.
	 */
	rc->rc_shutdown_user_ident = (uintptr_t)rc;
	struct kevent ke;

	EV_SET(&ke, rc->rc_shutdown_user_ident, EVFILT_USER,
	       EV_ADD | EV_CLEAR, 0, 0, NULL);
	if (kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL) < 0) {
		LOG("io_backend_init: EVFILT_USER add: %s", strerror(errno));
		close(rc->rc_kq_fd);
		rc->rc_kq_fd = -1;
		pthread_mutex_destroy(&rc->rc_mutex);
		return -1;
	}

	TRACE("io_backend_init: kqueue ready (fd=%d)", rc->rc_kq_fd);
	return 0;
}

void io_backend_fini(struct ring_context *rc)
{
	if (rc->rc_kq_fd >= 0) {
		close(rc->rc_kq_fd);
		rc->rc_kq_fd = -1;
	}
	pthread_mutex_destroy(&rc->rc_mutex);
}

/* ------------------------------------------------------------------ */
/* Completion dispatch                                                 */
/* ------------------------------------------------------------------ */

static void handle_backend_pread(struct aio_op *op, ssize_t res)
{
	struct io_context *ic = op->ao_ic;
	struct rpc_trans *rt = ic->ic_rt;

	if (!rt) {
		LOG("handle_backend_pread: NULL rt in context id=%u",
		    ic->ic_id);
		io_context_destroy(ic);
		return;
	}

	if (res < 0)
		LOG("backend_pread: fd=%d expected=%zu error=%s", ic->ic_fd,
		    ic->ic_expected_len, strerror((int)-res));

	rt->rt_io_result = res;
	io_context_destroy(ic);

	if (rt->rt_task)
		task_resume(rt->rt_task);

	rpc_protocol_free(rt);
}

static void handle_backend_pwrite(struct aio_op *op, ssize_t res)
{
	struct io_context *ic = op->ao_ic;
	struct rpc_trans *rt = ic->ic_rt;

	if (!rt) {
		LOG("handle_backend_pwrite: NULL rt in context id=%u",
		    ic->ic_id);
		io_context_destroy(ic);
		return;
	}

	if (res < 0 || (size_t)res < ic->ic_expected_len)
		LOG("backend_pwrite: fd=%d expected=%zu got=%zd", ic->ic_fd,
		    ic->ic_expected_len, res);

	rt->rt_io_result = res;
	io_context_destroy(ic);

	if (rt->rt_task)
		task_resume(rt->rt_task);

	rpc_protocol_free(rt);
}

/* ------------------------------------------------------------------ */
/* Event loop                                                         */
/* ------------------------------------------------------------------ */

#define KQUEUE_BATCH_SIZE 64

void io_backend_main_loop(volatile sig_atomic_t *running_flag,
			  struct ring_context *rc)
{
	struct kevent events[KQUEUE_BATCH_SIZE];

	TRACE("io_backend_main_loop: started (kqueue fd=%d)", rc->rc_kq_fd);

	while (1) {
		struct timespec ts = { .tv_sec = IO_URING_WAIT_SEC,
				       .tv_nsec = IO_URING_WAIT_NSEC };

		int running_local;

		__atomic_load(running_flag, &running_local, __ATOMIC_SEQ_CST);
		if (!running_local)
			break;

		int n = kevent(rc->rc_kq_fd, NULL, 0, events,
			       KQUEUE_BATCH_SIZE, &ts);
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

			if (ke->filter == EVFILT_USER) {
				/* Shutdown wake-up; next loop iteration
				 * will observe running_flag and break. */
				continue;
			}
			if (ke->filter != EVFILT_AIO) {
				LOG("io_backend_main_loop: unexpected filter %d",
				    ke->filter);
				continue;
			}

			struct aio_op *op = (struct aio_op *)ke->udata;

			if (!op) {
				LOG("io_backend_main_loop: NULL aio_op");
				continue;
			}

			TSAN_ACQUIRE(op->ao_ic);

			ssize_t res = aio_return(&op->ao_cb);
			int aio_err = aio_error(&op->ao_cb);

			if (res < 0)
				res = -aio_err;

			switch (op->ao_ic->ic_op_type) {
			case OP_TYPE_BACKEND_PREAD:
				handle_backend_pread(op, res);
				break;
			case OP_TYPE_BACKEND_PWRITE:
				handle_backend_pwrite(op, res);
				break;
			default:
				LOG("io_backend_main_loop: unexpected op_type=%d",
				    op->ao_ic->ic_op_type);
				io_context_destroy(op->ao_ic);
				break;
			}

			free(op);
		}
	}

	TRACE("io_backend_main_loop: exiting");
}

/* ------------------------------------------------------------------ */
/* Submission helpers                                                 */
/* ------------------------------------------------------------------ */

/*
 * Common allocator + aiocb setup.  Caller fills in aio_nbytes,
 * aio_offset, aio_buf, and aio_lio_opcode (or calls aio_read/write
 * directly).  Sigevent is preconfigured to land on this kqueue.
 */
static struct aio_op *aio_op_alloc(struct ring_context *rc,
				   struct io_context *ic, int fd)
{
	struct aio_op *op = calloc(1, sizeof(*op));

	if (!op)
		return NULL;
	op->ao_ic = ic;
	op->ao_rc = rc;
	op->ao_cb.aio_fildes = fd;
	op->ao_cb.aio_sigevent.sigev_notify = SIGEV_KEVENT;
	op->ao_cb.aio_sigevent.sigev_notify_kqueue = rc->rc_kq_fd;
	op->ao_cb.aio_sigevent.sigev_value.sival_ptr = op;
	return op;
}

int io_request_backend_pread(int fd, void *buf, size_t len, off_t offset,
			     struct rpc_trans *rt, struct ring_context *rc)
{
	struct io_context *ic =
		io_context_create(OP_TYPE_BACKEND_PREAD, fd, NULL, 0);

	if (!ic)
		return -ENOMEM;

	ic->ic_rt = rpc_trans_get(rt);
	ic->ic_expected_len = len;

	struct aio_op *op = aio_op_alloc(rc, ic, fd);

	if (!op) {
		io_context_destroy(ic);
		return -ENOMEM;
	}

	op->ao_cb.aio_buf = buf;
	op->ao_cb.aio_nbytes = len;
	op->ao_cb.aio_offset = offset;

	TSAN_RELEASE(ic);

	if (aio_read(&op->ao_cb) < 0) {
		int saved_errno = errno;

		LOG("io_request_backend_pread: aio_read: %s",
		    strerror(saved_errno));
		free(op);
		io_context_destroy(ic);
		return -saved_errno;
	}

	return 0;
}

int io_request_backend_pwrite(int fd, const void *buf, size_t len,
			      off_t offset, struct rpc_trans *rt,
			      struct ring_context *rc)
{
	struct io_context *ic =
		io_context_create(OP_TYPE_BACKEND_PWRITE, fd, NULL, 0);

	if (!ic)
		return -ENOMEM;

	ic->ic_rt = rpc_trans_get(rt);
	ic->ic_expected_len = len;

	struct aio_op *op = aio_op_alloc(rc, ic, fd);

	if (!op) {
		io_context_destroy(ic);
		return -ENOMEM;
	}

	/* Cast away const -- aio_buf on FreeBSD is volatile void * */
	op->ao_cb.aio_buf = (void *)(uintptr_t)buf;
	op->ao_cb.aio_nbytes = len;
	op->ao_cb.aio_offset = offset;

	TSAN_RELEASE(ic);

	if (aio_write(&op->ao_cb) < 0) {
		int saved_errno = errno;

		LOG("io_request_backend_pwrite: aio_write: %s",
		    strerror(saved_errno));
		free(op);
		io_context_destroy(ic);
		return -saved_errno;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Network-side stubs (filled in by later commits)                    */
/* ------------------------------------------------------------------ */

int io_handler_init(struct ring_context *rc,
		    const char *tls_cert,
		    const char *tls_key,
		    void *extra_check)
{
	(void)tls_cert;
	(void)tls_key;
	(void)extra_check;
	g_network_rc = rc;
	LOG("io_handler_init: network-side kqueue backend not yet implemented");
	return -ENOSYS;
}

void io_handler_fini(struct ring_context *rc)
{
	(void)rc;
}

void io_handler_main_loop(volatile sig_atomic_t *running_flag,
			  struct ring_context *rc)
{
	(void)running_flag;
	(void)rc;
	LOG("io_handler_main_loop: not yet implemented on kqueue backend");
}

void io_handler_stop(void) {}

void io_handler_signal_shutdown(void)
{
	/*
	 * Trigger EVFILT_USER on both rings to wake their main loops.
	 * The handler ring is touched here; the backend ring is woken
	 * in io_backend_fini path via close() of the kqueue fd.
	 */
	struct ring_context *rc = g_backend_rc;

	if (rc && rc->rc_kq_fd >= 0) {
		struct kevent ke;

		EV_SET(&ke, rc->rc_shutdown_user_ident, EVFILT_USER, 0,
		       NOTE_TRIGGER, 0, NULL);
		(void)kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL);
	}
}

int io_request_accept_op(int fd, struct connection_info *ci,
			 struct ring_context *rc)
{
	(void)fd;
	(void)ci;
	(void)rc;
	return -ENOSYS;
}

int io_request_read_op(int fd, struct connection_info *ci,
		       struct ring_context *rc)
{
	(void)fd;
	(void)ci;
	(void)rc;
	return -ENOSYS;
}

int io_request_write_op(int fd, char *buf, int len, uint64_t state,
			struct connection_info *ci, struct ring_context *rc)
{
	(void)fd;
	(void)buf;
	(void)len;
	(void)state;
	(void)ci;
	(void)rc;
	return -ENOSYS;
}
