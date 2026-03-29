/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>
#include <stdatomic.h>
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
#include "nfsv42_xdr.h"
#include "nfsv42_names.h"

#include "reffs/test.h"

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/network.h"
#include "reffs/task.h"
#include "reffs/io.h"
#include "reffs/fs.h"
#include "reffs/probe1.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "reffs/sb_registry.h"
#include "reffs/dstore.h"
#include "reffs/client.h"
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

	/* Per-sb usage breakdown. */
	struct cds_list_head *sb_list = super_block_list_head();
	struct super_block *sb;
	uint32_t sb_count = 0;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, sb_list, sb_link)
		sb_count++;
	rcu_read_unlock();

	if (sb_count > 0) {
		resok->fur_per_sb.fur_per_sb_val =
			calloc(sb_count, sizeof(probe_sb_fs_usage1));
		if (resok->fur_per_sb.fur_per_sb_val) {
			uint32_t i = 0;

			rcu_read_lock();
			cds_list_for_each_entry_rcu(sb, sb_list, sb_link) {
				if (i >= sb_count)
					break;
				probe_sb_fs_usage1 *sfu =
					&resok->fur_per_sb.fur_per_sb_val[i];
				sfu->sfu_sb_id = sb->sb_id;
				sfu->sfu_sb_path =
					strdup(sb->sb_path ? sb->sb_path : "");
				sfu->sfu_total_bytes = sb->sb_bytes_max;
				sfu->sfu_used_bytes = __atomic_load_n(
					&sb->sb_bytes_used, __ATOMIC_RELAXED);
				sfu->sfu_free_bytes =
					sb->sb_bytes_max - sfu->sfu_used_bytes;
				sfu->sfu_total_files = sb->sb_inodes_max;
				sfu->sfu_used_files = __atomic_load_n(
					&sb->sb_inodes_used, __ATOMIC_RELAXED);
				sfu->sfu_free_files =
					sb->sb_inodes_max - sfu->sfu_used_files;
				i++;
			}
			rcu_read_unlock();
			resok->fur_per_sb.fur_per_sb_len = i;
		}
	}

	return 0;
}

static int probe1_op_nfs4_op_stats(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	NFS4_OP_STATS1res *res = ph->ph_res;
	NFS4_OP_STATS1resok *resok = &res->NFS4_OP_STATS1res_u.nosr_resok;

	struct server_state *ss = server_state_find();
	if (!ss) {
		res->nosr_status = PROBE1ERR_NOENT;
		return res->nosr_status;
	}

	resok->nosr_ops.nosr_ops_val = calloc(OP_MAX, sizeof(probe_nfs4_op1));
	if (!resok->nosr_ops.nosr_ops_val) {
		res->nosr_status = PROBE1ERR_NOMEM;
		server_state_put(ss);
		return res->nosr_status;
	}
	resok->nosr_ops.nosr_ops_len = OP_MAX;

	for (unsigned int i = 0; i < OP_MAX; i++) {
		probe_nfs4_op1 *pno = &resok->nosr_ops.nosr_ops_val[i];
		struct reffs_op_stats *s = &ss->ss_nfs4_op_stats[i];

		pno->pno_op = i;
		pno->pno_name = strdup(nfs4_op_name((nfs_opnum4)i));
		pno->pno_calls = atomic_load_explicit(&s->os_calls,
						      memory_order_relaxed);
		pno->pno_errors = atomic_load_explicit(&s->os_errors,
						       memory_order_relaxed);
		pno->pno_bytes_in = atomic_load_explicit(&s->os_bytes_in,
							 memory_order_relaxed);
		pno->pno_bytes_out = atomic_load_explicit(&s->os_bytes_out,
							  memory_order_relaxed);
		pno->pno_duration_total = atomic_load_explicit(
			&s->os_duration_total, memory_order_relaxed);
		pno->pno_duration_max = atomic_load_explicit(
			&s->os_duration_max, memory_order_relaxed);
	}

	server_state_put(ss);

	/* Per-sb NFS4 op stats breakdown. */
	struct cds_list_head *sb_list = super_block_list_head();
	struct super_block *sb;
	uint32_t sb_count = 0;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, sb_list, sb_link)
		sb_count++;
	rcu_read_unlock();

	if (sb_count > 0) {
		resok->nosr_per_sb.nosr_per_sb_val =
			calloc(sb_count, sizeof(probe_sb_nfs4_op_stats1));
		if (resok->nosr_per_sb.nosr_per_sb_val) {
			uint32_t si = 0;

			rcu_read_lock();
			cds_list_for_each_entry_rcu(sb, sb_list, sb_link) {
				if (si >= sb_count)
					break;
				probe_sb_nfs4_op_stats1 *sns =
					&resok->nosr_per_sb.nosr_per_sb_val[si];
				sns->sns_sb_id = sb->sb_id;
				sns->sns_sb_path =
					strdup(sb->sb_path ? sb->sb_path : "");
				sns->sns_ops.sns_ops_val =
					calloc(OP_MAX, sizeof(probe_nfs4_op1));
				if (!sns->sns_ops.sns_ops_val) {
					si++;
					continue;
				}
				sns->sns_ops.sns_ops_len = OP_MAX;
				for (unsigned int j = 0; j < OP_MAX; j++) {
					probe_nfs4_op1 *pno =
						&sns->sns_ops.sns_ops_val[j];
					struct reffs_op_stats *s =
						&sb->sb_nfs4_op_stats[j];
					pno->pno_op = j;
					pno->pno_name = strdup(
						nfs4_op_name((nfs_opnum4)j));
					pno->pno_calls = atomic_load_explicit(
						&s->os_calls,
						memory_order_relaxed);
					pno->pno_errors = atomic_load_explicit(
						&s->os_errors,
						memory_order_relaxed);
					pno->pno_bytes_in =
						atomic_load_explicit(
							&s->os_bytes_in,
							memory_order_relaxed);
					pno->pno_bytes_out =
						atomic_load_explicit(
							&s->os_bytes_out,
							memory_order_relaxed);
					pno->pno_duration_total =
						atomic_load_explicit(
							&s->os_duration_total,
							memory_order_relaxed);
					pno->pno_duration_max =
						atomic_load_explicit(
							&s->os_duration_max,
							memory_order_relaxed);
				}
				si++;
			}
			rcu_read_unlock();
			resok->nosr_per_sb.nosr_per_sb_len = si;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Layout error stats                                                  */
/* ------------------------------------------------------------------ */

static void fill_layout_error(probe_layout_error1 *ple, uint32_t id,
			      const char *name,
			      const struct reffs_layout_error_stats *les)
{
	ple->ple_id = id;
	ple->ple_name = strdup(name ? name : "");
	ple->ple_total =
		atomic_load_explicit(&les->les_total, memory_order_relaxed);
	ple->ple_access =
		atomic_load_explicit(&les->les_access, memory_order_relaxed);
	ple->ple_io = atomic_load_explicit(&les->les_io, memory_order_relaxed);
	ple->ple_other =
		atomic_load_explicit(&les->les_other, memory_order_relaxed);
}

static int probe1_op_layout_errors(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	LAYOUT_ERRORS1res *res = ph->ph_res;
	LAYOUT_ERRORS1resok *resok = &res->LAYOUT_ERRORS1res_u.ler_resok;

	struct server_state *ss = server_state_find();

	if (!ss) {
		res->ler_status = PROBE1ERR_NOENT;
		return res->ler_status;
	}

	/* Global aggregate. */
	fill_layout_error(&resok->ler_global, 0, "global",
			  &ss->ss_layout_errors);

	/* Per-dstore. */
	struct dstore *dstores[REFFS_CONFIG_MAX_DATA_SERVERS];
	uint32_t nds = dstore_collect_available(dstores,
						REFFS_CONFIG_MAX_DATA_SERVERS);

	if (nds > 0) {
		resok->ler_dstores.ler_dstores_val =
			calloc(nds, sizeof(probe_layout_error1));
		if (resok->ler_dstores.ler_dstores_val) {
			resok->ler_dstores.ler_dstores_len = nds;
			for (uint32_t i = 0; i < nds; i++) {
				fill_layout_error(
					&resok->ler_dstores.ler_dstores_val[i],
					dstores[i]->ds_id,
					dstores[i]->ds_address,
					&dstores[i]->ds_layout_errors);
			}
		}
		for (uint32_t i = 0; i < nds; i++)
			dstore_put(dstores[i]);
	}

	/* Per-client. */
	if (ss->ss_client_ht) {
		struct cds_lfht_iter iter;
		struct cds_lfht_node *node;
		uint32_t ncli = 0;

		/* Count clients first. */
		rcu_read_lock();
		cds_lfht_first(ss->ss_client_ht, &iter);
		while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
			ncli++;
			cds_lfht_next(ss->ss_client_ht, &iter);
		}
		rcu_read_unlock();

		if (ncli > 0) {
			resok->ler_clients.ler_clients_val =
				calloc(ncli, sizeof(probe_layout_error1));
			if (resok->ler_clients.ler_clients_val) {
				uint32_t idx = 0;

				rcu_read_lock();
				cds_lfht_first(ss->ss_client_ht, &iter);
				while ((node = cds_lfht_iter_get_node(&iter)) !=
					       NULL &&
				       idx < ncli) {
					struct client *c = caa_container_of(
						node, struct client, c_node);
					char label[32];

					snprintf(label, sizeof(label),
						 "slot=%" PRIu64, c->c_id);
					fill_layout_error(
						&resok->ler_clients
							 .ler_clients_val[idx],
						(uint32_t)c->c_id, label,
						c->c_layout_errors);
					idx++;
					cds_lfht_next(ss->ss_client_ht, &iter);
				}
				resok->ler_clients.ler_clients_len = idx;
				rcu_read_unlock();
			}
		}
	}

	server_state_put(ss);

	/*
	 * Per-sb layout errors — empty for now.
	 * NOT_NOW_BROWN_COW: requires sb_layout_errors field in
	 * struct super_block and recording in the LAYOUTERROR handler.
	 */
	resok->ler_sbs.ler_sbs_len = 0;
	resok->ler_sbs.ler_sbs_val = NULL;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Superblock management ops                                           */
/* ------------------------------------------------------------------ */

static void fill_sb_info(probe_sb_info1 *psi, const struct super_block *sb)
{
	psi->psi_id = sb->sb_id;
	psi->psi_path = strdup(sb->sb_path ? sb->sb_path : "");
	psi->psi_state = (probe_sb_lifecycle1)sb->sb_lifecycle;
	psi->psi_storage_type = (probe_storage_type1)sb->sb_storage_type;
	psi->psi_bytes_max = sb->sb_bytes_max;
	psi->psi_bytes_used =
		__atomic_load_n(&sb->sb_bytes_used, __ATOMIC_RELAXED);
	psi->psi_inodes_max = sb->sb_inodes_max;
	psi->psi_inodes_used =
		__atomic_load_n(&sb->sb_inodes_used, __ATOMIC_RELAXED);
	psi->psi_flavors.psi_flavors_len = sb->sb_nflavors;
	if (sb->sb_nflavors > 0) {
		psi->psi_flavors.psi_flavors_val =
			calloc(sb->sb_nflavors, sizeof(probe_auth_flavor1));
		if (psi->psi_flavors.psi_flavors_val) {
			for (unsigned int i = 0; i < sb->sb_nflavors; i++)
				psi->psi_flavors.psi_flavors_val[i] =
					(probe_auth_flavor1)sb->sb_flavors[i];
		}
	}
}

static int probe1_op_sb_list(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_LIST1res *res = ph->ph_res;
	SB_LIST1resok *resok = &res->SB_LIST1res_u.slr_resok;
	struct cds_list_head *sb_list = super_block_list_head();
	struct super_block *sb;

	/* Count sbs. */
	uint32_t count = 0;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, sb_list, sb_link)
		count++;
	rcu_read_unlock();

	if (count == 0)
		return 0;

	resok->slr_sbs.slr_sbs_val = calloc(count, sizeof(probe_sb_info1));
	if (!resok->slr_sbs.slr_sbs_val) {
		res->slr_status = PROBE1ERR_NOMEM;
		return res->slr_status;
	}

	uint32_t i = 0;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, sb_list, sb_link) {
		if (i >= count)
			break;
		fill_sb_info(&resok->slr_sbs.slr_sbs_val[i], sb);
		i++;
	}
	rcu_read_unlock();

	resok->slr_sbs.slr_sbs_len = i;
	return 0;
}

static int probe1_op_sb_create(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_CREATE1args *args = ph->ph_args;
	SB_CREATE1res *res = ph->ph_res;
	probe_sb_info1 *resok = &res->SB_CREATE1res_u.scr_resok;
	int ret;

	struct server_state *ss = server_state_find();

	/* Allocate a unique, monotonic sb_id. */
	uint64_t new_id = sb_registry_alloc_id(ss ? ss->ss_state_dir : NULL);
	if (new_id == 0) {
		server_state_put(ss);
		res->scr_status = PROBE1ERR_IO;
		return res->scr_status;
	}

	/* Ensure mount path exists (mkdir -p). */
	ret = reffs_fs_mkdir_p(args->sca_path, 0755);
	if (ret && ret != -EEXIST) {
		server_state_put(ss);
		res->scr_status = PROBE1ERR_IO;
		return res->scr_status;
	}

	struct super_block *sb = super_block_alloc(
		new_id, args->sca_path,
		(enum reffs_storage_type)args->sca_storage_type, NULL);
	if (!sb) {
		server_state_put(ss);
		res->scr_status = PROBE1ERR_NOMEM;
		return res->scr_status;
	}
	uuid_generate(sb->sb_uuid);

	ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	if (ret) {
		super_block_put(sb);
		server_state_put(ss);
		res->scr_status = PROBE1ERR_IO;
		return res->scr_status;
	}

	/* Persist the registry. */

	if (ss && ss->ss_state_dir)
		sb_registry_save(ss->ss_state_dir);
	server_state_put(ss);

	fill_sb_info(resok, sb);
	super_block_put(sb);
	return 0;
}

static int probe1_op_sb_mount(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_MOUNT1args *args = ph->ph_args;
	probe_stat1 *res = ph->ph_res;

	struct super_block *sb = super_block_find(args->sma_id);

	if (!sb) {
		*res = PROBE1ERR_NOENT;
		return *res;
	}

	int ret = super_block_mount(sb, args->sma_path);

	super_block_put(sb);

	if (ret) {
		switch (ret) {
		case -EBUSY:
			*res = PROBE1ERR_BUSY;
			break;
		case -ENOENT:
			*res = PROBE1ERR_NOENT;
			break;
		case -ENOTDIR:
			*res = PROBE1ERR_NOTDIR;
			break;
		default:
			*res = PROBE1ERR_INVAL;
			break;
		}
		return *res;
	}

	struct server_state *ss = server_state_find();

	if (ss && ss->ss_state_dir)
		sb_registry_save(ss->ss_state_dir);
	server_state_put(ss);

	return 0;
}

static int probe1_op_sb_unmount(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_UNMOUNT1args *args = ph->ph_args;
	probe_stat1 *res = ph->ph_res;

	struct super_block *sb = super_block_find(args->sua_id);

	if (!sb) {
		*res = PROBE1ERR_NOENT;
		return *res;
	}

	int ret = super_block_unmount(sb);

	super_block_put(sb);

	if (ret) {
		*res = (ret == -EBUSY) ? PROBE1ERR_BUSY :
		       (ret == -EPERM) ? PROBE1ERR_PERM :
					 PROBE1ERR_INVAL;
		return *res;
	}

	struct server_state *ss = server_state_find();

	if (ss && ss->ss_state_dir)
		sb_registry_save(ss->ss_state_dir);
	server_state_put(ss);

	return 0;
}

static int probe1_op_sb_destroy(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_DESTROY1args *args = ph->ph_args;
	probe_stat1 *res = ph->ph_res;

	struct super_block *sb = super_block_find(args->sda_id);

	if (!sb) {
		*res = PROBE1ERR_NOENT;
		return *res;
	}

	int ret = super_block_destroy(sb);

	if (ret) {
		super_block_put(sb);
		*res = (ret == -EBUSY) ? PROBE1ERR_BUSY :
		       (ret == -EPERM) ? PROBE1ERR_PERM :
					 PROBE1ERR_INVAL;
		return *res;
	}

	super_block_release_dirents(sb);
	super_block_put(sb);

	struct server_state *ss = server_state_find();

	if (ss && ss->ss_state_dir)
		sb_registry_save(ss->ss_state_dir);
	server_state_put(ss);

	return 0;
}

static int probe1_op_sb_get(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_GET1args *args = ph->ph_args;
	SB_GET1res *res = ph->ph_res;
	probe_sb_info1 *resok = &res->SB_GET1res_u.sgr_resok;

	struct super_block *sb = super_block_find(args->sga_id);

	if (!sb) {
		res->sgr_status = PROBE1ERR_NOENT;
		return res->sgr_status;
	}

	fill_sb_info(resok, sb);
	super_block_put(sb);
	return 0;
}

static int probe1_op_sb_set_flavors(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_SET_FLAVORS1args *args = ph->ph_args;
	probe_stat1 *res = ph->ph_res;

	struct super_block *sb = super_block_find(args->sfa_id);

	if (!sb) {
		*res = PROBE1ERR_NOENT;
		return *res;
	}

	enum reffs_auth_flavor flavors[REFFS_CONFIG_MAX_FLAVORS];
	unsigned int n = args->sfa_flavors.sfa_flavors_len;

	if (n > REFFS_CONFIG_MAX_FLAVORS)
		n = REFFS_CONFIG_MAX_FLAVORS;

	for (unsigned int i = 0; i < n; i++)
		flavors[i] = (enum reffs_auth_flavor)
				     args->sfa_flavors.sfa_flavors_val[i];

	super_block_set_flavors(sb, flavors, n);
	super_block_put(sb);
	return 0;
}

static int probe1_op_sb_lint_flavors(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_LINT_FLAVORS1res *res = ph->ph_res;
	SB_LINT_FLAVORS1resok *resok = &res->SB_LINT_FLAVORS1res_u.lfr_resok;

	resok->lfr_warnings = super_block_lint_flavors();
	/* NOT_NOW_BROWN_COW: collect lint messages into lfr_messages.
	 * Must not be NULL — xdr_string calls strlen on it. */
	resok->lfr_messages = strdup("");
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
	RPC_OPERATION_INIT(PROBEPROC1, NFS4_OP_STATS, NULL, NULL,
			   xdr_NFS4_OP_STATS1res, NFS4_OP_STATS1res,
			   probe1_op_nfs4_op_stats),
	RPC_OPERATION_INIT(PROBEPROC1, LAYOUT_ERRORS, NULL, NULL,
			   xdr_LAYOUT_ERRORS1res, LAYOUT_ERRORS1res,
			   probe1_op_layout_errors),
	RPC_OPERATION_INIT(PROBEPROC1, SB_LIST, NULL, NULL, xdr_SB_LIST1res,
			   SB_LIST1res, probe1_op_sb_list),
	RPC_OPERATION_INIT(PROBEPROC1, SB_CREATE, xdr_SB_CREATE1args,
			   SB_CREATE1args, xdr_SB_CREATE1res, SB_CREATE1res,
			   probe1_op_sb_create),
	RPC_OPERATION_INIT(PROBEPROC1, SB_MOUNT, xdr_SB_MOUNT1args,
			   SB_MOUNT1args, xdr_probe_stat1, probe_stat1,
			   probe1_op_sb_mount),
	RPC_OPERATION_INIT(PROBEPROC1, SB_UNMOUNT, xdr_SB_UNMOUNT1args,
			   SB_UNMOUNT1args, xdr_probe_stat1, probe_stat1,
			   probe1_op_sb_unmount),
	RPC_OPERATION_INIT(PROBEPROC1, SB_DESTROY, xdr_SB_DESTROY1args,
			   SB_DESTROY1args, xdr_probe_stat1, probe_stat1,
			   probe1_op_sb_destroy),
	RPC_OPERATION_INIT(PROBEPROC1, SB_GET, xdr_SB_GET1args, SB_GET1args,
			   xdr_SB_GET1res, SB_GET1res, probe1_op_sb_get),
	RPC_OPERATION_INIT(PROBEPROC1, SB_SET_FLAVORS, xdr_SB_SET_FLAVORS1args,
			   SB_SET_FLAVORS1args, xdr_probe_stat1, probe_stat1,
			   probe1_op_sb_set_flavors),
	RPC_OPERATION_INIT(PROBEPROC1, SB_LINT_FLAVORS, NULL, NULL,
			   xdr_SB_LINT_FLAVORS1res, SB_LINT_FLAVORS1res,
			   probe1_op_sb_lint_flavors),
};

static struct rpc_program_handler *probe1_handler;

volatile sig_atomic_t probev1_registered = 0;

int probe1_protocol_register(void)
{
	if (probev1_registered)
		return 0;

	/* Verify that the generated enum matches the manually defined one */
	static_assert((int)PROBE1_TRACE_CAT_GENERAL ==
			      (int)REFFS_TRACE_CAT_GENERAL,
		      "REFFS_TRACE_CAT_GENERAL out of sync");
	static_assert((int)PROBE1_TRACE_CAT_IO == (int)REFFS_TRACE_CAT_IO,
		      "REFFS_TRACE_CAT_IO out of sync");
	static_assert((int)PROBE1_TRACE_CAT_RPC == (int)REFFS_TRACE_CAT_RPC,
		      "REFFS_TRACE_CAT_RPC out of sync");
	static_assert((int)PROBE1_TRACE_CAT_NFS == (int)REFFS_TRACE_CAT_NFS,
		      "REFFS_TRACE_CAT_NFS out of sync");
	static_assert((int)PROBE1_TRACE_CAT_NLM == (int)REFFS_TRACE_CAT_NLM,
		      "REFFS_TRACE_CAT_NLM out of sync");
	static_assert((int)PROBE1_TRACE_CAT_FS == (int)REFFS_TRACE_CAT_FS,
		      "REFFS_TRACE_CAT_FS out of sync");
	static_assert((int)PROBE1_TRACE_CAT_LOG == (int)REFFS_TRACE_CAT_LOG,
		      "REFFS_TRACE_CAT_LOG out of sync");
	static_assert((int)PROBE1_TRACE_CAT_SECURITY ==
			      (int)REFFS_TRACE_CAT_SECURITY,
		      "REFFS_TRACE_CAT_SECURITY out of sync");
	static_assert((int)PROBE1_TRACE_CAT_ALL == (int)REFFS_TRACE_CAT_ALL,
		      "REFFS_TRACE_CAT_ALL out of sync");

	static_assert((enum op_type)PROBE1_OP_TYPE_ALL == OP_TYPE_ALL,
		      "Enum values are out of sync between header and XDR");

	/* SB management enum sync checks. */
	static_assert((int)PROBE1_STORAGE_RAM == (int)REFFS_STORAGE_RAM,
		      "REFFS_STORAGE_RAM out of sync");
	static_assert((int)PROBE1_STORAGE_POSIX == (int)REFFS_STORAGE_POSIX,
		      "REFFS_STORAGE_POSIX out of sync");
	static_assert((int)PROBE1_SB_CREATED == (int)SB_CREATED,
		      "SB_CREATED out of sync");
	static_assert((int)PROBE1_SB_DESTROYED == (int)SB_DESTROYED,
		      "SB_DESTROYED out of sync");
	static_assert((int)PROBE1_AUTH_SYS == (int)REFFS_AUTH_SYS,
		      "REFFS_AUTH_SYS out of sync");
	static_assert((int)PROBE1_AUTH_KRB5 == (int)REFFS_AUTH_KRB5,
		      "REFFS_AUTH_KRB5 out of sync");

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
