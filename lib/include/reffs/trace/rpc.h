/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TRACE_RPC_H
#define _REFFS_TRACE_RPC_H

#include <stdint.h>
#include "reffs/rpc.h"
#include "reffs/network.h"
#include "reffs/trace/common.h"

static inline void trace_rpc_duration(struct rpc_trans *rt,
				      uint64_t duration_ns,
				      uint64_t avg_duration)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	reffs_trace_event(
		REFFS_TRACE_CAT_RPC, "rpc_duration", __LINE__,
		"OP: %u,%u,%u took %lu ns (max=%lu ns, avg=%lu ns, calls=%lu fails=%lu)",
		rt->rt_info.ri_program, rt->rt_info.ri_version,
		rt->rt_info.ri_procedure, duration_ns,
		ph->ph_op_handler->roh_duration_max, avg_duration,
		ph->ph_op_handler->roh_calls, ph->ph_op_handler->roh_fails);
}

static inline void trace_rpc_task(struct rpc_trans *rt, const char *name, int line)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	reffs_trace_event(REFFS_TRACE_CAT_RPC, name, line, "%p %u,%u,%u %p",
			  (void *)rt, rt->rt_info.ri_program,
			  rt->rt_info.ri_version, rt->rt_info.ri_procedure,
			  (void *)ph);
}

#endif /* _REFFS_TRACE_RPC_H */
