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
 * rc_shutdown_pipe is the async-signal-safe wake-up channel: the
 * signal handler writes one byte to rc_shutdown_pipe[1] (write(2)
 * is in the POSIX async-signal-safe list); the main loop watches
 * rc_shutdown_pipe[0] via EVFILT_READ and drains on wake-up.  This
 * replaces EVFILT_USER + NOTE_TRIGGER (the kqueue-native approach)
 * because kevent(2) is NOT async-signal-safe on FreeBSD -- calling
 * it from a signal handler is undefined behavior.
 */
struct ring_context {
	int rc_kq_fd;
	pthread_mutex_t rc_mutex;
	int rc_shutdown_pipe[2]; /* [0] read end, [1] write end */
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
	rc->rc_shutdown_pipe[0] = -1;
	rc->rc_shutdown_pipe[1] = -1;
	return rc;
}

void ring_context_free(struct ring_context *rc)
{
	free(rc);
}

/* ------------------------------------------------------------------ */
/* Shared kqueue setup: allocate kqueue fd + shutdown pipe, register   */
/* the pipe read end with EVFILT_READ so the main loop wakes when the  */
/* signal handler writes one byte.                                     */
/* ------------------------------------------------------------------ */

static int kq_setup(struct ring_context *rc, const char *tag)
{
	if (pthread_mutex_init(&rc->rc_mutex, NULL) != 0) {
		LOG("%s: mutex init failed", tag);
		return -1;
	}

	rc->rc_kq_fd = kqueue();
	if (rc->rc_kq_fd < 0) {
		LOG("%s: kqueue: %s", tag, strerror(errno));
		goto err_mutex;
	}

	/* Pipe with O_NONBLOCK + O_CLOEXEC on both ends. */
	if (pipe2(rc->rc_shutdown_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
		LOG("%s: pipe2: %s", tag, strerror(errno));
		goto err_kq;
	}

	struct kevent ke;

	EV_SET(&ke, rc->rc_shutdown_pipe[0], EVFILT_READ,
	       EV_ADD | EV_CLEAR, 0, 0, NULL);
	if (kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL) < 0) {
		LOG("%s: EVFILT_READ add (shutdown pipe): %s", tag,
		    strerror(errno));
		goto err_pipe;
	}

	TRACE("%s: kqueue=%d shutdown_pipe=(%d,%d)", tag, rc->rc_kq_fd,
	      rc->rc_shutdown_pipe[0], rc->rc_shutdown_pipe[1]);
	return 0;

err_pipe:
	close(rc->rc_shutdown_pipe[0]);
	close(rc->rc_shutdown_pipe[1]);
	rc->rc_shutdown_pipe[0] = rc->rc_shutdown_pipe[1] = -1;
err_kq:
	close(rc->rc_kq_fd);
	rc->rc_kq_fd = -1;
err_mutex:
	pthread_mutex_destroy(&rc->rc_mutex);
	return -1;
}

static void kq_teardown(struct ring_context *rc)
{
	if (rc->rc_shutdown_pipe[0] >= 0) {
		close(rc->rc_shutdown_pipe[0]);
		rc->rc_shutdown_pipe[0] = -1;
	}
	if (rc->rc_shutdown_pipe[1] >= 0) {
		close(rc->rc_shutdown_pipe[1]);
		rc->rc_shutdown_pipe[1] = -1;
	}
	if (rc->rc_kq_fd >= 0) {
		close(rc->rc_kq_fd);
		rc->rc_kq_fd = -1;
	}
	pthread_mutex_destroy(&rc->rc_mutex);
}

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

#define KQUEUE_BATCH_SIZE 64

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

			if (ke->filter == EVFILT_READ && (int)ke->ident == rc->rc_shutdown_pipe[0]) {
				char drain[64];
				while (read(rc->rc_shutdown_pipe[0], drain, sizeof(drain)) > 0) { /* drain */ }
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
/* Network-side: handler ring (init/fini/main_loop)                    */
/* ------------------------------------------------------------------ */

/*
 * The handler ring and the backend ring share the same ring_context
 * layout.  io_handler_init wires up the shutdown user-event filter
 * the same way io_backend_init does (see above); the only difference
 * is that the handler ring also stores the ring_context as
 * g_network_rc so outside code can find it.
 *
 * TLS args are accepted but not yet used by this backend -- TLS
 * plumbing mirrors the liburing path and will be wired in a
 * subsequent commit together with the rest of lib/io/tls.c.
 */
int io_handler_init(struct ring_context *rc,
		    const char *tls_cert,
		    const char *tls_key,
		    const char *tls_ca)
{
	(void)tls_cert;
	(void)tls_key;
	(void)tls_ca;

	int ret = kq_setup(rc, "io_handler_init");

	if (ret == 0)
		g_network_rc = rc;
	return ret;
}

void io_handler_fini(struct ring_context *rc)
{
	kq_teardown(rc);
	io_net_state_fini();
}

void io_handler_stop(void) {}

/*
 * Async-signal-safe shutdown wakeup.  write(2) is on the POSIX list
 * of async-signal-safe functions; kevent(2) is not.  Write a single
 * byte to each ring's shutdown pipe -- the main loop's EVFILT_READ
 * on the pipe fires, the loop observes the updated running flag,
 * and breaks out.  EAGAIN on a full pipe buffer is OK: the first
 * byte already pending delivers the wake-up.
 */
void io_handler_signal_shutdown(void)
{
	struct ring_context *rings[] = { g_network_rc, g_backend_rc };
	static const char wake = 'x';

	for (unsigned i = 0; i < sizeof(rings) / sizeof(rings[0]); i++) {
		struct ring_context *rc = rings[i];

		if (!rc || rc->rc_shutdown_pipe[1] < 0)
			continue;
		(void)write(rc->rc_shutdown_pipe[1], &wake, 1);
	}
}

/* ------------------------------------------------------------------ */
/* accept(2) via EVFILT_READ                                          */
/* ------------------------------------------------------------------ */

/*
 * On kqueue, accept is an EVFILT_READ readiness notification on the
 * listen fd.  When the event fires, the main loop calls accept4(2)
 * synchronously and dispatches to io_handle_accept.  We use
 * EV_ONESHOT so the filter auto-unregisters after firing; the next
 * io_request_accept_op re-arms it.
 */
int io_request_accept_op(int fd, struct connection_info *ci,
			 struct ring_context *rc)
{
	struct io_context *ic =
		io_context_create(OP_TYPE_ACCEPT, fd, NULL, 0);

	if (!ic)
		return -ENOMEM;

	ic->ic_ci = *ci;

	struct kevent ke;

	EV_SET(&ke, fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, ic);

	TSAN_RELEASE(ic);

	pthread_mutex_lock(&rc->rc_mutex);
	int ret = kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL);
	pthread_mutex_unlock(&rc->rc_mutex);

	if (ret < 0) {
		int saved_errno = errno;

		LOG("io_request_accept_op: kevent: %s", strerror(saved_errno));
		io_context_destroy(ic);
		return -saved_errno;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* connect(2) via nonblocking connect + EVFILT_WRITE                  */
/* ------------------------------------------------------------------ */

/*
 * connect(2) is issued synchronously in nonblocking mode.  If it
 * returns 0 immediately, we dispatch io_handle_connect straight
 * away.  If it returns EINPROGRESS, we register EVFILT_WRITE on the
 * socket; completion is signalled when it becomes writable, at
 * which point SO_ERROR tells us whether the connect succeeded.
 *
 * This helper is called from the same internal code path that
 * io_uring's connect.c uses; it is not a public io_request_*
 * function.  The static name keeps it file-scoped.
 */
static int kqueue_request_connect(struct ring_context *rc,
				  struct io_context *ic, int sockfd,
				  const struct sockaddr *addr,
				  socklen_t addrlen) __attribute__((unused));
static int kqueue_request_connect(struct ring_context *rc,
				  struct io_context *ic, int sockfd,
				  const struct sockaddr *addr,
				  socklen_t addrlen)
{
	/* Enforce nonblocking so connect() either succeeds or returns
	 * EINPROGRESS.  Many callers set this already; be defensive. */
	int flags = fcntl(sockfd, F_GETFL, 0);

	if (flags >= 0 && !(flags & O_NONBLOCK))
		(void)fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

	int ret = connect(sockfd, addr, addrlen);

	if (ret == 0) {
		/* Connected immediately -- dispatch completion inline. */
		TSAN_RELEASE(ic);
		io_handle_connect(ic, 0, rc);
		return 0;
	}
	if (errno != EINPROGRESS) {
		int saved_errno = errno;

		LOG("kqueue_request_connect: connect: %s",
		    strerror(saved_errno));
		return -saved_errno;
	}

	/* Pending: wait on writability. */
	struct kevent ke;

	EV_SET(&ke, sockfd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, ic);

	TSAN_RELEASE(ic);

	pthread_mutex_lock(&rc->rc_mutex);
	ret = kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL);
	pthread_mutex_unlock(&rc->rc_mutex);

	if (ret < 0) {
		int saved_errno = errno;

		LOG("kqueue_request_connect: kevent EVFILT_WRITE: %s",
		    strerror(saved_errno));
		return -saved_errno;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Network main loop                                                  */
/* ------------------------------------------------------------------ */

/*
 * Drain kqueue events for the network ring.  Dispatch is by
 * ic->ic_op_type, mirroring handler.c's CQE dispatch.
 *
 * EVFILT_READ on a listen socket -> accept_and_dispatch
 * EVFILT_READ on a connected socket -> read_and_dispatch
 * EVFILT_WRITE on a connecting socket -> connect_and_dispatch
 * EVFILT_WRITE on a connected socket -> write_and_dispatch
 * EVFILT_USER -> shutdown wakeup (handled in main loop)
 *
 * Read/write are filled in by a later commit; stubbed here.
 */
static void accept_and_dispatch(struct io_context *ic,
				struct ring_context *rc)
{
	int client_fd = accept4(ic->ic_fd, NULL, NULL,
				SOCK_NONBLOCK | SOCK_CLOEXEC);
	int err = (client_fd < 0) ? -errno : 0;

	if (err)
		LOG("accept4 fd=%d: %s", ic->ic_fd, strerror(-err));

	/*
	 * io_handle_accept expects the io_uring cqe->res convention:
	 * a nonnegative fd on success, a negative errno on failure.
	 * accept4 returns -1 on failure; translate to -errno so the
	 * shared handler reports the real error.
	 */
	io_handle_accept(ic, err ? err : client_fd, rc);
}

static void connect_and_dispatch(struct io_context *ic,
				 struct ring_context *rc)
{
	int so_err = 0;
	socklen_t so_errlen = sizeof(so_err);

	if (getsockopt(ic->ic_fd, SOL_SOCKET, SO_ERROR, &so_err,
		       &so_errlen) < 0)
		so_err = errno;

	io_handle_connect(ic, -so_err, rc);
}

/* Forward declarations; definitions follow the main loop. */
static void read_and_dispatch(struct io_context *ic, struct ring_context *rc);
static void write_and_dispatch(struct io_context *ic, struct ring_context *rc);
static void heartbeat_and_dispatch(struct io_context *ic,
				   struct ring_context *rc);

void io_handler_main_loop(volatile sig_atomic_t *running_flag,
			  struct ring_context *rc)
{
	struct kevent events[KQUEUE_BATCH_SIZE];

	TRACE("io_handler_main_loop: started (kqueue fd=%d)", rc->rc_kq_fd);

	while (1) {
		struct timespec ts = { .tv_sec = IO_URING_WAIT_SEC,
				       .tv_nsec = IO_URING_WAIT_NSEC };

		sig_atomic_t running_local;

		__atomic_load(running_flag, &running_local, __ATOMIC_SEQ_CST);
		if (!running_local)
			break;

		int n = kevent(rc->rc_kq_fd, NULL, 0, events,
			       KQUEUE_BATCH_SIZE, &ts);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			LOG("io_handler_main_loop: kevent: %s",
			    strerror(errno));
			usleep(10000);
			continue;
		}

		for (int i = 0; i < n; i++) {
			struct kevent *ke = &events[i];

			if (ke->filter == EVFILT_READ && (int)ke->ident == rc->rc_shutdown_pipe[0]) {
				char drain[64];
				while (read(rc->rc_shutdown_pipe[0], drain, sizeof(drain)) > 0) { /* drain */ }
				/* Shutdown wakeup; next iter picks up
				 * running_flag. */
				continue;
			}

			struct io_context *ic = (struct io_context *)ke->udata;

			if (!ic) {
				LOG("io_handler_main_loop: NULL io_context");
				continue;
			}

			TSAN_ACQUIRE(ic);

			switch (ic->ic_op_type) {
			case OP_TYPE_ACCEPT:
				accept_and_dispatch(ic, rc);
				break;
			case OP_TYPE_CONNECT:
				connect_and_dispatch(ic, rc);
				break;
			case OP_TYPE_READ:
				read_and_dispatch(ic, rc);
				break;
			case OP_TYPE_WRITE:
				write_and_dispatch(ic, rc);
				break;
			case OP_TYPE_HEARTBEAT:
				heartbeat_and_dispatch(ic, rc);
				break;
			default:
				LOG("io_handler_main_loop: unexpected op_type=%d",
				    ic->ic_op_type);
				io_context_destroy(ic);
				break;
			}
		}
	}

	TRACE("io_handler_main_loop: exiting");
}

/* ------------------------------------------------------------------ */
/* Network read/write via EVFILT_READ / EVFILT_WRITE                  */
/* ------------------------------------------------------------------ */

/*
 * Socket reads and writes are much cheaper to model as readiness
 * events than as aio: kqueue tells us "this fd has data / can
 * accept more", we call read(2) / write(2) synchronously.  This
 * mirrors the standard BSD event-loop pattern.
 *
 * The submission functions register EVFILT_READ or EVFILT_WRITE
 * with EV_ONESHOT so the filter auto-unregisters after firing;
 * each caller re-arms on the next op.  The io_context pointer is
 * carried in the udata so the main loop can dispatch back to the
 * right io_handle_* handler.
 */

int io_request_read_op(int fd, struct connection_info *ci,
		       struct ring_context *rc)
{
	struct io_context *ic =
		io_context_create(OP_TYPE_READ, fd, NULL, BUFFER_SIZE);

	if (!ic)
		return -ENOMEM;

	ic->ic_ci = *ci;
	io_context_update_time(ic);

	struct kevent ke;

	EV_SET(&ke, fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, ic);

	TSAN_RELEASE(ic);

	pthread_mutex_lock(&rc->rc_mutex);
	int ret = kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL);
	pthread_mutex_unlock(&rc->rc_mutex);

	if (ret < 0) {
		int saved_errno = errno;

		LOG("io_request_read_op: kevent: %s", strerror(saved_errno));
		io_context_destroy(ic);
		return -saved_errno;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Bookkeeping stubs                                                   */
/*                                                                     */
/* On Linux these functions live in handler.c/connect.c/write.c and    */
/* implement shared state (pending RPC request table, conn_info        */
/* registry, RPC reply writer).  The implementations are mostly        */
/* backend-agnostic but are currently compiled only when the liburing  */
/* backend is active; splitting them into a shared compilation unit    */
/* is a separate refactor.  For the FreeBSD build, provide stubs so    */
/* the binary links.  NFSv4 callback paths that call these will        */
/* return errors at runtime until the real implementations land.       */
/* ------------------------------------------------------------------ */

/*
 * Heartbeat accounting stubs.  On the FreeBSD kqueue backend the
 * actual timer ticks are delivered via EVFILT_TIMER (already wired),
 * so these counters are not yet consumed by anything.  Left as no-ops
 * until the port gets real read/write completion handlers.
 */
int io_heartbeat_init(struct ring_context *rc)
{
	(void)rc;
	return 0;
}

uint32_t io_heartbeat_period_get(void)
{
	return 0;
}

uint32_t io_heartbeat_period_set(uint32_t seconds)
{
	(void)seconds;
	return 0;
}

void io_heartbeat_update_completions(uint64_t count)
{
	(void)count;
}

int io_send_request(struct rpc_trans *rt)
{ (void)rt; return -ENOSYS; }
int io_schedule_heartbeat(struct ring_context *rc)
{ (void)rc; return 0; }

/*
 * Completion handlers for accept, connect, read, write, and TLS all
 * live in lib/io/handlers.c (shared).  io_handle_heartbeat below
 * is still a stub until PR #10 wires EVFILT_TIMER-driven heartbeat.
 */
int io_handle_heartbeat(struct io_context *ic, int result,
			struct ring_context *rc)
{
	(void)result;
	(void)rc;
	io_context_destroy(ic);
	return 0;
}

int io_request_write_op(int fd, char *buf, int len, uint64_t state,
			struct connection_info *ci, struct ring_context *rc)
{
	struct io_context *ic =
		io_context_create(OP_TYPE_WRITE, fd, buf, (size_t)len);

	if (!ic)
		return -ENOMEM;

	ic->ic_ci = *ci;
	ic->ic_state |= state;
	ic->ic_expected_len = (size_t)len;

	struct kevent ke;

	EV_SET(&ke, fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, ic);

	TSAN_RELEASE(ic);

	pthread_mutex_lock(&rc->rc_mutex);
	int ret = kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL);
	pthread_mutex_unlock(&rc->rc_mutex);

	if (ret < 0) {
		int saved_errno = errno;

		LOG("io_request_write_op: kevent: %s", strerror(saved_errno));
		io_context_destroy(ic);
		return -saved_errno;
	}

	return 0;
}

/*
 * io_resubmit_{write,read} -- stubs.  PR #7 commit 6 implements these
 * via kevent(EV_ADD|EV_ONESHOT, EVFILT_WRITE/EVFILT_READ) using the
 * existing ic as udata (no allocation).  Until then, callers hit
 * -ENOSYS; none are reachable on FreeBSD at this commit because
 * io_handle_read/write are still the log-and-drop stubs below.
 */
int io_resubmit_write(struct io_context *ic, struct ring_context *rc)
{
	(void)ic;
	(void)rc;
	return -ENOSYS;
}

int io_resubmit_read(struct io_context *ic, struct ring_context *rc)
{
	(void)ic;
	(void)rc;
	return -ENOSYS;
}

/* ------------------------------------------------------------------ */
/* Read/write completion handlers (called from main loop dispatch)    */
/* ------------------------------------------------------------------ */

static void read_and_dispatch(struct io_context *ic, struct ring_context *rc)
{
	ssize_t nread = read(ic->ic_fd, ic->ic_buffer, BUFFER_SIZE);
	int result = (nread < 0) ? -errno : (int)nread;

	io_handle_read(ic, result, rc);
}

static void write_and_dispatch(struct io_context *ic, struct ring_context *rc)
{
	ssize_t nwritten = write(ic->ic_fd, ic->ic_buffer,
				 ic->ic_expected_len);
	int result = (nwritten < 0) ? -errno : (int)nwritten;

	io_handle_write(ic, result, rc);
}

static void heartbeat_and_dispatch(struct io_context *ic,
				   struct ring_context *rc)
{
	io_handle_heartbeat(ic, 0, rc);
}

/*
 * Arm a one-shot timer on the network ring.  Called from the internal
 * heartbeat state machine (lib/io/heartbeat.c, currently still
 * liburing-specific) when it wires up to this backend.  The ident is
 * chosen by the caller; using the io_context pointer keeps it unique
 * without a separate registry.
 */
static int kqueue_arm_heartbeat_timer(struct ring_context *rc,
				      struct io_context *ic,
				      uint64_t timeout_ns) __attribute__((unused));
static int kqueue_arm_heartbeat_timer(struct ring_context *rc,
				      struct io_context *ic,
				      uint64_t timeout_ns)
{
	struct kevent ke;
	uintptr_t ident = (uintptr_t)ic;
	int64_t data_ms = (int64_t)(timeout_ns / 1000000ULL);

	EV_SET(&ke, ident, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, data_ms, ic);

	TSAN_RELEASE(ic);

	pthread_mutex_lock(&rc->rc_mutex);
	int ret = kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL);
	pthread_mutex_unlock(&rc->rc_mutex);

	if (ret < 0) {
		int saved_errno = errno;

		LOG("kqueue_arm_heartbeat_timer: kevent: %s",
		    strerror(saved_errno));
		return -saved_errno;
	}

	return 0;
}
