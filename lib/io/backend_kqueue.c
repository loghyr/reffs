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
 *   - timeout uses EVFILT_TIMER; cancel uses aio_cancel + EV_DELETE.
 *     Shutdown is an async-signal-safe write to a pipe fd whose
 *     read end is registered with EVFILT_READ.  kevent(2) is NOT
 *     async-signal-safe on FreeBSD, so the idiomatic EVFILT_USER
 *     NOTE_TRIGGER approach cannot be used from a signal handler.
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
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
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
#include "reffs/posix_shims.h"
#include "tsan_io.h"

/* ------------------------------------------------------------------ */
/* Per-op bookkeeping for aio                                          */
/* ------------------------------------------------------------------ */

/*
 * aio_read/aio_write need a struct aiocb that stays valid until
 * completion; we pair it with a back-pointer to the io_context so
 * the completion handler can dispatch correctly.
 *
 * struct ring_context, the kqueue substrate (kq_setup / kq_teardown /
 * globals), and io_handler_signal_shutdown live in lib/io/kqueue_common.{c,h}
 * and are shared with the Darwin thread-pool backend.
 */
struct aio_op {
	struct aiocb ao_cb;
	struct io_context *ao_ic;
	struct ring_context *ao_rc; /* which backend this submitted to */
};

/* ------------------------------------------------------------------ */
/* Backend ring setup / teardown                                      */
/* ------------------------------------------------------------------ */

int io_backend_init(struct ring_context *rc)
{
	return kq_setup(rc, "io_backend_init");
}

void io_backend_fini(struct ring_context *rc)
{
	kq_teardown(rc);
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

/* KQUEUE_BATCH_SIZE is defined in kqueue_common.h, shared with
 * kqueue_socket.c's io_handler_main_loop. */

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

			if (ke->filter == EVFILT_READ &&
			    (int)ke->ident == rc->rc_shutdown_pipe[0]) {
				char drain[64];
				while (read(rc->rc_shutdown_pipe[0], drain,
					    sizeof(drain)) > 0) { /* drain */
				}
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

			/*
			 * Check aio_error first.  aio_read/aio_write can
			 * fire EVFILT_AIO spuriously on some FreeBSD
			 * versions while still returning EINPROGRESS; if
			 * we call aio_return() in that state it returns
			 * a bogus value and we'd free the op prematurely.
			 * Skip this completion and wait for the real one.
			 */
			int aio_err = aio_error(&op->ao_cb);

			if (aio_err == EINPROGRESS)
				continue;

			ssize_t res = aio_return(&op->ao_cb);

			/* On error, aio_return returns -1 and we want the
			 * errno-coded value the caller expects.  On success
			 * aio_err == 0 and we keep res (byte count). */
			if (aio_err != 0)
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

int io_request_backend_pwrite(int fd, const void *buf, size_t len, off_t offset,
			      struct rpc_trans *rt, struct ring_context *rc)
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
