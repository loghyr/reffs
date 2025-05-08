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

#define BUCKET_COUNT 5
static const int64_t rpc_bucket_boundaries[BUCKET_COUNT] = {
	1000000, // 1ms
	10000000, // 10ms
	100000000, // 100ms
	1000000000, // 1s
	10000000000 // 10s
	// Last bucket is >=10s
};

void calculate_bucket_counts(struct hdr_histogram *hh, int64_t *counts)
{
	struct hdr_iter iter;

	// Initialize all counts to 0
	for (int i = 0; i <= BUCKET_COUNT; i++) {
		counts[i] = 0;
	}

	// Initialize iterator to go through all values
	hdr_iter_init(&iter, hh);

	// Iterate through all values and add them to the appropriate bucket
	while (hdr_iter_next(&iter)) {
		int64_t value = iter.value;
		int64_t count = iter.count;

		// Determine which bucket this value belongs to
		int bucket_index =
			BUCKET_COUNT; // Default to the last bucket (≥10s)

		for (int i = 0; i < BUCKET_COUNT; i++) {
			if (value < rpc_bucket_boundaries[i]) {
				bucket_index = i;
				break;
			}
		}

		// Add the count to the appropriate bucket
		counts[bucket_index] += count;
	}
}

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
	probe_program1 *pp = &resok->psgr_program;

	int64_t counts[BUCKET_COUNT + 1]; // +1 for the >=10s bucket

	struct rpc_program_handler *rph = rpc_program_handler_find(
		args->psga_program, args->psga_version);
	if (!rph) {
		res->psgr_status = PROBE1ERR_NOENT;
		goto out;
	}

	pp->pp_ops.pp_ops_val = calloc(rph->rph_ops_len, sizeof(probe_op1));
	if (!pp->pp_ops.pp_ops_val) {
		res->psgr_status = PROBE1ERR_NOMEM;
		goto out;
	}

	pp->pp_ops.pp_ops_len = rph->rph_ops_len;
	pp->pp_program = rph->rph_program;
	pp->pp_version = rph->rph_version;
	__atomic_load(&rph->rph_calls, &pp->pp_count, __ATOMIC_RELAXED);
	__atomic_load(&rph->rph_replied_errors, &pp->pp_replied_errors,
		      __ATOMIC_RELAXED);
	__atomic_load(&rph->rph_rejected_errors, &pp->pp_rejected_errors,
		      __ATOMIC_RELAXED);
	__atomic_load(&rph->rph_accepted_errors, &pp->pp_accepted_errors,
		      __ATOMIC_RELAXED);
	__atomic_load(&rph->rph_authed_errors, &pp->pp_authed_errors,
		      __ATOMIC_RELAXED);

	for (size_t i = 0; i < rph->rph_ops_len; i++) {
		pp->pp_ops.pp_ops_val[i].po_name =
			strdup(rph->rph_ops[i].roh_name);
		__atomic_load(&rph->rph_ops[i].roh_operation,
			      &pp->pp_ops.pp_ops_val[i].po_op,
			      __ATOMIC_RELAXED);
		__atomic_load(&rph->rph_ops[i].roh_stats.rs_calls,
			      &pp->pp_ops.pp_ops_val[i].po_calls,
			      __ATOMIC_RELAXED);
		__atomic_load(&rph->rph_ops[i].roh_stats.rs_fails,
			      &pp->pp_ops.pp_ops_val[i].po_errors,
			      __ATOMIC_RELAXED);
		__atomic_load(&rph->rph_ops[i].roh_stats.rs_duration_max,
			      &pp->pp_ops.pp_ops_val[i].po_max_duration,
			      __ATOMIC_RELAXED);
		__atomic_load(&rph->rph_ops[i].roh_stats.rs_duration_total,
			      &pp->pp_ops.pp_ops_val[i].po_total_duration,
			      __ATOMIC_RELAXED);

		struct hdr_histogram *hh =
			rph->rph_ops[i].roh_stats.rs_histogram;
		calculate_bucket_counts(hh, counts);

		pp->pp_ops.pp_ops_val[i].po_bucket_1ms = counts[0];
		pp->pp_ops.pp_ops_val[i].po_bucket_10ms = counts[1];
		pp->pp_ops.pp_ops_val[i].po_bucket_100ms = counts[2];
		pp->pp_ops.pp_ops_val[i].po_bucket_1s = counts[3];
		pp->pp_ops.pp_ops_val[i].po_bucket_10s = counts[4];
		pp->pp_ops.pp_ops_val[i].po_bucket_rest = counts[5];

		pp->pp_ops.pp_ops_val[i].po_median_ns =
			hdr_value_at_percentile(hh, 50.0);
		pp->pp_ops.pp_ops_val[i].po_p90_ns =
			hdr_value_at_percentile(hh, 90.0);
		pp->pp_ops.pp_ops_val[i].po_p99_ns =
			hdr_value_at_percentile(hh, 99.0);
		pp->pp_ops.pp_ops_val[i].po_p999_ns =
			hdr_value_at_percentile(hh, 99.9);

		pp->pp_ops.pp_ops_val[i].po_min_ns = hdr_min(hh);
		pp->pp_ops.pp_ops_val[i].po_max_ns = hdr_max(hh);
		pp->pp_ops.pp_ops_val[i].po_mean_ns = hdr_mean(hh);
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

	struct io_context_stats ics;

	io_context_stats(&ics);

	resok->pcr_created = ics.ics_created;
	resok->pcr_freed = ics.ics_freed;
	resok->pcr_active_cancelled = ics.ics_active_cancelled;
	resok->pcr_active_destroyed = ics.ics_active_destroyed;
	resok->pcr_cancelled_freed = ics.ics_cancelled_freed;
	resok->pcr_destroyed_freed = ics.ics_destroyed_freed;

	return 0;
}

static int probe1_op_rpc_dump_set(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	probe_dump1 *args = ph->ph_args;

	if (*args)
		rpc_enable_packet_logging();
	else
		rpc_disable_packet_logging();

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
	RPC_OPERATION_INIT(PROBEPROC1, RPC_DUMP_SET, xdr_probe_dump1,
			   probe_dump1, xdr_probe_stat1, probe_stat1,
			   probe1_op_rpc_dump_set),
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
