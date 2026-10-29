/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * One-shot NFSv4 error injection on one file (test trigger).
 *
 * A single slot under ei_mutex.  ei_armed lets every dispatched op skip
 * the mutex while nothing is armed; matching and disarming happen
 * together under the mutex, so concurrent compounds fire it once.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/client.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/super_block.h"
#include "reffs/trace/common.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/error_inject.h"

static pthread_mutex_t ei_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct error_inject_info ei_info;
static _Atomic bool ei_armed;

static uint64_t ei_realtime_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static const char *ei_status_name(nfsstat4 st)
{
	const char *name = st ? nfs4_err_name(st) : "NFS4_OK";

	return name ? name : "?";
}

static const char *ei_op_name(uint32_t op)
{
	const char *name = nfs4_op_name(op);

	return name ? name : "?";
}

/* Ops that only set the current FH or the session never match "any". */
static bool ei_op_is_fh_setter(uint32_t op)
{
	switch (op) {
	case OP_SEQUENCE:
	case OP_PUTFH:
	case OP_PUTPUBFH:
	case OP_PUTROOTFH:
	case OP_SAVEFH:
	case OP_RESTOREFH:
		return true;
	default:
		return false;
	}
}

static bool ei_op_selected_locked(uint32_t op)
{
	const struct error_inject_params *p = &ei_info.eii_params;

	if (!p->eip_nops)
		return !ei_op_is_fh_setter(op);
	for (uint32_t i = 0; i < p->eip_nops; i++)
		if (p->eip_ops[i] == op)
			return true;
	return false;
}

static bool ei_client_matches_locked(const struct compound *compound)
{
	const struct error_inject_params *p = &ei_info.eii_params;
	struct nfs4_client *nc = compound->c_nfs4_client;

	if (!p->eip_clientid && !p->eip_addr)
		return true;
	if (!nc)
		return false;
	if (p->eip_clientid)
		return (clientid4)nfs4_client_to_client(nc)->c_id ==
		       p->eip_clientid;
	return nc->nc_sin.sin_addr.s_addr == p->eip_addr;
}

static void ei_ops_string(const struct error_inject_params *p, char *buf,
			  size_t len)
{
	size_t off = 0;

	if (!p->eip_nops) {
		snprintf(buf, len, "any");
		return;
	}
	buf[0] = '\0';
	for (uint32_t i = 0; i < p->eip_nops && off < len; i++) {
		int n = snprintf(buf + off, len - off, "%s%s", i ? "," : "",
				 ei_op_name(p->eip_ops[i]));

		if (n < 0)
			break;
		off += (size_t)n;
	}
}

int error_inject_arm(const struct error_inject_params *params, uint32_t *id_out)
{
	struct error_inject_params p = *params;
	char ops[512];
	char addr[INET_ADDRSTRLEN] = "any";
	struct in_addr in = { .s_addr = params->eip_addr };
	uint32_t id;

	if (!p.eip_sb_id || !p.eip_ino || p.eip_nops > ERROR_INJECT_MAX_OPS)
		return -EINVAL;
	for (uint32_t i = 0; i < p.eip_nops; i++)
		if (p.eip_ops[i] >= OP_MAX)
			return -EINVAL;
	if (!p.eip_status)
		p.eip_status = NFS4ERR_BADHANDLE;

	ei_ops_string(&p, ops, sizeof(ops));
	if (p.eip_addr)
		inet_ntop(AF_INET, &in, addr, sizeof(addr));

	pthread_mutex_lock(&ei_mutex);
	id = ei_info.eii_id + 1;
	memset(&ei_info, 0, sizeof(ei_info));
	ei_info.eii_id = id;
	ei_info.eii_state = ERROR_INJECT_ARMED;
	ei_info.eii_params = p;
	ei_info.eii_armed_ns = ei_realtime_ns();
	atomic_store_explicit(&ei_armed, true, memory_order_release);
	pthread_mutex_unlock(&ei_mutex);

	reffs_trace_event(REFFS_TRACE_CAT_NFS, __func__, __LINE__,
			  "errinject: id=%u ARM clid=0x%016" PRIx64
			  " addr=%s sb=%" PRIu64 " ino=%" PRIu64
			  " ops=%s status=%s(%u)",
			  id, (uint64_t)p.eip_clientid, addr, p.eip_sb_id,
			  p.eip_ino, ops, ei_status_name(p.eip_status),
			  p.eip_status);
	if (id_out)
		*id_out = id;
	return 0;
}

int error_inject_disarm(void)
{
	uint32_t id;

	pthread_mutex_lock(&ei_mutex);
	if (ei_info.eii_state != ERROR_INJECT_ARMED) {
		pthread_mutex_unlock(&ei_mutex);
		return -ENOENT;
	}
	ei_info.eii_state = ERROR_INJECT_DISARMED;
	atomic_store_explicit(&ei_armed, false, memory_order_release);
	id = ei_info.eii_id;
	pthread_mutex_unlock(&ei_mutex);

	reffs_trace_event(REFFS_TRACE_CAT_NFS, __func__, __LINE__,
			  "errinject: id=%u DISARM", id);
	return 0;
}

int error_inject_status(struct error_inject_info *out)
{
	pthread_mutex_lock(&ei_mutex);
	*out = ei_info;
	pthread_mutex_unlock(&ei_mutex);
	return out->eii_id ? 0 : -ENOENT;
}

nfsstat4 error_inject_check(struct compound *compound, uint32_t op)
{
	struct inode *inode = compound->c_inode;
	struct error_inject_info fired;

	if (!atomic_load_explicit(&ei_armed, memory_order_acquire))
		return NFS4_OK;
	if (!inode || !inode->i_sb)
		return NFS4_OK;

	pthread_mutex_lock(&ei_mutex);
	if (ei_info.eii_state != ERROR_INJECT_ARMED ||
	    inode->i_sb->sb_id != ei_info.eii_params.eip_sb_id ||
	    inode->i_ino != ei_info.eii_params.eip_ino ||
	    !ei_client_matches_locked(compound)) {
		pthread_mutex_unlock(&ei_mutex);
		return NFS4_OK;
	}
	if (!ei_op_selected_locked(op)) {
		if (!ei_op_is_fh_setter(op))
			ei_info.eii_skipped++;
		pthread_mutex_unlock(&ei_mutex);
		return NFS4_OK;
	}

	ei_info.eii_state = ERROR_INJECT_FIRED;
	ei_info.eii_fired_ns = ei_realtime_ns();
	ei_info.eii_fired_op = op;
	ei_info.eii_fired_opidx = compound->c_curr_op;
	ei_info.eii_fired_xid =
		compound->c_rt ? compound->c_rt->rt_info.ri_xid : 0;
	ei_info.eii_fired_clientid = compound->c_nfs4_client ?
					     (clientid4)nfs4_client_to_client(
						     compound->c_nfs4_client)
						     ->c_id :
					     0;
	atomic_store_explicit(&ei_armed, false, memory_order_release);
	fired = ei_info;
	pthread_mutex_unlock(&ei_mutex);

	reffs_trace_event(REFFS_TRACE_CAT_NFS, __func__, __LINE__,
			  "errinject: id=%u FIRE op=%s(%u) opidx=%u xid=0x%08x"
			  " clid=0x%016" PRIx64 " sb=%" PRIu64 " ino=%" PRIu64
			  " status=%s(%u) skipped=%u",
			  fired.eii_id, ei_op_name(op), op,
			  fired.eii_fired_opidx, fired.eii_fired_xid,
			  (uint64_t)fired.eii_fired_clientid,
			  fired.eii_params.eip_sb_id, fired.eii_params.eip_ino,
			  ei_status_name(fired.eii_params.eip_status),
			  fired.eii_params.eip_status, fired.eii_skipped);
	LOG("errinject: id=%u fired %s on sb=%" PRIu64 " ino=%" PRIu64
	    " clid=0x%016" PRIx64 " xid=0x%08x -> %s",
	    fired.eii_id, ei_op_name(op), fired.eii_params.eip_sb_id,
	    fired.eii_params.eip_ino, (uint64_t)fired.eii_fired_clientid,
	    fired.eii_fired_xid, ei_status_name(fired.eii_params.eip_status));
	return fired.eii_params.eip_status;
}
