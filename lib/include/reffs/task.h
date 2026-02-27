/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TASK_H
#define _REFFS_TASK_H

#include <unistd.h>
#include <liburing.h>
#include "reffs/ring.h"
#include "reffs/network.h"

struct rpc_trans;

// Task struct for worker queue
struct task {
	char *t_buffer;
	int t_bytes_read;
	uint32_t t_xid;
	int t_fd; // For sending responses
	struct connection_info t_ci;
	struct ring_context *t_rc;
	int (*t_cb)(struct rpc_trans *rt);
};

#endif /* _REFFS_TASK_H */
