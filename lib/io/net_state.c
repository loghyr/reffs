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
 *   - conn_buffers[] / io_buffer_state_*: per-fd accumulating buffers
 *     for reassembling RPC records across partial TCP reads.
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
#include "reffs/trace/io.h"

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
			rt = pending_requests[i];
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

static struct buffer_state *conn_buffers[MAX_CONNECTIONS];

void io_client_fd_register(int fd)
{
	io_buffer_state_create(fd);
}

void io_client_fd_unregister(int fd)
{
	struct buffer_state *bs = io_buffer_state_get(fd);
	if (bs) {
		free(bs->bs_data);
		if (bs->bs_record.rs_data) {
			free(bs->bs_record.rs_data);
		}
		free(bs);
		conn_buffers[fd % MAX_CONNECTIONS] = NULL;
	}
}

struct buffer_state *io_buffer_state_create(int fd)
{
	struct buffer_state *bs = malloc(sizeof(struct buffer_state));
	if (!bs)
		return NULL;

	bs->bs_fd = fd;
	bs->bs_capacity = BUFFER_SIZE * 2;
	bs->bs_data = malloc(bs->bs_capacity);
	bs->bs_filled = 0;

	bs->bs_record.rs_last_fragment = false;
	bs->bs_record.rs_fragment_len = 0;
	bs->bs_record.rs_data = NULL;
	bs->bs_record.rs_total_len = 0;
	bs->bs_record.rs_capacity = 0;
	bs->bs_record.rs_position = 0;

	if (!bs->bs_data) {
		free(bs);
		return NULL;
	}

	int slot = fd % MAX_CONNECTIONS;
	if (conn_buffers[slot] != NULL) {
		LOG("io: conn_buffers alias: fd=%d slot=%d already occupied -- "
		    "increase MAX_CONNECTIONS or use a hash map",
		    fd, slot);
		free(bs->bs_data);
		free(bs);
		return NULL;
	}
	conn_buffers[slot] = bs;
	return bs;
}

struct buffer_state *io_buffer_state_get(int fd)
{
	return conn_buffers[fd % MAX_CONNECTIONS];
}

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

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (conn_buffers[i]) {
			if (conn_buffers[i]->bs_data) {
				free(conn_buffers[i]->bs_data);
			}
			if (conn_buffers[i]->bs_record.rs_data) {
				free(conn_buffers[i]->bs_record.rs_data);
			}
			free(conn_buffers[i]);
			conn_buffers[i] = NULL;
		}
	}
}
