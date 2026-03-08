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

#define NFS4_OP_ARG_SETUP(c, ph, field)                   \
	(&((COMPOUND4args *)(ph)->ph_args)                \
		  ->argarray.argarray_val[(c)->c_curr_op] \
		  .nfs_argop4_u.field)

#define NFS4_OP_RES_SETUP(c, ph, field)                   \
	(&((COMPOUND4res *)(ph)->ph_res)                  \
		  ->resarray.resarray_val[(c)->c_curr_op] \
		  .nfs_resop4_u.field)

void nfs4_op_access(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	ACCESS4args *args = NFS4_OP_ARG_SETUP(c, ph, opaccess);
	ACCESS4res *res = NFS4_OP_RES_SETUP(c, ph, opaccess);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_close(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	CLOSE4args *args = NFS4_OP_ARG_SETUP(c, ph, opclose);
	CLOSE4res *res = NFS4_OP_RES_SETUP(c, ph, opclose);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_commit(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	COMMIT4args *args = NFS4_OP_ARG_SETUP(c, ph, opcommit);
	COMMIT4res *res = NFS4_OP_RES_SETUP(c, ph, opcommit);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_create(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	CREATE4args *args = NFS4_OP_ARG_SETUP(c, ph, opcreate);
	CREATE4res *res = NFS4_OP_RES_SETUP(c, ph, opcreate);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_delegpurge(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	DELEGPURGE4args *args = NFS4_OP_ARG_SETUP(c, ph, opdelegpurge);
	DELEGPURGE4res *res = NFS4_OP_RES_SETUP(c, ph, opdelegpurge);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_delegreturn(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	DELEGRETURN4args *args = NFS4_OP_ARG_SETUP(c, ph, opdelegreturn);
	DELEGRETURN4res *res = NFS4_OP_RES_SETUP(c, ph, opdelegreturn);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_getattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opgetattr);
	GETATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opgetattr);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_getfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETFH4res *res = NFS4_OP_RES_SETUP(c, ph, opgetfh);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_link(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LINK4args *args = NFS4_OP_ARG_SETUP(c, ph, oplink);
	LINK4res *res = NFS4_OP_RES_SETUP(c, ph, oplink);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_lock(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOCK4args *args = NFS4_OP_ARG_SETUP(c, ph, oplock);
	LOCK4res *res = NFS4_OP_RES_SETUP(c, ph, oplock);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_lockt(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOCKT4args *args = NFS4_OP_ARG_SETUP(c, ph, oplockt);
	LOCKT4res *res = NFS4_OP_RES_SETUP(c, ph, oplockt);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_locku(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOCKU4args *args = NFS4_OP_ARG_SETUP(c, ph, oplocku);
	LOCKU4res *res = NFS4_OP_RES_SETUP(c, ph, oplocku);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_lookup(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOOKUP4args *args = NFS4_OP_ARG_SETUP(c, ph, oplookup);
	LOOKUP4res *res = NFS4_OP_RES_SETUP(c, ph, oplookup);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_lookupp(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOOKUPP4res *res = NFS4_OP_RES_SETUP(c, ph, oplookupp);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_nverify(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	NVERIFY4args *args = NFS4_OP_ARG_SETUP(c, ph, opnverify);
	NVERIFY4res *res = NFS4_OP_RES_SETUP(c, ph, opnverify);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_open(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPEN4args *args = NFS4_OP_ARG_SETUP(c, ph, opopen);
	OPEN4res *res = NFS4_OP_RES_SETUP(c, ph, opopen);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_openattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPENATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opopenattr);
	OPENATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opopenattr);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_open_confirm(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPEN_CONFIRM4args *args = NFS4_OP_ARG_SETUP(c, ph, opopen_confirm);
	OPEN_CONFIRM4res *res = NFS4_OP_RES_SETUP(c, ph, opopen_confirm);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_open_downgrade(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPEN_DOWNGRADE4args *args = NFS4_OP_ARG_SETUP(c, ph, opopen_downgrade);
	OPEN_DOWNGRADE4res *res = NFS4_OP_RES_SETUP(c, ph, opopen_downgrade);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_putfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	PUTFH4args *args = NFS4_OP_ARG_SETUP(c, ph, opputfh);
	PUTFH4res *res = NFS4_OP_RES_SETUP(c, ph, opputfh);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_putpubfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	PUTPUBFH4res *res = NFS4_OP_RES_SETUP(c, ph, opputpubfh);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_putrootfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	PUTROOTFH4res *res = NFS4_OP_RES_SETUP(c, ph, opputrootfh);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_read(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READ4args *args = NFS4_OP_ARG_SETUP(c, ph, opread);
	READ4res *res = NFS4_OP_RES_SETUP(c, ph, opread);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_readdir(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READDIR4args *args = NFS4_OP_ARG_SETUP(c, ph, opreaddir);
	READDIR4res *res = NFS4_OP_RES_SETUP(c, ph, opreaddir);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_readlink(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READLINK4res *res = NFS4_OP_RES_SETUP(c, ph, opreadlink);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_remove(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	REMOVE4args *args = NFS4_OP_ARG_SETUP(c, ph, opremove);
	REMOVE4res *res = NFS4_OP_RES_SETUP(c, ph, opremove);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_rename(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RENAME4args *args = NFS4_OP_ARG_SETUP(c, ph, oprename);
	RENAME4res *res = NFS4_OP_RES_SETUP(c, ph, oprename);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_renew(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RENEW4res *res = NFS4_OP_RES_SETUP(c, ph, oprenew);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_restorefh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RESTOREFH4res *res = NFS4_OP_RES_SETUP(c, ph, oprestorefh);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_savefh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SAVEFH4res *res = NFS4_OP_RES_SETUP(c, ph, opsavefh);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

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

void nfs4_op_setattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SETATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opsetattr);
	SETATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opsetattr);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_setclientid(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SETCLIENTID4args *args = NFS4_OP_ARG_SETUP(c, ph, opsetclientid);
	SETCLIENTID4res *res = NFS4_OP_RES_SETUP(c, ph, opsetclientid);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_setclientid_confirm(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SETCLIENTID_CONFIRM4res *res =
		NFS4_OP_RES_SETUP(c, ph, opsetclientid_confirm);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_verify(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	VERIFY4args *args = NFS4_OP_ARG_SETUP(c, ph, opverify);
	VERIFY4res *res = NFS4_OP_RES_SETUP(c, ph, opverify);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_write(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	WRITE4args *args = NFS4_OP_ARG_SETUP(c, ph, opwrite);
	WRITE4res *res = NFS4_OP_RES_SETUP(c, ph, opwrite);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_release_lockowner(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RELEASE_LOCKOWNER4res *res =
		NFS4_OP_RES_SETUP(c, ph, oprelease_lockowner);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_backchannel_ctl(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	BACKCHANNEL_CTL4args *args =
		NFS4_OP_ARG_SETUP(c, ph, opbackchannel_ctl);
	BACKCHANNEL_CTL4res *res = NFS4_OP_RES_SETUP(c, ph, opbackchannel_ctl);
	nfsstat4 *status = &res->bcr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_bind_conn_to_session(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	BIND_CONN_TO_SESSION4args *args =
		NFS4_OP_ARG_SETUP(c, ph, opbind_conn_to_session);
	BIND_CONN_TO_SESSION4res *res =
		NFS4_OP_RES_SETUP(c, ph, opbind_conn_to_session);
	nfsstat4 *status = &res->bctsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_exchange_id(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	EXCHANGE_ID4args *args = NFS4_OP_ARG_SETUP(c, ph, opexchange_id);
	EXCHANGE_ID4res *res = NFS4_OP_RES_SETUP(c, ph, opexchange_id);
	nfsstat4 *status = &res->eir_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_create_session(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	CREATE_SESSION4args *args = NFS4_OP_ARG_SETUP(c, ph, opcreate_session);
	CREATE_SESSION4res *res = NFS4_OP_RES_SETUP(c, ph, opcreate_session);
	nfsstat4 *status = &res->csr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_destroy_session(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	DESTROY_SESSION4res *res = NFS4_OP_RES_SETUP(c, ph, opdestroy_session);
	nfsstat4 *status = &res->dsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_free_stateid(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	FREE_STATEID4res *res = NFS4_OP_RES_SETUP(c, ph, opfree_stateid);
	nfsstat4 *status = &res->fsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_get_dir_delegation(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GET_DIR_DELEGATION4args *args =
		NFS4_OP_ARG_SETUP(c, ph, opget_dir_delegation);
	GET_DIR_DELEGATION4res *res =
		NFS4_OP_RES_SETUP(c, ph, opget_dir_delegation);
	nfsstat4 *status = &res->gddr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_getdeviceinfo(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETDEVICEINFO4args *args = NFS4_OP_ARG_SETUP(c, ph, opgetdeviceinfo);
	GETDEVICEINFO4res *res = NFS4_OP_RES_SETUP(c, ph, opgetdeviceinfo);
	nfsstat4 *status = &res->gdir_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_getdevicelist(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETDEVICELIST4args *args = NFS4_OP_ARG_SETUP(c, ph, opgetdevicelist);
	GETDEVICELIST4res *res = NFS4_OP_RES_SETUP(c, ph, opgetdevicelist);
	nfsstat4 *status = &res->gdlr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_layoutcommit(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LAYOUTCOMMIT4args *args = NFS4_OP_ARG_SETUP(c, ph, oplayoutcommit);
	LAYOUTCOMMIT4res *res = NFS4_OP_RES_SETUP(c, ph, oplayoutcommit);
	nfsstat4 *status = &res->locr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_layoutget(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LAYOUTGET4args *args = NFS4_OP_ARG_SETUP(c, ph, oplayoutget);
	LAYOUTGET4res *res = NFS4_OP_RES_SETUP(c, ph, oplayoutget);
	nfsstat4 *status = &res->logr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_layoutreturn(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LAYOUTRETURN4args *args = NFS4_OP_ARG_SETUP(c, ph, oplayoutreturn);
	LAYOUTRETURN4res *res = NFS4_OP_RES_SETUP(c, ph, oplayoutreturn);
	nfsstat4 *status = &res->lorr_status;

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

void nfs4_op_sequence(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SEQUENCE4args *args = NFS4_OP_ARG_SETUP(c, ph, opsequence);
	SEQUENCE4res *res = NFS4_OP_RES_SETUP(c, ph, opsequence);
	nfsstat4 *status = &res->sr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_set_ssv(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SET_SSV4args *args = NFS4_OP_ARG_SETUP(c, ph, opset_ssv);
	SET_SSV4res *res = NFS4_OP_RES_SETUP(c, ph, opset_ssv);
	nfsstat4 *status = &res->ssr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_test_stateid(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	TEST_STATEID4args *args = NFS4_OP_ARG_SETUP(c, ph, optest_stateid);
	TEST_STATEID4res *res = NFS4_OP_RES_SETUP(c, ph, optest_stateid);
	nfsstat4 *status = &res->tsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_want_delegation(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	WANT_DELEGATION4args *args =
		NFS4_OP_ARG_SETUP(c, ph, opwant_delegation);
	WANT_DELEGATION4res *res = NFS4_OP_RES_SETUP(c, ph, opwant_delegation);
	nfsstat4 *status = &res->wdr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_destroy_clientid(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	DESTROY_CLIENTID4res *res =
		NFS4_OP_RES_SETUP(c, ph, opdestroy_clientid);
	nfsstat4 *status = &res->dcr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_reclaim_complete(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RECLAIM_COMPLETE4args *args =
		NFS4_OP_ARG_SETUP(c, ph, opreclaim_complete);
	RECLAIM_COMPLETE4res *res =
		NFS4_OP_RES_SETUP(c, ph, opreclaim_complete);
	nfsstat4 *status = &res->rcr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_allocate(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	ALLOCATE4args *args = NFS4_OP_ARG_SETUP(c, ph, opallocate);
	ALLOCATE4res *res = NFS4_OP_RES_SETUP(c, ph, opallocate);
	nfsstat4 *status = &res->ar_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_copy(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	COPY4args *args = NFS4_OP_ARG_SETUP(c, ph, opcopy);
	COPY4res *res = NFS4_OP_RES_SETUP(c, ph, opcopy);
	nfsstat4 *status = &res->cr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_copy_notify(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	COPY_NOTIFY4args *args = NFS4_OP_ARG_SETUP(c, ph, opoffload_notify);
	COPY_NOTIFY4res *res = NFS4_OP_RES_SETUP(c, ph, opcopy_notify);
	nfsstat4 *status = &res->cnr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_deallocate(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	DEALLOCATE4args *args = NFS4_OP_ARG_SETUP(c, ph, opdeallocate);
	DEALLOCATE4res *res = NFS4_OP_RES_SETUP(c, ph, opdeallocate);
	nfsstat4 *status = &res->dr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
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

void nfs4_op_layouterror(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LAYOUTERROR4args *args = NFS4_OP_ARG_SETUP(c, ph, oplayouterror);
	LAYOUTERROR4res *res = NFS4_OP_RES_SETUP(c, ph, oplayouterror);
	nfsstat4 *status = &res->ler_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_layoutstats(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LAYOUTSTATS4args *args = NFS4_OP_ARG_SETUP(c, ph, oplayoutstats);
	LAYOUTSTATS4res *res = NFS4_OP_RES_SETUP(c, ph, oplayoutstats);
	nfsstat4 *status = &res->lsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_offload_cancel(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OFFLOAD_CANCEL4res *res = NFS4_OP_RES_SETUP(c, ph, opoffload_cancel);
	nfsstat4 *status = &res->ocr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_offload_status(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OFFLOAD_STATUS4res *res = NFS4_OP_RES_SETUP(c, ph, opoffload_status);
	nfsstat4 *status = &res->osr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d res=%p", __func__, *status, (void *)res);
}

void nfs4_op_read_plus(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READ_PLUS4args *args = NFS4_OP_ARG_SETUP(c, ph, opread_plus);
	READ_PLUS4res *res = NFS4_OP_RES_SETUP(c, ph, opread_plus);
	nfsstat4 *status = &res->rp_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_seek(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SEEK4args *args = NFS4_OP_ARG_SETUP(c, ph, opseek);
	SEEK4res *res = NFS4_OP_RES_SETUP(c, ph, opseek);
	nfsstat4 *status = &res->sa_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_write_same(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	WRITE_SAME4args *args = NFS4_OP_ARG_SETUP(c, ph, opwrite_same);
	WRITE_SAME4res *res = NFS4_OP_RES_SETUP(c, ph, opwrite_same);
	nfsstat4 *status = &res->wsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_clone(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	CLONE4args *args = NFS4_OP_ARG_SETUP(c, ph, opclone);
	CLONE4res *res = NFS4_OP_RES_SETUP(c, ph, opclone);
	nfsstat4 *status = &res->cl_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_getxattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETXATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opgetxattr);
	GETXATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opgetxattr);
	nfsstat4 *status = &res->gxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_setxattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SETXATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opsetxattr);
	SETXATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opsetxattr);
	nfsstat4 *status = &res->sxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_listxattrs(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LISTXATTRS4args *args = NFS4_OP_ARG_SETUP(c, ph, oplistxattrs);
	LISTXATTRS4res *res = NFS4_OP_RES_SETUP(c, ph, oplistxattrs);
	nfsstat4 *status = &res->lxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_removexattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	REMOVEXATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opremovexattr);
	REMOVEXATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opremovexattr);
	nfsstat4 *status = &res->rxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%d args=%p res=%p", __func__, *status, (void *)args,
	    (void *)res);
}

void nfs4_op_access_mask(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	ACCESS_MASK4args *args = NFS4_OP_ARG_SETUP(c, ph, opaccess_mask);
	ACCESS_MASK4res *res = NFS4_OP_RES_SETUP(c, ph, opaccess_mask);
	nfsstat4 *status = &res->amr_status;

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
