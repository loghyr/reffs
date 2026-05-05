/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Dstore control-plane operations -- NFSv4.2 compounds to data servers.
 *
 * The MDS acts as a plain NFSv4 client to the DS.  Each operation
 * builds a COMPOUND using the ec_demo client library (mds_compound)
 * and sends it over the MDS-->DS session (ds->ds_v4_session).
 *
 * All operations are synchronous (blocking).  The session is
 * single-slot, so operations to the same DS are serialized.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"
#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/layout_segment.h"
#include "reffs/log.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Add SEQUENCE + PUTFH to a compound.  Most DS ops start with this.
 */
static int add_seq_putfh(struct mds_compound *mc, struct mds_session *ms,
			 const uint8_t *fh, uint32_t fh_len)
{
	if (mds_compound_add_sequence(mc, ms))
		return -ENOSPC;

	nfs_argop4 *slot = mds_compound_add_op(mc, OP_PUTFH);

	if (!slot)
		return -ENOSPC;

	PUTFH4args *pf = &slot->nfs_argop4_u.opputfh;

	pf->object.nfs_fh4_len = fh_len;
	pf->object.nfs_fh4_val = (char *)fh;
	return 0;
}

/*
 * Send a compound and check for success.  Returns 0 on NFS4_OK,
 * -EIO on RPC or compound-level failure.
 */
static int send_and_check(struct mds_compound *mc, struct mds_session *ms,
			  uint32_t ds_id)
{
	int ret = mds_compound_send(mc, ms);

	if (ret) {
		LOG("dstore[%u]: NFSv4 compound RPC failed: ret=%d status=%u resarray_len=%u",
		    ds_id, ret, (unsigned)mc->mc_res.status,
		    mc->mc_res.resarray.resarray_len);
		return -EIO;
	}

	if (mc->mc_res.status != NFS4_OK) {
		LOG("dstore[%u]: NFSv4 compound failed: status=%d", ds_id,
		    mc->mc_res.status);
		return -EIO;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* CREATE                                                              */
/* ------------------------------------------------------------------ */

static int nfsv4_create(struct dstore *ds, const uint8_t *dir_fh,
			uint32_t dir_fh_len, const char *name, uint8_t *out_fh,
			uint32_t *out_fh_len)
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms)
		return -ENOTCONN;

	/* SEQUENCE + PUTFH(dir) + OPEN(CREATE) + GETFH
	 *
	 * Phase 2 note: dropped the trailing CLOSE that the original
	 * 5-op compound included.  The CLOSE used the special
	 * "current stateid" sentinel (seqid=NFS4_UINT32_MAX,
	 * other=0xFF...) which our DS handler does not resolve --
	 * the COMPOUND came back with status=NFS4ERR_BAD_STATEID
	 * (resarray_len=5, so SEQ+PUTFH+OPEN+GETFH all ran; only
	 * CLOSE was rejected).  For runway pool files, the open
	 * stateid leak is acceptable on a benchmark stack; the
	 * proper fix is either DS-side current-stateid support or
	 * splitting into a second compound that carries the
	 * server-allocated stateid explicitly.  Tracked as a
	 * follow-on; not blocking Phase 2 bring-up.
	 */
	ret = mds_compound_init(&mc, 4, "ds_create");
	if (ret)
		return ret;

	if (add_seq_putfh(&mc, ms, dir_fh, dir_fh_len))
		goto err;

	slot = mds_compound_add_op(&mc, OP_OPEN);
	if (!slot)
		goto err;

	OPEN4args *oa = &slot->nfs_argop4_u.opopen;

	oa->seqid = 0;
	oa->share_access = OPEN4_SHARE_ACCESS_BOTH;
	oa->share_deny = OPEN4_SHARE_DENY_NONE;
	oa->owner.clientid = ms->ms_clientid;
	oa->owner.owner.owner_val = ms->ms_owner;
	oa->owner.owner.owner_len = strlen(ms->ms_owner);
	oa->openhow.opentype = OPEN4_CREATE;
	oa->openhow.openflag4_u.how.mode = UNCHECKED4;
	/* No createattrs -- file gets DS default mode. */
	oa->openhow.openflag4_u.how.createhow4_u.createattrs.attrmask
		.bitmap4_len = 0;
	oa->openhow.openflag4_u.how.createhow4_u.createattrs.attrmask
		.bitmap4_val = NULL;
	oa->openhow.openflag4_u.how.createhow4_u.createattrs.attr_vals
		.attrlist4_len = 0;
	oa->openhow.openflag4_u.how.createhow4_u.createattrs.attr_vals
		.attrlist4_val = NULL;
	oa->claim.claim = CLAIM_NULL;
	oa->claim.open_claim4_u.file.utf8string_val = (char *)name;
	oa->claim.open_claim4_u.file.utf8string_len = strlen(name);

	if (!mds_compound_add_op(&mc, OP_GETFH))
		goto err;

	ret = send_and_check(&mc, ms, ds->ds_id);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	/* Extract the FH from GETFH result (op index 3). */
	if (mc.mc_res.resarray.resarray_len >= 4) {
		nfs_resop4 *getfh_slot = &mc.mc_res.resarray.resarray_val[3];
		GETFH4resok *gfr =
			&getfh_slot->nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

		if (gfr->object.nfs_fh4_len <= LAYOUT_SEG_MAX_FH) {
			memcpy(out_fh, gfr->object.nfs_fh4_val,
			       gfr->object.nfs_fh4_len);
			*out_fh_len = gfr->object.nfs_fh4_len;
		} else {
			ret = -EOVERFLOW;
		}
	} else {
		ret = -EIO;
	}

	mds_compound_fini(&mc);
	return ret;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/* ------------------------------------------------------------------ */
/* REMOVE                                                              */
/* ------------------------------------------------------------------ */

static int nfsv4_remove(struct dstore *ds, const uint8_t *dir_fh,
			uint32_t dir_fh_len, const char *name)
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms)
		return -ENOTCONN;

	ret = mds_compound_init(&mc, 3, "ds_remove");
	if (ret)
		return ret;

	if (add_seq_putfh(&mc, ms, dir_fh, dir_fh_len))
		goto err;

	slot = mds_compound_add_op(&mc, OP_REMOVE);
	if (!slot)
		goto err;

	REMOVE4args *ra = &slot->nfs_argop4_u.opremove;

	ra->target.utf8string_val = (char *)name;
	ra->target.utf8string_len = strlen(name);

	ret = send_and_check(&mc, ms, ds->ds_id);
	mds_compound_fini(&mc);
	return ret;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/* ------------------------------------------------------------------ */
/* GETATTR                                                             */
/* ------------------------------------------------------------------ */

static int nfsv4_getattr(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
			 struct layout_data_file *ldf)
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms) {
		ldf->ldf_stale = true;
		return -ENOTCONN;
	}

	/* SEQUENCE + PUTFH + GETATTR(size, mode, uid, gid, times) */
	ret = mds_compound_init(&mc, 3, "ds_getattr");
	if (ret)
		return ret;

	if (add_seq_putfh(&mc, ms, fh, fh_len))
		goto err;

	slot = mds_compound_add_op(&mc, OP_GETATTR);
	if (!slot)
		goto err;

	/*
	 * Request size (bit 4) and time_modify (bit 53 = word 1 bit 21).
	 * The reflected GETATTR needs size to update i_used/sb_bytes_used
	 * and mtime to detect DS-side writes.
	 */
	static uint32_t attr_bits[] = { 0x00000010, 0x00200000 };

	GETATTR4args *ga = &slot->nfs_argop4_u.opgetattr;

	ga->attr_request.bitmap4_len = 2;
	ga->attr_request.bitmap4_val = attr_bits;

	ret = send_and_check(&mc, ms, ds->ds_id);
	if (ret) {
		ldf->ldf_stale = true;
		mds_compound_fini(&mc);
		return ret;
	}

	/* Parse the GETATTR result -- extract size from fattr4. */
	if (mc.mc_res.resarray.resarray_len >= 3) {
		nfs_resop4 *ga_slot = &mc.mc_res.resarray.resarray_val[2];
		GETATTR4resok *gaok =
			&ga_slot->nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;

		/*
		 * Decode the fattr4 opaque.  Attrs are encoded in
		 * bitmap order: size (uint64), then time_modify
		 * (nfstime4: seconds uint64 + nseconds uint32).
		 */
		fattr4 *fa = &gaok->obj_attributes;

		if (fa->attr_vals.attrlist4_len >= 8) {
			XDR xdrs;
			uint64_t size;

			xdrmem_create(&xdrs, fa->attr_vals.attrlist4_val,
				      fa->attr_vals.attrlist4_len, XDR_DECODE);
			if (xdr_uint64_t(&xdrs, &size))
				ldf->ldf_size = (int64_t)size;

			/* time_modify: nfstime4 = {seconds, nseconds} */
			nfstime4 mtime = { 0 };
			if (xdr_nfstime4(&xdrs, &mtime)) {
				ldf->ldf_mtime.tv_sec = mtime.seconds;
				ldf->ldf_mtime.tv_nsec = mtime.nseconds;
			}
			xdr_destroy(&xdrs);
		}
		ldf->ldf_stale = false;
	} else {
		ldf->ldf_stale = true;
		ret = -EIO;
	}

	mds_compound_fini(&mc);
	return ret;

err:
	ldf->ldf_stale = true;
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/* ------------------------------------------------------------------ */
/* CHMOD (SETATTR mode)                                                */
/* ------------------------------------------------------------------ */

static int nfsv4_chmod(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
		       struct dstore_wcc *wcc __attribute__((unused)))
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms)
		return -ENOTCONN;

	/* SEQUENCE + PUTFH + SETATTR(mode=0640) */
	ret = mds_compound_init(&mc, 3, "ds_chmod");
	if (ret)
		return ret;

	if (add_seq_putfh(&mc, ms, fh, fh_len))
		goto err;

	slot = mds_compound_add_op(&mc, OP_SETATTR);
	if (!slot)
		goto err;

	SETATTR4args *sa = &slot->nfs_argop4_u.opsetattr;

	/* Anonymous stateid. */
	memset(&sa->stateid, 0, sizeof(sa->stateid));

	/* Encode mode=0640 as fattr4. */
	static uint32_t mode_bits[] = { 0, 0x00000002 }; /* bit 33 = mode */
	uint32_t mode_val = 0640;
	char mode_buf[4];
	XDR mx;

	xdrmem_create(&mx, mode_buf, sizeof(mode_buf), XDR_ENCODE);
	xdr_uint32_t(&mx, &mode_val);
	xdr_destroy(&mx);

	sa->obj_attributes.attrmask.bitmap4_len = 2;
	sa->obj_attributes.attrmask.bitmap4_val = mode_bits;
	sa->obj_attributes.attr_vals.attrlist4_len = 4;
	sa->obj_attributes.attr_vals.attrlist4_val = mode_buf;

	ret = send_and_check(&mc, ms, ds->ds_id);
	mds_compound_fini(&mc);
	return ret;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/* ------------------------------------------------------------------ */
/* TRUNCATE (SETATTR size)                                             */
/* ------------------------------------------------------------------ */

static int nfsv4_truncate(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
			  uint64_t size,
			  struct dstore_wcc *wcc __attribute__((unused)))
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms)
		return -ENOTCONN;

	/* SEQUENCE + PUTFH + SETATTR(size) */
	ret = mds_compound_init(&mc, 3, "ds_truncate");
	if (ret)
		return ret;

	if (add_seq_putfh(&mc, ms, fh, fh_len))
		goto err;

	slot = mds_compound_add_op(&mc, OP_SETATTR);
	if (!slot)
		goto err;

	SETATTR4args *sa = &slot->nfs_argop4_u.opsetattr;

	memset(&sa->stateid, 0, sizeof(sa->stateid));

	/* Encode size as fattr4. */
	static uint32_t size_bits[] = { 0x00000010 }; /* bit 4 = size */
	char size_buf[8];
	XDR sx;

	xdrmem_create(&sx, size_buf, sizeof(size_buf), XDR_ENCODE);
	xdr_uint64_t(&sx, &size);
	xdr_destroy(&sx);

	sa->obj_attributes.attrmask.bitmap4_len = 1;
	sa->obj_attributes.attrmask.bitmap4_val = size_bits;
	sa->obj_attributes.attr_vals.attrlist4_len = 8;
	sa->obj_attributes.attr_vals.attrlist4_val = size_buf;

	ret = send_and_check(&mc, ms, ds->ds_id);
	mds_compound_fini(&mc);
	return ret;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/* ------------------------------------------------------------------ */
/* FENCE (SETATTR uid, gid)                                            */
/* ------------------------------------------------------------------ */

static int nfsv4_fence(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
		       struct layout_data_file *ldf, uint32_t fence_min,
		       uint32_t fence_max,
		       struct dstore_wcc *wcc __attribute__((unused)))
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	int ret;

	if (!ms)
		return -ENOTCONN;

	/*
	 * Fencing rotates synthetic uid/gid within the configured range.
	 * For NFSv4, we set the owner/owner_group attrs as numeric strings.
	 *
	 * For now, just update the ldf in-memory -- the DS doesn't enforce
	 * fencing for AUTH_SYS.  NOT_NOW_BROWN_COW: actual SETATTR(owner).
	 */
	uint32_t new_uid = ldf->ldf_uid + 1;

	if (new_uid > fence_max)
		new_uid = fence_min;
	ldf->ldf_uid = new_uid;
	ldf->ldf_gid = new_uid;

	/* Send a no-op compound to keep the session alive. */
	ret = mds_compound_init(&mc, 2, "ds_fence");
	if (ret)
		return ret;

	if (add_seq_putfh(&mc, ms, fh, fh_len))
		goto err;

	ret = send_and_check(&mc, ms, ds->ds_id);
	mds_compound_fini(&mc);
	return ret;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/* ------------------------------------------------------------------ */
/* InBand READ                                                         */
/* ------------------------------------------------------------------ */

static ssize_t nfsv4_read(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
			  void *buf, size_t len, uint64_t offset,
			  uint32_t uid __attribute__((unused)),
			  uint32_t gid __attribute__((unused)))
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms)
		return -ENOTCONN;

	ret = mds_compound_init(&mc, 3, "ds_read");
	if (ret)
		return ret;

	if (add_seq_putfh(&mc, ms, fh, fh_len))
		goto err;

	slot = mds_compound_add_op(&mc, OP_READ);
	if (!slot)
		goto err;

	READ4args *ra = &slot->nfs_argop4_u.opread;

	memset(&ra->stateid, 0, sizeof(ra->stateid)); /* anonymous */
	ra->offset = offset;
	ra->count = (uint32_t)(len > UINT32_MAX ? UINT32_MAX : len);

	ret = send_and_check(&mc, ms, ds->ds_id);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	ssize_t bytes_read = 0;

	if (mc.mc_res.resarray.resarray_len >= 3) {
		nfs_resop4 *r_slot = &mc.mc_res.resarray.resarray_val[2];
		READ4resok *rok =
			&r_slot->nfs_resop4_u.opread.READ4res_u.resok4;

		size_t copy = rok->data.data_len;

		if (copy > len)
			copy = len;
		memcpy(buf, rok->data.data_val, copy);
		bytes_read = (ssize_t)copy;
	}

	mds_compound_fini(&mc);
	return bytes_read;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/* ------------------------------------------------------------------ */
/* InBand WRITE                                                        */
/* ------------------------------------------------------------------ */

static ssize_t nfsv4_write(struct dstore *ds, const uint8_t *fh,
			   uint32_t fh_len, const void *buf, size_t len,
			   uint64_t offset,
			   uint32_t uid __attribute__((unused)),
			   uint32_t gid __attribute__((unused)))
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms)
		return -ENOTCONN;

	ret = mds_compound_init(&mc, 3, "ds_write");
	if (ret)
		return ret;

	if (add_seq_putfh(&mc, ms, fh, fh_len))
		goto err;

	slot = mds_compound_add_op(&mc, OP_WRITE);
	if (!slot)
		goto err;

	WRITE4args *wa = &slot->nfs_argop4_u.opwrite;

	memset(&wa->stateid, 0, sizeof(wa->stateid)); /* anonymous */
	wa->offset = offset;
	wa->stable = FILE_SYNC4;
	wa->data.data_val = (char *)buf;
	wa->data.data_len = (uint32_t)(len > UINT32_MAX ? UINT32_MAX : len);

	ret = send_and_check(&mc, ms, ds->ds_id);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	ssize_t bytes_written = 0;

	if (mc.mc_res.resarray.resarray_len >= 3) {
		nfs_resop4 *w_slot = &mc.mc_res.resarray.resarray_val[2];
		WRITE4resok *wok =
			&w_slot->nfs_resop4_u.opwrite.WRITE4res_u.resok4;

		bytes_written = (ssize_t)wok->count;
	}

	mds_compound_fini(&mc);
	return bytes_written;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/* ------------------------------------------------------------------ */
/* InBand COMMIT                                                       */
/* ------------------------------------------------------------------ */

static int nfsv4_commit(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
			uint64_t offset, uint32_t count)
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms)
		return -ENOTCONN;

	ret = mds_compound_init(&mc, 3, "ds_commit");
	if (ret)
		return ret;

	if (add_seq_putfh(&mc, ms, fh, fh_len))
		goto err;

	slot = mds_compound_add_op(&mc, OP_COMMIT);
	if (!slot)
		goto err;

	COMMIT4args *ca = &slot->nfs_argop4_u.opcommit;

	ca->offset = offset;
	ca->count = count;

	ret = send_and_check(&mc, ms, ds->ds_id);
	mds_compound_fini(&mc);
	return ret;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/* ------------------------------------------------------------------ */
/* Tight-coupling control plane                                        */
/* ------------------------------------------------------------------ */

/*
 * nfsv4_probe_tight_coupling -- capability probe.
 *
 * Sends SEQUENCE + PUTROOTFH + TRUST_STATEID(anonymous stateid).
 * The DS handler returns NFS4ERR_INVAL for any special stateid,
 * which is the correct signal that tight coupling is supported.
 *
 * Returns 0 if tight coupling is available,
 *        -ENOTSUP if the DS does not support TRUST_STATEID,
 *        -EIO on transport error.
 */
static int nfsv4_probe_tight_coupling(struct dstore *ds)
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms)
		return -EIO;

	/*
	 * SEQUENCE + PUTROOTFH + TRUST_STATEID(anon)
	 *
	 * We send an anonymous stateid (all-zero) which the DS handler
	 * recognises as a special stateid and rejects with NFS4ERR_INVAL.
	 * That error code is the capability probe signal.
	 */
	ret = mds_compound_init(&mc, 3, "ds_probe_tight_coupling");
	if (ret)
		return ret;

	if (mds_compound_add_sequence(&mc, ms))
		goto err;

	if (!mds_compound_add_op(&mc, OP_PUTROOTFH))
		goto err;

	slot = mds_compound_add_op(&mc, OP_TRUST_STATEID);
	if (!slot)
		goto err;

	TRUST_STATEID4args *ta = &slot->nfs_argop4_u.optrust_stateid;

	/* Anonymous stateid: seqid=0, other=all-zeros. */
	memset(&ta->tsa_layout_stateid, 0, sizeof(ta->tsa_layout_stateid));
	ta->tsa_iomode = LAYOUTIOMODE4_READ;
	ta->tsa_expire.seconds = 0;
	ta->tsa_expire.nseconds = 0;
	ta->tsa_principal.utf8string_len = 0;
	ta->tsa_principal.utf8string_val = NULL;

	ret = mds_compound_send(&mc, ms);
	if (ret == 0) {
		/* NFS4_OK: DS bug, but treat as supported per design. */
		TRACE("dstore[%u]: TRUST_STATEID probe returned NFS4_OK "
		      "(unexpected -- treating as tight coupling available)",
		      ds->ds_id);
	} else if (ret == -EREMOTEIO) {
		if (mc.mc_res.status == NFS4ERR_INVAL) {
			/* Expected: DS rejected anonymous stateid. */
			ret = 0;
		} else if (mc.mc_res.status == NFS4ERR_NOTSUPP) {
			ret = -ENOTSUP;
		} else {
			TRACE("dstore[%u]: TRUST_STATEID probe: status=%d",
			      ds->ds_id, mc.mc_res.status);
			ret = -EIO;
		}
	}
	/* else ret == -EIO: RPC transport failure */

	mds_compound_fini(&mc);
	return ret;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/*
 * nfsv4_trust_stateid -- register a layout stateid on the DS.
 */
static int nfsv4_trust_stateid(struct dstore *ds, const uint8_t *fh,
			       uint32_t fh_len, uint32_t stid_seqid,
			       const uint8_t *stid_other, uint32_t iomode,
			       uint64_t clientid __attribute__((unused)),
			       int64_t expire_sec, uint32_t expire_nsec,
			       const char *principal)
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms)
		return -ENOTCONN;

	/* SEQUENCE + PUTFH + TRUST_STATEID */
	ret = mds_compound_init(&mc, 3, "ds_trust_stateid");
	if (ret)
		return ret;

	if (add_seq_putfh(&mc, ms, fh, fh_len))
		goto err;

	slot = mds_compound_add_op(&mc, OP_TRUST_STATEID);
	if (!slot)
		goto err;

	TRUST_STATEID4args *ta = &slot->nfs_argop4_u.optrust_stateid;

	ta->tsa_layout_stateid.seqid = stid_seqid;
	memcpy(ta->tsa_layout_stateid.other, stid_other, NFS4_OTHER_SIZE);
	ta->tsa_iomode = (layoutiomode4)iomode;
	ta->tsa_expire.seconds = expire_sec;
	ta->tsa_expire.nseconds = expire_nsec;

	if (principal && *principal) {
		ta->tsa_principal.utf8string_val = (char *)principal;
		ta->tsa_principal.utf8string_len = strlen(principal);
	} else {
		ta->tsa_principal.utf8string_val = NULL;
		ta->tsa_principal.utf8string_len = 0;
	}

	ret = send_and_check(&mc, ms, ds->ds_id);
	mds_compound_fini(&mc);
	return ret;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/*
 * nfsv4_revoke_stateid -- remove a single stateid from the DS trust table.
 */
static int nfsv4_revoke_stateid(struct dstore *ds, const uint8_t *fh,
				uint32_t fh_len, uint32_t stid_seqid,
				const uint8_t *stid_other)
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms)
		return -ENOTCONN;

	/* SEQUENCE + PUTFH + REVOKE_STATEID */
	ret = mds_compound_init(&mc, 3, "ds_revoke_stateid");
	if (ret)
		return ret;

	if (add_seq_putfh(&mc, ms, fh, fh_len))
		goto err;

	slot = mds_compound_add_op(&mc, OP_REVOKE_STATEID);
	if (!slot)
		goto err;

	REVOKE_STATEID4args *ra = &slot->nfs_argop4_u.oprevoke_stateid;

	ra->rsa_layout_stateid.seqid = stid_seqid;
	memcpy(ra->rsa_layout_stateid.other, stid_other, NFS4_OTHER_SIZE);

	ret = send_and_check(&mc, ms, ds->ds_id);
	mds_compound_fini(&mc);
	return ret;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/*
 * nfsv4_bulk_revoke_stateid -- revoke all stateids for a client.
 *
 * Per the design, no PUTFH is needed: BULK_REVOKE_STATEID operates on the
 * entire trust table, not on a specific file.
 *
 * clientid == 0 means "revoke all" (MDS restart cleanup).
 */
static int nfsv4_bulk_revoke_stateid(struct dstore *ds, uint64_t clientid)
{
	struct mds_session *ms = ds->ds_v4_session;
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!ms)
		return -ENOTCONN;

	/* SEQUENCE + BULK_REVOKE_STATEID (no PUTFH) */
	ret = mds_compound_init(&mc, 2, "ds_bulk_revoke_stateid");
	if (ret)
		return ret;

	if (mds_compound_add_sequence(&mc, ms))
		goto err;

	slot = mds_compound_add_op(&mc, OP_BULK_REVOKE_STATEID);
	if (!slot)
		goto err;

	BULK_REVOKE_STATEID4args *ba =
		&slot->nfs_argop4_u.opbulk_revoke_stateid;

	ba->brsa_clientid = clientid;

	ret = send_and_check(&mc, ms, ds->ds_id);
	mds_compound_fini(&mc);
	return ret;

err:
	mds_compound_fini(&mc);
	return -ENOSPC;
}

/* ------------------------------------------------------------------ */
/* Vtable                                                              */
/* ------------------------------------------------------------------ */

const struct dstore_ops dstore_ops_nfsv4 = {
	.name = "nfsv4",
	.create = nfsv4_create,
	.remove = nfsv4_remove,
	.chmod = nfsv4_chmod,
	.truncate = nfsv4_truncate,
	.fence = nfsv4_fence,
	.getattr = nfsv4_getattr,
	.read = nfsv4_read,
	.write = nfsv4_write,
	.commit = nfsv4_commit,
	.probe_tight_coupling = nfsv4_probe_tight_coupling,
	.trust_stateid = nfsv4_trust_stateid,
	.revoke_stateid = nfsv4_revoke_stateid,
	.bulk_revoke_stateid = nfsv4_bulk_revoke_stateid,
};
