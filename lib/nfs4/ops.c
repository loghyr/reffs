/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "nfs4_internal.h"
#include "ops.h"

void nfs4_op_secinfo(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SECINFO4args *args = NFS4_OP_ARG_SETUP(c, ph, opsecinfo);
	SECINFO4res *res = NFS4_OP_RES_SETUP(c, ph, opsecinfo);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_secinfo_no_name(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SECINFO_NO_NAME4res *res = NFS4_OP_RES_SETUP(c, ph, opsecinfo_no_name);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_io_advise(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	IO_ADVISE4args *args = NFS4_OP_ARG_SETUP(c, ph, opio_advise);
	IO_ADVISE4res *res = NFS4_OP_RES_SETUP(c, ph, opio_advise);
	nfsstat4 *status = &res->ior_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_illegal(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	nfs_argop4 *argop = &((COMPOUND4args *)ph->ph_args)
				     ->argarray.argarray_val[c->c_curr_op];
	nfs_resop4 *resop = &((COMPOUND4res *)ph->ph_res)
				     ->resarray.resarray_val[c->c_curr_op];
	nfsstat4 *status = &resop->nfs_resop4_u.opillegal.status;

	resop->resop = OP_ILLEGAL;
	*status = NFS4ERR_OP_ILLEGAL;

	LOG("%s op=%s(%d) status=%d", __func__, nfs4_op_name(argop->argop),
	    argop->argop, *status);
}

const char *nfs4_op_name(nfs_opnum4 op)
{
	switch (op) {
	case OP_ACCESS:
		return "ACCESS";
	case OP_CLOSE:
		return "CLOSE";
	case OP_COMMIT:
		return "COMMIT";
	case OP_CREATE:
		return "CREATE";
	case OP_DELEGPURGE:
		return "DELEGPURGE";
	case OP_DELEGRETURN:
		return "DELEGRETURN";
	case OP_GETATTR:
		return "GETATTR";
	case OP_GETFH:
		return "GETFH";
	case OP_LINK:
		return "LINK";
	case OP_LOCK:
		return "LOCK";
	case OP_LOCKT:
		return "LOCKT";
	case OP_LOCKU:
		return "LOCKU";
	case OP_LOOKUP:
		return "LOOKUP";
	case OP_LOOKUPP:
		return "LOOKUPP";
	case OP_NVERIFY:
		return "NVERIFY";
	case OP_OPEN:
		return "OPEN";
	case OP_OPENATTR:
		return "OPENATTR";
	case OP_OPEN_CONFIRM:
		return "OPEN_CONFIRM";
	case OP_OPEN_DOWNGRADE:
		return "OPEN_DOWNGRADE";
	case OP_PUTFH:
		return "PUTFH";
	case OP_PUTPUBFH:
		return "PUTPUBFH";
	case OP_PUTROOTFH:
		return "PUTROOTFH";
	case OP_READ:
		return "READ";
	case OP_READDIR:
		return "READDIR";
	case OP_READLINK:
		return "READLINK";
	case OP_REMOVE:
		return "REMOVE";
	case OP_RENAME:
		return "RENAME";
	case OP_RENEW:
		return "RENEW";
	case OP_RESTOREFH:
		return "RESTOREFH";
	case OP_SAVEFH:
		return "SAVEFH";
	case OP_SECINFO:
		return "SECINFO";
	case OP_SETATTR:
		return "SETATTR";
	case OP_SETCLIENTID:
		return "SETCLIENTID";
	case OP_SETCLIENTID_CONFIRM:
		return "SETCLIENTID_CONFIRM";
	case OP_VERIFY:
		return "VERIFY";
	case OP_WRITE:
		return "WRITE";
	case OP_RELEASE_LOCKOWNER:
		return "RELEASE_LOCKOWNER";
	case OP_BACKCHANNEL_CTL:
		return "BACKCHANNEL_CTL";
	case OP_BIND_CONN_TO_SESSION:
		return "BIND_CONN_TO_SESSION";
	case OP_EXCHANGE_ID:
		return "EXCHANGE_ID";
	case OP_CREATE_SESSION:
		return "CREATE_SESSION";
	case OP_DESTROY_SESSION:
		return "DESTROY_SESSION";
	case OP_FREE_STATEID:
		return "FREE_STATEID";
	case OP_GET_DIR_DELEGATION:
		return "GET_DIR_DELEGATION";
	case OP_GETDEVICEINFO:
		return "GETDEVICEINFO";
	case OP_GETDEVICELIST:
		return "GETDEVICELIST";
	case OP_LAYOUTCOMMIT:
		return "LAYOUTCOMMIT";
	case OP_LAYOUTGET:
		return "LAYOUTGET";
	case OP_LAYOUTRETURN:
		return "LAYOUTRETURN";
	case OP_SECINFO_NO_NAME:
		return "SECINFO_NO_NAME";
	case OP_SEQUENCE:
		return "SEQUENCE";
	case OP_SET_SSV:
		return "SET_SSV";
	case OP_TEST_STATEID:
		return "TEST_STATEID";
	case OP_WANT_DELEGATION:
		return "WANT_DELEGATION";
	case OP_DESTROY_CLIENTID:
		return "DESTROY_CLIENTID";
	case OP_RECLAIM_COMPLETE:
		return "RECLAIM_COMPLETE";
	case OP_ALLOCATE:
		return "ALLOCATE";
	case OP_COPY:
		return "COPY";
	case OP_COPY_NOTIFY:
		return "COPY_NOTIFY";
	case OP_DEALLOCATE:
		return "DEALLOCATE";
	case OP_IO_ADVISE:
		return "IO_ADVISE";
	case OP_LAYOUTERROR:
		return "LAYOUTERROR";
	case OP_LAYOUTSTATS:
		return "LAYOUTSTATS";
	case OP_OFFLOAD_CANCEL:
		return "OFFLOAD_CANCEL";
	case OP_OFFLOAD_STATUS:
		return "OFFLOAD_STATUS";
	case OP_READ_PLUS:
		return "READ_PLUS";
	case OP_SEEK:
		return "SEEK";
	case OP_WRITE_SAME:
		return "WRITE_SAME";
	case OP_CLONE:
		return "CLONE";
	case OP_GETXATTR:
		return "GETXATTR";
	case OP_SETXATTR:
		return "SETXATTR";
	case OP_LISTXATTRS:
		return "LISTXATTRS";
	case OP_REMOVEXATTR:
		return "REMOVEXATTR";
	case OP_ACCESS_MASK:
		return "ACCESS_MASK";
	case OP_ILLEGAL:
		return "ILLEGAL";
	default:
		return "UNKNOWN";
	}
}
