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

#include <arpa/inet.h>
#include <netinet/in.h>

#include "probe1_xdr.h"

#include "reffs/test.h"

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/network.h"
#include "reffs/task.h"
#include "reffs/io.h"
#include "reffs/fs.h"
#include "reffs/probe1.h"
#include "reffs/trace/rpc.h"

struct probe_time1 probe_time1_from_time_t(time_t ts)
{
	struct probe_time1 pt;
	pt.seconds = (unsigned int)ts;
	pt.nseconds = 0;
	return pt;
}

time_t probe_time1_to_time_t(struct probe_time1 pt)
{
	return (time_t)pt.seconds;
}

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

void probe_fd1_get_addr(struct probe_fd1 *pf, struct connection_info *ci)
{
	char ip[INET6_ADDRSTRLEN];
	uint16_t port;

	// Extract server (local) port
	if (ci->ci_local.ss_family == AF_INET) {
		struct sockaddr_in *sa = (struct sockaddr_in *)&ci->ci_local;
		pf->pf_server_port = ntohs(sa->sin_port);
	} else if (ci->ci_local.ss_family == AF_INET6) {
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&ci->ci_local;
		pf->pf_server_port = ntohs(sa6->sin6_port);
	} else {
		pf->pf_server_port = 0;
	}

	// Extract client IP and port
	if (ci->ci_peer.ss_family == AF_INET) {
		struct sockaddr_in *peer = (struct sockaddr_in *)&ci->ci_peer;
		inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof(ip));
		port = ntohs(peer->sin_port);
	} else if (ci->ci_peer.ss_family == AF_INET6) {
		struct sockaddr_in6 *peer6 =
			(struct sockaddr_in6 *)&ci->ci_peer;
		inet_ntop(AF_INET6, &peer6->sin6_addr, ip, sizeof(ip));
		port = ntohs(peer6->sin6_port);
	} else {
		snprintf(pf->pf_client, PROBE1_ADDR_LEN, "<invalid>");
		return;
	}

	// Format: IP.%hhu.%hhu (truncated port bytes)
	snprintf((char *)pf->pf_client, PROBE1_ADDR_LEN, "%s.%hhu.%hhu", ip,
		 (unsigned char)((port >> 8) & 0xFF),
		 (unsigned char)(port & 0xFF));
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

static int probe1_op_trace_set(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	TRACE_SET1args *args = ph->ph_args;
	probe_stat1 *res = ph->ph_res;

	if (args->tsa_cat == PROBE1_TRACE_CAT_ALL) {
		for (probe_trace_category1 i = PROBE1_TRACE_CAT_GENERAL;
		     i < PROBE1_TRACE_CAT_ALL; i++) {
			if (args->tsa_set)
				reffs_trace_enable_category(
					(enum reffs_trace_category)i);
			else
				reffs_trace_disable_category(
					(enum reffs_trace_category)i);
		}
	} else if (args->tsa_cat < PROBE1_TRACE_CAT_GENERAL ||
		   args->tsa_cat > PROBE1_TRACE_CAT_ALL) {
		*res = PROBE1ERR_INVAL;
	} else {
		if (args->tsa_set)
			reffs_trace_enable_category(
				(enum reffs_trace_category)args->tsa_cat);
		else
			reffs_trace_disable_category(
				(enum reffs_trace_category)args->tsa_cat);
	}

	return 0;
}

static int probe1_op_traces_list(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	TRACES_LIST1args *args = ph->ph_args;
	TRACES_LIST1res *res = ph->ph_res;
	TRACES_LIST1resok *resok = &res->TRACES_LIST1res_u.tlr_resok;

	if (args->tla_cat < PROBE1_TRACE_CAT_GENERAL ||
	    args->tla_cat > PROBE1_TRACE_CAT_ALL) {
		res->tlr_status = PROBE1ERR_INVAL;
	} else if (args->tla_cat == PROBE1_TRACE_CAT_ALL) {
		resok->tlr_list.tlr_list_val =
			calloc(PROBE1_TRACE_CAT_ALL,
			       sizeof(*resok->tlr_list.tlr_list_val));
		if (!resok->tlr_list.tlr_list_val) {
			res->tlr_status = PROBE1ERR_NOMEM;
		} else {
			resok->tlr_list.tlr_list_len = PROBE1_TRACE_CAT_ALL;

			for (probe_trace_category1 i = PROBE1_TRACE_CAT_GENERAL;
			     i < PROBE1_TRACE_CAT_ALL; i++) {
				resok->tlr_list.tlr_list_val[i].ptl_cat = i;
				resok->tlr_list.tlr_list_val[i].ptl_set =
					reffs_trace_is_category_enabled(
						(enum reffs_trace_category)i);
			}
		}
	} else {
		resok->tlr_list.tlr_list_val =
			calloc(1, sizeof(*resok->tlr_list.tlr_list_val));
		if (!resok->tlr_list.tlr_list_val) {
			res->tlr_status = PROBE1ERR_NOMEM;
		} else {
			resok->tlr_list.tlr_list_len = 1;

			resok->tlr_list.tlr_list_val[0].ptl_cat = args->tla_cat;
			resok->tlr_list.tlr_list_val[0]
				.ptl_set = reffs_trace_is_category_enabled(
				(enum reffs_trace_category)args->tla_cat);
		}
	}

	return 0;
}

static int probe1_op_graceful_cleanup(struct rpc_trans __attribute__((unused)) *
				      rt)
{
	io_handler_stop();
	return 0;
}

static int probe1_op_heartbeat(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	HEARTBEAT1args *args = ph->ph_args;
	HEARTBEAT1res *res = ph->ph_res;
	HEARTBEAT1resok *resok = &res->HEARTBEAT1res_u.hbr_resok;

	/* Return what it was */
	resok->hbr_period = io_heartbeat_period_get();

	if (args->hba_period_set) {
		io_heartbeat_period_set(args->hba_period);
	}

	return 0;
}

static int probe1_op_io_contexts_list(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	IO_CONTEXTS_LIST1args *args = ph->ph_args;
	IO_CONTEXTS_LIST1res *res = ph->ph_res;
	IO_CONTEXTS_LIST1resok *resok = &res->IO_CONTEXTS_LIST1res_u.iclr_resok;

	struct io_context *ic_list = NULL;
	struct io_context *ic;
	int count;
	int i = 0;

	probe_io_context1 *pic = NULL;
	time_t now = time(NULL);

	ic_list = io_context_probe(
		args->icla_fd, (enum op_type)args->icla_op,
		IO_CONTEXT_ENTRY_STATE_ACTIVE |
			IO_CONTEXT_ENTRY_STATE_MARKED_DESTROYED |
			IO_CONTEXT_ENTRY_STATE_PENDING_FREE,
		&count);
	if (!ic_list) {
		res->iclr_status = PROBE1ERR_NOENT;
		goto out;
	}

	pic = calloc(count, sizeof(*pic));
	if (!pic) {
		res->iclr_status = PROBE1ERR_NOMEM;
		goto out;
	}

	resok->iclr_now = probe_time1_from_time_t(now);

	resok->iclr_pic.iclr_pic_val = pic;
	resok->iclr_pic.iclr_pic_len = count;

	for (ic = ic_list; ic; ic = ic_list) {
		pic[i].pic_op_type = (probe_op_type1)ic->ic_op_type;
		pic[i].pic_fd = ic->ic_fd;
		pic[i].pic_id = ic->ic_id;
		pic[i].pic_xid = ic->ic_xid;
		pic[i].pic_buffer_len = ic->ic_buffer_len;
		pic[i].pic_position = ic->ic_position;
		pic[i].pic_expected_len = ic->ic_expected_len;
		pic[i].pic_state = ic->ic_state;
		pic[i].pic_count = ic->ic_count;

		pic[i].pic_action_time =
			probe_time1_from_time_t(ic->ic_action_time);

		ic_list = ic->ic_next;
		i++;
		free(ic);
	}

out:
	if (ic_list) {
		for (ic = ic_list; ic; ic = ic_list) {
			ic_list = ic->ic_next;
			free(ic);
		}
	}

	return 0;
}

static int probe1_op_fd_infos_list(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	FD_INFOS_LIST1args *args = ph->ph_args;
	FD_INFOS_LIST1res *res = ph->ph_res;
	FD_INFOS_LIST1resok *resok = &res->FD_INFOS_LIST1res_u.filr_resok;

	struct io_context *ic_list = NULL;
	struct io_context *ic;
	int count;
	int i = 0;

	probe_fd1 *pf = NULL;

	ic_list = io_context_probe(args->fila_fd, OP_TYPE_READ,
				   IO_CONTEXT_ENTRY_STATE_ACTIVE, &count);
	if (!ic_list) {
		res->filr_status = PROBE1ERR_NOENT;
		goto out;
	}

	pf = calloc(count, sizeof(*pf));
	if (!pf) {
		res->filr_status = PROBE1ERR_NOMEM;
		goto out;
	}

	resok->filr_pf.filr_pf_val = pf;
	resok->filr_pf.filr_pf_len = count;

	for (ic = ic_list; ic; ic = ic_list) {
		pf[i].pf_fd = ic->ic_fd;
		probe_fd1_get_addr(&pf[i], &ic->ic_ci);

		ic_list = ic->ic_next;
		i++;
		free(ic);
	}

out:
	if (ic_list) {
		for (ic = ic_list; ic; ic = ic_list) {
			ic_list = ic->ic_next;
			free(ic);
		}
	}
	return 0;
}

static int probe1_op_fs_usage(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	FS_USAGE1res *res = ph->ph_res;
	FS_USAGE1resok *resok = &res->FS_USAGE1res_u.fur_resok;

	struct reffs_fs_usage_stats stats;

	reffs_fs_usage(&stats);

	resok->fur_total_bytes = stats.total_bytes;
	resok->fur_used_bytes = stats.used_bytes;
	resok->fur_free_bytes = stats.free_bytes;
	resok->fur_total_files = stats.total_files;
	resok->fur_used_files = stats.used_files;
	resok->fur_free_files = stats.free_files;

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
	RPC_OPERATION_INIT(PROBEPROC1, TRACE_SET, xdr_TRACE_SET1args,
			   TRACE_SET1args, xdr_probe_stat1, probe_stat1,
			   probe1_op_trace_set),
	RPC_OPERATION_INIT(PROBEPROC1, TRACES_LIST, xdr_TRACES_LIST1args,
			   TRACES_LIST1args, xdr_TRACES_LIST1res,
			   TRACES_LIST1res, probe1_op_traces_list),
	RPC_OPERATION_INIT(PROBEPROC1, GRACEFUL_CLEANUP, NULL, NULL,
			   xdr_probe_stat1, probe_stat1,
			   probe1_op_graceful_cleanup),
	RPC_OPERATION_INIT(PROBEPROC1, HEARTBEAT, xdr_HEARTBEAT1args,
			   HEARTBEAT1args, xdr_HEARTBEAT1res, HEARTBEAT1res,
			   probe1_op_heartbeat),
	RPC_OPERATION_INIT(PROBEPROC1, IO_CONTEXTS_LIST,
			   xdr_IO_CONTEXTS_LIST1args, IO_CONTEXTS_LIST1args,
			   xdr_IO_CONTEXTS_LIST1res, IO_CONTEXTS_LIST1res,
			   probe1_op_io_contexts_list),
	RPC_OPERATION_INIT(PROBEPROC1, FD_INFOS_LIST, xdr_FD_INFOS_LIST1args,
			   FD_INFOS_LIST1args, xdr_FD_INFOS_LIST1res,
			   FD_INFOS_LIST1res, probe1_op_fd_infos_list),
	RPC_OPERATION_INIT(PROBEPROC1, FS_USAGE, NULL, NULL, xdr_FS_USAGE1res,
			   FS_USAGE1res, probe1_op_fs_usage),
};

static struct rpc_program_handler *probe1_handler;

volatile sig_atomic_t probev1_registered = 0;

int probe1_protocol_register(void)
{
	if (probev1_registered)
		return 0;

	/* Verify that the generated enum matches the manually defined one */
	static_assert((enum reffs_trace_category)PROBE1_TRACE_CAT_ALL ==
			      REFFS_TRACE_CAT_ALL,
		      "Enum values are out of sync between header and XDR");

	static_assert((enum op_type)PROBE1_OP_TYPE_ALL == OP_TYPE_ALL,
		      "Enum values are out of sync between header and XDR");

	static_assert(PROBE1_IO_CONTEXT_ENTRY_STATE_ACTIVE ==
			      (uint64_t)IO_CONTEXT_ENTRY_STATE_ACTIVE,
		      "IO_CONTEXT flags out of sync");
	static_assert(PROBE1_IO_CONTEXT_ENTRY_STATE_MARKED_DESTROYED ==
			      (uint64_t)IO_CONTEXT_ENTRY_STATE_MARKED_DESTROYED,
		      "IO_CONTEXT flags out of sync");
	static_assert(PROBE1_IO_CONTEXT_ENTRY_STATE_PENDING_FREE ==
			      (uint64_t)IO_CONTEXT_ENTRY_STATE_PENDING_FREE,
		      "IO_CONTEXT flags out of sync");
	static_assert(PROBE1_IO_CONTEXT_DIRECT_TLS_DATA ==
			      (uint64_t)IO_CONTEXT_DIRECT_TLS_DATA,
		      "IO_CONTEXT flags out of sync");
	static_assert(PROBE1_IO_CONTEXT_TLS_BIO_PROCESSED ==
			      (uint64_t)IO_CONTEXT_TLS_BIO_PROCESSED,
		      "IO_CONTEXT flags out of sync");

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
