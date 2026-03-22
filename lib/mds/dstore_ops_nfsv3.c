/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Dstore control-plane operations — NFSv3 RPCs to data servers.
 *
 * All operations are synchronous (blocking).  They use the dstore's
 * CLIENT handle and are serialised by ds_clnt_mutex when needed.
 *
 * Reference: Peterson & Weldon for nothing here — this is plain
 * NFSv3 (RFC 1813).
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>

#include "nfsv3_xdr.h"
#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/layout_segment.h"
#include "reffs/log.h"

#define DS_RPC_TIMEOUT_SEC 10

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static struct timeval ds_timeout(void)
{
	return (struct timeval){ .tv_sec = DS_RPC_TIMEOUT_SEC, .tv_usec = 0 };
}

static nfs_fh3 make_fh3(const uint8_t *fh, uint32_t len)
{
	nfs_fh3 f;

	f.data.data_val = (char *)fh;
	f.data.data_len = len;
	return f;
}

/* ------------------------------------------------------------------ */
/* CREATE                                                              */
/* ------------------------------------------------------------------ */

static int nfsv3_create(struct dstore *ds, const uint8_t *dir_fh,
			    uint32_t dir_fh_len, const char *name,
			    uint8_t *out_fh, uint32_t *out_fh_len)
{
	CREATE3args args;
	CREATE3res res;
	struct timeval tv = ds_timeout();
	enum clnt_stat rpc_stat;

	memset(&args, 0, sizeof(args));
	memset(&res, 0, sizeof(res));

	args.where.dir = make_fh3(dir_fh, dir_fh_len);
	args.where.name = (char *)name;
	args.how.mode = UNCHECKED;
	/* sattr3: mode 0640, other fields unset (server defaults). */
	args.how.createhow3_u.obj_attributes.mode.set_it = true;
	args.how.createhow3_u.obj_attributes.mode.set_mode3_u.mode = 0640;

	pthread_mutex_lock(&ds->ds_clnt_mutex);
	if (!ds->ds_clnt) {
		pthread_mutex_unlock(&ds->ds_clnt_mutex);
		return -ENOTCONN;
	}

	rpc_stat = clnt_call(ds->ds_clnt, NFSPROC3_CREATE,
			     (xdrproc_t)xdr_CREATE3args, (caddr_t)&args,
			     (xdrproc_t)xdr_CREATE3res, (caddr_t)&res, tv);
	pthread_mutex_unlock(&ds->ds_clnt_mutex);

	if (rpc_stat != RPC_SUCCESS) {
		LOG("dstore[%u]: CREATE %s RPC failed", ds->ds_id, name);
		return -EIO;
	}

	if (res.status != NFS3_OK) {
		LOG("dstore[%u]: CREATE %s failed: status=%d", ds->ds_id,
		    name, res.status);
		xdr_free((xdrproc_t)xdr_CREATE3res, (caddr_t)&res);
		return -EIO;
	}

	/* Extract the filehandle from the post_op_fh3. */
	post_op_fh3 *pofh = &res.CREATE3res_u.resok.obj;

	if (pofh->handle_follows && pofh->post_op_fh3_u.handle.data.data_len <=
					    LAYOUT_SEG_MAX_FH) {
		*out_fh_len = pofh->post_op_fh3_u.handle.data.data_len;
		memcpy(out_fh, pofh->post_op_fh3_u.handle.data.data_val,
		       *out_fh_len);
	} else if (!pofh->handle_follows) {
		LOG("dstore[%u]: CREATE %s: server did not return FH",
		    ds->ds_id, name);
	} else {
		LOG("dstore[%u]: CREATE %s: FH too large (%u > %d)",
		    ds->ds_id, name,
		    pofh->post_op_fh3_u.handle.data.data_len,
		    LAYOUT_SEG_MAX_FH);
		xdr_free((xdrproc_t)xdr_CREATE3res, (caddr_t)&res);
		return -EIO;
	}

	xdr_free((xdrproc_t)xdr_CREATE3res, (caddr_t)&res);
	return 0;
}

/* ------------------------------------------------------------------ */
/* REMOVE                                                              */
/* ------------------------------------------------------------------ */

static int nfsv3_remove(struct dstore *ds, const uint8_t *dir_fh,
			    uint32_t dir_fh_len, const char *name)
{
	REMOVE3args args;
	REMOVE3res res;
	struct timeval tv = ds_timeout();
	enum clnt_stat rpc_stat;

	memset(&args, 0, sizeof(args));
	memset(&res, 0, sizeof(res));

	args.object.dir = make_fh3(dir_fh, dir_fh_len);
	args.object.name = (char *)name;

	pthread_mutex_lock(&ds->ds_clnt_mutex);
	if (!ds->ds_clnt) {
		pthread_mutex_unlock(&ds->ds_clnt_mutex);
		return -ENOTCONN;
	}

	rpc_stat = clnt_call(ds->ds_clnt, NFSPROC3_REMOVE,
			     (xdrproc_t)xdr_REMOVE3args, (caddr_t)&args,
			     (xdrproc_t)xdr_REMOVE3res, (caddr_t)&res, tv);
	pthread_mutex_unlock(&ds->ds_clnt_mutex);

	if (rpc_stat != RPC_SUCCESS) {
		LOG("dstore[%u]: REMOVE RPC failed", ds->ds_id);
		return -EIO;
	}

	int ret = (res.status == NFS3_OK) ? 0 : -EIO;

	xdr_free((xdrproc_t)xdr_REMOVE3res, (caddr_t)&res);
	return ret;
}

/* ------------------------------------------------------------------ */
/* CHMOD (SETATTR mode)                                                */
/* ------------------------------------------------------------------ */

static int nfsv3_chmod(struct dstore *ds, const uint8_t *fh,
			   uint32_t fh_len)
{
	SETATTR3args args;
	SETATTR3res res;
	struct timeval tv = ds_timeout();
	enum clnt_stat rpc_stat;

	memset(&args, 0, sizeof(args));
	memset(&res, 0, sizeof(res));

	args.object = make_fh3(fh, fh_len);
	args.new_attributes.mode.set_it = true;
	args.new_attributes.mode.set_mode3_u.mode = 0640;

	pthread_mutex_lock(&ds->ds_clnt_mutex);
	if (!ds->ds_clnt) {
		pthread_mutex_unlock(&ds->ds_clnt_mutex);
		return -ENOTCONN;
	}

	rpc_stat = clnt_call(ds->ds_clnt, NFSPROC3_SETATTR,
			     (xdrproc_t)xdr_SETATTR3args, (caddr_t)&args,
			     (xdrproc_t)xdr_SETATTR3res, (caddr_t)&res, tv);
	pthread_mutex_unlock(&ds->ds_clnt_mutex);

	if (rpc_stat != RPC_SUCCESS) {
		LOG("dstore[%u]: SETATTR(mode) RPC failed", ds->ds_id);
		return -EIO;
	}

	int ret = (res.status == NFS3_OK) ? 0 : -EIO;

	xdr_free((xdrproc_t)xdr_SETATTR3res, (caddr_t)&res);
	return ret;
}

/* ------------------------------------------------------------------ */
/* TRUNCATE (SETATTR size)                                             */
/* ------------------------------------------------------------------ */

static int nfsv3_truncate(struct dstore *ds, const uint8_t *fh,
			      uint32_t fh_len, uint64_t size)
{
	SETATTR3args args;
	SETATTR3res res;
	struct timeval tv = ds_timeout();
	enum clnt_stat rpc_stat;

	memset(&args, 0, sizeof(args));
	memset(&res, 0, sizeof(res));

	args.object = make_fh3(fh, fh_len);
	args.new_attributes.size.set_it = true;
	args.new_attributes.size.set_size3_u.size = size;

	pthread_mutex_lock(&ds->ds_clnt_mutex);
	if (!ds->ds_clnt) {
		pthread_mutex_unlock(&ds->ds_clnt_mutex);
		return -ENOTCONN;
	}

	rpc_stat = clnt_call(ds->ds_clnt, NFSPROC3_SETATTR,
			     (xdrproc_t)xdr_SETATTR3args, (caddr_t)&args,
			     (xdrproc_t)xdr_SETATTR3res, (caddr_t)&res, tv);
	pthread_mutex_unlock(&ds->ds_clnt_mutex);

	if (rpc_stat != RPC_SUCCESS) {
		LOG("dstore[%u]: SETATTR(size=%lu) RPC failed", ds->ds_id,
		    (unsigned long)size);
		return -EIO;
	}

	int ret = (res.status == NFS3_OK) ? 0 : -EIO;

	xdr_free((xdrproc_t)xdr_SETATTR3res, (caddr_t)&res);
	return ret;
}

/* ------------------------------------------------------------------ */
/* FENCE (SETATTR uid/gid rotation)                                    */
/* ------------------------------------------------------------------ */

static int nfsv3_fence(struct dstore *ds, const uint8_t *fh,
			   uint32_t fh_len, struct layout_data_file *ldf,
			   uint32_t fence_min, uint32_t fence_max)
{
	SETATTR3args args;
	SETATTR3res res;
	struct timeval tv = ds_timeout();
	enum clnt_stat rpc_stat;

	if (fence_min > fence_max)
		return -EINVAL;

	/* Bump uid and gid, wrapping within the range. */
	uint32_t new_uid = ldf->ldf_uid + 1;
	uint32_t new_gid = ldf->ldf_gid + 1;

	if (new_uid > fence_max || new_uid < fence_min)
		new_uid = fence_min;
	if (new_gid > fence_max || new_gid < fence_min)
		new_gid = fence_min;

	memset(&args, 0, sizeof(args));
	memset(&res, 0, sizeof(res));

	args.object = make_fh3(fh, fh_len);
	args.new_attributes.uid.set_it = true;
	args.new_attributes.uid.set_uid3_u.uid = new_uid;
	args.new_attributes.gid.set_it = true;
	args.new_attributes.gid.set_gid3_u.gid = new_gid;

	pthread_mutex_lock(&ds->ds_clnt_mutex);
	if (!ds->ds_clnt) {
		pthread_mutex_unlock(&ds->ds_clnt_mutex);
		return -ENOTCONN;
	}

	rpc_stat = clnt_call(ds->ds_clnt, NFSPROC3_SETATTR,
			     (xdrproc_t)xdr_SETATTR3args, (caddr_t)&args,
			     (xdrproc_t)xdr_SETATTR3res, (caddr_t)&res, tv);
	pthread_mutex_unlock(&ds->ds_clnt_mutex);

	if (rpc_stat != RPC_SUCCESS) {
		LOG("dstore[%u]: FENCE RPC failed", ds->ds_id);
		return -EIO;
	}

	if (res.status != NFS3_OK) {
		LOG("dstore[%u]: FENCE failed: status=%d", ds->ds_id,
		    res.status);
		xdr_free((xdrproc_t)xdr_SETATTR3res, (caddr_t)&res);
		return -EIO;
	}

	/* Update the cached values only after DS confirms. */
	ldf->ldf_uid = new_uid;
	ldf->ldf_gid = new_gid;

	TRACE("dstore[%u]: fenced to uid=%u gid=%u", ds->ds_id, new_uid,
	      new_gid);

	xdr_free((xdrproc_t)xdr_SETATTR3res, (caddr_t)&res);
	return 0;
}

/* ------------------------------------------------------------------ */
/* GETATTR                                                             */
/* ------------------------------------------------------------------ */

static int nfsv3_getattr(struct dstore *ds, const uint8_t *fh,
			     uint32_t fh_len, struct layout_data_file *ldf)
{
	GETATTR3args args;
	GETATTR3res res;
	struct timeval tv = ds_timeout();
	enum clnt_stat rpc_stat;

	memset(&args, 0, sizeof(args));
	memset(&res, 0, sizeof(res));

	args.object = make_fh3(fh, fh_len);

	pthread_mutex_lock(&ds->ds_clnt_mutex);
	if (!ds->ds_clnt) {
		pthread_mutex_unlock(&ds->ds_clnt_mutex);
		ldf->ldf_stale = true;
		return -ENOTCONN;
	}

	rpc_stat = clnt_call(ds->ds_clnt, NFSPROC3_GETATTR,
			     (xdrproc_t)xdr_GETATTR3args, (caddr_t)&args,
			     (xdrproc_t)xdr_GETATTR3res, (caddr_t)&res, tv);
	pthread_mutex_unlock(&ds->ds_clnt_mutex);

	if (rpc_stat != RPC_SUCCESS) {
		LOG("dstore[%u]: GETATTR RPC failed", ds->ds_id);
		ldf->ldf_stale = true;
		return -EIO;
	}

	if (res.status != NFS3_OK) {
		LOG("dstore[%u]: GETATTR failed: status=%d", ds->ds_id,
		    res.status);
		ldf->ldf_stale = true;
		xdr_free((xdrproc_t)xdr_GETATTR3res, (caddr_t)&res);
		return -EIO;
	}

	fattr3 *fa = &res.GETATTR3res_u.resok.obj_attributes;

	ldf->ldf_size = (int64_t)fa->size;
	ldf->ldf_uid = fa->uid;
	ldf->ldf_gid = fa->gid;
	ldf->ldf_mode = (uint16_t)fa->mode;

	/* Convert NFS3 nfstime3 to struct timespec. */
	ldf->ldf_atime.tv_sec = fa->atime.seconds;
	ldf->ldf_atime.tv_nsec = fa->atime.nseconds;
	ldf->ldf_mtime.tv_sec = fa->mtime.seconds;
	ldf->ldf_mtime.tv_nsec = fa->mtime.nseconds;
	ldf->ldf_ctime.tv_sec = fa->ctime.seconds;
	ldf->ldf_ctime.tv_nsec = fa->ctime.nseconds;

	ldf->ldf_stale = false;

	xdr_free((xdrproc_t)xdr_GETATTR3res, (caddr_t)&res);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Vtable                                                              */
/* ------------------------------------------------------------------ */

const struct dstore_ops dstore_ops_nfsv3 = {
	.name = "nfsv3",
	.create = nfsv3_create,
	.remove = nfsv3_remove,
	.chmod = nfsv3_chmod,
	.truncate = nfsv3_truncate,
	.fence = nfsv3_fence,
	.getattr = nfsv3_getattr,
};
