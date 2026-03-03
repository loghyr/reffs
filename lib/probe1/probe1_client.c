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
#include <sys/statvfs.h>
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

static void format_size_c(char *buf, size_t buf_len, int64_t size, bool human)
{
	if (!human) {
		snprintf(buf, buf_len, "%ld", size);
		return;
	}

	const char *units[] = { "", "K", "M", "G", "T", "P" };
	int i = 0;
	double d_size = (double)size;
	const char *prefix = "";

	if (d_size < 0) {
		prefix = "-";
		d_size = -d_size;
	}

	while (d_size >= 1024 && i < 5) {
		d_size /= 1024;
		i++;
	}

	if (i == 0) {
		snprintf(buf, buf_len, "%s%ld", prefix, (long)d_size);
	} else {
		snprintf(buf, buf_len, "%s%.2f%s", prefix, d_size, units[i]);
	}
}

static int fs_usage_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	FS_USAGE1args *args = ph->ph_args;
	FS_USAGE1res *res = ph->ph_res;
	FS_USAGE1resok *resok = &res->FS_USAGE1res_u.fur_resok;

	if (res->fur_status) {
		LOG("error = %d", res->fur_status);
	} else {
		struct statvfs sv;
		bool have_mount = false;
		if (args->fua_mount_path && strlen(args->fua_mount_path) > 0) {
			if (statvfs(args->fua_mount_path, &sv) == 0) {
				have_mount = true;
			} else {
				LOG("Warning: Could not get stats for mount path %s: %s",
				    args->fua_mount_path, strerror(errno));
			}
		}

		char r_total[32], r_used[32], r_free[32];
		format_size_c(r_total, sizeof(r_total), resok->fur_total_bytes,
			      args->fua_human_readable);
		format_size_c(r_used, sizeof(r_used), resok->fur_used_bytes,
			      args->fua_human_readable);
		format_size_c(r_free, sizeof(r_free), resok->fur_free_bytes,
			      args->fua_human_readable);

		if (have_mount) {
			char m_total[32], m_used[32], m_free[32];
			char d_total[32], d_used[32], d_free[32];

			uint64_t ms_total = (uint64_t)sv.f_blocks * sv.f_frsize;
			uint64_t ms_free = (uint64_t)sv.f_bavail * sv.f_frsize;
			uint64_t ms_used =
				((uint64_t)sv.f_blocks - (uint64_t)sv.f_bfree) *
				sv.f_frsize;

			format_size_c(m_total, sizeof(m_total), ms_total,
				      args->fua_human_readable);
			format_size_c(m_used, sizeof(m_used), ms_used,
				      args->fua_human_readable);
			format_size_c(m_free, sizeof(m_free), ms_free,
				      args->fua_human_readable);

			format_size_c(d_total, sizeof(d_total),
				      resok->fur_total_bytes - ms_total,
				      args->fua_human_readable);
			format_size_c(d_used, sizeof(d_used),
				      resok->fur_used_bytes - ms_used,
				      args->fua_human_readable);
			format_size_c(d_free, sizeof(d_free),
				      resok->fur_free_bytes - ms_free,
				      args->fua_human_readable);

			LOG("%-15s %20s %20s %15s", "Metric", "Server (RPC)",
			    "Mount (statvfs)", "Diff");
			LOG("-------------------------------------------------------------------------");
			LOG("%-15s %20s %20s %15s", "Total Bytes", r_total,
			    m_total, d_total);
			LOG("%-15s %20s %20s %15s", "Used Bytes", r_used,
			    m_used, d_used);
			LOG("%-15s %20s %20s %15s", "Free Bytes", r_free,
			    m_free, d_free);

			LOG("%-15s %20lu %20lu %15ld", "Total Inodes",
			    resok->fur_total_files, (uint64_t)sv.f_files,
			    (long)resok->fur_total_files - (long)sv.f_files);
			LOG("%-15s %20lu %20lu %15ld", "Used Inodes",
			    resok->fur_used_files,
			    (uint64_t)sv.f_files - (uint64_t)sv.f_ffree,
			    (long)resok->fur_used_files -
				    (long)(sv.f_files - sv.f_ffree));
			LOG("%-15s %20lu %20lu %15ld", "Free Inodes",
			    resok->fur_free_files, (uint64_t)sv.f_ffree,
			    (long)resok->fur_free_files - (long)sv.f_ffree);
		} else {
			LOG("Filesystem Usage:");
			LOG("  Bytes: total=%s, used=%s, free=%s", r_total,
			    r_used, r_free);
			LOG("  Inodes: total=%lu, used=%lu, free=%lu",
			    resok->fur_total_files, resok->fur_used_files,
			    resok->fur_free_files);
		}
	}

	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_fs_usage(bool human, const char *path)
{
	int ret;

	struct rpc_trans *rt = rpc_trans_create();
	if (!rt)
		return NULL;

	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_FS_USAGE;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}

	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	FS_USAGE1args *args = ph->ph_args;
	args->fua_human_readable = human;
	args->fua_mount_path = path ? strdup(path) : strdup("");

	rt->rt_cb = fs_usage_cb;

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
