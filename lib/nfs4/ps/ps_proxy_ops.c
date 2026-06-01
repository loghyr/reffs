/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"

#include "nfs4/errors.h"

#include "ec_client.h"
#include "ps_owner.h" /* PS_OWNER_TAG_SIZE for the wrapped owner cap */
#include "ps_proxy_ops.h"
#include "ps_proxy_ops_internal.h" /* ps_proxy_send_with_kick (whitebox) */
#include "ps_state.h" /* PS_MAX_FH_SIZE */
#include "ps_write_buffer.h"
#include "ps_write_buffer_internal.h" /* struct ps_write_buffer (field access
				       * for the byte-copy critical section) */

/*
 * Worker-side compound send: wraps mds_compound_send_with_auth and,
 * if the result classifies as a session-killer (per ps_session_is_dead)
 * AND the session is tagged with a kick listener id (set at session
 * publish time -- see ec_client.h ms_kick_listener_id and the wiring
 * in src/reffsd.c + lib/nfs4/ps/ps_renewal.c), wakes the renewal
 * thread so reconnect runs on the next tick instead of waiting out
 * the rest of the renewal interval.
 *
 * Returns the underlying send_with_auth value verbatim -- this is a
 * pure side effect on the kick path; the caller's per-op result
 * inspection is unchanged.
 *
 * For sessions without a kick tag (ec_demo, MDS-to-DS dstore vtable),
 * this is a thin pass-through with one extra atomic_load on the hot
 * path -- negligible overhead next to the wire round-trip the call
 * just did.
 */
int ps_proxy_send_with_kick(struct mds_compound *mc, struct mds_session *ms,
			    const struct authunix_parms *creds)
{
	int ret = mds_compound_send_with_auth(mc, ms, creds);
	uint32_t listener_id = atomic_load_explicit(&ms->ms_kick_listener_id,
						    memory_order_relaxed);

	if (listener_id == 0)
		return ret;

	/*
	 * SEQUENCE is always op 0 in our compounds.  If resarray is
	 * shorter than 1 the SEQUENCE never decoded (wire-level failure
	 * before any op result landed) -- ps_session_is_dead's errno
	 * branch covers that case with sr_status defaulted to NFS4_OK.
	 */
	nfsstat4 sr_status = NFS4_OK;

	if (mc->mc_res.resarray.resarray_len > 0) {
		nfs_resop4 *seq_res = &mc->mc_res.resarray.resarray_val[0];

		if (seq_res->resop == OP_SEQUENCE)
			sr_status = seq_res->nfs_resop4_u.opsequence.sr_status;
	}

	if (ps_session_is_dead(ret, sr_status))
		ps_listener_kick_reconnect(listener_id);

	return ret;
}

/*
 * GETATTR(TYPE, MODE) bitmap reused by mkdir / symlink / mknod
 * forwarders to piggyback the type+mode lookup on a single compound.
 *   word 0, bit 1  = FATTR4_TYPE  (offset 1 in the spec's attr table)
 *   word 1, bit 1  = FATTR4_MODE  (offset 33 = 32 + 1, into word 1)
 * See lib/xdr/nfsv42_xdr.x and ps_proxy_ops.c:1721 ps_proxy_bitmap_test
 * for the bit layout.
 */
static const uint32_t ps_proxy_attr_req_type_mode[] = { 0x2u, 0x2u };

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

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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
	 * a follow-up GETATTR round-trip.
	 */
	slot->nfs_argop4_u.opgetattr.attr_request.bitmap4_val =
		(uint32_t *)ps_proxy_attr_req_type_mode;
	slot->nfs_argop4_u.opgetattr.attr_request.bitmap4_len = 2;

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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
	slot->nfs_argop4_u.opgetattr.attr_request.bitmap4_val =
		(uint32_t *)ps_proxy_attr_req_type_mode;
	slot->nfs_argop4_u.opgetattr.attr_request.bitmap4_len = 2;

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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

void ps_proxy_mknod_reply_free(struct ps_proxy_mknod_reply *reply)
{
	if (!reply)
		return;
	free(reply->attrset_mask);
	memset(reply, 0, sizeof(*reply));
}

int ps_proxy_forward_mknod(struct mds_session *ms, const uint8_t *parent_fh,
			   uint32_t parent_fh_len, const char *name,
			   uint32_t name_len, uint32_t type, uint32_t specdata1,
			   uint32_t specdata2, const uint32_t *createattrs_mask,
			   uint32_t createattrs_mask_len,
			   const uint8_t *createattrs_vals,
			   uint32_t createattrs_vals_len,
			   const struct authunix_parms *creds,
			   struct ps_proxy_mknod_reply *reply)
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
	/*
	 * NF4DIR / NF4LNK / NF4REG have their own forwarders; reject
	 * here so a mistaken caller can't slip a wrong-type CREATE
	 * past the dispatcher in dir.c.
	 */
	if (type != NF4BLK && type != NF4CHR && type != NF4SOCK &&
	    type != NF4FIFO)
		return -EINVAL;

	memset(reply, 0, sizeof(*reply));

	/* SEQUENCE + PUTFH + CREATE + GETFH + GETATTR = 5 ops */
	ret = mds_compound_init(&mc, 5, "ps-proxy-mknod");
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

	cra->objtype.type = type;
	if (type == NF4BLK || type == NF4CHR) {
		cra->objtype.createtype4_u.devdata.specdata1 = specdata1;
		cra->objtype.createtype4_u.devdata.specdata2 = specdata2;
	}
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
	 * Same FATTR4_TYPE | FATTR4_MODE bit set the mkdir / symlink
	 * forwarders ask for: lets the materialiser stamp the local
	 * proxy-SB inode without a follow-up GETATTR round-trip.
	 */
	slot->nfs_argop4_u.opgetattr.attr_request.bitmap4_val =
		(uint32_t *)ps_proxy_attr_req_type_mode;
	slot->nfs_argop4_u.opgetattr.attr_request.bitmap4_len = 2;

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

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
	ps_proxy_mknod_reply_free(reply);
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

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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
 * Open-owner bytes on the wire are opaque<NFS4_OPAQUE_LIMIT> -- 1024.
 * The hook in nfs4_op_open wraps the raw end-client owner with a
 * PS_OWNER_TAG_SIZE (8) byte tag (see ps_owner.h) before handing
 * it here, so the cap on what reaches the wire is the spec maximum
 * (1024) plus the tag (8) = 1032.  Cap matches what the upstream
 * MDS will accept; a misbehaving caller is rejected loudly rather
 * than amplified upstream.
 */
#define PS_PROXY_OPEN_OWNER_MAX (NFS4_OPAQUE_LIMIT + PS_OWNER_TAG_SIZE)

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
		    req->createmode != PS_PROXY_OPEN_CREATEMODE_GUARDED &&
		    req->createmode != PS_PROXY_OPEN_CREATEMODE_EXCLUSIVE &&
		    req->createmode != PS_PROXY_OPEN_CREATEMODE_EXCLUSIVE_1)
			return -EINVAL;
		/*
		 * UNCHECKED4 / GUARDED4 / EXCLUSIVE4_1 carry createattrs;
		 * EXCLUSIVE4 carries verifier only.  createattrs is allowed
		 * to be empty (server picks defaults) but a non-zero-length
		 * mask must come with non-NULL bytes, and vice versa.
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
		switch (req->createmode) {
		case PS_PROXY_OPEN_CREATEMODE_GUARDED:
			oa->openhow.openflag4_u.how.mode = GUARDED4;
			break;
		case PS_PROXY_OPEN_CREATEMODE_EXCLUSIVE:
			oa->openhow.openflag4_u.how.mode = EXCLUSIVE4;
			break;
		case PS_PROXY_OPEN_CREATEMODE_EXCLUSIVE_1:
			oa->openhow.openflag4_u.how.mode = EXCLUSIVE4_1;
			break;
		case PS_PROXY_OPEN_CREATEMODE_UNCHECKED:
		default:
			oa->openhow.openflag4_u.how.mode = UNCHECKED4;
			break;
		}

		switch (oa->openhow.openflag4_u.how.mode) {
		case UNCHECKED4:
		case GUARDED4: {
			fattr4 *cattrs = &oa->openhow.openflag4_u.how
						  .createhow4_u.createattrs;

			cattrs->attrmask.bitmap4_val =
				(uint32_t *)req->createattrs_mask;
			cattrs->attrmask.bitmap4_len =
				req->createattrs_mask_len;
			cattrs->attr_vals.attrlist4_val =
				(char *)req->createattrs_vals;
			cattrs->attr_vals.attrlist4_len =
				req->createattrs_vals_len;
			break;
		}
		case EXCLUSIVE4:
			memcpy(&oa->openhow.openflag4_u.how.createhow4_u
					.createverf,
			       req->createverf, NFS4_VERIFIER_SIZE);
			break;
		case EXCLUSIVE4_1: {
			creatverfattr *cb =
				&oa->openhow.openflag4_u.how.createhow4_u
					 .ch_createboth;

			memcpy(cb->cva_verf, req->createverf,
			       NFS4_VERIFIER_SIZE);
			cb->cva_attrs.attrmask.bitmap4_val =
				(uint32_t *)req->createattrs_mask;
			cb->cva_attrs.attrmask.bitmap4_len =
				req->createattrs_mask_len;
			cb->cva_attrs.attr_vals.attrlist4_val =
				(char *)req->createattrs_vals;
			cb->cva_attrs.attr_vals.attrlist4_len =
				req->createattrs_vals_len;
			break;
		}
		}
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

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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
	memcpy(reply->stateid_other, ores->stateid.other,
	       PS_STATEID_OTHER_SIZE);
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
	    !reply)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	memset(reply, 0, sizeof(*reply));

	/*
	 * RFC 8881 S18.22 allows READ with count == 0 (zero-byte read,
	 * still valid as a no-op).  Short-circuit here so we don't
	 * round-trip to the MDS just to copy zero bytes back.  Set EOF
	 * iff offset is at or past the file size -- but the PS does not
	 * know the file size without a GETATTR, so the safe answer for
	 * count == 0 is "not at EOF, zero bytes read" (matches what a
	 * normal MDS would return).
	 */
	if (count == 0)
		return 0;

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

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
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

/*
 * PS Phase 3: pipeline-driven read.
 *
 * Builds a struct mds_file from the caller-supplied upstream FH and
 * end-client open stateid, hands it to ec_read_codec_with_file, and
 * copies the requested byte range out of the decoded payload.
 *
 * Codec is hard-coded to RS 4+2 / FFV2 / 4 KiB shards for this slice.
 * See .claude/design/proxy-server-phase3.md Risk #1.
 */
int ps_proxy_pipeline_read(struct mds_session *ms, const uint8_t *upstream_fh,
			   uint32_t upstream_fh_len, uint32_t stateid_seqid,
			   const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
			   uint64_t offset, uint32_t count,
			   const struct authunix_parms *creds,
			   struct ps_proxy_read_reply *reply)
{
	struct mds_file mf;
	uint8_t *whole_buf = NULL;
	size_t whole_len = 0;
	size_t out_len = 0;
	int ret;

	if (!ms || !upstream_fh || upstream_fh_len == 0 || !stateid_other ||
	    !reply)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	memset(reply, 0, sizeof(*reply));

	if (count == 0)
		return 0;

	/*
	 * The pipeline reads from stripe 0 up to buf_len bytes (no
	 * intrinsic offset support today).  Allocate a buffer covering
	 * [0, offset+count) and copy out the [offset, offset+count) slice
	 * for the client.  Wasteful for large offsets but correct; real
	 * partial-range support is a follow-on optimisation, see plan
	 * "Deferred / NOT_NOW_BROWN_COW".
	 */
	whole_len = (size_t)offset + (size_t)count;

	whole_buf = calloc(1, whole_len);
	if (!whole_buf)
		return -ENOMEM;

	/*
	 * mds_file is a {stateid, fh} tuple.  We construct it from the
	 * end-client's open stateid (carried verbatim) and the upstream
	 * FH discovered at PS startup.  ec_read_codec_with_file does
	 * not take ownership; the local copy here is sufficient for the
	 * duration of the call.
	 */
	memset(&mf, 0, sizeof(mf));
	mf.mf_stateid.seqid = stateid_seqid;
	memcpy(mf.mf_stateid.other, stateid_other, sizeof(mf.mf_stateid.other));
	/*
	 * mf_fh.nfs_fh4_val is `char *` in the generated XDR; cast away
	 * the const here -- ec_read_codec_with_file (and the LAYOUTGET
	 * primitive it calls) does not mutate the FH bytes.
	 */
	mf.mf_fh.nfs_fh4_val = (char *)upstream_fh;
	mf.mf_fh.nfs_fh4_len = upstream_fh_len;

	/*
	 * NOT_NOW_BROWN_COW (Phase 5 follow-up): wire listener_id into
	 * ps_proxy_pipeline_read so the per-mirror short-circuit can
	 * also fire on the READ path.  Slice 5.2 ships the write-path
	 * dispatch; reads still take the RPC path on every mirror
	 * regardless of co-residency.
	 */
	ret = ec_read_codec_with_file(ms, &mf, whole_buf, whole_len, &out_len,
				      /* k */ 4, /* m */ 2, EC_CODEC_RS,
				      LAYOUT4_FLEX_FILES_V2,
				      /* skip_ds_mask */ 0,
				      /* shard_size */ 4096, creds,
				      /* pls */ NULL);
	if (ret) {
		free(whole_buf);
		return ret;
	}

	/* Map decoded length to the requested byte range. */
	size_t avail = (out_len > offset) ? (out_len - (size_t)offset) : 0;
	size_t copy = (avail < count) ? avail : count;

	if (copy > 0) {
		reply->data = malloc(copy);
		if (!reply->data) {
			free(whole_buf);
			return -ENOMEM;
		}
		memcpy(reply->data, whole_buf + offset, copy);
		reply->data_len = (uint32_t)copy;
	}

	/*
	 * EOF approximation: the pipeline doesn't surface a definitive
	 * EOF signal.  If we got back fewer bytes than requested, the
	 * file is at most `out_len` bytes long, so the read reached
	 * end-of-file.  An NFS client following up with another READ
	 * past offset+copy will get a 0-byte EOF reply via the same
	 * logic.  Better signalling is a follow-on (cache the inode
	 * size on first GETATTR).
	 */
	reply->eof = (copy < count);

	free(whole_buf);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Phase 4a: pipeline-driven WRITE -- buffer-fill side.                */
/*                                                                    */
/* This is the WRITE entry point op handlers route to on proxy SBs.   */
/* It does not call upstream; it appends client bytes to the          */
/* per-(stateid, fh) write buffer on the listener.  The COMMIT-time   */
/* flush through ec_write_codec_with_file is slice 4a.2c.             */
/* ------------------------------------------------------------------ */

/*
 * Compose the per-listener write verifier (PS Phase 4b slice 4b.4).
 *
 * The 4a helper that packed pls_boot_gen into 8 bytes is now folded
 * into ps_compose_write_verf in ps_write_buffer.c -- pass
 * `mds_verf_set == false` to get the listener-only encoding 4a
 * used.  This shim keeps the buffer/commit reply call sites short
 * while delegating the actual mix to the shared helper.
 */
static_assert(PS_PROXY_VERIFIER_SIZE == PS_WRITE_VERIFIER_SIZE,
	      "verifier widths must match across ps_proxy_ops.h and "
	      "ps_write_buffer.h");

/*
 * Grow the buffer's pwb_data to at least `need_capacity` bytes.
 * Doubling strategy keeps amortised reallocation cost low for
 * sequential-append workloads (the common Linux NFS client
 * pattern).  Caller holds pwb_mutex.
 */
static int pwb_ensure_capacity(struct ps_write_buffer *buf,
			       size_t need_capacity)
{
	size_t new_cap;
	uint8_t *new_data;

	if (buf->pwb_capacity >= need_capacity)
		return 0;

	new_cap = buf->pwb_capacity ? buf->pwb_capacity : 4096;
	while (new_cap < need_capacity) {
		if (new_cap > REFFS_PS_WRITE_BUFFER_MAX / 2) {
			new_cap = need_capacity; /* final jump -- no overflow */
			break;
		}
		new_cap *= 2;
	}

	new_data = realloc(buf->pwb_data, new_cap);
	if (!new_data)
		return -ENOMEM;

	/*
	 * Zero the freshly grown tail so sparse writes (offsets beyond
	 * the previous high_water) read back as zeros, matching the
	 * POSIX sparse-file semantics the design's test inventory pins
	 * (test_write_buffer_sparse_holes_zero_filled).
	 */
	if (new_cap > buf->pwb_capacity)
		memset(new_data + buf->pwb_capacity, 0,
		       new_cap - buf->pwb_capacity);

	buf->pwb_data = new_data;
	buf->pwb_capacity = new_cap;
	return 0;
}

/*
 * Flush every dirty stripe in `buf` whose byte range [base, base +
 * stripe_size) intersects [range_start, range_start + range_count).
 * `range_count == 0` is the "flush every dirty stripe" sentinel
 * (RFC 8881 S18.3.4 COMMIT semantics) and disables the filter; the
 * sentinel is only used by `ps_proxy_pipeline_commit`, whose
 * `count == 0` clients ask for a full commit.  The
 * `ps_proxy_pipeline_write` inline-flush caller always passes
 * `range_count == data_len > 0` (data_len == 0 is rejected before
 * this helper is reached) and therefore never triggers the
 * sentinel.
 *
 * Caller MUST hold `buf->pwb_mutex`.  The lock is held across the
 * per-stripe RPCs (LAYOUTGET + CHUNK_READ + CHUNK_WRITE storm) per
 * the design's serialisation argument (Risk #1 in proxy-server-
 * phase4b.md tracks future async pipelining).
 *
 * Per-stripe dispatch:
 *   - fully-dirty (pds_partial_mask == NULL): direct
 *     ec_write_stripe_with_file from buf->pwb_data + base
 *     (the 4b.2 full-stripe path).
 *   - partial-mask: RMW.  Alloc a stripe-sized scratch buffer,
 *     ec_read_stripe_with_file the existing stripe from the DS,
 *     overwrite the shards covered by pds_partial_mask with bytes
 *     from buf->pwb_data, then ec_write_stripe_with_file the merged
 *     stripe (the 4b.3 RMW path).
 *
 * Successful per-stripe flushes remove the dirty entry from
 * `pwb_dirty_ht`.  Any captured MDS verifier (from the per-stripe
 * primitive's writeverf out-param) is folded into `buf->pwb_mds_verf`
 * last-writer-wins -- the MDS verifier is monotonic per upstream
 * boot epoch, so two stripes flushing in sequence see the same
 * verifier unless an MDS restart happened between them, in which
 * case the later stripe's verifier is the correct one to keep
 * (Risk #7 in proxy-server-phase4b.md).
 *
 * Shared by `ps_proxy_pipeline_commit` (slice 4b.5; the full or
 * range-bound dirty walk at COMMIT time) and
 * `ps_proxy_pipeline_write` (slice 4b.6; the FILE_SYNC4 / DATA_SYNC4
 * inline flush over just the bytes this WRITE touched).
 *
 * `pls` is the owning listener (always non-NULL today; both
 * pipeline callers reach this helper only after a successful
 * ps_state_find).  Slice 4b.7 uses it to bump
 * pls_rmw_reads_total / pls_rmw_read_failures_total around the
 * RMW prefix CHUNK_READ -- relaxed atomics for the
 * ps-write-buffer-stats probe surface.
 *
 * Returns:
 *    0       every intersecting dirty stripe flushed cleanly (or no
 *            stripes intersected); dirty entries for the flushed
 *            stripes are removed; pwb_mds_verf reflects the most
 *            recent per-stripe verifier if any was captured.
 *   -EIO     a per-stripe flush failed; the loop bails on the first
 *            failure and the remaining dirty entries -- including
 *            the one that failed -- are kept for a client retry.
 *   -ENOMEM  scratch / stripe_nos allocation failed; same retention
 *            semantics as -EIO.
 */
static int pwb_flush_range_locked(struct ps_write_buffer *buf,
				  struct ps_listener_state *pls,
				  struct mds_session *ms,
				  const struct authunix_parms *creds,
				  uint64_t range_start, uint64_t range_count)
{
	struct mds_file mf;
	size_t stripe_size = 0;
	uint32_t *stripe_nos = NULL;
	size_t n_stripes = 0;
	size_t capacity = 0;
	int ret = 0;

	if (!buf->pwb_geom_set)
		return 0; /* no geometry -> no dirty stripes ever marked */

	stripe_size =
		(size_t)buf->pwb_geom.pwbg_k * buf->pwb_geom.pwbg_shard_size;
	if (stripe_size == 0 || !buf->pwb_dirty_ht)
		return 0;

	/*
	 * Construct an mds_file from the buffer's stashed key.  The
	 * codec only reads it; we don't take ownership of any upstream
	 * state.  stateid_seqid is whichever WRITE landed last under
	 * pwb_mutex; stateid_other was set at alloc time and never
	 * changes.
	 */
	memset(&mf, 0, sizeof(mf));
	mf.mf_stateid.seqid = buf->pwb_stateid_seqid;
	memcpy(mf.mf_stateid.other, buf->pwb_stateid_other,
	       PS_STATEID_OTHER_SIZE);
	mf.mf_fh.nfs_fh4_val = (char *)buf->pwb_upstream_fh;
	mf.mf_fh.nfs_fh4_len = buf->pwb_upstream_fh_len;

	{
		struct cds_lfht_iter iter;
		struct cds_lfht_node *node;
		size_t total;

		/*
		 * Two-pass to keep allocation OUT of the rcu_read_lock
		 * section (patterns/rcu-violations.md Pattern 1).
		 * pwb_mutex is held across both passes, so the entry
		 * count cannot change between them.
		 */
		total = ps_write_buffer_dirty_count(buf);
		if (total > 0) {
			stripe_nos = malloc(total * sizeof(uint32_t));
			if (!stripe_nos)
				return -ENOMEM;
			capacity = total;
		}

		rcu_read_lock();
		cds_lfht_first(buf->pwb_dirty_ht, &iter);
		while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
			struct ps_dirty_stripe *ds = caa_container_of(
				node, struct ps_dirty_stripe, pds_ht_node);
			uint32_t s = ds->pds_stripe_no;

			cds_lfht_next(buf->pwb_dirty_ht, &iter);

			/*
			 * Range filter (slice 4b.5 + 4b.6): skip
			 * stripes whose [base, base + stripe_size) does
			 * not intersect [range_start, range_end).  count
			 * == 0 is the "every dirty stripe" sentinel.
			 */
			if (range_count > 0) {
				uint64_t base = (uint64_t)s * stripe_size;
				uint64_t end = base + stripe_size;
				uint64_t range_end = range_start + range_count;

				if (end <= range_start || base >= range_end)
					continue;
			}

			/* Defensive: capacity == total from the count
			 * walk; pwb_mutex prevented any growth. */
			if (n_stripes < capacity)
				stripe_nos[n_stripes++] = s;
		}
		rcu_read_unlock();
	}

	for (size_t i = 0; i < n_stripes; i++) {
		uint32_t s = stripe_nos[i];
		size_t base = (size_t)s * stripe_size;
		struct ps_dirty_stripe *ds;
		bool is_partial;
		uint8_t stripe_mds_verf[PS_WRITE_VERIFIER_SIZE];
		bool stripe_mds_verf_set = false;

		ds = ps_write_buffer_dirty_lookup(buf, s);
		if (!ds)
			continue; /* defensive; cannot happen */
		is_partial = (ds->pds_partial_mask != NULL);

		if (is_partial) {
			uint8_t *scratch;
			uint32_t k = buf->pwb_geom.pwbg_k;
			size_t shard_sz = buf->pwb_geom.pwbg_shard_size;
			bool capacity_ok = true;

			scratch = malloc(stripe_size);
			if (!scratch) {
				ret = -ENOMEM;
				break;
			}

			/*
			 * Slice 4b.7 observability: count the RMW prefix
			 * read attempt before we make the call, and the
			 * failure on a non-zero return.  Both relaxed --
			 * the probe reports a self-consistent snapshot, not
			 * a transactional one.  pls is non-NULL by
			 * helper-doc contract; both callers reach this
			 * helper only after a successful ps_state_find.
			 */
			atomic_fetch_add_explicit(&pls->pls_rmw_reads_total, 1,
						  memory_order_relaxed);
			ret = ec_read_stripe_with_file(
				ms, &mf, (uint64_t)s, scratch, stripe_size,
				(int)buf->pwb_geom.pwbg_k,
				(int)buf->pwb_geom.pwbg_m, EC_CODEC_RS,
				LAYOUT4_FLEX_FILES_V2,
				buf->pwb_geom.pwbg_shard_size, creds, pls,
				NULL);
			if (ret) {
				atomic_fetch_add_explicit(
					&pls->pls_rmw_read_failures_total, 1,
					memory_order_relaxed);
				free(scratch);
				break;
			}

			for (uint32_t shard = 0; shard < k; shard++) {
				size_t shard_off;

				if (!ps_dirty_stripe_shard_is_dirty(ds, shard))
					continue;
				shard_off = base + (size_t)shard * shard_sz;
				if (shard_off + shard_sz > buf->pwb_capacity) {
					capacity_ok = false;
					break;
				}
				memcpy(scratch + (size_t)shard * shard_sz,
				       buf->pwb_data + shard_off, shard_sz);
			}
			if (!capacity_ok) {
				free(scratch);
				ret = -EIO;
				break;
			}

			ret = ec_write_stripe_with_file(
				ms, &mf, (uint64_t)s, scratch, stripe_size,
				(int)buf->pwb_geom.pwbg_k,
				(int)buf->pwb_geom.pwbg_m, EC_CODEC_RS,
				LAYOUT4_FLEX_FILES_V2,
				buf->pwb_geom.pwbg_shard_size, creds,
				stripe_mds_verf, &stripe_mds_verf_set, pls,
				NULL);
			free(scratch);
			if (ret)
				break;
		} else {
			if (base + stripe_size > buf->pwb_high_water) {
				/* Defensive: a fully-dirty stripe whose
				 * bytes extend past pwb_high_water cannot
				 * have come from a legitimate WRITE. */
				ret = -EIO;
				break;
			}

			ret = ec_write_stripe_with_file(
				ms, &mf, (uint64_t)s, buf->pwb_data + base,
				stripe_size, (int)buf->pwb_geom.pwbg_k,
				(int)buf->pwb_geom.pwbg_m, EC_CODEC_RS,
				LAYOUT4_FLEX_FILES_V2,
				buf->pwb_geom.pwbg_shard_size, creds,
				stripe_mds_verf, &stripe_mds_verf_set, pls,
				NULL);
			if (ret)
				break;
		}

		if (stripe_mds_verf_set) {
			memcpy(buf->pwb_mds_verf, stripe_mds_verf,
			       PS_WRITE_VERIFIER_SIZE);
			buf->pwb_mds_verf_set = true;
		}
		ps_write_buffer_dirty_remove(buf, s);
	}

	free(stripe_nos);
	return ret;
}

int ps_proxy_pipeline_write(struct mds_session *ms, const uint8_t *upstream_fh,
			    uint32_t upstream_fh_len, uint32_t stateid_seqid,
			    const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
			    uint64_t offset, uint32_t stable,
			    const uint8_t *data, uint32_t data_len,
			    const struct authunix_parms *creds,
			    struct ps_proxy_write_reply *reply)
{
	uint32_t listener_id;
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;
	uint64_t buf_gen;
	int ret;

	/*
	 * Slice 4b.6 activates `stable` and `creds`: a WRITE with
	 * `stable != UNSTABLE4` triggers an inline flush of the
	 * stripes this WRITE just touched, with `creds` threaded
	 * through ec_*_stripe_with_file's LAYOUTGET / CHUNK_WRITE /
	 * FINALIZE / COMMIT compounds.  UNSTABLE4 keeps the 4a
	 * deferred-flush contract.  `stateid_seqid` is stashed in the
	 * buffer for COMMIT-time LAYOUTGET.
	 *
	 * stable_how4 (RFC 8881 S3.1.16): 0 UNSTABLE4, 1 DATA_SYNC4,
	 * 2 FILE_SYNC4.  The XDR decoder already rejects out-of-range
	 * enum values on the wire, so this check is defence-in-depth
	 * for the internal-caller path -- an op handler or future
	 * slice that hands us an int instead of the decoded enum -- so
	 * the inline-flush branch only ever sees a recognised value.
	 */
	if (!ms || !upstream_fh || upstream_fh_len == 0 || !stateid_other ||
	    !data || data_len == 0 || !reply)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;
	if (stable > 2)
		return -EINVAL;

	memset(reply, 0, sizeof(*reply));

	/*
	 * Derive listener_id from the session's kick tag.  Sessions
	 * not owned by a PS listener (ec_demo, dstore MDS-to-DS) have
	 * ms_kick_listener_id == 0; those callers do not reach this
	 * shim (op handlers only dispatch here for proxy SBs).  An
	 * unowned session reaching us is a programmer error.
	 */
	listener_id = atomic_load_explicit(&ms->ms_kick_listener_id,
					   memory_order_relaxed);
	if (listener_id == 0)
		return -EINVAL;

	pls = (struct ps_listener_state *)ps_state_find(listener_id);
	if (!pls)
		return -ENOENT;

	/*
	 * Single-WRITE cap check.  After ps_state_find so we can
	 * record the rejection in the per-listener stats counter;
	 * before enter_quiesce so a hammering client doesn't keep
	 * us churning the active-refs counter for guaranteed-FBIG
	 * requests.
	 */
	if (data_len > REFFS_PS_WRITE_BUFFER_MAX) {
		atomic_fetch_add_explicit(&pls->pls_fbig_rejections_total, 1,
					  memory_order_relaxed);
		return -EFBIG;
	}

	/*
	 * Take the per-listener quiesce reservation.  After this point
	 * every NULL/failure path either calls leave_quiesce directly
	 * (the no-buffer / cap-exceeded branches) or hands the
	 * reservation to ps_write_buffer_release_find_ref via
	 * drop_find_ref (the happy path).
	 */
	if (!ps_write_buffer_enter_quiesce_or_bail(pls))
		return -EAGAIN; /* listener draining / stopped */

	buf = ps_write_buffer_lookup_or_alloc(pls, stateid_other, upstream_fh,
					      upstream_fh_len);
	if (!buf) {
		/*
		 * lookup_or_alloc released our enter_quiesce reservation
		 * already on its NULL path (symmetric contract); nothing
		 * else to clean up here.
		 */
		return -EAGAIN;
	}

	/*
	 * Total buffered cap check.  Done UNDER pwb_mutex so a
	 * concurrent WRITE on the same buffer cannot grow capacity
	 * past the cap in parallel with us.
	 */
	pthread_mutex_lock(&buf->pwb_mutex);

	buf_gen = buf->pwb_listener_gen;
	if (buf_gen !=
	    atomic_load_explicit(&pls->pls_boot_gen, memory_order_acquire)) {
		pthread_mutex_unlock(&buf->pwb_mutex);
		ps_write_buffer_drop(buf, pls);
		return -ESTALE;
	}

	size_t need = (size_t)offset + (size_t)data_len;

	if (need > REFFS_PS_WRITE_BUFFER_MAX) {
		atomic_fetch_add_explicit(&pls->pls_cap_rejections_total, 1,
					  memory_order_relaxed);
		pthread_mutex_unlock(&buf->pwb_mutex);
		ps_write_buffer_release_find_ref(buf, pls);
		return -EAGAIN; /* transient cap pressure */
	}

	ret = pwb_ensure_capacity(buf, need);
	if (ret != 0) {
		pthread_mutex_unlock(&buf->pwb_mutex);
		ps_write_buffer_release_find_ref(buf, pls);
		return ret;
	}

	memcpy(buf->pwb_data + offset, data, data_len);
	if (need > buf->pwb_high_water)
		buf->pwb_high_water = need;
	/*
	 * Stash the latest stateid_seqid for pipeline_commit's
	 * LAYOUTGET.  Last writer wins; if a concurrent WRITE on the
	 * same buffer overwrote ours, pipeline_commit gets whichever
	 * seqid landed under pwb_mutex last.  Phase 4a's
	 * single-writer-per-(stateid, fh) model means there is only
	 * one writer in the happy path.
	 */
	buf->pwb_stateid_seqid = stateid_seqid;

	/*
	 * Per-stripe dirty marking (Phase 4b slice 4b.1).  Snapshot
	 * the buffer's EC geometry on first WRITE; subsequent WRITEs
	 * reuse the snapshot.  Today the geometry is the same
	 * compile-time constant the COMMIT-side encode uses
	 * (k=4, m=2, shard=4096); 4b.2 will plumb the real geometry
	 * from the layout cache.  Mark failure is treated as a
	 * transient pressure event (NFS4ERR_DELAY); the bytes are in
	 * the buffer but the dirty bitmap is inconsistent -- the
	 * client will retry the WRITE under the new bitmap state.
	 */
	{
		static const struct ps_write_buffer_geom phase4a_geom = {
			.pwbg_k = 4,
			.pwbg_m = 2,
			.pwbg_shard_size = 4096,
		};
		int gret = ps_write_buffer_set_geom(buf, &phase4a_geom);

		/*
		 * set_geom may return -EINVAL on a true geometry
		 * mismatch (geometry already set to different fields),
		 * but for slice 4b.1 the geometry is a constant so this
		 * cannot fail in practice.  Pin the contract with an
		 * assertion that catches a future-slice regression.
		 */
		if (gret == 0)
			(void)ps_write_buffer_mark_dirty(buf, offset, data_len);
	}

	/*
	 * Slice 4b.6 inline flush.  If the client asked for FILE_SYNC4
	 * (2) or DATA_SYNC4 (1), walk the dirty stripes whose byte
	 * ranges intersect this WRITE's [offset, offset + data_len)
	 * and flush each one via the shared per-stripe helper.  We
	 * still hold pwb_mutex across the per-stripe RPCs (same
	 * serialisation argument as the COMMIT path); the client
	 * kernel already serialises ops per (stateid, fh).
	 *
	 * The helper captures any per-stripe MDS verifier into
	 * pwb_mds_verf last-writer-wins, which the verifier snapshot
	 * below picks up.  On success the reply carries the requested
	 * stable level + the composed (listener XOR mds) verifier;
	 * on failure the dirty bits stay set for any unflushed
	 * stripes and the WRITE returns -EIO so the client can retry.
	 *
	 * reffs does not distinguish DATA_SYNC4 from FILE_SYNC4 on
	 * the DS side (the dstore vtable has no metadata-sync split
	 * separate from data-sync); both modes drive the same flush.
	 */
	if (stable != 0) {
		int flush_ret = pwb_flush_range_locked(buf, pls, ms, creds,
						       offset, data_len);

		if (flush_ret) {
			pthread_mutex_unlock(&buf->pwb_mutex);
			ps_write_buffer_release_find_ref(buf, pls);
			return flush_ret;
		}
	}

	/*
	 * Snapshot the composed-verifier state under pwb_mutex BEFORE
	 * we drop the find ref.  The UNSTABLE4 path leaves
	 * pwb_mds_verf_set false; the inline-flush path above may
	 * have just set it, which is the slice 4b.4 / 4b.6 verifier-
	 * mix design's intended cross-WRITE state.
	 */
	bool snap_mds_set = buf->pwb_mds_verf_set;
	uint8_t snap_mds_verf[PS_WRITE_VERIFIER_SIZE] = { 0 };

	if (snap_mds_set)
		memcpy(snap_mds_verf, buf->pwb_mds_verf,
		       PS_WRITE_VERIFIER_SIZE);

	pthread_mutex_unlock(&buf->pwb_mutex);

	ps_write_buffer_release_find_ref(buf, pls);

	/*
	 * Reply.  count == data_len (we accepted every byte into the
	 * buffer); committed mirrors `stable` -- UNSTABLE4 for the
	 * 4a deferred-flush path, DATA_SYNC4 / FILE_SYNC4 when the
	 * 4b.6 inline flush completed; the composed verifier (listener
	 * XOR MDS-if-captured) lets the client detect either a listener
	 * restart OR an upstream DS reboot between WRITE and COMMIT
	 * as a mismatch (RFC 8881 S18.32.4 semantics; Risk #3a in
	 * proxy-server-phase4b.md).
	 */
	reply->count = data_len;
	reply->committed = stable;
	ps_compose_write_verf(pls, snap_mds_set, snap_mds_verf,
			      reply->verifier);
	return 0;
}

int ps_proxy_pipeline_commit(struct mds_session *ms, const uint8_t *upstream_fh,
			     uint32_t upstream_fh_len, uint64_t offset,
			     uint32_t count, const struct authunix_parms *creds,
			     struct ps_proxy_commit_reply *reply)
{
	uint32_t listener_id;
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;
	uint64_t buf_gen;
	size_t remaining_dirty = 0;
	int ret;

	if (!ms || !upstream_fh || upstream_fh_len == 0 || !reply)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;
	/*
	 * Slice 4b.5 range honouring promotes count to u64 to compute
	 * `offset + count`.  Reject ranges that would wrap; without
	 * this guard a malicious client could set offset near
	 * UINT64_MAX and force `req_end` to zero, which would falsely
	 * skip every stripe in the dirty walk and return success for
	 * bytes that were never flushed.
	 */
	if (count > 0 && offset > UINT64_MAX - (uint64_t)count)
		return -EINVAL;

	memset(reply, 0, sizeof(*reply));

	listener_id = atomic_load_explicit(&ms->ms_kick_listener_id,
					   memory_order_relaxed);
	if (listener_id == 0)
		return -EINVAL;
	pls = (struct ps_listener_state *)ps_state_find(listener_id);
	if (!pls)
		return -ENOENT;

	if (!ps_write_buffer_enter_quiesce_or_bail(pls))
		return -EAGAIN;

	buf = ps_write_buffer_find_by_fh(pls, upstream_fh, upstream_fh_len);
	if (!buf) {
		/*
		 * No buffered bytes for this FH -- spec-permitted no-op
		 * COMMIT.  Return success with the listener verifier so
		 * a client that interleaves COMMITs across reads-only
		 * files still gets a consistent verifier.  No buffer
		 * means no captured MDS verifier; the composer falls
		 * back to listener-only.
		 * find_by_fh released our enter_quiesce reservation on
		 * its NULL path (symmetric contract).
		 */
		ps_compose_write_verf(pls, false, NULL, reply->verifier);
		return 0;
	}

	pthread_mutex_lock(&buf->pwb_mutex);
	buf_gen = buf->pwb_listener_gen;
	if (buf_gen !=
	    atomic_load_explicit(&pls->pls_boot_gen, memory_order_acquire)) {
		pthread_mutex_unlock(&buf->pwb_mutex);
		ps_write_buffer_drop(buf, pls);
		return -ESTALE;
	}

	/*
	 * Delegate the per-stripe flush walk (collect + RMW or full-
	 * stripe write + verifier capture) to the shared helper.  The
	 * helper honours the range filter for count > 0; count == 0
	 * is the RFC 8881 S18.3.4 "commit everything" sentinel.
	 *
	 * Slice 4b.6 extracted this walk into pwb_flush_range_locked
	 * so the FILE_SYNC4 / DATA_SYNC4 inline flush in
	 * ps_proxy_pipeline_write can share the same per-stripe
	 * machinery -- including the captured MDS-verifier last-
	 * writer-wins fold into pwb_mds_verf.
	 */
	ret = pwb_flush_range_locked(buf, pls, ms, creds, offset, count);

	/*
	 * Snapshot remaining dirty count under pwb_mutex: it
	 * disambiguates "every dirty stripe was flushed" from "range
	 * filter left some stripes behind" once the mutex is dropped.
	 */
	remaining_dirty = ps_write_buffer_dirty_count(buf);
	pthread_mutex_unlock(&buf->pwb_mutex);

	if (ret == 0) {
		/*
		 * Slice 4b.5: drop the buffer only when no dirty
		 * stripes remain (the full-flush case, identical to 4a's
		 * success contract).  When the range filter left some
		 * dirty stripes behind, release the find ref instead so
		 * the buffer survives for a later wider-range COMMIT.
		 *
		 * Verifier policy on this success path: return the
		 * listener-only encoding (mds_verf_set = false).  The
		 * WRITE replies the client correlates against were
		 * issued earlier in THIS buffer's lifetime, and at those
		 * earlier WRITEs pwb_mds_verf_set was still false (the
		 * per-stripe capture only happens INSIDE this COMMIT's
		 * loop, after the WRITE replies were already sent).
		 * Folding the captured verifier here would cause
		 * V_w != V_c in the happy single-cycle case and trigger
		 * an unnecessary client rewrite on every WRITE/COMMIT
		 * pair.  4b.6 (FILE_SYNC4 inline flush) is the slice
		 * that produces a cross-WRITE captured verifier and the
		 * fold happens via the WRITE reply path -- which already
		 * routes through ps_compose_write_verf with a real
		 * pwb_mds_verf snapshot.
		 */
		if (remaining_dirty == 0)
			ps_write_buffer_drop(buf, pls);
		else
			ps_write_buffer_release_find_ref(buf, pls);
		ps_compose_write_verf(pls, false, NULL, reply->verifier);
		return 0;
	}

	/*
	 * Failure: keep the buffer with remaining dirty entries.  The
	 * client retries COMMIT and the walk picks up where this one
	 * left off.  Propagate the helper's ret verbatim (-EIO from a
	 * per-stripe codec failure, -ENOMEM from a scratch / stripe_
	 * nos allocation failure) so the op handler can map errno to
	 * the right wire status.
	 */
	ps_write_buffer_release_find_ref(buf, pls);
	return ret;
}

int ps_proxy_pipeline_close(struct mds_session *ms, const uint8_t *upstream_fh,
			    uint32_t upstream_fh_len, uint32_t stateid_seqid,
			    const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
			    const struct authunix_parms *creds)
{
	uint32_t listener_id;
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;
	struct mds_file mf;
	uint64_t buf_gen;
	int ret;

	(void)stateid_seqid; /* stashed in the buffer at WRITE time */

	if (!ms || !upstream_fh || upstream_fh_len == 0 || !stateid_other)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	listener_id = atomic_load_explicit(&ms->ms_kick_listener_id,
					   memory_order_relaxed);
	if (listener_id == 0)
		return -EINVAL;
	pls = (struct ps_listener_state *)ps_state_find(listener_id);
	if (!pls)
		return -ENOENT;

	if (!ps_write_buffer_enter_quiesce_or_bail(pls))
		return -EAGAIN;

	/*
	 * Non-allocating lookup: CLOSE on a file that had no buffered
	 * writes is the common case (read-only opens, opens that
	 * already COMMIT'd).  If no buffer exists, return success;
	 * the op handler still calls forward_close.
	 */
	buf = ps_write_buffer_lookup(pls, stateid_other, upstream_fh,
				     upstream_fh_len);
	if (!buf)
		return 0; /* no buffered bytes; quiesce already released */

	pthread_mutex_lock(&buf->pwb_mutex);
	buf_gen = buf->pwb_listener_gen;
	if (buf_gen !=
	    atomic_load_explicit(&pls->pls_boot_gen, memory_order_acquire)) {
		pthread_mutex_unlock(&buf->pwb_mutex);
		ps_write_buffer_drop(buf, pls);
		return -ESTALE;
	}

	memset(&mf, 0, sizeof(mf));
	mf.mf_stateid.seqid = buf->pwb_stateid_seqid;
	memcpy(mf.mf_stateid.other, buf->pwb_stateid_other,
	       PS_STATEID_OTHER_SIZE);
	mf.mf_fh.nfs_fh4_val = (char *)buf->pwb_upstream_fh;
	mf.mf_fh.nfs_fh4_len = buf->pwb_upstream_fh_len;

	/*
	 * Best-effort flush.  Same call shape as pipeline_commit, but
	 * here the buffer is UNCONDITIONALLY dropped on the way out --
	 * CLOSE has no retry surface; if the flush fails the bytes
	 * are lost but the upstream open stateid still gets released
	 * by the caller's subsequent forward_close.  Bytes-loss path
	 * is operator-visible via the close_flush_timeouts_total
	 * counter the design's "ps-write-buffer-stats" probe op
	 * exposes (slice 4a.4).
	 */
	ret = ec_write_codec_with_file(ms, &mf, buf->pwb_data,
				       buf->pwb_high_water, /* k */ 4,
				       /* m */ 2, EC_CODEC_RS,
				       LAYOUT4_FLEX_FILES_V2,
				       /* shard_size */ 4096, creds, pls);
	pthread_mutex_unlock(&buf->pwb_mutex);

	ps_write_buffer_drop(buf, pls);
	return ret == 0 ? 0 : -EIO;
}

/* ------------------------------------------------------------------ */
/* Layout passthrough (task #150) -- foundation stubs.                */
/*                                                                    */
/* The reply_free helpers below are fully wired so the handler hooks  */
/* in lib/nfs4/server/layout.c can call them on every error path.     */
/* The forwarders themselves return -ENOSYS until the real            */
/* deep-copy implementation lands (next commit in this session).      */
/* ------------------------------------------------------------------ */

void ps_proxy_layoutget_reply_free(struct ps_proxy_layoutget_reply *reply)
{
	if (!reply)
		return;
	if (reply->layouts) {
		for (uint32_t i = 0; i < reply->nlayouts; i++)
			free(reply->layouts[i].lo_content_body);
		free(reply->layouts);
	}
	memset(reply, 0, sizeof(*reply));
}

void ps_proxy_getdeviceinfo_reply_free(
	struct ps_proxy_getdeviceinfo_reply *reply)
{
	if (!reply)
		return;
	free(reply->da_addr_body);
	memset(reply, 0, sizeof(*reply));
}

int ps_proxy_forward_layoutget(
	struct mds_session *ms, const uint8_t *upstream_fh,
	uint32_t upstream_fh_len, bool signal_layout_avail,
	uint32_t layout_type, uint32_t iomode, uint64_t offset, uint64_t length,
	uint64_t minlength, uint32_t stateid_seqid,
	const uint8_t stateid_other[PS_STATEID_OTHER_SIZE], uint32_t maxcount,
	const struct authunix_parms *creds,
	struct ps_proxy_layoutget_reply *reply)
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

	ret = mds_compound_init(&mc, 3, "ps-proxy-layoutget");
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

	slot = mds_compound_add_op(&mc, OP_LAYOUTGET);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	LAYOUTGET4args *la = &slot->nfs_argop4_u.oplayoutget;

	la->loga_signal_layout_avail = signal_layout_avail;
	la->loga_layout_type = (layouttype4)layout_type;
	la->loga_iomode = (layoutiomode4)iomode;
	la->loga_offset = offset;
	la->loga_length = length;
	la->loga_minlength = minlength;
	la->loga_stateid.seqid = stateid_seqid;
	memcpy(la->loga_stateid.other, stateid_other, PS_STATEID_OTHER_SIZE);
	la->loga_maxcount = maxcount;

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.oplayoutget.logr_status);
	if (ret)
		goto out;

	LAYOUTGET4resok *lresok =
		&res->nfs_resop4_u.oplayoutget.LAYOUTGET4res_u.logr_resok4;

	reply->return_on_close = lresok->logr_return_on_close;
	reply->stateid_seqid = lresok->logr_stateid.seqid;
	memcpy(reply->stateid_other, lresok->logr_stateid.other,
	       PS_STATEID_OTHER_SIZE);
	reply->nlayouts = lresok->logr_layout.logr_layout_len;

	if (reply->nlayouts == 0) {
		ret = 0;
		goto out;
	}

	reply->layouts = calloc(reply->nlayouts, sizeof(*reply->layouts));
	if (!reply->layouts) {
		ret = -ENOMEM;
		goto out_free_reply;
	}

	for (uint32_t i = 0; i < reply->nlayouts; i++) {
		layout4 *src = &lresok->logr_layout.logr_layout_val[i];
		struct ps_proxy_layout_segment *dst = &reply->layouts[i];

		dst->lo_offset = src->lo_offset;
		dst->lo_length = src->lo_length;
		dst->lo_iomode = src->lo_iomode;
		dst->lo_content_type = src->lo_content.loc_type;
		dst->lo_content_body_len =
			src->lo_content.loc_body.loc_body_len;
		if (dst->lo_content_body_len == 0) {
			dst->lo_content_body = NULL;
			continue;
		}
		dst->lo_content_body = malloc(dst->lo_content_body_len);
		if (!dst->lo_content_body) {
			ret = -ENOMEM;
			goto out_free_reply;
		}
		memcpy(dst->lo_content_body,
		       src->lo_content.loc_body.loc_body_val,
		       dst->lo_content_body_len);
	}

	ret = 0;
	goto out;

out_free_reply:
	ps_proxy_layoutget_reply_free(reply);
out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_proxy_forward_getdeviceinfo(struct mds_session *ms,
				   const uint8_t deviceid[16],
				   uint32_t layout_type, uint32_t maxcount,
				   const struct authunix_parms *creds,
				   struct ps_proxy_getdeviceinfo_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !deviceid || !reply)
		return -EINVAL;

	memset(reply, 0, sizeof(*reply));

	ret = mds_compound_init(&mc, 2, "ps-proxy-getdeviceinfo");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_GETDEVICEINFO);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	GETDEVICEINFO4args *ga = &slot->nfs_argop4_u.opgetdeviceinfo;

	memcpy(ga->gdia_device_id, deviceid, NFS4_DEVICEID4_SIZE);
	ga->gdia_layout_type = (layouttype4)layout_type;
	ga->gdia_maxcount = maxcount;
	ga->gdia_notify_types.bitmap4_len = 0;
	ga->gdia_notify_types.bitmap4_val = NULL;

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opgetdeviceinfo.gdir_status);
	if (ret)
		goto out;

	GETDEVICEINFO4resok *gresok = &res->nfs_resop4_u.opgetdeviceinfo
					       .GETDEVICEINFO4res_u.gdir_resok4;

	reply->da_layout_type = gresok->gdir_device_addr.da_layout_type;
	reply->da_addr_body_len =
		gresok->gdir_device_addr.da_addr_body.da_addr_body_len;
	if (reply->da_addr_body_len > 0) {
		reply->da_addr_body = malloc(reply->da_addr_body_len);
		if (!reply->da_addr_body) {
			ret = -ENOMEM;
			goto out_free_reply;
		}
		memcpy(reply->da_addr_body,
		       gresok->gdir_device_addr.da_addr_body.da_addr_body_val,
		       reply->da_addr_body_len);
	}
	if (gresok->gdir_notification.bitmap4_len > 0)
		reply->notification_mask =
			gresok->gdir_notification.bitmap4_val[0];

	ret = 0;
	goto out;

out_free_reply:
	ps_proxy_getdeviceinfo_reply_free(reply);
out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_proxy_forward_layoutreturn(
	struct mds_session *ms, const uint8_t *upstream_fh,
	uint32_t upstream_fh_len, bool reclaim, uint32_t layout_type,
	uint32_t iomode, uint32_t return_type, uint64_t offset, uint64_t length,
	uint32_t stateid_seqid,
	const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
	const uint8_t *lr_body, uint32_t lr_body_len,
	const struct authunix_parms *creds,
	struct ps_proxy_layoutreturn_reply *reply)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !upstream_fh || upstream_fh_len == 0 || !reply)
		return -EINVAL;
	if (upstream_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;
	if (lr_body_len > 0 && !lr_body)
		return -EINVAL;

	memset(reply, 0, sizeof(*reply));

	ret = mds_compound_init(&mc, 3, "ps-proxy-layoutreturn");
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

	slot = mds_compound_add_op(&mc, OP_LAYOUTRETURN);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	LAYOUTRETURN4args *lra = &slot->nfs_argop4_u.oplayoutreturn;

	lra->lora_reclaim = reclaim;
	lra->lora_layout_type = (layouttype4)layout_type;
	lra->lora_iomode = (layoutiomode4)iomode;
	lra->lora_layoutreturn.lr_returntype = (layoutreturn_type4)return_type;
	if (return_type == LAYOUTRETURN4_FILE) {
		layoutreturn_file4 *lrf =
			&lra->lora_layoutreturn.layoutreturn4_u.lr_layout;

		lrf->lrf_offset = offset;
		lrf->lrf_length = length;
		lrf->lrf_stateid.seqid = stateid_seqid;
		memcpy(lrf->lrf_stateid.other, stateid_other, NFS4_OTHER_SIZE);
		lrf->lrf_body.lrf_body_val = (char *)lr_body;
		lrf->lrf_body.lrf_body_len = lr_body_len;
	}

	ret = ps_proxy_send_with_kick(&mc, ms, creds);
	if (ret && ret != -EREMOTEIO)
		goto out;
	ret = 0;

	res = mds_compound_result(&mc, 1);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.opputfh.status);
	if (ret)
		goto out;

	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = nfs4_to_errno(res->nfs_resop4_u.oplayoutreturn.lorr_status);
	if (ret)
		goto out;

	if (return_type == LAYOUTRETURN4_FILE) {
		layoutreturn_stateid *st =
			&res->nfs_resop4_u.oplayoutreturn.LAYOUTRETURN4res_u
				 .lorr_stateid;

		reply->stateid_present = st->lrs_present;
		if (reply->stateid_present) {
			reply->stateid_seqid =
				st->layoutreturn_stateid_u.lrs_stateid.seqid;
			memcpy(reply->stateid_other,
			       st->layoutreturn_stateid_u.lrs_stateid.other,
			       PS_STATEID_OTHER_SIZE);
		}
	}

	ret = 0;
out:
	mds_compound_fini(&mc);
	return ret;
}
