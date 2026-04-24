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

int ps_proxy_forward_lookup(struct mds_session *ms, const uint8_t *parent_fh,
			    uint32_t parent_fh_len, const char *name,
			    uint32_t name_len, uint8_t *child_fh_buf,
			    uint32_t child_fh_buf_len,
			    uint32_t *child_fh_len_out,
			    const uint32_t *attr_request,
			    uint32_t attr_request_len,
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

	ret = mds_compound_send(&mc, ms);
	if (ret)
		goto out;

	/* PUTFH status at index 1. */
	res = mds_compound_result(&mc, 1);
	if (!res || res->nfs_resop4_u.opputfh.status != NFS4_OK) {
		ret = -EREMOTEIO;
		goto out;
	}

	/*
	 * LOOKUP status at index 2.  Surface NFS4ERR_NOENT as -ENOENT
	 * so the caller can return NFS4ERR_NOENT to the client without
	 * re-parsing a generic -EREMOTEIO; any other non-OK status
	 * collapses to -EREMOTEIO (network / protocol / state
	 * violation on the upstream -- not actionable at this layer).
	 */
	res = mds_compound_result(&mc, 2);
	if (!res) {
		ret = -EREMOTEIO;
		goto out;
	}
	if (res->nfs_resop4_u.oplookup.status != NFS4_OK) {
		if (res->nfs_resop4_u.oplookup.status == NFS4ERR_NOENT)
			ret = -ENOENT;
		else
			ret = -EREMOTEIO;
		goto out;
	}

	/* GETFH result at index 3. */
	res = mds_compound_result(&mc, 3);
	if (!res || res->nfs_resop4_u.opgetfh.status != NFS4_OK) {
		ret = -EREMOTEIO;
		goto out;
	}

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
		if (!res || res->nfs_resop4_u.opgetattr.status != NFS4_OK) {
			ret = -EREMOTEIO;
			goto out;
		}

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
