/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include "attr.h"
#include "nfsv42_names.h"

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "compound.h"
#include "ops.h"
#include "errors.h"

bitmap4 supported_attributes_bm;

int nfs4_attribute_init(void)
{
	bitmap4 *bm = &supported_attributes_bm;
	bitmap4_init(bm, FATTR4_ATTRIBUTE_MAX);

	bitmap4_attribute_set(bm, FATTR4_SUPPORTED_ATTRS);
	bitmap4_attribute_set(bm, FATTR4_TYPE);
	bitmap4_attribute_set(bm, FATTR4_FH_EXPIRE_TYPE);
	bitmap4_attribute_set(bm, FATTR4_CHANGE);
	bitmap4_attribute_set(bm, FATTR4_SIZE);
	bitmap4_attribute_set(bm, FATTR4_LINK_SUPPORT);
	bitmap4_attribute_set(bm, FATTR4_SYMLINK_SUPPORT);
	bitmap4_attribute_set(bm, FATTR4_NAMED_ATTR);
	bitmap4_attribute_set(bm, FATTR4_FSID);
	bitmap4_attribute_set(bm, FATTR4_UNIQUE_HANDLES);
	bitmap4_attribute_set(bm, FATTR4_LEASE_TIME);
	bitmap4_attribute_set(bm, FATTR4_RDATTR_ERROR);
	bitmap4_attribute_set(bm, FATTR4_FILEHANDLE);
	bitmap4_attribute_set(bm, FATTR4_SUPPATTR_EXCLCREAT);
	bitmap4_attribute_clear(bm, FATTR4_ACL);
	bitmap4_attribute_clear(bm, FATTR4_ACLSUPPORT);
	bitmap4_attribute_set(bm, FATTR4_ARCHIVE);
	bitmap4_attribute_set(bm, FATTR4_CANSETTIME);
	bitmap4_attribute_set(bm, FATTR4_CASE_INSENSITIVE);
	bitmap4_attribute_set(bm, FATTR4_CASE_PRESERVING);
	bitmap4_attribute_set(bm, FATTR4_CHOWN_RESTRICTED);
	bitmap4_attribute_set(bm, FATTR4_FILEID);
	bitmap4_attribute_set(bm, FATTR4_FILES_AVAIL);
	bitmap4_attribute_set(bm, FATTR4_FILES_FREE);
	bitmap4_attribute_set(bm, FATTR4_FILES_TOTAL);
	bitmap4_attribute_clear(bm, FATTR4_FS_LOCATIONS);
	bitmap4_attribute_set(bm, FATTR4_HIDDEN);
	bitmap4_attribute_set(bm, FATTR4_HOMOGENEOUS);
	bitmap4_attribute_set(bm, FATTR4_MAXFILESIZE);
	bitmap4_attribute_set(bm, FATTR4_MAXLINK);
	bitmap4_attribute_set(bm, FATTR4_MAXNAME);
	bitmap4_attribute_set(bm, FATTR4_MAXREAD);
	bitmap4_attribute_set(bm, FATTR4_MAXWRITE);
	bitmap4_attribute_set(bm, FATTR4_MIMETYPE);
	bitmap4_attribute_set(bm, FATTR4_MODE);
	bitmap4_attribute_set(bm, FATTR4_NO_TRUNC);
	bitmap4_attribute_set(bm, FATTR4_NUMLINKS);
	bitmap4_attribute_set(bm, FATTR4_OWNER);
	bitmap4_attribute_set(bm, FATTR4_OWNER_GROUP);
	bitmap4_attribute_clear(bm, FATTR4_QUOTA_AVAIL_HARD);
	bitmap4_attribute_clear(bm, FATTR4_QUOTA_AVAIL_SOFT);
	bitmap4_attribute_clear(bm, FATTR4_QUOTA_USED);
	bitmap4_attribute_set(bm, FATTR4_RAWDEV);
	bitmap4_attribute_set(bm, FATTR4_SPACE_AVAIL);
	bitmap4_attribute_set(bm, FATTR4_SPACE_FREE);
	bitmap4_attribute_set(bm, FATTR4_SPACE_TOTAL);
	bitmap4_attribute_set(bm, FATTR4_SPACE_USED);
	bitmap4_attribute_set(bm, FATTR4_SYSTEM);
	bitmap4_attribute_set(bm, FATTR4_TIME_ACCESS);
	bitmap4_attribute_set(bm, FATTR4_TIME_ACCESS_SET);
	bitmap4_attribute_set(bm, FATTR4_TIME_BACKUP);
	bitmap4_attribute_set(bm, FATTR4_TIME_CREATE);
	bitmap4_attribute_set(bm, FATTR4_TIME_DELTA);
	bitmap4_attribute_set(bm, FATTR4_TIME_METADATA);
	bitmap4_attribute_set(bm, FATTR4_TIME_MODIFY);
	bitmap4_attribute_set(bm, FATTR4_TIME_MODIFY_SET);
	bitmap4_attribute_set(bm, FATTR4_MOUNTED_ON_FILEID);
	bitmap4_attribute_set(bm, FATTR4_DIR_NOTIF_DELAY);
	bitmap4_attribute_set(bm, FATTR4_DIRENT_NOTIF_DELAY);
	bitmap4_attribute_clear(bm, FATTR4_DACL);
	bitmap4_attribute_clear(bm, FATTR4_SACL);
	bitmap4_attribute_set(bm, FATTR4_CHANGE_POLICY);
	bitmap4_attribute_set(bm, FATTR4_FS_STATUS);
	bitmap4_attribute_clear(bm, FATTR4_FS_LAYOUT_TYPES);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_HINT);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_TYPES);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_BLKSIZE);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_ALIGNMENT);
	bitmap4_attribute_clear(bm, FATTR4_FS_LOCATIONS_INFO);
	bitmap4_attribute_clear(bm, FATTR4_MDSTHRESHOLD);
	bitmap4_attribute_set(bm, FATTR4_RETENTION_GET);
	bitmap4_attribute_set(bm, FATTR4_RETENTION_SET);
	bitmap4_attribute_set(bm, FATTR4_RETENTEVT_GET);
	bitmap4_attribute_set(bm, FATTR4_RETENTEVT_SET);
	bitmap4_attribute_set(bm, FATTR4_RETENTION_HOLD);
	bitmap4_attribute_set(bm, FATTR4_MODE_SET_MASKED);
	bitmap4_attribute_set(bm, FATTR4_FS_CHARSET_CAP);
	bitmap4_attribute_set(bm, FATTR4_CLONE_BLKSIZE);
	bitmap4_attribute_set(bm, FATTR4_SPACE_FREED);
	bitmap4_attribute_set(bm, FATTR4_CHANGE_ATTR_TYPE);
	bitmap4_attribute_clear(bm, FATTR4_SEC_LABEL);
	bitmap4_attribute_clear(bm, FATTR4_MODE_UMASK);
	bitmap4_attribute_clear(bm, FATTR4_XATTR_SUPPORT);
	bitmap4_attribute_set(bm, FATTR4_OFFLINE);
	bitmap4_attribute_clear(bm, FATTR4_TIME_DELEG_ACCESS);
	bitmap4_attribute_clear(bm, FATTR4_TIME_DELEG_MODIFY);
	bitmap4_attribute_set(bm, FATTR4_OPEN_ARGUMENTS);
	bitmap4_attribute_set(bm, FATTR4_UNCACHEABLE);

	return 0;
}

int nfs4_attribute_fini(void)
{
	bitmap4_destroy(&supported_attributes_bm);

	return 0;
}

#ifdef NOT_NOW_BROWN_COW
static void __attribute__((constructor)) nfs4_attribute_load(void)
{
	if (nfs4_attribute_init() != 0)
		abort();
}

static void __attribute__((destructor)) nfs4_attribute_unload(void)
{
	nfs4_attribute_fini();
}
#endif

void nfs4_op_getattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opgetattr);
	GETATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opgetattr);
	nfsstat4 *status = &res->status;
	GETATTR4resok *resok = NFS4_OP_RESOK_SETUP(res, GETATTR4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_readdir(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READDIR4args *args = NFS4_OP_ARG_SETUP(c, ph, opreaddir);
	READDIR4res *res = NFS4_OP_RES_SETUP(c, ph, opreaddir);
	nfsstat4 *status = &res->status;
	READDIR4resok *resok = NFS4_OP_RESOK_SETUP(res, READDIR4res_u, resok4);

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_BADHANDLE;
		goto out;
	}

	*status = NFS4ERR_NOTSUPP;

out:
	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_setattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SETATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opsetattr);
	SETATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opsetattr);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_verify(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	VERIFY4args *args = NFS4_OP_ARG_SETUP(c, ph, opverify);
	VERIFY4res *res = NFS4_OP_RES_SETUP(c, ph, opverify);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_nverify(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	NVERIFY4args *args = NFS4_OP_ARG_SETUP(c, ph, opnverify);
	NVERIFY4res *res = NFS4_OP_RES_SETUP(c, ph, opnverify);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_access(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	ACCESS4args *args = NFS4_OP_ARG_SETUP(c, ph, opaccess);
	ACCESS4res *res = NFS4_OP_RES_SETUP(c, ph, opaccess);
	nfsstat4 *status = &res->status;
	ACCESS4resok *resok = NFS4_OP_RESOK_SETUP(res, ACCESS4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_access_mask(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	ACCESS_MASK4args *args = NFS4_OP_ARG_SETUP(c, ph, opaccess_mask);
	ACCESS_MASK4res *res = NFS4_OP_RES_SETUP(c, ph, opaccess_mask);
	nfsstat4 *status = &res->amr_status;
	ACCESS_MASK4resok *resok =
		NFS4_OP_RESOK_SETUP(res, ACCESS_MASK4res_u, amr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}
