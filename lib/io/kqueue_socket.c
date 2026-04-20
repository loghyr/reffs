/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Socket-side kqueue I/O: io_handler_init/fini/main_loop,
 * accept/connect/read/write/heartbeat dispatch, io_send_request
 * outbound RPC.  Shared between the FreeBSD aio backend
 * (backend_kqueue.c handles file I/O) and the Darwin thread-pool
 * backend (backend_darwin.c handles file I/O).  Compiled under
 * IO_BACKEND_KQUEUE OR IO_BACKEND_DARWIN -- see lib/io/Makefile.am.
 *
 * The shared kqueue substrate (struct ring_context, kq_setup,
 * ring_context_alloc/free, io_handler_stop,
 * io_handler_signal_shutdown, the global rc pointers) lives in
 * lib/io/kqueue_common.{c,h}.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

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
#include "reffs/trace/io.h"
#include "kqueue_common.h"
#include "posix_shims.h"
#include "tsan_io.h"

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
int io_handler_init(struct ring_context *rc, const char *tls_cert,
		    const char *tls_key, const char *tls_ca)
{
	(void)tls_cert;
	(void)tls_key;
	(void)tls_ca;

	int ret = kq_setup(rc, "io_handler_init");

	if (ret == 0)
		io_network_set_global(rc);
	return ret;
}

void io_handler_fini(struct ring_context *rc)
{
	kq_teardown(rc);
	io_net_state_fini();
}

/* io_handler_stop and io_handler_signal_shutdown live in
 * lib/io/kqueue_common.c (shared with the Darwin backend). */

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
	struct io_context *ic = io_context_create(OP_TYPE_ACCEPT, fd, NULL, 0);

	if (!ic)
		return -ENOMEM;

	if (ci)
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
static void accept_and_dispatch(struct io_context *ic, struct ring_context *rc)
{
	/* reffs_accept4_nb_cloexec uses accept4 on Linux/FreeBSD,
	 * accept+fcntl on Darwin. */
	int client_fd = reffs_accept4_nb_cloexec(ic->ic_fd, NULL, NULL);
	int err = (client_fd < 0) ? -errno : 0;

	if (err)
		LOG("accept fd=%d: %s", ic->ic_fd, strerror(-err));

	/*
	 * io_handle_accept expects the io_uring cqe->res convention:
	 * a nonnegative fd on success, a negative errno on failure.
	 * accept4 returns -1 on failure; translate to -errno so the
	 * shared handler reports the real error.
	 */
	io_handle_accept(ic, err ? err : client_fd, rc);
}

static void connect_and_dispatch(struct io_context *ic, struct ring_context *rc)
{
	int so_err = 0;
	socklen_t so_errlen = sizeof(so_err);

	if (getsockopt(ic->ic_fd, SOL_SOCKET, SO_ERROR, &so_err, &so_errlen) <
	    0)
		so_err = errno;

	io_handle_connect(ic, -so_err, rc);
}

/* Forward declarations; definitions follow the main loop. */
static void read_and_dispatch(struct io_context *ic, const struct kevent *ke,
			      struct ring_context *rc);
static void write_and_dispatch(struct io_context *ic, const struct kevent *ke,
			       struct ring_context *rc);
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

		int n = kevent(rc->rc_kq_fd, NULL, 0, events, KQUEUE_BATCH_SIZE,
			       &ts);
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

			if (ke->filter == EVFILT_READ &&
			    (int)ke->ident == rc->rc_shutdown_pipe[0]) {
				char drain[64];
				while (read(rc->rc_shutdown_pipe[0], drain,
					    sizeof(drain)) > 0) { /* drain */
				}
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

			io_heartbeat_update_completions(1);

			switch (ic->ic_op_type) {
			case OP_TYPE_ACCEPT:
				accept_and_dispatch(ic, rc);
				break;
			case OP_TYPE_CONNECT:
				connect_and_dispatch(ic, rc);
				break;
			case OP_TYPE_READ:
				read_and_dispatch(ic, ke, rc);
				break;
			case OP_TYPE_WRITE:
				write_and_dispatch(ic, ke, rc);
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
	if (fd <= 0)
		return -EBADF;

	/*
	 * Allocate a BUFFER_SIZE read buffer up front and hand it to
	 * io_context_create.  read_and_dispatch calls read(2) into this
	 * buffer when EVFILT_READ fires; without a real buffer, read(2)
	 * would EFAULT on every connection's first read.  On kevent
	 * failure io_context_destroy frees the buffer (unconditional
	 * free(ic->ic_buffer)).
	 */
	char *buffer = malloc(BUFFER_SIZE);
	if (!buffer)
		return -ENOMEM;

	struct io_context *ic =
		io_context_create(OP_TYPE_READ, fd, buffer, BUFFER_SIZE);

	if (!ic) {
		free(buffer);
		return -ENOMEM;
	}

	if (ci)
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
 * Heartbeat: EVFILT_TIMER ticks drive a periodic watchdog that
 * re-arms stuck listeners and sweeps idle connections.  Mirrors the
 * liburing heartbeat.c but uses kqueue primitives (kqueue_arm_heartbeat_timer
 * below) and skips the io_uring-specific CQ-overflow check.
 */

/* Forward declaration -- definition follows the main loop. */
static int kqueue_arm_heartbeat_timer(struct ring_context *rc,
				      struct io_context *ic,
				      uint64_t timeout_ns);

#define KQUEUE_HEARTBEAT_INTERVAL_SEC 1
#define KQUEUE_LISTENER_CHECK_INTERVAL 5
#define KQUEUE_CONN_CHECK_INTERVAL 10
#define KQUEUE_CONN_TIMEOUT 60

/*
 * kqueue_heartbeat_period is read on the main loop thread
 * (io_schedule_heartbeat below) and written by the probe1 RPC worker
 * thread via io_heartbeat_period_set.  _Atomic with RELAXED ordering:
 * the value is advisory (next timer arm picks it up), no
 * happens-before needed.
 */
static _Atomic uint32_t kqueue_heartbeat_period = KQUEUE_HEARTBEAT_INTERVAL_SEC;
static uint64_t kqueue_heartbeat_completions = 0;
static time_t kqueue_last_listener_check = 0;
static time_t kqueue_last_conn_check = 0;

int io_heartbeat_init(struct ring_context *rc)
{
	time_t now = time(NULL);

	kqueue_last_listener_check = now;
	kqueue_last_conn_check = now;
	return io_schedule_heartbeat(rc);
}

uint32_t io_heartbeat_period_get(void)
{
	return atomic_load_explicit(&kqueue_heartbeat_period,
				    memory_order_relaxed);
}

uint32_t io_heartbeat_period_set(uint32_t seconds)
{
	return atomic_exchange_explicit(&kqueue_heartbeat_period, seconds,
					memory_order_relaxed);
}

void io_heartbeat_update_completions(uint64_t count)
{
	kqueue_heartbeat_completions += count;
}

/*
 * io_send_request -- outbound RPC: establish a TCP connection if
 * needed, then dispatch the send via io_rpc_trans_cb.
 *
 * Mirrors the liburing implementation in lib/io/connect.c
 * (io_send_request), using kqueue_request_connect instead of the
 * io_uring_prep_connect path.  Two reachable cases:
 *
 *   1. rt->rt_fd <= 0 (fresh client): socket(AF_INET, SOCK_STREAM),
 *      conn_info register CONNECTING, nonblocking, kqueue_request_connect.
 *      On EINPROGRESS, EVFILT_WRITE fires when connected and the main
 *      loop dispatches io_handle_connect which calls io_rpc_trans_cb.
 *
 *   2. rt->rt_fd > 0 (existing connection): verify it's in a sendable
 *      state, then dispatch io_rpc_trans_cb directly.
 *
 * Callers: NFSv4 backchannel (CB_RECALL, CB_GETATTR, CB_LAYOUTRECALL)
 * from lib/nfs4/server/cb.c; probe1_client outbound.
 */
int io_send_request(struct rpc_trans *rt)
{
	int ret;

	TRACE("fd=%d xid=0x%08x", rt->rt_fd, rt->rt_info.ri_xid);

	ret = io_register_request(rt);
	if (ret)
		return ret;

	if (rt->rt_fd <= 0) {
		struct sockaddr_in *addr = malloc(sizeof(*addr));
		if (!addr) {
			io_unregister_request(rt->rt_info.ri_xid);
			return ENOMEM;
		}

		int sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd < 0) {
			int err = errno;
			LOG("Failed to create socket: %s", strerror(err));
			free(addr);
			io_unregister_request(rt->rt_info.ri_xid);
			return err;
		}

		struct conn_info *ci = io_conn_register(sockfd, CONN_CONNECTING,
							CONN_ROLE_CLIENT);
		if (!ci) {
			LOG("Failed to register connection");
			io_socket_close(sockfd, ENOMEM);
			free(addr);
			io_unregister_request(rt->rt_info.ri_xid);
			return ENOMEM;
		}

		int flags = fcntl(sockfd, F_GETFL, 0);
		if (flags >= 0)
			(void)fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

		memset(addr, 0, sizeof(*addr));
		addr->sin_family = AF_INET;
		addr->sin_port = htons(rt->rt_port);
		if (inet_pton(AF_INET, rt->rt_addr_str, &addr->sin_addr) <= 0) {
			LOG("Invalid address: %s", rt->rt_addr_str);
			io_socket_close(sockfd, EINVAL);
			free(addr);
			io_unregister_request(rt->rt_info.ri_xid);
			return EINVAL;
		}

		rt->rt_fd = sockfd;

		struct io_context *ic = io_context_create(
			OP_TYPE_CONNECT, sockfd, addr, sizeof(*addr));
		if (!ic) {
			free(addr);
			io_socket_close(sockfd, ENOMEM);
			io_unregister_request(rt->rt_info.ri_xid);
			return ENOMEM;
		}
		/*
		 * ic owns the addr buffer from here on.
		 * io_context_destroy will free ic->ic_buffer; do NOT
		 * free(addr) on any post-ic-create error path or it is
		 * a double-free.
		 */

		ic->ic_xid = rt->rt_info.ri_xid;

		ci = io_conn_get(sockfd);
		if (ci)
			ci->ci_xid = rt->rt_info.ri_xid;

		trace_io_connect_submit(ic);

		/*
		 * kqueue_request_connect owns ic from here -- either
		 * dispatches io_handle_connect inline (on immediate success)
		 * or registers EVFILT_WRITE for the pending case.  On
		 * failure it returns -errno and the caller cleans up.
		 */
		ret = kqueue_request_connect(rt->rt_rc, ic, sockfd,
					     (struct sockaddr *)addr,
					     sizeof(*addr));
		if (ret < 0) {
			io_socket_close(sockfd, -ret);
			io_context_destroy(ic); /* frees addr via ic_buffer */
			io_unregister_request(rt->rt_info.ri_xid);
			return -ret;
		}

		/*
		 * addr is owned by ic (via io_context_create); not
		 * freeing here.  On connect completion io_handle_connect
		 * destroys ic, which frees ic->ic_buffer (== addr).
		 */
		return 0;
	}

	struct conn_info *ci = io_conn_get(rt->rt_fd);
	if (!ci ||
	    (ci->ci_state != CONN_CONNECTED && ci->ci_state != CONN_READING &&
	     ci->ci_state != CONN_WRITING && ci->ci_state != CONN_READWRITE)) {
		LOG("Connection is not ready for fd=%d", rt->rt_fd);
		return ENOTCONN;
	}

	return io_rpc_trans_cb(rt);
}

int io_schedule_heartbeat(struct ring_context *rc)
{
	struct io_context *ic =
		io_context_create(OP_TYPE_HEARTBEAT, -1, NULL, 0);

	if (!ic) {
		LOG("io_schedule_heartbeat: failed to create ic");
		return -ENOMEM;
	}

	uint32_t period = atomic_load_explicit(&kqueue_heartbeat_period,
					       memory_order_relaxed);
	return kqueue_arm_heartbeat_timer(rc, ic,
					  (uint64_t)period * 1000000000ULL);
}

int io_handle_heartbeat(struct io_context *ic, int result,
			struct ring_context *rc)
{
	time_t now = time(NULL);

	(void)result;

	/*
	 * Listener watchdog: if any registered listener dropped out of
	 * the LISTENING state with no pending accept op, resubmit.  This
	 * is the single concrete availability gap in the PR #7 port --
	 * without it a transient accept failure is terminal.
	 */
	if (now - kqueue_last_listener_check >=
	    KQUEUE_LISTENER_CHECK_INTERVAL) {
		int num_listeners;
		int *listener_fds = io_heartbeat_get_listeners(&num_listeners);

		for (int i = 0; i < num_listeners; i++) {
			int fd = listener_fds[i];
			if (fd <= 0)
				continue;
			struct conn_info *ci = io_conn_get(fd);
			if (ci && ci->ci_state != CONN_LISTENING &&
			    ci->ci_accept_count == 0) {
				LOG("Listener fd=%d not in LISTENING -- resubmitting accept",
				    fd);
				int ret = io_request_accept_op(fd, NULL, rc);
				if (ret != 0)
					LOG("Watchdog failed to resubmit accept for fd=%d: %s",
					    fd, strerror(-ret));
			}
		}

		kqueue_last_listener_check = now;
	}

	/*
	 * Connection idle-timeout sweep: close connections that have
	 * been inactive longer than KQUEUE_CONN_TIMEOUT.  Delegates to
	 * the shared conn_info module which holds the activity timestamps.
	 */
	if (now - kqueue_last_conn_check >= KQUEUE_CONN_CHECK_INTERVAL) {
		int closed = io_conn_check_timeouts(KQUEUE_CONN_TIMEOUT);
		if (closed > 0)
			TRACE("Heartbeat: closed %d inactive connections",
			      closed);
		kqueue_last_conn_check = now;
	}

	io_context_destroy(ic);
	return io_schedule_heartbeat(rc);
}

int io_request_write_op(int fd, char *buf, int len, uint64_t state,
			struct connection_info *ci, struct ring_context *rc)
{
	struct io_context *ic =
		io_context_create(OP_TYPE_WRITE, fd, buf, (size_t)len);

	if (!ic)
		return -ENOMEM;

	if (ci)
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
 * io_resubmit_write -- re-arm EVFILT_WRITE on ic->ic_fd with ic as
 * udata (no allocation).  The main loop's write_and_dispatch will
 * call write(2) when the socket is writable and then invoke
 * io_handle_write with the byte count or -errno.
 *
 * The per-fd write gate (conn_info's ci_write_active + WRITE_OWNED)
 * ensures at most one EVFILT_WRITE knote is registered per fd at
 * any time, so this EV_ADD|EV_ONESHOT registration will not collide
 * with another in-flight writer's knote.  (PR #7 addendum B2 has
 * the proof; TLS-data submissions via io_do_tls are deferred to
 * PR #8, which must extend the gate to cover them.)
 */
int io_resubmit_write(struct io_context *ic, struct ring_context *rc)
{
	size_t remaining = ic->ic_buffer_len - ic->ic_position;
	uint32_t chunk_size = (remaining > IO_MAX_WRITE_SIZE) ?
				      IO_MAX_WRITE_SIZE :
				      (uint32_t)remaining;

	ic->ic_expected_len = chunk_size;

	struct kevent ke;

	EV_SET(&ke, ic->ic_fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, ic);

	TSAN_RELEASE(ic);

	pthread_mutex_lock(&rc->rc_mutex);
	int ret = kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL);
	pthread_mutex_unlock(&rc->rc_mutex);

	if (ret < 0) {
		int saved_errno = errno;

		LOG("io_resubmit_write: kevent: %s", strerror(saved_errno));
		if (ic->ic_state & IO_CONTEXT_WRITE_OWNED)
			io_socket_close(ic->ic_fd, saved_errno);
		io_context_destroy(ic);
		return -saved_errno;
	}

	return 0;
}

/*
 * io_resubmit_read -- re-arm EVFILT_READ on ic->ic_fd with ic as
 * udata.  The main loop's read_and_dispatch will call read(2) when
 * the socket has data and invoke io_handle_read with bytes_read
 * (or -errno / 0 for EOF).
 */
int io_resubmit_read(struct io_context *ic, struct ring_context *rc)
{
	struct kevent ke;

	EV_SET(&ke, ic->ic_fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, ic);

	TSAN_RELEASE(ic);

	pthread_mutex_lock(&rc->rc_mutex);
	int ret = kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL);
	pthread_mutex_unlock(&rc->rc_mutex);

	if (ret < 0) {
		int saved_errno = errno;

		LOG("io_resubmit_read: kevent: %s", strerror(saved_errno));
		return -saved_errno;
	}

	io_context_update_time(ic);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Read/write completion handlers (called from main loop dispatch)    */
/* ------------------------------------------------------------------ */

/*
 * read_and_dispatch -- EVFILT_READ fired on this connection.  Call
 * read(2) synchronously and dispatch to the shared io_handle_read.
 *
 * EAGAIN: re-arm EVFILT_READ with the same ic and return.  The kernel
 * spuriously fires EVFILT_READ under load; re-arming rather than
 * dropping the read gives us correctness at a small perf cost.
 *
 * EV_EOF with no data: the peer closed cleanly.  Pass 0 so
 * io_handle_read takes the "connection closed" branch (it treats any
 * non-positive result as close).
 *
 * EV_EOF with data still buffered: kqueue guarantees ke->data bytes
 * are readable before EOF; a single read(2) returns them and we
 * dispatch normally.  The next iteration gets ke->data == 0 + EV_EOF
 * and the close-path runs then.
 */
static void read_and_dispatch(struct io_context *ic, const struct kevent *ke,
			      struct ring_context *rc)
{
	ssize_t nread = read(ic->ic_fd, ic->ic_buffer, BUFFER_SIZE);
	int result;

	if (nread < 0) {
		int saved_errno = errno;
		if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
			/* Spurious wakeup -- re-arm and wait again.
			 * io_resubmit_read keeps ic alive on failure
			 * (returns the -errno to the caller without
			 * destroying), so if it fails we must clean up
			 * here via io_handle_read's error branch. */
			int rerr = io_resubmit_read(ic, rc);
			if (rerr != 0)
				io_handle_read(ic, rerr, rc);
			return;
		}
		result = -saved_errno;
	} else if (nread == 0 && (ke->flags & EV_EOF)) {
		result = 0; /* clean close */
	} else {
		result = (int)nread;
	}

	io_handle_read(ic, result, rc);
}

/*
 * write_and_dispatch -- EVFILT_WRITE fired on this connection.  Call
 * write(2) synchronously and dispatch to the shared io_handle_write.
 *
 * EAGAIN: re-arm EVFILT_WRITE with the same ic (no allocation, no
 * state change) and return.  The kernel buffer filled between the
 * readiness notification and our write(2); the next EVFILT_WRITE
 * will let us retry.
 *
 * EV_EOF: peer closed; treat as -ECONNRESET so io_handle_write goes
 * through io_socket_close.  (write(2) may also succeed with zero
 * bytes, handled via the "bytes_written <= 0" branch in io_handle_write.)
 *
 * ke->fflags carries the socket-level errno when kqueue has one
 * (e.g. ECONNREFUSED on a connect + EVFILT_WRITE path).  We prefer it
 * when set.
 */
static void write_and_dispatch(struct io_context *ic, const struct kevent *ke,
			       struct ring_context *rc)
{
	if (ke->fflags != 0) {
		io_handle_write(ic, -(int)ke->fflags, rc);
		return;
	}
	if (ke->flags & EV_EOF) {
		io_handle_write(ic, -ECONNRESET, rc);
		return;
	}

	/*
	 * Write from ic_position, not from the start of the buffer.
	 * On partial-write continuation, io_handle_write advances
	 * ic_position and transfers buffer ownership to a new ic;
	 * io_resubmit_write re-arms EVFILT_WRITE on the new ic (whose
	 * position is non-zero).  Writing from offset 0 every time
	 * would corrupt every chunk after the first.
	 */
	char *src = (char *)ic->ic_buffer + ic->ic_position;
	ssize_t nwritten = write(ic->ic_fd, src, ic->ic_expected_len);

	if (nwritten < 0) {
		int saved_errno = errno;
		if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
			/*
			 * Kernel buffer full -- re-arm EVFILT_WRITE on the
			 * same ic.  io_resubmit_write destroys ic and closes
			 * the socket on kevent failure, so we must NOT touch
			 * ic after a non-zero return.  The destroy already
			 * drove io_handle_write through the close path via
			 * io_socket_close -> io_conn_unregister's pending-
			 * queue drain.
			 */
			(void)io_resubmit_write(ic, rc);
			return;
		}
		io_handle_write(ic, -saved_errno, rc);
		return;
	}

	io_handle_write(ic, (int)nwritten, rc);
}

static void heartbeat_and_dispatch(struct io_context *ic,
				   struct ring_context *rc)
{
	io_handle_heartbeat(ic, 0, rc);
}

/*
 * Arm a one-shot EVFILT_TIMER on the network ring.  Called by
 * io_schedule_heartbeat above.  The ident is the io_context pointer
 * (ensures uniqueness across concurrent heartbeats without a
 * separate registry); when the timer fires the main loop pulls the
 * ic back out of ke->udata and dispatches to io_handle_heartbeat.
 */
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
		/*
		 * Destroy ic here rather than leaking it.  The caller
		 * (io_schedule_heartbeat) doesn't have a handle to ic
		 * after this returns, so if we don't clean up we leak
		 * on every arm failure -- and repeated arm failures
		 * silently kill the watchdog.
		 */
		io_context_destroy(ic);
		return -saved_errno;
	}

	return 0;
}
