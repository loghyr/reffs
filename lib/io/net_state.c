/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Backend-agnostic network-state bookkeeping:
 *
 *   - pending_requests[] / request_mutex: in-flight outbound RPC
 *     tracking keyed by XID, used by the connect completion handler
 *     to match a CONNECT CQE to the originating io_send_request.
 *
 *   - listener_fds[] / num_listeners: server-side listen sockets; the
 *     listener-restart path uses this to re-arm the accept op when a
 *     listener socket is closed.
 *
 *   - io_buffer_append: append helper for per-fd record-reassembly
 *     buffers.  The buffers themselves live on struct conn_info as
 *     ci_bs and are managed (lazy-allocate, drain-free) in
 *     lib/io/conn_info.c so their lifecycle is coordinated with
 *     conn_mutex + the CONN_CLOSING gate -- see
 *     .claude/design/io-buffer-state-fd-recycle.md.
 *
 * Extracted from lib/io/handler.c so both the io_uring and kqueue
 * backends share a single implementation.  The only state this
 * module owns is bookkeeping; actual I/O submission remains backend-
 * specific.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/rpc.h"
#include "trace/io.h"

/* ------------------------------------------------------------------ */
/* Request tracking                                                    */
/* ------------------------------------------------------------------ */

static struct rpc_trans *pending_requests[MAX_PENDING_REQUESTS];
static pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;

int io_register_request(struct rpc_trans *rt)
{
	pthread_mutex_lock(&request_mutex);

	TRACE("rt=%p xid=0x%08x", (void *)rt, rt->rt_info.ri_xid);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] == NULL) {
			TRACE("rt=%p xid=0x%08x", (void *)rt,
			      rt->rt_info.ri_xid);
			pending_requests[i] = rt;
			pthread_mutex_unlock(&request_mutex);
			return 0;
		}
	}

	pthread_mutex_unlock(&request_mutex);
	return ENOENT;
}

struct rpc_trans *io_find_request_by_xid(uint32_t xid)
{
	struct rpc_trans *rt = NULL;

	TRACE("xid=0x%08x", xid);
	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] &&
		    pending_requests[i]->rt_info.ri_xid == xid) {
			TRACE("rt=%p xid=0x%08x", (void *)pending_requests[i],
			      xid);
			/*
			 * Take a ref under request_mutex so the rt can't be
			 * freed between this function returning and the
			 * caller's deref.  Caller MUST rpc_protocol_free()
			 * the returned rt when done.  See #31.
			 */
			rt = rpc_trans_get(pending_requests[i]);
			break;
		}
	}
	pthread_mutex_unlock(&request_mutex);

	return rt;
}

int io_unregister_request(uint32_t xid)
{
	TRACE("xid=0x%08x", xid);
	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] &&
		    pending_requests[i]->rt_info.ri_xid == xid) {
			TRACE("rt=%p xid=0x%08x", (void *)pending_requests[i],
			      xid);
			pending_requests[i] = NULL;
			pthread_mutex_unlock(&request_mutex);
			return 0;
		}
	}
	pthread_mutex_unlock(&request_mutex);

	return ENOENT;
}

/* ------------------------------------------------------------------ */
/* Listener registry                                                   */
/* ------------------------------------------------------------------ */

static int listener_fds[MAX_LISTENERS];
static int num_listeners = 0;

void io_add_listener(int fd)
{
	if (fd > 0)
		listener_fds[num_listeners++] = fd;
}

void io_check_for_listener_restart(int fd, struct connection_info *ci,
				   struct ring_context *rc)
{
	for (int i = 0; i < num_listeners; i++) {
		if (fd == listener_fds[i]) {
			LOG("Listener socket closed, immediately resubmitting accept");
			io_request_accept_op(fd, ci, rc);
			break;
		}
	}
}

int *io_heartbeat_get_listeners(int *num)
{
	*num = num_listeners;
	return listener_fds;
}

/* ------------------------------------------------------------------ */
/* Per-fd buffer state                                                 */
/* ------------------------------------------------------------------ */

/*
 * io_buffer_state_create() and io_buffer_state_get() live in
 * lib/io/conn_info.c so they can manage ci_bs on struct conn_info
 * under conn_mutex.  io_client_fd_register / io_client_fd_unregister
 * are gone -- buffer state is lazy-allocated on first read and freed
 * at the CONN_CLOSING -> CONN_UNUSED transition.  See
 * .claude/design/io-buffer-state-fd-recycle.md.
 */

bool io_buffer_append(struct buffer_state *bs, const char *data, size_t len)
{
	if (len > SIZE_MAX / 2 || bs->bs_filled > SIZE_MAX - len) {
		return false;
	}

	if (bs->bs_filled + len > bs->bs_capacity) {
		size_t min_needed = bs->bs_filled + len;
		size_t new_capacity = bs->bs_capacity;

		while (new_capacity < min_needed) {
			if (new_capacity > SIZE_MAX / 2) {
				return false;
			}
			new_capacity *= 2;
		}

		char *new_data = realloc(bs->bs_data, new_capacity);
		if (!new_data)
			return false;

		bs->bs_data = new_data;
		bs->bs_capacity = new_capacity;
	}

	memcpy(bs->bs_data + bs->bs_filled, data, len);
	bs->bs_filled += len;

	return true;
}

/* ------------------------------------------------------------------ */
/* Shutdown cleanup                                                    */
/* ------------------------------------------------------------------ */

/*
 * Free everything held by this module.  Called from each backend's
 * io_handler_fini after in-flight operations have drained.
 *
 * The per-fd buffer_state used to be freed here via a sweep of the
 * conn_buffers[] array; after the fold-in, those buffers live on
 * struct conn_info and are freed by io_conn_cleanup() in
 * lib/io/conn_info.c.  Callers must invoke io_conn_cleanup() BEFORE
 * io_net_state_fini() so any conn_info still holding a bs releases
 * it (otherwise the bs leaks at shutdown).
 */
void io_net_state_fini(void)
{
	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i]) {
			rpc_protocol_free(pending_requests[i]);
			pending_requests[i] = NULL;
		}
	}
	pthread_mutex_unlock(&request_mutex);
}
