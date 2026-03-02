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
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "nlm_prot.h"
#include "nlm4_prot.h"

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/nlm.h"
#include "reffs/nsm.h"
#include "reffs/nlm_lock.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/filehandle.h"

static struct inode *nlm4_fh_to_inode(netobj *fh, struct super_block **sb_out)
{
	struct network_file_handle *nfh;
	struct super_block *sb;
	struct inode *inode;

	if (fh->n_len != sizeof(struct network_file_handle)) {
		TRACE("NLM4: FH length mismatch: expected %lu, got %u",
		      sizeof(struct network_file_handle), fh->n_len);
		return NULL;
	}

	nfh = (struct network_file_handle *)fh->n_bytes;
	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		TRACE("NLM4: SB %lu not found", nfh->nfh_sb);
		return NULL;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		TRACE("NLM4: Inode %lu not found in SB %lu", nfh->nfh_ino,
		      nfh->nfh_sb);
		super_block_put(sb);
		return NULL;
	}

	*sb_out = sb;
	return inode;
}

static int nlm4_op_null(struct rpc_trans *rt)
{
	(void)rt;
	TRACE("NLM4: NULL called");
	return 0;
}

static int nlm4_op_test(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct nlm4_testargs *args = ph->ph_args;
	struct nlm4_testres *res = ph->ph_res;
	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	res->cookie = args->cookie;

	TRACE("NLM4: TEST called for %s", args->alock.caller_name);

	if (reffs_nlm4_in_grace()) {
		res->stat.stat = NLM4_DENIED_GRACE_PERIOD;
		return 0;
	}

	inode = nlm4_fh_to_inode(&args->alock.fh, &sb);
	if (!inode) {
		res->stat.stat = NLM4_FAILED;
		return 0;
	}

	reffs_nlm4_test(inode, args, res);

	inode_put(inode);
	super_block_put(sb);
	return 0;
}

static int nlm4_op_lock(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct nlm4_lockargs *args = ph->ph_args;
	struct nlm4_res *res = ph->ph_res;
	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	res->cookie = args->cookie;
	res->state = sm_get_state();

	TRACE("NLM4: LOCK called for %s (reclaim=%d, block=%d)",
	      args->alock.caller_name, args->reclaim, args->block);

	if (reffs_nlm4_in_grace() && !args->reclaim) {
		res->stat.stat = NLM4_DENIED_GRACE_PERIOD;
		return 0;
	}

	inode = nlm4_fh_to_inode(&args->alock.fh, &sb);
	if (!inode) {
		res->stat.stat = NLM4_FAILED;
		return 0;
	}

	res->stat.stat = reffs_nlm4_lock(inode, args);

	inode_put(inode);
	super_block_put(sb);
	return 0;
}

static int nlm4_op_unlock(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct nlm4_unlockargs *args = ph->ph_args;
	struct nlm4_res *res = ph->ph_res;
	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	res->cookie = args->cookie;
	res->state = sm_get_state();

	TRACE("NLM4: UNLOCK called for %s", args->alock.caller_name);

	inode = nlm4_fh_to_inode(&args->alock.fh, &sb);
	if (!inode) {
		res->stat.stat = NLM4_FAILED;
		return 0;
	}

	res->stat.stat = reffs_nlm4_unlock(inode, args);

	inode_put(inode);
	super_block_put(sb);
	return 0;
}

static int nlm4_op_cancel(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct nlm4_cancargs *args = ph->ph_args;
	struct nlm4_res *res = ph->ph_res;
	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	res->cookie = args->cookie;
	res->state = sm_get_state();

	TRACE("NLM4: CANCEL called for %s", args->alock.caller_name);

	if (reffs_nlm4_in_grace()) {
		res->stat.stat = NLM4_DENIED_GRACE_PERIOD;
		return 0;
	}

	inode = nlm4_fh_to_inode(&args->alock.fh, &sb);
	if (!inode) {
		res->stat.stat = NLM4_FAILED;
		return 0;
	}

	res->stat.stat = reffs_nlm4_cancel(inode, args);

	inode_put(inode);
	super_block_put(sb);
	return 0;
}

static int nlm4_op_share(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct nlm4_shareargs *args = ph->ph_args;
	struct nlm4_shareres *res = ph->ph_res;
	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	res->cookie = args->cookie;
	res->state = sm_get_state();

	TRACE("NLM4: SHARE called for %s", args->share.caller_name);

	if (reffs_nlm4_in_grace() && !args->reclaim) {
		res->stat = NLM4_DENIED_GRACE_PERIOD;
		return 0;
	}

	inode = nlm4_fh_to_inode(&args->share.fh, &sb);
	if (!inode) {
		res->stat = NLM4_FAILED;
		return 0;
	}

	res->stat = reffs_nlm4_share(inode, args);

	inode_put(inode);
	super_block_put(sb);
	return 0;
}

static int nlm4_op_unshare(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct nlm4_shareargs *args = ph->ph_args;
	struct nlm4_shareres *res = ph->ph_res;
	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	res->cookie = args->cookie;
	res->state = sm_get_state();

	TRACE("NLM4: UNSHARE called for %s", args->share.caller_name);

	inode = nlm4_fh_to_inode(&args->share.fh, &sb);
	if (!inode) {
		res->stat = NLM4_FAILED;
		return 0;
	}

	res->stat = reffs_nlm4_unshare(inode, args);

	inode_put(inode);
	super_block_put(sb);
	return 0;
}

static int nlm4_op_free_all(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct nlm4_notify *args = ph->ph_args;

	TRACE("NLM4: FREE_ALL called for %s", args->name);

	reffs_nlm4_free_all(args);
	return 0;
}

static struct rpc_operations_handler nlm4_operations_handler[] = {
	RPC_OPERATION_INIT(NLMPROC4, NULL, NULL, NULL, NULL, NULL,
			   nlm4_op_null),
	RPC_OPERATION_INIT(NLMPROC4, TEST, xdr_nlm4_testargs,
			   struct nlm4_testargs, xdr_nlm4_testres,
			   struct nlm4_testres, nlm4_op_test),
	RPC_OPERATION_INIT(NLMPROC4, LOCK, xdr_nlm4_lockargs,
			   struct nlm4_lockargs, xdr_nlm4_res, struct nlm4_res,
			   nlm4_op_lock),
	RPC_OPERATION_INIT(NLMPROC4, CANCEL, xdr_nlm4_cancargs,
			   struct nlm4_cancargs, xdr_nlm4_res, struct nlm4_res,
			   nlm4_op_cancel),
	RPC_OPERATION_INIT(NLMPROC4, UNLOCK, xdr_nlm4_unlockargs,
			   struct nlm4_unlockargs, xdr_nlm4_res,
			   struct nlm4_res, nlm4_op_unlock),
	RPC_OPERATION_INIT(NLMPROC4, SHARE, xdr_nlm4_shareargs,
			   struct nlm4_shareargs, xdr_nlm4_shareres,
			   struct nlm4_shareres, nlm4_op_share),
	RPC_OPERATION_INIT(NLMPROC4, UNSHARE, xdr_nlm4_shareargs,
			   struct nlm4_shareargs, xdr_nlm4_shareres,
			   struct nlm4_shareres, nlm4_op_unshare),
	RPC_OPERATION_INIT(NLMPROC4, FREE_ALL, xdr_nlm4_notify,
			   struct nlm4_notify, NULL, NULL, nlm4_op_free_all),
};

/* 32-bit stubs for NLM v1/v3 */
static int nlm_op_null(struct rpc_trans *rt)
{
	(void)rt;
	TRACE("NLM: NULL called");
	return 0;
}

static struct rpc_operations_handler nlm_operations_handler[] = {
	RPC_OPERATION_INIT(NLMPROC, NULL, NULL, NULL, NULL, NULL, nlm_op_null),
};

static struct rpc_operations_handler nlm_versx_operations_handler[] = {
	RPC_OPERATION_INIT(NLMPROC, NULL, NULL, NULL, NULL, NULL, nlm_op_null),
};

static struct rpc_program_handler *nlm_v1_handler;
static struct rpc_program_handler *nlm_v3_handler;
static volatile sig_atomic_t nlm_v1_v3_registered = 0;

int nlm_protocol_register(void)
{
	if (nlm_v1_v3_registered)
		return 0;

	nlm_v1_handler = rpc_program_handler_alloc(
		NLM_PROG, NLM_VERS, nlm_operations_handler,
		sizeof(nlm_operations_handler) /
			sizeof(*nlm_operations_handler));

	if (!nlm_v1_handler)
		return ENOMEM;

	nlm_v3_handler = rpc_program_handler_alloc(
		NLM_PROG, NLM_VERSX, nlm_versx_operations_handler,
		sizeof(nlm_versx_operations_handler) /
			sizeof(*nlm_versx_operations_handler));

	if (!nlm_v3_handler) {
		rpc_program_handler_put(nlm_v1_handler);
		return ENOMEM;
	}

	nlm_v1_v3_registered = 1;
	return 0;
}

int nlm_protocol_deregister(void)
{
	if (!nlm_v1_v3_registered)
		return 0;

	rpc_program_handler_put(nlm_v1_handler);
	rpc_program_handler_put(nlm_v3_handler);
	nlm_v1_handler = NULL;
	nlm_v3_handler = NULL;
	nlm_v1_v3_registered = 0;
	return 0;
}

static struct rpc_program_handler *nlm4_handler;

static volatile sig_atomic_t nlm4_registered = 0;

int nlm4_protocol_register(void)
{
	if (nlm4_registered)
		return 0;

	reffs_nlm4_init_grace(REFFS_NLM4_GRACE_PERIOD);

	nlm4_handler = rpc_program_handler_alloc(
		NLM_PROG, NLM4_VERS, nlm4_operations_handler,
		sizeof(nlm4_operations_handler) /
			sizeof(*nlm4_operations_handler));

	if (!nlm4_handler)
		return ENOMEM;

	nlm4_registered = 1;
	return 0;
}

int nlm4_protocol_deregister(void)
{
	if (!nlm4_registered)
		return 0;

	rpc_program_handler_put(nlm4_handler);
	nlm4_handler = NULL;
	nlm4_registered = 0;
	return 0;
}
