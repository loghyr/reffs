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

#include "stats1_xdr.h"

#include "reffs/test.h"

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/network.h"
#include "reffs/task.h"
#include "reffs/io.h"
#include "reffs/trace/rpc.h"

static int stat1_op_null(struct rpc_trans __attribute__((unused)) * rt)
{
	return 0;
}

static int stat1_op_gather(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	GATHER1args *args = ph->ph_args;
	GATHER1res *res = ph->ph_res;
	GATHER1resok *resok = &res->GATHER1res_u.sgr_resok;
	stat_program1 *sp = &resok->sgr_program;

	struct rpc_program_handler *rph =
		rpc_program_handler_find(args->sga_program, args->sga_version);

	if (!rph) {
		res->sgr_status = STAT1ERR_NOENT;
		goto out;
	}

	sp->sp_ops.sp_ops_val =
		calloc(rt->rt_rph->rph_ops_len, sizeof(stat_op1));
	if (!sp->sp_ops.sp_ops_val) {
		res->sgr_status = STAT1ERR_NOMEM;
		goto out;
	}

	sp->sp_ops.sp_ops_len = rt->rt_rph->rph_ops_len;
	sp->sp_program = rt->rt_info.ri_program;
	sp->sp_version = rt->rt_info.ri_version;
	sp->sp_count = uatomic_read(&rt->rt_rph->rph_calls, __ATOMIC_RELAXED);
	sp->sp_replied_errors =
		uatomic_read(&rt->rt_rph->rph_replied_errors, __ATOMIC_RELAXED);
	sp->sp_rejected_errors = uatomic_read(&rt->rt_rph->rph_rejected_errors,
					      __ATOMIC_RELAXED);
	sp->sp_accepted_errors = uatomic_read(&rt->rt_rph->rph_accepted_errors,
					      __ATOMIC_RELAXED);
	sp->sp_authed_errors =
		uatomic_read(&rt->rt_rph->rph_authed_errors, __ATOMIC_RELAXED);

	for (size_t i = 0; i < rt->rt_rph->rph_ops_len; i++) {
		sp->sp_ops.sp_ops_val[i].so_op =
			uatomic_read(&rt->rt_rph->rph_ops[i].roh_operation,
				     __ATOMIC_RELAXED);
		sp->sp_ops.sp_ops_val[i].so_calls = uatomic_read(
			&rt->rt_rph->rph_ops[i].roh_calls, __ATOMIC_RELAXED);
		sp->sp_ops.sp_ops_val[i].so_errors = uatomic_read(
			&rt->rt_rph->rph_ops[i].roh_fails, __ATOMIC_RELAXED);
		sp->sp_ops.sp_ops_val[i].so_max_duration =
			uatomic_read(&rt->rt_rph->rph_ops[i].roh_duration_max,
				     __ATOMIC_RELAXED);
		sp->sp_ops.sp_ops_val[i].so_total_duration =
			uatomic_read(&rt->rt_rph->rph_ops[i].roh_duration_total,
				     __ATOMIC_RELAXED);
	}

out:
	rpc_program_handler_put(rph);
	return res->sgr_status;
}

static int stat1_op_context(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	CONTEXT1res *res = ph->ph_res;
	CONTEXT1resok *resok = &res->CONTEXT1res_u.scr_resok;

	resok->scr_created = get_context_created();
	resok->scr_freed = get_context_freed();

	return 0;
}

struct rpc_operations_handler stat1_operations_handler[] = {
	RPC_OPERATION_INIT(STATPROC1, NULL, NULL, NULL, NULL, NULL,
			   stat1_op_null),
	RPC_OPERATION_INIT(STATPROC1, GATHER, xdr_GATHER1args, GATHER1args,
			   xdr_GATHER1res, GATHER1res, stat1_op_gather),
	RPC_OPERATION_INIT(STATPROC1, CONTEXT, NULL, NULL, xdr_CONTEXT1res,
			   CONTEXT1res, stat1_op_context),
};

static struct rpc_program_handler *stat1_handler;

volatile sig_atomic_t statv1_registered = 0;

int stat1_protocol_register(void)
{
	if (statv1_registered)
		return 0;

	statv1_registered = 1;

	stat1_handler = rpc_program_handler_alloc(
		STAT_PROGRAM, STAT_V1, stat1_operations_handler,
		sizeof(stat1_operations_handler) /
			sizeof(*stat1_operations_handler));
	if (!stat1_handler) {
		statv1_registered = 0;
		return ENOMEM;
	}

	return 0;
}

int stat1_protocol_deregister(void)
{
	if (!statv1_registered)
		return 0;

	rpc_program_handler_put(stat1_handler);
	stat1_handler = NULL;
	statv1_registered = 0;

	return 0;
}

