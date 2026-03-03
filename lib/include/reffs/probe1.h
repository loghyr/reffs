/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_PROBE1_H
#define _REFFS_PROBE1_H

#include "reffs/rpc.h"

int probe1_protocol_deregister(void);
int probe1_protocol_register(void);

struct rpc_trans *probe1_client_op_null(void);
struct rpc_trans *probe1_client_op_context(void);
struct rpc_trans *probe1_client_op_stats_gather(uint32_t program,
						uint32_t vers);
struct rpc_trans *probe1_client_op_fs_usage(bool human, const char *path);

#endif
