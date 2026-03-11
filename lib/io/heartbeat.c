/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <liburing.h>
#include <linux/time_types.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/log.h"
#include "reffs/ring.h"

// Heartbeat interval in seconds
#define HEARTBEAT_INTERVAL 1

#ifdef HAVE_VM
#define STALLED_CHECK_INTERVAL 1
#define DESTROYED_CHECK_INTERVAL 1
#else
#define DESTROYED_CHECK_INTERVAL 60
#define STALLED_CHECK_INTERVAL 60
#endif

#define LISTENER_CHECK_INTERVAL 5
#define CONNECTION_CHECK_INTERVAL 10
#define STATS_LOG_INTERVAL 10

static uint32_t io_heartbeat_period = HEARTBEAT_INTERVAL;

// Structure to track when different checks were last performed
struct heartbeat_state {
	time_t last_heartbeat;
	time_t last_stalled_check;
	time_t last_destroyed_check;
	time_t last_listener_check;
	time_t last_connection_check;
	time_t last_stat_time;
	time_t last_overflow_check;
	uint64_t last_completions;
	uint64_t total_completions;
};

static struct heartbeat_state hb_state = { 0 };

// Initialize the heartbeat system
int io_heartbeat_init(struct ring_context *rc)
{
	time_t now = time(NULL);

	// Initialize timestamps
	hb_state.last_heartbeat = now;
	hb_state.last_stalled_check = now;
	hb_state.last_destroyed_check = now;
	hb_state.last_listener_check = now;
	hb_state.last_connection_check = now;
	hb_state.last_stat_time = now;
	hb_state.last_overflow_check = now;
	hb_state.last_completions = 0;
	hb_state.total_completions = 0;

	// Schedule the first heartbeat
	return io_schedule_heartbeat(rc);
}

uint32_t io_heartbeat_period_get(void)
{
	return io_heartbeat_period;
}

uint32_t io_heartbeat_period_set(uint32_t seconds)
{
	uint32_t hb = io_heartbeat_period;
	io_heartbeat_period = seconds;
	return hb;
}

// Schedule a heartbeat operation using io_uring timeout
int io_schedule_heartbeat(struct ring_context *rc)
{
	struct io_uring_sqe *sqe;
	struct __kernel_timespec *ts;
	struct io_context *ic;

	int ret = 0;

	ts = calloc(1, sizeof(*ts));
	if (!ts) {
		LOG("Failed to create heartbeat timestamp");
		return -ENOMEM;
	}

	// Create a context for the heartbeat operation
	ic = io_context_create(OP_TYPE_HEARTBEAT, -1, ts, sizeof(*ts));
	if (!ic) {
		free(ts);
		LOG("Failed to create heartbeat context");
		return -ENOMEM;
	}

	// Get a submission queue entry
	pthread_mutex_lock(&rc->rc_mutex);
	sqe = io_uring_get_sqe(&rc->rc_ring);
	if (!sqe) {
		pthread_mutex_unlock(&rc->rc_mutex);
		LOG("Failed to get SQE for heartbeat");
		io_context_destroy(ic);
		return -ENOSPC;
	}

	// Set up the timeout
	ts->tv_sec = io_heartbeat_period;
	ts->tv_nsec = 0;

	// Prepare the timeout operation
	io_uring_prep_timeout(sqe, (struct __kernel_timespec *)ic->ic_buffer, 0,
			      0);
	io_uring_sqe_set_data(sqe, ic);

	TRACE("Scheduled next heartbeat in %u seconds", io_heartbeat_period);

	// Submit the operation
	ret = io_uring_submit(&rc->rc_ring);
	pthread_mutex_unlock(&rc->rc_mutex);
	return ret;
}

// Handle heartbeat completion - perform health checks and reschedule
int io_handle_heartbeat(struct io_context *ic, int result,
			struct ring_context *rc)
{
	time_t now = time(NULL);

	// For timeouts, the result will be -ETIME (timer expired)
	// This is the expected result for a timeout operation
	if (result != -ETIME && result < 0) {
		LOG("Unexpected heartbeat result: %d (%s)", result,
		    strerror(-result));
	}

	TRACE("HEARTBEAT: Processing at timestamp %ld ctx(c=%ld, f=%ld)",
	      (long)now, io_context_get_created(), io_context_get_freed());

	// Check for CQ ring overflow
	if (now - hb_state.last_overflow_check >= 10) {
		if (io_uring_cq_has_overflow(&rc->rc_ring)) {
			LOG("WARNING: CQ ring overflow detected! Context count: %ld",
			    io_context_get_created() - io_context_get_freed());

			hb_state.last_overflow_check = now;

			// Try to flush events from overflow
			int ret = io_uring_get_events(&rc->rc_ring);
			if (ret < 0) {
				LOG("Error getting events: %s", strerror(-ret));
			} else {
				LOG("Flushed %d events from overflow", ret);
			}
		}
	}

	// Log completion rate
	if (now - hb_state.last_stat_time >= STATS_LOG_INTERVAL) {
		uint64_t rate = (hb_state.total_completions -
				 hb_state.last_completions) /
				(now - hb_state.last_stat_time);
		TRACE("Completion processing rate: %lu/sec (total: %lu)", rate,
		      hb_state.total_completions);
		hb_state.last_completions = hb_state.total_completions;
		hb_state.last_stat_time = now;
	}

	// Heartbeat actions
	if (now - hb_state.last_heartbeat >= HEARTBEAT_INTERVAL) {
		hb_state.last_heartbeat = now;
#ifdef VERBOSE_DEBUG
		io_context_list_active(false);
		io_context_log_stats();
#endif
	}

	// Check for stalled contexts
	if (now - hb_state.last_stalled_check >= STALLED_CHECK_INTERVAL) {
		hb_state.last_stalled_check = now;
#ifdef VERBOSE_DEBUG
		io_context_check_stalled();
#endif
	}

// Release destroyed contexts
#ifdef HAVE_DESTROY_CACHE
	if (now - hb_state.last_destroyed_check >= DESTROYED_CHECK_INTERVAL) {
		hb_state.last_destroyed_check = now;
		io_context_release_destroyed();
	}
#endif
	// Check listener sockets
	if (now - hb_state.last_listener_check >= LISTENER_CHECK_INTERVAL) {
		int num_listeners;
		int *listener_fds;

		listener_fds = io_heartbeat_get_listeners(&num_listeners);

		// Check each listener
		for (int i = 0; i < num_listeners; i++) {
			int fd = listener_fds[i];
			if (fd <= 0)
				continue;

			struct conn_info *ci = io_conn_get(fd);
			if (ci && ci->ci_state != CONN_LISTENING &&
			    ci->ci_accept_count == 0) {
				LOG("Listener fd=%d not in LISTENING state - resubmitting accept",
				    fd);

				// Try to resubmit accept operation
				int ret = io_request_accept_op(fd, NULL, rc);
				if (ret != 0) {
					LOG("Watchdog failed to resubmit accept for fd=%d: %s",
					    fd, strerror(ret));
				} else {
					LOG("Watchdog successfully resubmitted accept for fd=%d",
					    fd);
				}
			}
		}

		hb_state.last_listener_check = now;
	}

	// Check connection timeouts and read operations
	if (now - hb_state.last_connection_check >= CONNECTION_CHECK_INTERVAL) {
		// Define timeout in seconds
		const int conn_timeout = 60; // 1 minute timeout

		// Scan all active connections
		for (int fd = 3; fd < MAX_CONNECTIONS; fd++) {
			struct conn_info *ci = io_conn_get(fd);
			if (!ci || ci->ci_state != CONN_CONNECTED)
				continue;

			// Check for stale connections
			if (now - ci->ci_last_activity > conn_timeout) {
				LOG("Connection fd=%d inactive for %ld seconds - closing",
				    fd, (long)(now - ci->ci_last_activity));
				io_socket_close(fd, ETIMEDOUT);
				continue;
			}

			// Ensure each active connection has a pending read operation
			if (ci->ci_read_count == 0) {
				LOG("Connection fd=%d has no pending read operations - submitting read",
				    fd);
				int ret = io_request_read_op(fd, NULL, rc);
				if (ret != 0) {
					LOG("Failed to submit read for fd=%d: %s",
					    fd, strerror(ret));
					// If we can't submit a read, the connection is effectively dead
					io_socket_close(fd, ret);
				}
			} else if (ci->ci_read_count > 1) {
				// This is a potential issue - more than one reader
				LOG("Warning: Connection fd=%d has %d pending read operations",
				    fd, ci->ci_read_count);
			}
		}

		hb_state.last_connection_check = now;
	}

#ifdef VERBOSE_DEBUG
#ifndef HAVE_VM
	io_conn_dump_all();
#endif
#endif

	TRACE("SQ head=%u, tail=%u; CQ head=%u, tail=%u", *rc->rc_ring.sq.khead,
	      *rc->rc_ring.sq.ktail, *rc->rc_ring.cq.khead,
	      *rc->rc_ring.cq.ktail);

	// Schedule the next heartbeat
	io_context_destroy(ic); // always destroy before rescheduling
	return io_schedule_heartbeat(rc);
}

// Function to update completion count - called from main loop
void io_heartbeat_update_completions(uint64_t count)
{
	hb_state.total_completions += count;
}
