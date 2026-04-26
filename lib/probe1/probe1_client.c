/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>
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
#include "reffs/super_block.h"
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

	LOG("count=%" PRIu64 " replied=%" PRIu64 " rejected=%" PRIu64
	    " accepted=%" PRIu64 " authed=%" PRIu64,
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
		LOG("%3u %15s %10" PRIu64 " %16" PRIu64 " %16" PRIu64
		    " %16" PRIu64 " %16" PRIu64,
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
		LOG("created=%" PRIu64 " freed=%" PRIu64
		    " active_cancelled=%" PRIu64 " active_destroyed=%" PRIu64
		    " cancelled_freed=%" PRIu64 " destroyed_freed=%" PRIu64,
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
		snprintf(buf, buf_len, "%" PRId64, size);
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
	FS_USAGE1res *res = ph->ph_res;
	FS_USAGE1resok *resok = &res->FS_USAGE1res_u.fur_resok;

	if (res->fur_status) {
		LOG("error = %d", res->fur_status);
	} else {
		struct statvfs sv;
		bool have_mount = false;
		if (ph->ph_path && strlen(ph->ph_path) > 0) {
			if (statvfs(ph->ph_path, &sv) == 0) {
				have_mount = true;
			} else {
				LOG("Warning: Could not get stats for mount path %s: %s",
				    ph->ph_path, strerror(errno));
			}
		}

		char r_total[32], r_used[32], r_free[32];
		format_size_c(r_total, sizeof(r_total), resok->fur_total_bytes,
			      ph->ph_human);
		format_size_c(r_used, sizeof(r_used), resok->fur_used_bytes,
			      ph->ph_human);
		format_size_c(r_free, sizeof(r_free), resok->fur_free_bytes,
			      ph->ph_human);

		if (have_mount) {
			char m_total[32], m_used[32], m_free[32];
			char d_total[32], d_used[32], d_free[32];

			uint64_t ms_total = (uint64_t)sv.f_blocks * sv.f_frsize;
			uint64_t ms_free = (uint64_t)sv.f_bavail * sv.f_frsize;
			uint64_t ms_used =
				((uint64_t)sv.f_blocks - (uint64_t)sv.f_bfree) *
				sv.f_frsize;

			format_size_c(m_total, sizeof(m_total), ms_total,
				      ph->ph_human);
			format_size_c(m_used, sizeof(m_used), ms_used,
				      ph->ph_human);
			format_size_c(m_free, sizeof(m_free), ms_free,
				      ph->ph_human);

			format_size_c(d_total, sizeof(d_total),
				      resok->fur_total_bytes - ms_total,
				      ph->ph_human);
			format_size_c(d_used, sizeof(d_used),
				      resok->fur_used_bytes - ms_used,
				      ph->ph_human);
			format_size_c(d_free, sizeof(d_free),
				      resok->fur_free_bytes - ms_free,
				      ph->ph_human);

			LOG("%-15s %20s %20s %15s", "Metric", "Server (RPC)",
			    "Mount (statvfs)", "Diff");
			LOG("-------------------------------------------------------------------------");
			LOG("%-15s %20s %20s %15s", "Total Bytes", r_total,
			    m_total, d_total);
			LOG("%-15s %20s %20s %15s", "Used Bytes", r_used,
			    m_used, d_used);
			LOG("%-15s %20s %20s %15s", "Free Bytes", r_free,
			    m_free, d_free);

			LOG("%-15s %20" PRIu64 " %20" PRIu64 " %15ld",
			    "Total Inodes", resok->fur_total_files,
			    (uint64_t)sv.f_files,
			    (long)resok->fur_total_files - (long)sv.f_files);
			LOG("%-15s %20" PRIu64 " %20" PRIu64 " %15ld",
			    "Used Inodes", resok->fur_used_files,
			    (uint64_t)sv.f_files - (uint64_t)sv.f_ffree,
			    (long)resok->fur_used_files -
				    (long)(sv.f_files - (uint64_t)sv.f_ffree));
			LOG("%-15s %20" PRIu64 " %20" PRIu64 " %15ld",
			    "Free Inodes", resok->fur_free_files,
			    (uint64_t)sv.f_ffree,
			    (long)resok->fur_free_files - (long)sv.f_ffree);
		} else {
			LOG("Filesystem Usage:");
			LOG("  Bytes: total=%s, used=%s, free=%s", r_total,
			    r_used, r_free);
			LOG("  Inodes: total=%" PRIu64 ", used=%" PRIu64
			    ", free=%" PRIu64,
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
	ph->ph_human = human;
	ph->ph_path = path ? strdup(path) : strdup("");

	rt->rt_cb = fs_usage_cb;

	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		rt = NULL;
	}

	return rt;
}

static int nfs4_op_stats_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	NFS4_OP_STATS1res *res = ph->ph_res;
	NFS4_OP_STATS1resok *resok = &res->NFS4_OP_STATS1res_u.nosr_resok;

	if (res->nosr_status) {
		LOG("error = %d", res->nosr_status);
		goto done;
	}

	LOG("\n%3s %25s %10s %10s %12s %12s %16s %16s", "OP", "Name", "Calls",
	    "Errors", "BytesIn", "BytesOut", "TotalDur(ns)", "MaxDur(ns)");

	for (uint32_t i = 0; i < resok->nosr_ops.nosr_ops_len; i++) {
		probe_nfs4_op1 *pno = &resok->nosr_ops.nosr_ops_val[i];
		if (!pno->pno_calls)
			continue;
		LOG("%3u %25s %10" PRIu64 " %10" PRIu64 " %12" PRIu64
		    " %12" PRIu64 " %16" PRIu64 " %16" PRIu64,
		    pno->pno_op, pno->pno_name, pno->pno_calls, pno->pno_errors,
		    pno->pno_bytes_in, pno->pno_bytes_out,
		    pno->pno_duration_total, pno->pno_duration_max);
	}

done:
	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_nfs4_op_stats(void)
{
	int ret;

	struct rpc_trans *rt = rpc_trans_create();
	if (!rt)
		return NULL;

	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_NFS4_OP_STATS;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}

	rt->rt_cb = nfs4_op_stats_cb;

	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		rt = NULL;
	}

	return rt;
}

/* ------------------------------------------------------------------ */
/* Superblock management client ops                                    */
/* ------------------------------------------------------------------ */

static const char *lifecycle_name(probe_sb_lifecycle1 s)
{
	switch (s) {
	case PROBE1_SB_CREATED:
		return "CREATED";
	case PROBE1_SB_MOUNTED:
		return "MOUNTED";
	case PROBE1_SB_UNMOUNTED:
		return "UNMOUNTED";
	case PROBE1_SB_DESTROYED:
		return "DESTROYED";
	}
	return "UNKNOWN";
}

static void print_sb_info(const probe_sb_info1 *psi)
{
	LOG("  id=%lu path=%s state=%s storage=%u flavors=%u "
	    "bytes=%lu/%lu inodes=%lu/%lu",
	    (unsigned long)psi->psi_id, psi->psi_path ? psi->psi_path : "",
	    lifecycle_name(psi->psi_state), psi->psi_storage_type,
	    psi->psi_flavors.psi_flavors_len,
	    (unsigned long)psi->psi_bytes_used,
	    (unsigned long)psi->psi_bytes_max,
	    (unsigned long)psi->psi_inodes_used,
	    (unsigned long)psi->psi_inodes_max);
}

static int sb_list_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_LIST1res *res = ph->ph_res;

	if (res->slr_status) {
		LOG("sb-list error = %d", res->slr_status);
	} else {
		SB_LIST1resok *resok = &res->SB_LIST1res_u.slr_resok;

		LOG("Superblocks (%u):", resok->slr_sbs.slr_sbs_len);
		for (uint32_t i = 0; i < resok->slr_sbs.slr_sbs_len; i++)
			print_sb_info(&resok->slr_sbs.slr_sbs_val[i]);
	}
	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_sb_list(void)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_SB_LIST;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	rt->rt_cb = sb_list_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

static int sb_create_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_CREATE1res *res = ph->ph_res;

	if (res->scr_status)
		LOG("sb-create error = %d", res->scr_status);
	else
		print_sb_info(&res->SB_CREATE1res_u.scr_resok);
	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_sb_create(const char *path,
					     uint32_t storage_type)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_SB_CREATE;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_CREATE1args *args = ph->ph_args;

	args->sca_path = strdup(path);
	args->sca_storage_type = (probe_storage_type1)storage_type;

	rt->rt_cb = sb_create_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

static int sb_stat_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	probe_stat1 *res = ph->ph_res;

	if (*res)
		LOG("error = %d", *res);
	else
		LOG("OK");
	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_sb_mount(uint64_t id, const char *path)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_SB_MOUNT;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_MOUNT1args *args = ph->ph_args;

	args->sma_id = id;
	args->sma_path = strdup(path);

	rt->rt_cb = sb_stat_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

struct rpc_trans *probe1_client_op_sb_unmount(uint64_t id)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_SB_UNMOUNT;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_UNMOUNT1args *args = ph->ph_args;

	args->sua_id = id;

	rt->rt_cb = sb_stat_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

struct rpc_trans *probe1_client_op_sb_destroy(uint64_t id)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_SB_DESTROY;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_DESTROY1args *args = ph->ph_args;

	args->sda_id = id;

	rt->rt_cb = sb_stat_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

static int sb_get_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_GET1res *res = ph->ph_res;

	if (res->sgr_status)
		LOG("sb-get error = %d", res->sgr_status);
	else
		print_sb_info(&res->SB_GET1res_u.sgr_resok);
	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_sb_get(uint64_t id)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_SB_GET;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_GET1args *args = ph->ph_args;

	args->sga_id = id;

	rt->rt_cb = sb_get_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

static int inode_layout_list_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	INODE_LAYOUT_LIST1res *res = ph->ph_res;

	if (res->ill_status) {
		LOG("inode-layout-list error = %d", res->ill_status);
	} else {
		INODE_LAYOUT_LIST1resok *resok =
			&res->INODE_LAYOUT_LIST1res_u.ill_resok;
		printf("lss_gen=%" PRIu64 " mirrors=%u\n", resok->ill_lss_gen,
		       resok->ill_mirrors.ill_mirrors_len);
		for (uint32_t i = 0; i < resok->ill_mirrors.ill_mirrors_len;
		     i++) {
			probe_layout_mirror1 *m =
				&resok->ill_mirrors.ill_mirrors_val[i];
			printf("  [%u] dstore=%u fh_len=%u size=%" PRId64
			       " mtime=%" PRId64 ".%09u\n",
			       i, m->plm_dstore_id, m->plm_ds_fh.plm_ds_fh_len,
			       m->plm_size, m->plm_mtime_sec,
			       m->plm_mtime_nsec);
		}
	}
	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_inode_layout_list(uint64_t sb_id,
						     uint64_t inum)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_INODE_LAYOUT_LIST;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	INODE_LAYOUT_LIST1args *args = ph->ph_args;

	args->ill_sb_id = sb_id;
	args->ill_inum = inum;

	rt->rt_cb = inode_layout_list_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

/* Slice B: dstore lifecycle ops. */

static const char *dstore_state_name(probe_dstore_state1 s)
{
	switch (s) {
	case PROBE1_DSTORE_ALIVE:
		return "ALIVE";
	case PROBE1_DSTORE_DRAINING:
		return "DRAINING";
	case PROBE1_DSTORE_DRAINED:
		return "DRAINED";
	case PROBE1_DSTORE_LOST:
		return "LOST";
	case PROBE1_DSTORE_DESTROYED:
		return "DESTROYED";
	}
	return "?";
}

static int dstore_list_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	DSTORE_LIST1res *res = ph->ph_res;

	if (res->dlr_status) {
		LOG("dstore-list error = %d", res->dlr_status);
	} else {
		DSTORE_LIST1resok *resok = &res->DSTORE_LIST1res_u.dlr_resok;

		printf("%-4s %-30s %-30s %-10s %-7s %-5s %-10s %s\n", "ID",
		       "ADDRESS", "PATH", "STATE", "DRAINED", "LOST",
		       "INSTANCES", "RUNWAY");
		for (uint32_t i = 0; i < resok->dlr_dstores.dlr_dstores_len;
		     i++) {
			probe_dstore_info1 *d =
				&resok->dlr_dstores.dlr_dstores_val[i];
			printf("%-4u %-30s %-30s %-10s %-7s %-5s %-10" PRIu64
			       " %u\n",
			       d->pdi_id, d->pdi_address ? d->pdi_address : "",
			       d->pdi_path ? d->pdi_path : "",
			       dstore_state_name(d->pdi_state),
			       d->pdi_drained ? "yes" : "no",
			       d->pdi_lost ? "yes" : "no",
			       d->pdi_instance_count, d->pdi_runway_capacity);
		}
	}
	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_dstore_list(void)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_DSTORE_LIST;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}

	rt->rt_cb = dstore_list_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

static int dstore_drain_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	probe_stat1 *res = ph->ph_res;

	if (*res)
		LOG("dstore-drain error = %d", *res);
	else
		printf("OK\n");
	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_dstore_drain(uint32_t dstore_id)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_DSTORE_DRAIN;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	DSTORE_DRAIN1args *args = ph->ph_args;

	args->dda_id = dstore_id;
	rt->rt_cb = dstore_drain_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

struct rpc_trans *probe1_client_op_dstore_undrain(uint32_t dstore_id)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_DSTORE_UNDRAIN;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	DSTORE_UNDRAIN1args *args = ph->ph_args;

	args->dua_id = dstore_id;
	/* Reuse drain_cb: same status-only response shape. */
	rt->rt_cb = dstore_drain_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

struct rpc_trans *probe1_client_op_sb_set_flavors(uint64_t id,
						  uint32_t *flavors,
						  uint32_t nflavors)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_SB_SET_FLAVORS;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_SET_FLAVORS1args *args = ph->ph_args;

	args->sfa_id = id;
	args->sfa_flavors.sfa_flavors_len = nflavors;
	args->sfa_flavors.sfa_flavors_val =
		calloc(nflavors, sizeof(probe_auth_flavor1));
	if (args->sfa_flavors.sfa_flavors_val)
		memcpy(args->sfa_flavors.sfa_flavors_val, flavors,
		       nflavors * sizeof(probe_auth_flavor1));

	rt->rt_cb = sb_stat_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

static int sb_lint_cb(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_LINT_FLAVORS1res *res = ph->ph_res;

	if (res->lfr_status)
		LOG("sb-lint-flavors error = %d", res->lfr_status);
	else
		LOG("lint-flavors: %u warnings",
		    res->SB_LINT_FLAVORS1res_u.lfr_resok.lfr_warnings);
	io_handler_stop();
	return 0;
}

struct rpc_trans *probe1_client_op_sb_lint_flavors(void)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_SB_LINT_FLAVORS;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	rt->rt_cb = sb_lint_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

struct rpc_trans *probe1_client_op_sb_set_client_rules(
	uint64_t id, const struct sb_client_rule *rules, unsigned int nrules)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_SB_SET_CLIENT_RULES;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_SET_CLIENT_RULES1args *args = ph->ph_args;

	args->scra_id = id;
	if (nrules > 0) {
		args->scra_rules.scra_rules_val =
			calloc(nrules, sizeof(probe_client_rule1));
		if (!args->scra_rules.scra_rules_val) {
			rpc_protocol_free(rt);
			return NULL;
		}
		args->scra_rules.scra_rules_len = nrules;
		for (unsigned int i = 0; i < nrules; i++) {
			const struct sb_client_rule *r = &rules[i];
			probe_client_rule1 *pr =
				&args->scra_rules.scra_rules_val[i];
			pr->pcr_match = strdup(r->scr_match);
			pr->pcr_rw = r->scr_rw;
			pr->pcr_root_squash = r->scr_root_squash;
			pr->pcr_all_squash = r->scr_all_squash;
			if (r->scr_nflavors > 0) {
				pr->pcr_flavors.pcr_flavors_val =
					calloc(r->scr_nflavors,
					       sizeof(probe_auth_flavor1));
				if (pr->pcr_flavors.pcr_flavors_val) {
					pr->pcr_flavors.pcr_flavors_len =
						r->scr_nflavors;
					for (unsigned int j = 0;
					     j < r->scr_nflavors; j++)
						pr->pcr_flavors
							.pcr_flavors_val[j] =
							(probe_auth_flavor1)r
								->scr_flavors[j];
				}
			}
		}
	}

	rt->rt_cb = sb_stat_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

struct rpc_trans *probe1_client_op_sb_set_dstores(uint64_t id,
						  const uint32_t *dstore_ids,
						  uint32_t ndstores)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_SB_SET_DSTORES;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_SET_DSTORES1args *args = ph->ph_args;

	args->sda_id = id;
	if (ndstores > 16)
		ndstores = 16;
	args->sda_dstore_ids.sda_dstore_ids_val =
		calloc(ndstores, sizeof(uint32_t));
	if (!args->sda_dstore_ids.sda_dstore_ids_val) {
		rpc_protocol_free(rt);
		return NULL;
	}
	args->sda_dstore_ids.sda_dstore_ids_len = ndstores;
	for (uint32_t i = 0; i < ndstores; i++)
		args->sda_dstore_ids.sda_dstore_ids_val[i] = dstore_ids[i];

	rt->rt_cb = sb_stat_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
	}
	return rt;
}

struct rpc_trans *probe1_client_op_sb_set_stripe_unit(uint64_t id,
						      uint32_t stripe_unit)
{
	int ret;
	struct rpc_trans *rt = rpc_trans_create();

	if (!rt)
		return NULL;
	rt->rt_info.ri_program = PROBE_PROGRAM;
	rt->rt_info.ri_version = PROBE_V1;
	rt->rt_info.ri_procedure = PROBEPROC1_SB_SET_STRIPE_UNIT;

	ret = rpc_protocol_allocate_call(rt);
	if (ret) {
		rpc_protocol_free(rt);
		return NULL;
	}
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	SB_SET_STRIPE_UNIT1args *args = ph->ph_args;

	args->ssu_id = id;
	args->ssu_stripe_unit = stripe_unit;

	rt->rt_cb = sb_stat_cb;
	if (rpc_prepare_send_call(rt)) {
		rpc_protocol_free(rt);
		return NULL;
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
