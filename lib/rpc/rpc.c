/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdatomic.h>
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
#include "reffs/test.h"
#include "reffs/rpc.h"

CDS_LIST_HEAD(rpc_program_handler_list);

static void rpc_program_handler_free_rcu(struct rcu_head *rcu)
{
	struct rpc_program_handler *rph =
		caa_container_of(rcu, struct rpc_program_handler, rph_rcu);

	free(rph);
}

static void rpc_program_handler_release(struct urcu_ref *ref)
{
	struct rpc_program_handler *rph =
		caa_container_of(ref, struct rpc_program_handler, rph_ref);

	uint32_t flags = __atomic_fetch_and(&rph->rph_flags, ~RPH_IN_LIST,
					    __ATOMIC_ACQUIRE);
	if (flags & RPH_IN_LIST)
		cds_list_del_init(&rph->rph_list);

	call_rcu(&rph->rph_rcu, rpc_program_handler_free_rcu);
}

struct rpc_program_handler *
rpc_program_handler_alloc(uint32_t program, uint32_t version,
			  struct rpc_operations_handler *ops,
			  size_t ops_len)
{
	struct rpc_program_handler *rph;

	rph = rpc_program_handler_find(program, version);
	if (rph) {
		rpc_program_handler_put(rph);
		return NULL;
	}

	rph = calloc(1, sizeof(*rph));
	if (!rph) {
		LOG("Could not alloc a rph");
		return NULL;
	}

	rph->rph_program = program;
	rph->rph_version = version;
	rph->rph_ops = ops;
	rph->rph_ops_len = ops_len;

	urcu_ref_init(&rph->rph_ref);
	__atomic_fetch_or(&rph->rph_flags, RPH_IN_LIST, __ATOMIC_RELEASE);
	cds_list_add_rcu(&rph->rph_list, &rpc_program_handler_list);

	return rph;
}

struct rpc_program_handler *rpc_program_handler_find(uint32_t program,
						     uint32_t version)
{
	struct rpc_program_handler *rph = NULL;
	struct rpc_program_handler *tmp;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(tmp, &rpc_program_handler_list, rph_list)
		if (program == tmp->rph_program &&
		    version == tmp->rph_version) {
			rph = rpc_program_handler_get(tmp);
			TRACE(REFFS_TRACE_LEVEL_INFO,
			      "RPC program %u and version %u have a match",
			      program, version);
			break;
		}
	rcu_read_unlock();

	return rph;
}

struct rpc_program_handler *
rpc_program_handler_get(struct rpc_program_handler *rph)
{
	if (!rph)
		return NULL;

	if (!urcu_ref_get_unless_zero(&rph->rph_ref))
		return NULL;

	return rph;
}

void rpc_program_handler_put(struct rpc_program_handler *rph)
{
	if (!rph)
		return;

	urcu_ref_put(&rph->rph_ref, rpc_program_handler_release);
}

static int rpc_parse_call_data(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	XDR xdrs = { 0 };

	size_t len;

	uint32_t *p = (uint32_t *)(rt->rt_body + rt->rt_offset);

	uint32_t start_pos, end_pos;

	if (!ph->ph_op_handler->roh_args_f)
		return 0;

	xdrmem_create(&xdrs, (char *)p, rt->rt_body_len - rt->rt_offset,
		      XDR_DECODE);

	start_pos = xdr_getpos(&xdrs);

	if (!ph->ph_op_handler->roh_args_f(&xdrs, ph->ph_args)) {
		xdr_destroy(&xdrs);
		return EINVAL;
	}

	end_pos = xdr_getpos(&xdrs);

	len = end_pos - start_pos;

	xdr_destroy(&xdrs);

	rt->rt_offset += len;

	return 0;
}

int rpc_protocol_allocate_call(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	rt->rt_rph = rpc_program_handler_find(rt->rt_info.ri_program,
					      rt->rt_info.ri_version);
	if (!rt->rt_rph) {
		TRACE(REFFS_TRACE_LEVEL_WARNING,
		      "RPC program %u and version %u have no handler",
		      rt->rt_info.ri_program, rt->rt_info.ri_version);
		return ENOENT;
	}

	for (size_t i = 0; i < rt->rt_rph->rph_ops_len; i++) {
		if (rt->rt_rph->rph_ops[i].roh_operation ==
		    rt->rt_info.ri_procedure) {
			TRACE(REFFS_TRACE_LEVEL_INFO,
			      "RPC program %u and version %u matches operation %d",
			      rt->rt_info.ri_program, rt->rt_info.ri_version,
			      rt->rt_info.ri_procedure);
			if (!rt->rt_rph->rph_ops[i].roh_action)
				return 0;

			ph->ph_op_handler = &rt->rt_rph->rph_ops[i];

			if (rt->rt_rph->rph_ops[i].roh_args_f &&
			    rt->rt_rph->rph_ops[i].roh_args_size) {
				ph->ph_args = calloc(
					1,
					rt->rt_rph->rph_ops[i].roh_args_size);
				if (!ph->ph_args)
					return ENOMEM;
			}

			if (rt->rt_rph->rph_ops[i].roh_res_f &&
			    rt->rt_rph->rph_ops[i].roh_res_size) {
				ph->ph_res = calloc(
					1, rt->rt_rph->rph_ops[i].roh_res_size);
				if (!ph->ph_res) {
					free(ph->ph_args);
					ph->ph_args = NULL;
					return ENOMEM;
				}
			}

			return rpc_parse_call_data(rt);
		}
	}

	TRACE(REFFS_TRACE_LEVEL_WARNING,
	      "RPC program %u and version %u have no operation",
	      rt->rt_info.ri_program, rt->rt_info.ri_version);
	return ENOENT;
}

int rpc_protocol_op_call(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	int ret = 0;

	if (ph->ph_op_handler && ph->ph_op_handler->roh_action) {
		struct timespec start, end;
		uint64_t duration_ns;

		clock_gettime(CLOCK_MONOTONIC, &start);
		ret = ph->ph_op_handler->roh_action(rt);
		clock_gettime(CLOCK_MONOTONIC, &end);

		duration_ns = (end.tv_sec - start.tv_sec) * 1000000000ULL +
			      (end.tv_nsec - start.tv_nsec);

		ph->ph_op_handler->roh_calls++;
		ph->ph_op_handler->roh_duration_total += duration_ns;

		if (duration_ns > ph->ph_op_handler->roh_duration_max) {
			ph->ph_op_handler->roh_duration_max = duration_ns;
		}

		uint64_t avg_duration =
			ph->ph_op_handler->roh_duration_total / ph->ph_op_handler->roh_calls;

		TRACE(REFFS_TRACE_LEVEL_WARNING,
		      "OP: %u,%u,%u took %lu ns (max: %lu ns, avg: %lu ns, calls: %lu)",
		      rt->rt_info.ri_program, rt->rt_info.ri_version,
		      rt->rt_info.ri_procedure, duration_ns,
		      ph->ph_op_handler->roh_duration_max, avg_duration,
		      ph->ph_op_handler->roh_calls);
	} else {
		rt->rt_info.ri_accept_stat = PROG_UNAVAIL;
	}

	return ret;
}

void rpc_protocol_free(struct rpc_trans *rt)
{
	struct protocol_handler *ph;

	if (!rt)
		return;

	switch (rt->rt_info.ri_cred.rc_flavor) {
	case AUTH_SYS:
		xdr_free((xdrproc_t)xdr_authunix_parms,
			 (char *)&rt->rt_info.ri_cred.rc_unix);
		break;
	default:
		break;
	}

	ph = (struct protocol_handler *)rt->rt_context;
	if (ph) {
		if (ph->ph_op_handler) {
			if (ph->ph_op_handler->roh_args_f) {
				xdr_free(ph->ph_op_handler->roh_args_f,
					 ph->ph_args);
				free(ph->ph_args);
			}

			if (ph->ph_op_handler->roh_res_f) {
				xdr_free(ph->ph_op_handler->roh_res_f,
					 ph->ph_res);
				free(ph->ph_res);
			}
		}
		free(ph);
	}

	rpc_program_handler_put(rt->rt_rph);

	free(rt->rt_reply);
	free(rt);
}
