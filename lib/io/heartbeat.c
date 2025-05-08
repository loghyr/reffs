/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <liburing.h>
#include <errno.h>

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/network.h"
#include "reffs/server.h"
#include "reffs/task.h"
#include "reffs/io.h"
#include "reffs/trace/io.h"

// Heartbeat interval in seconds
#define HEARTBEAT_INTERVAL 60
#define STALLED_CHECK_INTERVAL 60
#define DESTROYED_CHECK_INTERVAL 60
#define LISTENER_CHECK_INTERVAL 5
#define CONNECTION_CHECK_INTERVAL 10
#define STATS_LOG_INTERVAL 10

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
int io_heartbeat_init(struct io_uring *ring)
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
	return io_schedule_heartbeat(ring, HEARTBEAT_INTERVAL);
}

// Schedule a heartbeat operation using io_uring timeout
int io_schedule_heartbeat(struct io_uring *ring, unsigned int seconds)
{
	struct io_uring_sqe *sqe;
	struct __kernel_timespec ts;
	struct io_context *ic;

	// Create a context for the heartbeat operation
	ic = io_context_create(OP_TYPE_HEARTBEAT, -1, NULL, 0);
	if (!ic) {
		LOG("Failed to create heartbeat context");
		return -ENOMEM;
	}

	// Get a submission queue entry
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		LOG("Failed to get SQE for heartbeat");
		io_context_destroy(ic);
		return -ENOSPC;
	}

	// Set up the timeout
	ts.tv_sec = seconds;
	ts.tv_nsec = 0;

	// Prepare the timeout operation
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	io_uring_sqe_set_data(sqe, ic);

	LOG("Scheduled next heartbeat in %u seconds", seconds);

	// Submit the operation
	return io_uring_submit(ring);
}

// Handle heartbeat completion - perform health checks and reschedule
int io_handle_heartbeat(struct io_context *ic, int result,
			struct io_uring *ring)
{
	time_t now = time(NULL);

	// For timeouts, the result will be -ETIME (timer expired)
	// This is the expected result for a timeout operation
	if (result != -ETIME && result < 0) {
		LOG("Unexpected heartbeat result: %d (%s)", result,
		    strerror(-result));
	}

	LOG("HEARTBEAT: Processing at timestamp %ld ctx(c=%ld, f=%ld)",
	    (long)now, io_context_get_created(), io_context_get_freed());

	// Check for CQ ring overflow
	if (now - hb_state.last_overflow_check >= 10) {
		if (io_uring_cq_has_overflow(ring)) {
			LOG("WARNING: CQ ring overflow detected! Context count: %ld",
			    io_context_get_created() - io_context_get_freed());

			hb_state.last_overflow_check = now;

			// Try to flush events from overflow
			int ret = io_uring_get_events(ring);
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
		LOG("Completion processing rate: %lu/sec (total: %lu)", rate,
		    hb_state.total_completions);
		hb_state.last_completions = hb_state.total_completions;
		hb_state.last_stat_time = now;
	}

	// Heartbeat actions
	if (now - hb_state.last_heartbeat >= HEARTBEAT_INTERVAL) {
		hb_state.last_heartbeat = now;
		io_context_list_active(false);
		io_context_log_stats();
	}

	// Check for stalled contexts
	if (now - hb_state.last_stalled_check >= STALLED_CHECK_INTERVAL) {
		hb_state.last_stalled_check = now;
		io_context_check_stalled();
	}

	// Release destroyed contexts
	if (now - hb_state.last_destroyed_check >= DESTROYED_CHECK_INTERVAL) {
		hb_state.last_destroyed_check = now;
		io_context_release_destroyed();
	}

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
				int ret = io_request_accept_op(fd, NULL, ring);
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
				int ret = io_request_read_op(fd, NULL, ring);
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

	io_conn_dump_all();

	// We should destroy the context here only if it wasn't already handled
	// by the main error handler
	if (result == 0) {
		io_context_destroy(ic);
	}

	// Schedule the next heartbeat
	return io_schedule_heartbeat(ring, HEARTBEAT_INTERVAL);
}

// Function to update completion count - called from main loop
void io_heartbeat_update_completions(uint64_t count)
{
	hb_state.total_completions += count;
}
