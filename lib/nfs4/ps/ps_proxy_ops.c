/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"

#include "nfs4/errors.h"

#include "ec_client.h"
#include "ps_proxy_ops.h"
#include "ps_state.h" /* PS_MAX_FH_SIZE */

void ps_proxy_getattr_reply_free(struct ps_proxy_getattr_reply *reply)
{
	if (!reply)
		return;
	free(reply->attrmask);
	free(reply->attr_vals);
	memset(reply, 0, sizeof(*reply));
}

int ps_proxy_forward_getattr(struct mds_session *ms, const uint8_t *upstream_fh,
			     uint32_t upstream_fh_len,
			     const uint32_t *requested_mask,
			     uint32_t requested_mask_len,
			     const struct authunix_parms *creds,
			     struct ps_proxy_getattr_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !upstream_fh || upstream_fh_len == 0 || !requested_mask ||
	    requested_mask_len == 0 || !reply)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	/*
	 * Zero the output up front so every early-return path leaves
	 * the caller's struct in a well-defined state even without a
	 * ps_proxy_getattr_reply_free call.
	 */
	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH + GETATTR = 3 ops */
	ret = mds_compound_init(&mc, 3, "ps-proxy-getattr");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	/*
	 * PUTFH takes a non-const nfs_fh4.  The buffer is read-only
	 * from our side but TIRPC's encode path doesn't mark its
	 * argument const; cast away the qualifier to satisfy the
	 * type without copying (the upstream FH lives for the
	 * duration of mds_compound_send below).
	 */
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)upstream_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = upstream_fh_len;

	slot = mds_compound_add_op(&mc, OP_GETATTR);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	GETATTR4args *ga_args = &slot->nfs_argop4_u.opgetattr;

	/* Same const-cast rationale as PUTFH: TIRPC encode is read-only. */
	ga_args->attr_request.bitmap4_val = (uint32_t *)requested_mask;
	ga_args->attr_request.bitmap4_len = requested_mask_len;

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	/*
	 * -EREMOTEIO from mds_compound_send means the wire round-trip
	 * succeeded but the COMPOUND4res top-level status was not
	 * NFS4_OK (i.e. at least one op in the compound failed).  We
	 * still want to inspect individual op statuses below to
	 * surface the per-op error to the caller (NFS4ERR_NOENT on
	 * LOOKUP, etc).  Only true wire failures (-EIO et al) bail.
	 */
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* GETATTR result at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opgetattr.status);
	if (ret)
		goto out;

	GETATTR4resok *gresok =
		&res->nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
	fattr4 *src = &gresok->obj_attributes;

	/*
	 * Copy out of the compound's XDR-owned buffers so the caller
	 * can hold the reply across mds_compound_fini.  Bitmap is
	 * optional in the wire sense (a zero-len bitmap is legal if
	 * the MDS elected not to return anything), and so is the
	 * attr_vals payload; handle both with guarded callocs.
	 */
	if (src->attrmask.bitmap4_len > 0) {
		reply->attrmask = calloc(src->attrmask.bitmap4_len,
					 sizeof(*reply->attrmask));
		if (!reply->attrmask) {
			ret = -ENOMEM;
			goto out_free_reply;
		}
		memcpy(reply->attrmask, src->attrmask.bitmap4_val,
		       src->attrmask.bitmap4_len * sizeof(*reply->attrmask));
		reply->attrmask_len = src->attrmask.bitmap4_len;
	}

	if (src->attr_vals.attrlist4_len > 0) {
		reply->attr_vals = calloc(src->attr_vals.attrlist4_len, 1);
		if (!reply->attr_vals) {
			ret = -ENOMEM;
			goto out_free_reply;
		}
		memcpy(reply->attr_vals, src->attr_vals.attrlist4_val,
		       src->attr_vals.attrlist4_len);
		reply->attr_vals_len = src->attr_vals.attrlist4_len;
	}

	ret = 0;
	goto out;

out_free_reply:
	ps_proxy_getattr_reply_free(reply);
out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_proxy_forward_lookup(struct mds_session *ms, const uint8_t *parent_fh,
			    uint32_t parent_fh_len, const char *name,
			    uint32_t name_len, uint8_t *child_fh_buf,
			    uint32_t child_fh_buf_len,
			    uint32_t *child_fh_len_out,
			    const uint32_t *attr_request,
			    uint32_t attr_request_len,
			    const struct authunix_parms *creds,
			    struct ps_proxy_attrs_min *attrs_out)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !parent_fh || parent_fh_len == 0 || !name || name_len == 0 ||
	    !child_fh_buf || !child_fh_len_out)
		return -EINVAL;
	if (parent_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	/*
	 * A caller that passes an attr_request without a sink (or a
	 * sink without a request) is programmer-inconsistent -- the
	 * compound either asks for attrs (both non-NULL with non-zero
	 * length) or does not (both NULL/zero).  Mismatch is never
	 * what the caller means; fail fast.
	 */
	bool want_attrs = (attr_request && attr_request_len > 0 && attrs_out);

	if ((!!attr_request != !!attrs_out) ||
	    (attr_request && attr_request_len == 0) ||
	    (attrs_out && !attr_request))
		return -EINVAL;

	/* 4 ops without GETATTR, 5 ops with. */
	unsigned nops = want_attrs ? 5 : 4;

	ret = mds_compound_init(&mc, nops, "ps-proxy-lookup");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	/* Same const-cast rationale as ps_proxy_forward_getattr. */
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)parent_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = parent_fh_len;

	slot = mds_compound_add_op(&mc, OP_LOOKUP);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.oplookup.objname.utf8string_val = (char *)name;
	slot->nfs_argop4_u.oplookup.objname.utf8string_len = name_len;

	slot = mds_compound_add_op(&mc, OP_GETFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	if (want_attrs) {
		slot = mds_compound_add_op(&mc, OP_GETATTR);
		if (!slot) {
			ret = -ENOSPC;
			goto out;
		}
		GETATTR4args *ga = &slot->nfs_argop4_u.opgetattr;

		/* Same const-cast rationale as ps_proxy_forward_getattr. */
		ga->attr_request.bitmap4_val = (uint32_t *)attr_request;
		ga->attr_request.bitmap4_len = attr_request_len;
	}

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	/*
	 * -EREMOTEIO from mds_compound_send means the wire round-trip
	 * succeeded but the COMPOUND4res top-level status was not
	 * NFS4_OK (i.e. at least one op in the compound failed).  We
	 * still want to inspect individual op statuses below to
	 * surface the per-op error to the caller (NFS4ERR_NOENT on
	 * LOOKUP, etc).  Only true wire failures (-EIO et al) bail.
	 */
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/*
	 * LOOKUP status at index 2.  nfs4_to_errno maps NFS4ERR_NOENT
	 * to -ENOENT (so the caller can return NFS4ERR_NOENT to the
	 * client) and other status codes to their errno equivalents
	 * (NFS4ERR_ACCESS -> -EACCES, etc.); unknown statuses collapse
	 * to -EREMOTEIO.
	 */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.oplookup.status);
	if (ret)
		goto out;

	/* GETFH result at index 3. */
	res = mds_compound_result(&mc, 3);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opgetfh.status);
	if (ret)
		goto out;

	GETFH4resok *fhresok = &res->nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

	/*
	 * Zero-length FH would be a broken MDS response (no usable
	 * anchor); matches the guard in ps_discovery_fetch_root_fh.
	 */
	if (fhresok->object.nfs_fh4_len == 0) {
		ret = -EREMOTEIO;
		goto out;
	}
	if (fhresok->object.nfs_fh4_len > child_fh_buf_len) {
		ret = -ENOSPC;
		goto out;
	}

	if (want_attrs) {
		/* GETATTR result at index 4. */
		res = mds_compound_result(&mc, 4);
		if (!res) {
			ret = -EREMOTEIO;
			goto out;
		}
		ret = nfs4_to_errno(res->nfs_resop4_u.opgetattr.status);
		if (ret)
			goto out;

		GETATTR4resok *gresok =
			&res->nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
		fattr4 *src = &gresok->obj_attributes;

		/*
		 * attrlist4 is `char *` on the wire (generated XDR);
		 * the parser takes `const uint8_t *` because it only
		 * does unsigned byte-level decodes.  Cast to drop the
		 * signedness difference -- same pattern as the PUTFH
		 * nfs_fh4_val const-cast elsewhere in this file.
		 */
		ret = ps_proxy_parse_attrs_min(
			src->attrmask.bitmap4_val, src->attrmask.bitmap4_len,
			(const uint8_t *)src->attr_vals.attrlist4_val,
			src->attr_vals.attrlist4_len, attrs_out);
		if (ret < 0)
			goto out;
	}

	memcpy(child_fh_buf, fhresok->object.nfs_fh4_val,
	       fhresok->object.nfs_fh4_len);
	*child_fh_len_out = fhresok->object.nfs_fh4_len;
	ret = 0;

out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_proxy_forward_write(struct mds_session *ms, const uint8_t *upstream_fh,
			   uint32_t upstream_fh_len, uint32_t stateid_seqid,
			   const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
			   uint64_t offset, uint32_t stable,
			   const uint8_t *data, uint32_t data_len,
			   const struct authunix_parms *creds,
			   struct ps_proxy_write_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !upstream_fh || upstream_fh_len == 0 || !stateid_other ||
	    !data || data_len == 0 || !reply)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;
	/* stable_how4: UNSTABLE4=0, DATA_SYNC4=1, FILE_SYNC4=2. */
	if (stable > FILE_SYNC4)
		return -EINVAL;

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH + WRITE = 3 ops */
	ret = mds_compound_init(&mc, 3, "ps-proxy-write");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)upstream_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = upstream_fh_len;

	slot = mds_compound_add_op(&mc, OP_WRITE);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	WRITE4args *wa = &slot->nfs_argop4_u.opwrite;

	wa->stateid.seqid = stateid_seqid;
	memcpy(wa->stateid.other, stateid_other, PS_STATEID_OTHER_SIZE);
	wa->offset = offset;
	wa->stable = (stable_how4)stable;
	wa->data.data_val = (char *)data;
	wa->data.data_len = data_len;

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	/*
	 * -EREMOTEIO from mds_compound_send means the wire round-trip
	 * succeeded but the COMPOUND4res top-level status was not
	 * NFS4_OK (i.e. at least one op in the compound failed).  We
	 * still want to inspect individual op statuses below to
	 * surface the per-op error to the caller (NFS4ERR_NOENT on
	 * LOOKUP, etc).  Only true wire failures (-EIO et al) bail.
	 */
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* WRITE result at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opwrite.status);
	if (ret)
		goto out;

	WRITE4resok *wresok = &res->nfs_resop4_u.opwrite.WRITE4res_u.resok4;

	reply->count = wresok->count;
	reply->committed = (uint32_t)wresok->committed;
	memcpy(reply->verifier, wresok->writeverf, PS_PROXY_VERIFIER_SIZE);
	ret = 0;

out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_proxy_forward_commit(struct mds_session *ms, const uint8_t *upstream_fh,
			    uint32_t upstream_fh_len, uint64_t offset,
			    uint32_t count, const struct authunix_parms *creds,
			    struct ps_proxy_commit_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !upstream_fh || !reply)
		return -EINVAL;
	if (upstream_fh_len == 0)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH + COMMIT = 3 ops */
	ret = mds_compound_init(&mc, 3, "ps-proxy-commit");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)upstream_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = upstream_fh_len;

	slot = mds_compound_add_op(&mc, OP_COMMIT);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	COMMIT4args *ca = &slot->nfs_argop4_u.opcommit;

	ca->offset = offset;
	ca->count = count;

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* COMMIT result at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opcommit.status);
	if (ret)
		goto out;

	COMMIT4resok *cresok = &res->nfs_resop4_u.opcommit.COMMIT4res_u.resok4;

	memcpy(reply->verifier, cresok->writeverf, PS_PROXY_VERIFIER_SIZE);
	ret = 0;

out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_proxy_forward_remove(struct mds_session *ms, const uint8_t *parent_fh,
			    uint32_t parent_fh_len, const char *name,
			    uint32_t name_len,
			    const struct authunix_parms *creds,
			    struct ps_proxy_remove_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !parent_fh || !name || !reply)
		return -EINVAL;
	if (parent_fh_len == 0 || name_len == 0)
		return -EINVAL;
	if (parent_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH + REMOVE = 3 ops */
	ret = mds_compound_init(&mc, 3, "ps-proxy-remove");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)parent_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = parent_fh_len;

	slot = mds_compound_add_op(&mc, OP_REMOVE);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opremove.target.utf8string_val = (char *)name;
	slot->nfs_argop4_u.opremove.target.utf8string_len = name_len;

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* REMOVE result at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opremove.status);
	if (ret)
		goto out;

	REMOVE4resok *rresok = &res->nfs_resop4_u.opremove.REMOVE4res_u.resok4;

	reply->atomic = rresok->cinfo.atomic;
	reply->before = rresok->cinfo.before;
	reply->after = rresok->cinfo.after;
	ret = 0;

out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_proxy_forward_rename(struct mds_session *ms, const uint8_t *src_fh,
			    uint32_t src_fh_len, const char *oldname,
			    uint32_t oldname_len, const uint8_t *dst_fh,
			    uint32_t dst_fh_len, const char *newname,
			    uint32_t newname_len,
			    const struct authunix_parms *creds,
			    struct ps_proxy_rename_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !src_fh || !oldname || !dst_fh || !newname || !reply)
		return -EINVAL;
	if (src_fh_len == 0 || oldname_len == 0 || dst_fh_len == 0 ||
	    newname_len == 0)
		return -EINVAL;
	if (src_fh_len > PS_MAX_FH_SIZE || dst_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH(src) + SAVEFH + PUTFH(dst) + RENAME = 5 ops */
	ret = mds_compound_init(&mc, 5, "ps-proxy-rename");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)src_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = src_fh_len;

	slot = mds_compound_add_op(&mc, OP_SAVEFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)dst_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = dst_fh_len;

	slot = mds_compound_add_op(&mc, OP_RENAME);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.oprename.oldname.utf8string_val = (char *)oldname;
	slot->nfs_argop4_u.oprename.oldname.utf8string_len = oldname_len;
	slot->nfs_argop4_u.oprename.newname.utf8string_val = (char *)newname;
	slot->nfs_argop4_u.oprename.newname.utf8string_len = newname_len;

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH(src) status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* SAVEFH status at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opsavefh.status);
	if (ret)
		goto out;

	/* PUTFH(dst) status at index 3. */
	res = mds_compound_result(&mc, 3);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* RENAME result at index 4. */
	res = mds_compound_result(&mc, 4);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.oprename.status);
	if (ret)
		goto out;

	RENAME4resok *rrresok = &res->nfs_resop4_u.oprename.RENAME4res_u.resok4;

	reply->source_cinfo.atomic = rrresok->source_cinfo.atomic;
	reply->source_cinfo.before = rrresok->source_cinfo.before;
	reply->source_cinfo.after = rrresok->source_cinfo.after;
	reply->target_cinfo.atomic = rrresok->target_cinfo.atomic;
	reply->target_cinfo.before = rrresok->target_cinfo.before;
	reply->target_cinfo.after = rrresok->target_cinfo.after;
	ret = 0;

out:
	mds_compound_fini(&mc);
	return ret;
}

void ps_proxy_mkdir_reply_free(struct ps_proxy_mkdir_reply *reply)
{
	if (!reply)
		return;
	free(reply->attrset_mask);
	memset(reply, 0, sizeof(*reply));
}

int ps_proxy_forward_mkdir(struct mds_session *ms, const uint8_t *parent_fh,
			   uint32_t parent_fh_len, const char *name,
			   uint32_t name_len, const uint32_t *createattrs_mask,
			   uint32_t createattrs_mask_len,
			   const uint8_t *createattrs_vals,
			   uint32_t createattrs_vals_len,
			   const struct authunix_parms *creds,
			   struct ps_proxy_mkdir_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !parent_fh || !name || !reply)
		return -EINVAL;
	if (parent_fh_len == 0 || name_len == 0)
		return -EINVAL;
	if (parent_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH + CREATE + GETFH + GETATTR = 5 ops */
	ret = mds_compound_init(&mc, 5, "ps-proxy-mkdir");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)parent_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = parent_fh_len;

	slot = mds_compound_add_op(&mc, OP_CREATE);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	CREATE4args *cra = &slot->nfs_argop4_u.opcreate;

	cra->objtype.type = NF4DIR;
	cra->objname.utf8string_val = (char *)name;
	cra->objname.utf8string_len = name_len;
	cra->createattrs.attrmask.bitmap4_val = (uint32_t *)createattrs_mask;
	cra->createattrs.attrmask.bitmap4_len = createattrs_mask_len;
	cra->createattrs.attr_vals.attrlist4_val = (char *)createattrs_vals;
	cra->createattrs.attr_vals.attrlist4_len = createattrs_vals_len;

	slot = mds_compound_add_op(&mc, OP_GETFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	slot = mds_compound_add_op(&mc, OP_GETATTR);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	/*
	 * Request FATTR4_TYPE | FATTR4_MODE so the caller's hook can
	 * materialise a local inode with the right type bits without
	 * a follow-up GETATTR round-trip.  Same bit set as
	 * ps_proxy_lookup_forward_for_inode uses.
	 */
	static const uint32_t mkdir_attr_req[] = { 0x2u, 0x2u };

	slot->nfs_argop4_u.opgetattr.attr_request.bitmap4_val =
		(uint32_t *)mkdir_attr_req;
	slot->nfs_argop4_u.opgetattr.attr_request.bitmap4_len = 2;

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* CREATE result at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opcreate.status);
	if (ret)
		goto out;

	CREATE4resok *cresok = &res->nfs_resop4_u.opcreate.CREATE4res_u.resok4;

	reply->cinfo.atomic = cresok->cinfo.atomic;
	reply->cinfo.before = cresok->cinfo.before;
	reply->cinfo.after = cresok->cinfo.after;

	if (cresok->attrset.bitmap4_len > 0) {
		reply->attrset_mask = calloc(cresok->attrset.bitmap4_len,
					     sizeof(*reply->attrset_mask));
		if (!reply->attrset_mask) {
			ret = -ENOMEM;
			goto out_free_reply;
		}
		memcpy(reply->attrset_mask, cresok->attrset.bitmap4_val,
		       cresok->attrset.bitmap4_len *
			       sizeof(*reply->attrset_mask));
		reply->attrset_mask_len = cresok->attrset.bitmap4_len;
	}

	/* GETFH result at index 3. */
	res = mds_compound_result(&mc, 3);
	if (!res) {
		ret = -EREMOTEIO;
		goto out_free_reply;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opgetfh.status);
	if (ret)
		goto out_free_reply;

	GETFH4resok *fhresok = &res->nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

	if (fhresok->object.nfs_fh4_len == 0) {
		ret = -EREMOTEIO;
		goto out_free_reply;
	}
	if (fhresok->object.nfs_fh4_len > PS_MAX_FH_SIZE) {
		ret = -ENOSPC;
		goto out_free_reply;
	}
	memcpy(reply->child_fh, fhresok->object.nfs_fh4_val,
	       fhresok->object.nfs_fh4_len);
	reply->child_fh_len = fhresok->object.nfs_fh4_len;

	/* GETATTR result at index 4. */
	res = mds_compound_result(&mc, 4);
	if (!res) {
		ret = -EREMOTEIO;
		goto out_free_reply;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opgetattr.status);
	if (ret)
		goto out_free_reply;

	GETATTR4resok *gresok =
		&res->nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;

	ret = ps_proxy_parse_attrs_min(
		gresok->obj_attributes.attrmask.bitmap4_val,
		gresok->obj_attributes.attrmask.bitmap4_len,
		(const uint8_t *)gresok->obj_attributes.attr_vals.attrlist4_val,
		gresok->obj_attributes.attr_vals.attrlist4_len,
		&reply->child_attrs);
	if (ret)
		goto out_free_reply;

	ret = 0;
	goto out;

out_free_reply:
	ps_proxy_mkdir_reply_free(reply);
out:
	mds_compound_fini(&mc);
	return ret;
}

void ps_proxy_symlink_reply_free(struct ps_proxy_symlink_reply *reply)
{
	if (!reply)
		return;
	free(reply->attrset_mask);
	memset(reply, 0, sizeof(*reply));
}

int ps_proxy_forward_symlink(struct mds_session *ms, const uint8_t *parent_fh,
			     uint32_t parent_fh_len, const char *name,
			     uint32_t name_len, const char *linkdata,
			     uint32_t linkdata_len,
			     const uint32_t *createattrs_mask,
			     uint32_t createattrs_mask_len,
			     const uint8_t *createattrs_vals,
			     uint32_t createattrs_vals_len,
			     const struct authunix_parms *creds,
			     struct ps_proxy_symlink_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !parent_fh || !name || !linkdata || !reply)
		return -EINVAL;
	if (parent_fh_len == 0 || name_len == 0 || linkdata_len == 0)
		return -EINVAL;
	if (parent_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH + CREATE + GETFH + GETATTR = 5 ops */
	ret = mds_compound_init(&mc, 5, "ps-proxy-symlink");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)parent_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = parent_fh_len;

	slot = mds_compound_add_op(&mc, OP_CREATE);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	CREATE4args *cra = &slot->nfs_argop4_u.opcreate;

	cra->objtype.type = NF4LNK;
	cra->objtype.createtype4_u.linkdata.linktext4_val = (char *)linkdata;
	cra->objtype.createtype4_u.linkdata.linktext4_len = linkdata_len;
	cra->objname.utf8string_val = (char *)name;
	cra->objname.utf8string_len = name_len;
	cra->createattrs.attrmask.bitmap4_val = (uint32_t *)createattrs_mask;
	cra->createattrs.attrmask.bitmap4_len = createattrs_mask_len;
	cra->createattrs.attr_vals.attrlist4_val = (char *)createattrs_vals;
	cra->createattrs.attr_vals.attrlist4_len = createattrs_vals_len;

	slot = mds_compound_add_op(&mc, OP_GETFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	slot = mds_compound_add_op(&mc, OP_GETATTR);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	static const uint32_t symlink_attr_req[] = { 0x2u, 0x2u };

	slot->nfs_argop4_u.opgetattr.attr_request.bitmap4_val =
		(uint32_t *)symlink_attr_req;
	slot->nfs_argop4_u.opgetattr.attr_request.bitmap4_len = 2;

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* CREATE result at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opcreate.status);
	if (ret)
		goto out;

	CREATE4resok *cresok = &res->nfs_resop4_u.opcreate.CREATE4res_u.resok4;

	reply->cinfo.atomic = cresok->cinfo.atomic;
	reply->cinfo.before = cresok->cinfo.before;
	reply->cinfo.after = cresok->cinfo.after;

	if (cresok->attrset.bitmap4_len > 0) {
		reply->attrset_mask = calloc(cresok->attrset.bitmap4_len,
					     sizeof(*reply->attrset_mask));
		if (!reply->attrset_mask) {
			ret = -ENOMEM;
			goto out_free_reply;
		}
		memcpy(reply->attrset_mask, cresok->attrset.bitmap4_val,
		       cresok->attrset.bitmap4_len *
			       sizeof(*reply->attrset_mask));
		reply->attrset_mask_len = cresok->attrset.bitmap4_len;
	}

	/* GETFH result at index 3. */
	res = mds_compound_result(&mc, 3);
	if (!res) {
		ret = -EREMOTEIO;
		goto out_free_reply;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opgetfh.status);
	if (ret)
		goto out_free_reply;

	GETFH4resok *fhresok = &res->nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

	if (fhresok->object.nfs_fh4_len == 0) {
		ret = -EREMOTEIO;
		goto out_free_reply;
	}
	if (fhresok->object.nfs_fh4_len > PS_MAX_FH_SIZE) {
		ret = -ENOSPC;
		goto out_free_reply;
	}
	memcpy(reply->child_fh, fhresok->object.nfs_fh4_val,
	       fhresok->object.nfs_fh4_len);
	reply->child_fh_len = fhresok->object.nfs_fh4_len;

	/* GETATTR result at index 4. */
	res = mds_compound_result(&mc, 4);
	if (!res) {
		ret = -EREMOTEIO;
		goto out_free_reply;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opgetattr.status);
	if (ret)
		goto out_free_reply;

	GETATTR4resok *gresok =
		&res->nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;

	ret = ps_proxy_parse_attrs_min(
		gresok->obj_attributes.attrmask.bitmap4_val,
		gresok->obj_attributes.attrmask.bitmap4_len,
		(const uint8_t *)gresok->obj_attributes.attr_vals.attrlist4_val,
		gresok->obj_attributes.attr_vals.attrlist4_len,
		&reply->child_attrs);
	if (ret)
		goto out_free_reply;

	ret = 0;
	goto out;

out_free_reply:
	ps_proxy_symlink_reply_free(reply);
out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_proxy_forward_link(struct mds_session *ms, const uint8_t *src_fh,
			  uint32_t src_fh_len, const uint8_t *dst_dir_fh,
			  uint32_t dst_dir_fh_len, const char *newname,
			  uint32_t newname_len,
			  const struct authunix_parms *creds,
			  struct ps_proxy_link_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !src_fh || !dst_dir_fh || !newname || !reply)
		return -EINVAL;
	if (src_fh_len == 0 || dst_dir_fh_len == 0 || newname_len == 0)
		return -EINVAL;
	if (src_fh_len > PS_MAX_FH_SIZE || dst_dir_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH(src) + SAVEFH + PUTFH(dst_dir) + LINK = 5 ops */
	ret = mds_compound_init(&mc, 5, "ps-proxy-link");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)src_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = src_fh_len;

	slot = mds_compound_add_op(&mc, OP_SAVEFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)dst_dir_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = dst_dir_fh_len;

	slot = mds_compound_add_op(&mc, OP_LINK);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.oplink.newname.utf8string_val = (char *)newname;
	slot->nfs_argop4_u.oplink.newname.utf8string_len = newname_len;

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH(src) at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* SAVEFH at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opsavefh.status);
	if (ret)
		goto out;

	/* PUTFH(dst_dir) at index 3. */
	res = mds_compound_result(&mc, 3);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* LINK result at index 4. */
	res = mds_compound_result(&mc, 4);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.oplink.status);
	if (ret)
		goto out;

	LINK4resok *lresok = &res->nfs_resop4_u.oplink.LINK4res_u.resok4;

	reply->atomic = lresok->cinfo.atomic;
	reply->before = lresok->cinfo.before;
	reply->after = lresok->cinfo.after;
	ret = 0;

out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_proxy_forward_close(struct mds_session *ms, const uint8_t *upstream_fh,
			   uint32_t upstream_fh_len, uint32_t close_seqid,
			   uint32_t stateid_seqid,
			   const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
			   const struct authunix_parms *creds,
			   struct ps_proxy_close_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !upstream_fh || upstream_fh_len == 0 || !stateid_other ||
	    !reply)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH + CLOSE = 3 ops */
	ret = mds_compound_init(&mc, 3, "ps-proxy-close");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)upstream_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = upstream_fh_len;

	slot = mds_compound_add_op(&mc, OP_CLOSE);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	CLOSE4args *ca = &slot->nfs_argop4_u.opclose;

	ca->seqid = close_seqid;
	ca->open_stateid.seqid = stateid_seqid;
	memcpy(ca->open_stateid.other, stateid_other, PS_STATEID_OTHER_SIZE);

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	/*
	 * -EREMOTEIO from mds_compound_send means the wire round-trip
	 * succeeded but the COMPOUND4res top-level status was not
	 * NFS4_OK (i.e. at least one op in the compound failed).  We
	 * still want to inspect individual op statuses below to
	 * surface the per-op error to the caller (NFS4ERR_NOENT on
	 * LOOKUP, etc).  Only true wire failures (-EIO et al) bail.
	 */
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* CLOSE result at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opclose.status);
	if (ret)
		goto out;

	stateid4 *new_sid = &res->nfs_resop4_u.opclose.CLOSE4res_u.open_stateid;

	reply->stateid_seqid = new_sid->seqid;
	memcpy(reply->stateid_other, new_sid->other, PS_STATEID_OTHER_SIZE);
	ret = 0;

out:
	mds_compound_fini(&mc);
	return ret;
}

void ps_proxy_readdir_reply_free(struct ps_proxy_readdir_reply *reply)
{
	if (!reply)
		return;
	struct ps_proxy_readdir_entry *e = reply->entries;

	while (e) {
		struct ps_proxy_readdir_entry *next = e->next;

		free(e->name);
		free(e->attrmask);
		free(e->attr_vals);
		free(e);
		e = next;
	}
	memset(reply, 0, sizeof(*reply));
}

int ps_proxy_forward_readdir(struct mds_session *ms, const uint8_t *upstream_fh,
			     uint32_t upstream_fh_len, uint64_t cookie,
			     const uint8_t cookieverf[PS_PROXY_VERIFIER_SIZE],
			     uint32_t dircount, uint32_t maxcount,
			     const uint32_t *attr_request,
			     uint32_t attr_request_len,
			     const struct authunix_parms *creds,
			     struct ps_proxy_readdir_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !upstream_fh || upstream_fh_len == 0 || !cookieverf ||
	    !reply)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;
	if (attr_request_len > 0 && !attr_request)
		return -EINVAL;

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH + READDIR = 3 ops */
	ret = mds_compound_init(&mc, 3, "ps-proxy-readdir");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)upstream_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = upstream_fh_len;

	slot = mds_compound_add_op(&mc, OP_READDIR);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	READDIR4args *ra = &slot->nfs_argop4_u.opreaddir;

	ra->cookie = cookie;
	memcpy(ra->cookieverf, cookieverf, PS_PROXY_VERIFIER_SIZE);
	ra->dircount = dircount;
	ra->maxcount = maxcount;
	ra->attr_request.bitmap4_val = (uint32_t *)attr_request;
	ra->attr_request.bitmap4_len = attr_request_len;

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	/*
	 * -EREMOTEIO from mds_compound_send means the wire round-trip
	 * succeeded but the COMPOUND4res top-level status was not
	 * NFS4_OK (i.e. at least one op in the compound failed).  We
	 * still want to inspect individual op statuses below to
	 * surface the per-op error to the caller (NFS4ERR_NOENT on
	 * LOOKUP, etc).  Only true wire failures (-EIO et al) bail.
	 */
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* READDIR result at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opreaddir.status);
	if (ret)
		goto out;

	READDIR4resok *rresok =
		&res->nfs_resop4_u.opreaddir.READDIR4res_u.resok4;

	memcpy(reply->cookieverf, rresok->cookieverf, PS_PROXY_VERIFIER_SIZE);
	reply->eof = rresok->reply.eof;

	/*
	 * Deep-copy the entry4 linked list.  Every field that lives in
	 * the compound's XDR buffer (name, bitmap, attr bytes) has to
	 * migrate to PS-owned heap so the caller can hold the reply
	 * past mds_compound_fini.
	 */
	entry4 *src_head = rresok->reply.entries;
	struct ps_proxy_readdir_entry **tail_link = &reply->entries;

	for (entry4 *src = src_head; src != NULL; src = src->nextentry) {
		struct ps_proxy_readdir_entry *dst = calloc(1, sizeof(*dst));

		if (!dst) {
			ret = -ENOMEM;
			goto out_free_reply;
		}
		dst->cookie = src->cookie;

		if (src->name.utf8string_len > 0) {
			dst->name = strndup(src->name.utf8string_val,
					    src->name.utf8string_len);
			if (!dst->name) {
				free(dst);
				ret = -ENOMEM;
				goto out_free_reply;
			}
		}

		if (src->attrs.attrmask.bitmap4_len > 0) {
			dst->attrmask = calloc(src->attrs.attrmask.bitmap4_len,
					       sizeof(*dst->attrmask));
			if (!dst->attrmask) {
				free(dst->name);
				free(dst);
				ret = -ENOMEM;
				goto out_free_reply;
			}
			memcpy(dst->attrmask, src->attrs.attrmask.bitmap4_val,
			       src->attrs.attrmask.bitmap4_len *
				       sizeof(*dst->attrmask));
			dst->attrmask_len = src->attrs.attrmask.bitmap4_len;
		}

		if (src->attrs.attr_vals.attrlist4_len > 0) {
			dst->attr_vals =
				calloc(src->attrs.attr_vals.attrlist4_len, 1);
			if (!dst->attr_vals) {
				free(dst->attrmask);
				free(dst->name);
				free(dst);
				ret = -ENOMEM;
				goto out_free_reply;
			}
			memcpy(dst->attr_vals,
			       src->attrs.attr_vals.attrlist4_val,
			       src->attrs.attr_vals.attrlist4_len);
			dst->attr_vals_len = src->attrs.attr_vals.attrlist4_len;
		}

		*tail_link = dst;
		tail_link = &dst->next;
	}

	ret = 0;
	goto out;

out_free_reply:
	ps_proxy_readdir_reply_free(reply);
out:
	mds_compound_fini(&mc);
	return ret;
}

/*
 * RFC 8881 S5.8.1 attribute numbers.  Hard-coded rather than
 * pulled from nfsv42_xdr.h to keep this parser self-contained --
 * the point of the minimum parser is that it does not depend on
 * the fattr4 C type definitions; it walks the wire bytes
 * directly.  If RFC 8881 ever renumbers these (it won't), the
 * parser's tests catch the mismatch.
 */
#define PS_PROXY_FATTR4_TYPE_BIT 1
#define PS_PROXY_FATTR4_MODE_BIT 33

static uint32_t ps_proxy_be32_load(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool ps_proxy_bitmap_test(const uint32_t *attrmask,
				 uint32_t attrmask_len, uint32_t bit)
{
	uint32_t word = bit / 32;
	uint32_t off = bit % 32;

	if (word >= attrmask_len)
		return false;
	return (attrmask[word] & (1u << off)) != 0;
}

int ps_proxy_parse_attrs_min(const uint32_t *attrmask, uint32_t attrmask_len,
			     const uint8_t *attr_vals, uint32_t attr_vals_len,
			     struct ps_proxy_attrs_min *out)
{
	if (!out)
		return -EINVAL;
	if (attrmask_len > 0 && !attrmask)
		return -EINVAL;
	if (attr_vals_len > 0 && !attr_vals)
		return -EINVAL;

	memset(out, 0, sizeof(*out));

	/*
	 * Empty reply: attrmask and attr_vals both zero-length is the
	 * "server supports nothing we asked for" case.  Return cleanly;
	 * caller sees have_type == have_mode == false.
	 */
	if (attrmask_len == 0 && attr_vals_len == 0)
		return 0;

	/*
	 * Defensive: attrmask_len * 32 must fit in uint32_t so the bit
	 * counter does not truncate.  The on-wire cap (RPC record size
	 * / 4) is far below this; reject rather than silently walk a
	 * subset of the mask.
	 */
	if (attrmask_len > UINT32_MAX / 32)
		return -EINVAL;

	uint32_t cursor = 0;
	uint32_t total_bits = attrmask_len * 32;

	for (uint32_t bit = 0; bit < total_bits; bit++) {
		if (!ps_proxy_bitmap_test(attrmask, attrmask_len, bit))
			continue;

		switch (bit) {
		case PS_PROXY_FATTR4_TYPE_BIT:
			if (cursor + 4 > attr_vals_len)
				return -EINVAL;
			out->type = ps_proxy_be32_load(attr_vals + cursor);
			out->have_type = true;
			cursor += 4;
			break;
		case PS_PROXY_FATTR4_MODE_BIT:
			if (cursor + 4 > attr_vals_len)
				return -EINVAL;
			out->mode = ps_proxy_be32_load(attr_vals + cursor);
			out->have_mode = true;
			cursor += 4;
			break;
		default:
			/*
			 * Bit we do not know how to size-or-decode.
			 * Returning -ENOTSUP rather than best-effort
			 * skip keeps the parser safe: the caller
			 * requested FATTR4_TYPE | FATTR4_MODE, and
			 * any other bit in the reply means either
			 * the MDS added attrs we did not ask for
			 * (protocol violation) or our request mask
			 * drifted from what this parser handles.
			 */
			return -ENOTSUP;
		}
	}

	/*
	 * Trailing bytes after the last decoded attr indicate a
	 * framing mismatch -- the reply claims fewer attrs than it
	 * carries data for, which is malformed.
	 */
	if (cursor != attr_vals_len)
		return -EINVAL;

	return 0;
}

void ps_proxy_read_reply_free(struct ps_proxy_read_reply *reply)
{
	if (!reply)
		return;
	free(reply->data);
	memset(reply, 0, sizeof(*reply));
}

/*
 * Open-owner bytes on the wire are opaque<>.  Most production clients
 * keep the owner tight (32-64 bytes); the RFC does not impose a hard
 * cap but we bound what we'll forward here so a misbehaving client
 * cannot push unbounded opaque into the MDS via the PS.  512 is
 * well above any real client but small enough that a malformed
 * request is rejected loudly rather than amplified upstream.
 */
#define PS_PROXY_OPEN_OWNER_MAX 512

int ps_proxy_forward_open(struct mds_session *ms, const uint8_t *current_fh,
			  uint32_t current_fh_len, const char *name,
			  uint32_t name_len,
			  const struct ps_proxy_open_request *req,
			  const struct authunix_parms *creds,
			  struct ps_proxy_open_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !current_fh || current_fh_len == 0 || !req || !reply)
		return -EINVAL;
	if (current_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;
	if (req->owner_data_len > PS_PROXY_OPEN_OWNER_MAX)
		return -EINVAL;
	if (req->owner_data_len > 0 && !req->owner_data)
		return -EINVAL;

	bool is_claim_null = (req->claim_type == PS_PROXY_OPEN_CLAIM_NULL);
	bool is_claim_fh = (req->claim_type == PS_PROXY_OPEN_CLAIM_FH);

	if (!is_claim_null && !is_claim_fh)
		return -EINVAL;
	if (is_claim_null && (!name || name_len == 0))
		return -EINVAL;

	bool is_create = (req->opentype == PS_PROXY_OPEN_OPENTYPE_CREATE);

	if (is_create) {
		/*
		 * RFC 8881 S18.16.1: CREATE-mode OPEN is only valid with
		 * CLAIM_NULL.  CLAIM_FH targets an existing FH; there is
		 * nothing to create.
		 */
		if (!is_claim_null)
			return -EINVAL;
		if (req->createmode != PS_PROXY_OPEN_CREATEMODE_UNCHECKED &&
		    req->createmode != PS_PROXY_OPEN_CREATEMODE_GUARDED)
			return -EINVAL;
		/*
		 * createattrs is allowed to be empty (server picks
		 * defaults) but a non-zero-length mask must come with
		 * non-NULL bytes, and vice versa.
		 */
		if (req->createattrs_mask_len > 0 && !req->createattrs_mask)
			return -EINVAL;
		if (req->createattrs_vals_len > 0 && !req->createattrs_vals)
			return -EINVAL;
	} else if (req->opentype != PS_PROXY_OPEN_OPENTYPE_NOCREATE) {
		return -EINVAL;
	}

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH + OPEN + GETFH = 4 ops */
	ret = mds_compound_init(&mc, 4, "ps-proxy-open");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)current_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = current_fh_len;

	slot = mds_compound_add_op(&mc, OP_OPEN);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	OPEN4args *oa = &slot->nfs_argop4_u.opopen;

	oa->seqid = req->seqid;
	oa->share_access = req->share_access;
	oa->share_deny = req->share_deny;
	oa->owner.clientid = req->owner_clientid;
	oa->owner.owner.owner_val = (char *)req->owner_data;
	oa->owner.owner.owner_len = req->owner_data_len;
	if (is_create) {
		oa->openhow.opentype = OPEN4_CREATE;
		oa->openhow.openflag4_u.how.mode =
			(req->createmode == PS_PROXY_OPEN_CREATEMODE_GUARDED) ?
				GUARDED4 :
				UNCHECKED4;
		fattr4 *cattrs =
			&oa->openhow.openflag4_u.how.createhow4_u.createattrs;

		cattrs->attrmask.bitmap4_val =
			(uint32_t *)req->createattrs_mask;
		cattrs->attrmask.bitmap4_len = req->createattrs_mask_len;
		cattrs->attr_vals.attrlist4_val = (char *)req->createattrs_vals;
		cattrs->attr_vals.attrlist4_len = req->createattrs_vals_len;
	} else {
		oa->openhow.opentype = OPEN4_NOCREATE;
	}
	if (is_claim_null) {
		oa->claim.claim = CLAIM_NULL;
		oa->claim.open_claim4_u.file.utf8string_val = (char *)name;
		oa->claim.open_claim4_u.file.utf8string_len = name_len;
	} else {
		/* CLAIM_FH: no per-claim payload (RFC 8881 S18.16.1). */
		oa->claim.claim = CLAIM_FH;
	}

	slot = mds_compound_add_op(&mc, OP_GETFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	/*
	 * -EREMOTEIO from mds_compound_send means the wire round-trip
	 * succeeded but the COMPOUND4res top-level status was not
	 * NFS4_OK (i.e. at least one op in the compound failed).  We
	 * still want to inspect individual op statuses below to
	 * surface the per-op error to the caller (NFS4ERR_NOENT on
	 * LOOKUP, etc).  Only true wire failures (-EIO et al) bail.
	 */
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/*
	 * OPEN result at index 2.  nfs4_to_errno preserves common
	 * non-OK statuses (NFS4ERR_NOENT -> -ENOENT, NFS4ERR_ACCESS
	 * -> -EACCES, etc.); the hook re-encodes via errno_to_nfs4
	 * so the wire status the client sees matches what the upstream
	 * MDS returned.
	 */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opopen.status);
	if (ret)
		goto out;

	OPEN4resok *ores = &res->nfs_resop4_u.opopen.OPEN4res_u.resok4;

	reply->stateid_seqid = ores->stateid.seqid;
	memcpy(reply->stateid_other, ores->stateid.other, NFS4_OTHER_SIZE);
	reply->rflags = ores->rflags;

	/* GETFH result at index 3. */
	res = mds_compound_result(&mc, 3);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opgetfh.status);
	if (ret)
		goto out;

	GETFH4resok *fhresok = &res->nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

	if (fhresok->object.nfs_fh4_len == 0) {
		ret = -EREMOTEIO;
		goto out;
	}
	if (fhresok->object.nfs_fh4_len > PS_MAX_FH_SIZE) {
		ret = -ENOSPC;
		goto out;
	}
	memcpy(reply->child_fh, fhresok->object.nfs_fh4_val,
	       fhresok->object.nfs_fh4_len);
	reply->child_fh_len = fhresok->object.nfs_fh4_len;

	ret = 0;

out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_proxy_forward_read(struct mds_session *ms, const uint8_t *upstream_fh,
			  uint32_t upstream_fh_len, uint32_t stateid_seqid,
			  const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
			  uint64_t offset, uint32_t count,
			  const struct authunix_parms *creds,
			  struct ps_proxy_read_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !upstream_fh || upstream_fh_len == 0 || !stateid_other ||
	    !reply || count == 0)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH + READ = 3 ops */
	ret = mds_compound_init(&mc, 3, "ps-proxy-read");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	/* Same const-cast rationale as ps_proxy_forward_getattr. */
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)upstream_fh;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = upstream_fh_len;

	slot = mds_compound_add_op(&mc, OP_READ);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	READ4args *ra = &slot->nfs_argop4_u.opread;

	ra->stateid.seqid = stateid_seqid;
	memcpy(ra->stateid.other, stateid_other, PS_STATEID_OTHER_SIZE);
	ra->offset = offset;
	ra->count = count;

	ret = mds_compound_send_with_auth(&mc, ms, creds);
	/*
	 * -EREMOTEIO from mds_compound_send means the wire round-trip
	 * succeeded but the COMPOUND4res top-level status was not
	 * NFS4_OK (i.e. at least one op in the compound failed).  We
	 * still want to inspect individual op statuses below to
	 * surface the per-op error to the caller (NFS4ERR_NOENT on
	 * LOOKUP, etc).  Only true wire failures (-EIO et al) bail.
	 */
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	/* READ result at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opread.status);
	if (ret)
		goto out;

	READ4resok *rresok = &res->nfs_resop4_u.opread.READ4res_u.resok4;

	reply->eof = rresok->eof;
	if (rresok->data.data_len > 0) {
		reply->data = calloc(rresok->data.data_len, 1);
		if (!reply->data) {
			ret = -ENOMEM;
			goto out_free_reply;
		}
		memcpy(reply->data, rresok->data.data_val,
		       rresok->data.data_len);
		reply->data_len = rresok->data.data_len;
	}

	ret = 0;
	goto out;

out_free_reply:
	ps_proxy_read_reply_free(reply);
out:
	mds_compound_fini(&mc);
	return ret;
}
