/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"

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

	ret = mds_compound_send(&mc, ms);
	if (ret)
		goto out;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res || res->nfs_resop4_u.opputfh.status != NFS4_OK) {
		ret = -EREMOTEIO;
		goto out;
	}

	/* GETATTR result at index 2. */
	res = mds_compound_result(&mc, 2);
	if (!res || res->nfs_resop4_u.opgetattr.status != NFS4_OK) {
		ret = -EREMOTEIO;
		goto out;
	}

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
