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
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <rpc/clnt.h>
#include <rpc/rpc_msg.h>
#include <errno.h>

#include "probe1_xdr.h"

#include "reffs/test.h"

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/network.h"
#include "reffs/task.h"
#include "reffs/io.h"
#include "reffs/probe1.h"
#include "reffs/trace/rpc.h"

static int stats_gather_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	STATS_GATHER1res *res = ph->ph_res;
	STATS_GATHER1resok *resok = &res->STATS_GATHER1res_u.psgr_resok;
	probe_program1 *pp = &resok->psgr_program;

	if (res->psgr_status) {
		LOG("error = %d", res->psgr_status);
		goto done;
	}

	LOG("count=%lu replied=%lu rejected=%lu accepted=%lu authed=%lu",
	    pp->pp_count, pp->pp_replied_errors, pp->pp_rejected_errors,
	    pp->pp_accepted_errors, pp->pp_authed_errors);

	LOG("\n%3s %15s %10s %16s %16s %16s %16s", "OP", "Name", "Calls",
	    "Errors", "Max", "Total", "Average");
	LOG("%3s %15s %10s %16s %16s %16s %16s", "---", "---------------",
	    "----------", "----------------", "----------------",
	    "----------------", "----------------");

	for (uint32_t i = 0; i < pp->pp_ops.pp_ops_len; i++) {
		uint64_t avg = pp->pp_ops.pp_ops_val[i].po_total_duration /
			       pp->pp_ops.pp_ops_val[i].po_calls;
		LOG("%3u %15s %10lu %16lu %16lu %16lu %16lu",
		    pp->pp_ops.pp_ops_val[i].po_op,
		    pp->pp_ops.pp_ops_val[i].po_name,
		    pp->pp_ops.pp_ops_val[i].po_calls,
		    pp->pp_ops.pp_ops_val[i].po_errors,
		    pp->pp_ops.pp_ops_val[i].po_max_duration,
		    pp->pp_ops.pp_ops_val[i].po_total_duration, avg);
	}

done:
	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_stats_gather(uint32_t program, uint32_t vers)
{
	int ret;

	struct rpc_trans *rt = rpc_trans_create();
	if (!rt)
		return NULL;

	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_STATS_GATHER;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}

	rt->rt_cb = stats_gather_cb;

	STATS_GATHER1args *args = ph->ph_args;
	args->psga_program = program;
	args->psga_version = vers;

	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		rt = NULL;
	}

	return rt;
}

static int context_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	CONTEXT1res *res = ph->ph_res;
	CONTEXT1resok *resok = &res->CONTEXT1res_u.pcr_resok;

	if (res->pcr_status)
		LOG("error = %d", res->pcr_status);
	else
		LOG("created=%lu freed=%lu active_cancelled=%lu active_destroyed=%lu cancelled_freed=%lu destroyed_freed=%lu",
		    resok->pcr_created, resok->pcr_freed,
		    resok->pcr_active_cancelled, resok->pcr_active_destroyed,
		    resok->pcr_cancelled_freed, resok->pcr_destroyed_freed);

	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_context(void)
{
	int ret;

	struct rpc_trans *rt = rpc_trans_create();
	if (!rt)
		return NULL;

	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_CONTEXT;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}

	rt->rt_cb = context_cb;

	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		rt = NULL;
	}

	return rt;
}

static int null_cb(struct rpc_trans __attribute__((unused)) * rt)
{
	LOG("NULL replied");

	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_null(void)
{
	int ret;

	struct rpc_trans *rt = rpc_trans_create();
	if (!rt)
		return NULL;

	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_NULL;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}

	rt->rt_cb = null_cb;

	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		rt = NULL;
	}

	return rt;
}
