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

static int probe1_op_null(struct rpc_trans __attribute__((unused)) * rt)
{
	return 0;
}

static int probe1_op_stats_gather(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	STATS_GATHER1args *args = ph->ph_args;
	STATS_GATHER1res *res = ph->ph_res;
	STATS_GATHER1resok *resok = &res->STATS_GATHER1res_u.psgr_resok;
	stat_program1 *sp = &resok->psgr_program;

	struct rpc_program_handler *rph = rpc_program_handler_find(
		args->psga_program, args->psga_version);

	if (!rph) {
		res->psgr_status = PROBE1ERR_NOENT;
		goto out;
	}

	sp->sp_ops.sp_ops_val =
		calloc(rt->rt_rph->rph_ops_len, sizeof(stat_op1));
	if (!sp->sp_ops.sp_ops_val) {
		res->psgr_status = PROBE1ERR_NOMEM;
		goto out;
	}

	sp->sp_ops.sp_ops_len = rt->rt_rph->rph_ops_len;
	sp->sp_program = rt->rt_info.ri_program;
	sp->sp_version = rt->rt_info.ri_version;
	__atomic_load(&rt->rt_rph->rph_calls, &sp->sp_count, __ATOMIC_RELAXED);
	__atomic_load(&rt->rt_rph->rph_replied_errors, &sp->sp_replied_errors,
		      __ATOMIC_RELAXED);
	__atomic_load(&rt->rt_rph->rph_rejected_errors, &sp->sp_rejected_errors,
		      __ATOMIC_RELAXED);
	__atomic_load(&rt->rt_rph->rph_accepted_errors, &sp->sp_accepted_errors,
		      __ATOMIC_RELAXED);
	__atomic_load(&rt->rt_rph->rph_authed_errors, &sp->sp_authed_errors,
		      __ATOMIC_RELAXED);

	for (size_t i = 0; i < rt->rt_rph->rph_ops_len; i++) {
		__atomic_load(&rt->rt_rph->rph_ops[i].roh_operation,
			      &sp->sp_ops.sp_ops_val[i].so_op,
			      __ATOMIC_RELAXED);
		__atomic_load(&rt->rt_rph->rph_ops[i].roh_calls,
			      &sp->sp_ops.sp_ops_val[i].so_calls,
			      __ATOMIC_RELAXED);
		__atomic_load(&rt->rt_rph->rph_ops[i].roh_fails,
			      &sp->sp_ops.sp_ops_val[i].so_errors,
			      __ATOMIC_RELAXED);
		__atomic_load(&rt->rt_rph->rph_ops[i].roh_duration_max,
			      &sp->sp_ops.sp_ops_val[i].so_max_duration,
			      __ATOMIC_RELAXED);
		__atomic_load(&rt->rt_rph->rph_ops[i].roh_duration_total,
			      &sp->sp_ops.sp_ops_val[i].so_total_duration,
			      __ATOMIC_RELAXED);
	}

out:
	rpc_program_handler_put(rph);
	return res->psgr_status;
}

static int probe1_op_context(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	CONTEXT1res *res = ph->ph_res;
	CONTEXT1resok *resok = &res->CONTEXT1res_u.pcr_resok;

	resok->pcr_created = get_context_created();
	resok->pcr_freed = get_context_freed();

	return 0;
}

struct rpc_operations_handler probe1_operations_handler[] = {
	RPC_OPERATION_INIT(PROBEPROC1, NULL, NULL, NULL, NULL, NULL,
			   probe1_op_null),
	RPC_OPERATION_INIT(PROBEPROC1, STATS_GATHER, xdr_STATS_GATHER1args,
			   STATS_GATHER1args, xdr_STATS_GATHER1res,
			   STATS_GATHER1res, probe1_op_stats_gather),
	RPC_OPERATION_INIT(PROBEPROC1, CONTEXT, NULL, NULL, xdr_CONTEXT1res,
			   CONTEXT1res, probe1_op_context),
};

static struct rpc_program_handler *probe1_handler;

volatile sig_atomic_t probev1_registered = 0;

int probe1_protocol_register(void)
{
	if (probev1_registered)
		return 0;

	probev1_registered = 1;

	probe1_handler = rpc_program_handler_alloc(
		PROBE_PROGRAM, PROBE_V1, probe1_operations_handler,
		sizeof(probe1_operations_handler) /
			sizeof(*probe1_operations_handler));
	if (!probe1_handler) {
		probev1_registered = 0;
		return ENOMEM;
	}

	return 0;
}

int probe1_protocol_deregister(void)
{
	if (!probev1_registered)
		return 0;

	rpc_program_handler_put(probe1_handler);
	probe1_handler = NULL;
	probev1_registered = 0;

	return 0;
}
