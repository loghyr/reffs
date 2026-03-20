/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TASK_H
#define _REFFS_TASK_H

#include <stdatomic.h>
#include <unistd.h>
#include <liburing.h>
#include "reffs/ring.h"
#include "reffs/network.h"

struct rpc_trans;

enum task_state {
	TASK_IDLE = 0,
	TASK_RUNNING = 1,
	TASK_PAUSED = 2,
	TASK_DONE = 3,
};

// Task struct for worker queue
struct task {
	char *t_buffer;
	int t_bytes_read;
	uint32_t t_xid;
	int t_fd; // For sending responses
	struct connection_info t_ci;
	struct ring_context *t_rc;
	int (*t_cb)(struct rpc_trans *rt);
	_Atomic enum task_state t_state;
	struct rpc_trans *t_rt; /* non-NULL once rpc_trans is allocated */
};

#endif /* _REFFS_TASK_H */
