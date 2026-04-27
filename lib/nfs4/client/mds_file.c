/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * File operations for the EC demo client.
 *
 * PUTROOTFH + LOOKUP* + OPEN (CLAIM_NULL) to open/create a file
 * relative to the MDS root, CLOSE to release the open stateid.
 *
 * NFSv4 OPEN takes a single-component name; multi-component paths
 * are walked via LOOKUP ops between PUTROOTFH and OPEN.  The
 * compound shape is:
 *     SEQUENCE PUTROOTFH (LOOKUP "comp_i")* OPEN(last) GETFH
 * For a path like "/ffv1-csm/test.bin" that yields one LOOKUP for
 * "ffv1-csm" then an OPEN for "test.bin".  A bare "test.bin" at
 * root takes zero LOOKUPs, matching the original 4-op shape.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"

/* ------------------------------------------------------------------ */
/* OPEN                                                                */
/* ------------------------------------------------------------------ */

/*
 * Split path into "/"-separated components.  Returns a heap array of
 * component pointers into a single heap buffer plus the count via
 * *nout.  Caller frees both via free(*components_out) and
 * free(*buf_out).  A path like "/a/b/c" yields {"a","b","c"} (3
 * components).  Empty / NULL path returns 0 components.
 *
 * The components point into buf_out -- they live as long as the
 * buffer does, so libtirpc's xdr_opaque-style "borrow the bytes
 * during clnt_call" pattern works for both LOOKUP4args.objname and
 * OPEN4args.claim.file: we keep both buffers alive until
 * mds_compound_send returns.
 */
static int split_path(const char *path, char ***components_out, char **buf_out,
		      size_t *nout)
{
	*components_out = NULL;
	*buf_out = NULL;
	*nout = 0;

	if (!path)
		return -EINVAL;

	while (*path == '/')
		path++;
	if (!*path)
		return -EINVAL;

	char *buf = strdup(path);
	if (!buf)
		return -ENOMEM;

	/*
	 * Upper bound on component count: separator count + 1.  This
	 * over-estimates for "//"-runs and trailing slashes (strtok_r
	 * collapses empty tokens), which is fine -- the array is
	 * sized once and the real count is reported via *nout.
	 */
	size_t cap = 0;
	for (const char *p = path; *p; p++)
		if (*p == '/')
			cap++;
	cap++;

	char **comps = calloc(cap, sizeof(char *));
	if (!comps) {
		free(buf);
		return -ENOMEM;
	}

	size_t n = 0;
	char *save = NULL;
	for (char *tok = strtok_r(buf, "/", &save); tok != NULL;
	     tok = strtok_r(NULL, "/", &save)) {
		if (*tok == '\0')
			continue;
		comps[n++] = tok;
	}

	if (n == 0) {
		free(comps);
		free(buf);
		return -EINVAL;
	}

	*components_out = comps;
	*buf_out = buf;
	*nout = n;
	return 0;
}

int mds_file_open(struct mds_session *ms, const char *path, struct mds_file *mf)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	char **comps = NULL;
	char *path_buf = NULL;
	size_t ncomps = 0;
	int ret;

	memset(mf, 0, sizeof(*mf));

	ret = split_path(path, &comps, &path_buf, &ncomps);
	if (ret)
		return ret;

	/* SEQ + PUTROOTFH + (ncomps-1)*LOOKUP + OPEN + GETFH */
	int nops = 4 + ((int)ncomps - 1);
	ret = mds_compound_init(&mc, nops, "open");
	if (ret) {
		free(comps);
		free(path_buf);
		return ret;
	}

	/* Op 0: SEQUENCE */
	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out_err;

	/* Op 1: PUTROOTFH */
	slot = mds_compound_add_op(&mc, OP_PUTROOTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out_err;
	}

	/* LOOKUP for each component except the last. */
	for (size_t i = 0; i < ncomps - 1; i++) {
		slot = mds_compound_add_op(&mc, OP_LOOKUP);
		if (!slot) {
			ret = -ENOSPC;
			goto out_err;
		}
		slot->nfs_argop4_u.oplookup.objname.utf8string_val = comps[i];
		slot->nfs_argop4_u.oplookup.objname.utf8string_len =
			strlen(comps[i]);
	}

	/* OPEN: last component is the file to create/open. */
	slot = mds_compound_add_op(&mc, OP_OPEN);
	if (!slot) {
		ret = -ENOSPC;
		goto out_err;
	}

	OPEN4args *open_args = &slot->nfs_argop4_u.opopen;
	const char *fname = comps[ncomps - 1];

	open_args->seqid = 0;
	open_args->share_access = OPEN4_SHARE_ACCESS_BOTH |
				  OPEN4_SHARE_ACCESS_WANT_NO_DELEG;
	open_args->share_deny = OPEN4_SHARE_DENY_NONE;

	/* Open owner: use clientid + "ec_demo". */
	open_args->owner.clientid = ms->ms_clientid;
	open_args->owner.owner.owner_val = (char *)"ec_demo";
	open_args->owner.owner.owner_len = 7;

	/* Create if not exists, open if exists. */
	open_args->openhow.opentype = OPEN4_CREATE;
	open_args->openhow.openflag4_u.how.mode = UNCHECKED4;
	/* No initial attributes -- let the server use defaults. */
	memset(&open_args->openhow.openflag4_u.how.createhow4_u.createattrs, 0,
	       sizeof(fattr4));

	open_args->claim.claim = CLAIM_NULL;
	open_args->claim.open_claim4_u.file.utf8string_val = (char *)fname;
	open_args->claim.open_claim4_u.file.utf8string_len = strlen(fname);

	/* GETFH -- get the filehandle for the opened file. */
	slot = mds_compound_add_op(&mc, OP_GETFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out_err;
	}

	ret = mds_compound_send(&mc, ms);
	if (ret)
		goto out_err;

	/*
	 * OPEN result lives at index (2 + (ncomps - 1)): SEQ at 0,
	 * PUTROOTFH at 1, then ncomps-1 LOOKUPs, then OPEN.
	 * GETFH follows at OPEN+1.
	 */
	int open_idx = 2 + (int)(ncomps - 1);
	int getfh_idx = open_idx + 1;

	nfs_resop4 *open_res = mds_compound_result(&mc, open_idx);

	if (!open_res || open_res->nfs_resop4_u.opopen.status != NFS4_OK) {
		ret = -EREMOTEIO;
		goto out_err;
	}

	OPEN4resok *resok = &open_res->nfs_resop4_u.opopen.OPEN4res_u.resok4;
	memcpy(&mf->mf_stateid, &resok->stateid, sizeof(stateid4));

	/* Extract filehandle from GETFH result. */
	nfs_resop4 *getfh_res = mds_compound_result(&mc, getfh_idx);

	if (!getfh_res || getfh_res->nfs_resop4_u.opgetfh.status != NFS4_OK) {
		ret = -EREMOTEIO;
		goto out_err;
	}

	GETFH4resok *fhresok =
		&getfh_res->nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

	mf->mf_fh.nfs_fh4_len = fhresok->object.nfs_fh4_len;
	mf->mf_fh.nfs_fh4_val = malloc(fhresok->object.nfs_fh4_len);
	if (!mf->mf_fh.nfs_fh4_val) {
		ret = -ENOMEM;
		goto out_err;
	}
	memcpy(mf->mf_fh.nfs_fh4_val, fhresok->object.nfs_fh4_val,
	       fhresok->object.nfs_fh4_len);

	mds_compound_fini(&mc);
	free(comps);
	free(path_buf);
	return 0;

out_err:
	mds_compound_fini(&mc);
	free(comps);
	free(path_buf);
	return ret;
}

/* ------------------------------------------------------------------ */
/* CLOSE                                                               */
/* ------------------------------------------------------------------ */

int mds_file_close(struct mds_session *ms, struct mds_file *mf)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	/* SEQUENCE + PUTFH + CLOSE = 3 ops */
	ret = mds_compound_init(&mc, 3, "close");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	/* PUTFH -- set current FH to the file. */
	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = mf->mf_fh;

	/* CLOSE */
	slot = mds_compound_add_op(&mc, OP_CLOSE);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	CLOSE4args *close_args = &slot->nfs_argop4_u.opclose;

	close_args->seqid = 0;
	memcpy(&close_args->open_stateid, &mf->mf_stateid, sizeof(stateid4));

	ret = mds_compound_send(&mc, ms);
	mds_compound_fini(&mc);

	free(mf->mf_fh.nfs_fh4_val);
	memset(mf, 0, sizeof(*mf));

	return ret;
}

/* ------------------------------------------------------------------ */
/* WRITE -- inband MDS write (no layouts)                               */
/* ------------------------------------------------------------------ */

int mds_file_write(struct mds_session *ms, struct mds_file *mf,
		   const uint8_t *data, uint32_t len, uint64_t offset)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	ret = mds_compound_init(&mc, 3, "write");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = mf->mf_fh;

	slot = mds_compound_add_op(&mc, OP_WRITE);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	WRITE4args *wa = &slot->nfs_argop4_u.opwrite;

	memcpy(&wa->stateid, &mf->mf_stateid, sizeof(stateid4));
	wa->offset = offset;
	wa->stable = FILE_SYNC4;
	wa->data.data_len = len;
	wa->data.data_val = (char *)data;

	ret = mds_compound_send(&mc, ms);
	if (ret) {
		fprintf(stderr,
			"mds_file_write: compound failed"
			" off=%llu len=%u ret=%d\n",
			(unsigned long long)offset, len, ret);
		mds_compound_fini(&mc);
		return ret;
	}

	nfs_resop4 *res = mds_compound_result(&mc, 2);

	if (!res || res->nfs_resop4_u.opwrite.status != NFS4_OK) {
		nfsstat4 st = res ? res->nfs_resop4_u.opwrite.status :
				    (nfsstat4)-1;

		fprintf(stderr,
			"mds_file_write: WRITE failed"
			" off=%llu len=%u status=%d\n",
			(unsigned long long)offset, len, (int)st);
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	mds_compound_fini(&mc);
	return 0;
}

/* ------------------------------------------------------------------ */
/* READ -- inband MDS read (no layouts)                                 */
/* ------------------------------------------------------------------ */

int mds_file_read(struct mds_session *ms, struct mds_file *mf, uint8_t *buf,
		  uint32_t len, uint64_t offset, uint32_t *nread)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	ret = mds_compound_init(&mc, 3, "read");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = mf->mf_fh;

	slot = mds_compound_add_op(&mc, OP_READ);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	READ4args *ra = &slot->nfs_argop4_u.opread;

	memcpy(&ra->stateid, &mf->mf_stateid, sizeof(stateid4));
	ra->offset = offset;
	ra->count = len;

	ret = mds_compound_send(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	nfs_resop4 *res = mds_compound_result(&mc, 2);

	if (!res || res->nfs_resop4_u.opread.status != NFS4_OK) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	READ4resok *resok = &res->nfs_resop4_u.opread.READ4res_u.resok4;
	uint32_t got = resok->data.data_len;

	if (got > len)
		got = len;
	memcpy(buf, resok->data.data_val, got);
	if (nread)
		*nread = got;

	mds_compound_fini(&mc);
	return 0;
}

/* ------------------------------------------------------------------ */
/* REMOVE -- delete a file by name from the root directory              */
/* ------------------------------------------------------------------ */

int mds_file_remove(struct mds_session *ms, const char *name)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	ret = mds_compound_init(&mc, 3, "remove");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	slot = mds_compound_add_op(&mc, OP_PUTROOTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	slot = mds_compound_add_op(&mc, OP_REMOVE);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	REMOVE4args *rm = &slot->nfs_argop4_u.opremove;

	rm->target.utf8string_len = strlen(name);
	rm->target.utf8string_val = (char *)name;

	ret = mds_compound_send(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	nfs_resop4 *res = mds_compound_result(&mc, 2);

	if (!res || res->nfs_resop4_u.opremove.status != NFS4_OK) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	mds_compound_fini(&mc);
	return 0;
}

/* ------------------------------------------------------------------ */
/* GETATTR -- retrieve owner and owner_group strings                    */
/* ------------------------------------------------------------------ */

/*
 * Parse an XDR utf8str_cs from a byte stream.
 * Returns pointer past the string, or NULL on truncation.
 */
static const char *parse_utf8str(const char *p, size_t remaining, char *out,
				 size_t out_size)
{
	if (remaining < 4)
		return NULL;

	uint32_t len;

	memcpy(&len, p, 4);
	len = ntohl(len);
	p += 4;
	remaining -= 4;

	if (len > remaining || len >= out_size)
		return NULL;

	memcpy(out, p, len);
	out[len] = '\0';

	uint32_t padded = (len + 3) & ~3u;

	return p + (padded <= remaining ? padded : remaining);
}

int mds_file_getattr(struct mds_session *ms, struct mds_file *mf, char *owner,
		     size_t owner_size, char *owner_group,
		     size_t owner_group_size)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	/* SEQUENCE + PUTFH + GETATTR = 3 ops */
	ret = mds_compound_init(&mc, 3, "getattr");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	/* PUTFH */
	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = mf->mf_fh;

	/* GETATTR requesting owner (36) and owner_group (37). */
	slot = mds_compound_add_op(&mc, OP_GETATTR);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	/*
	 * Bitmap: word 1, bits 4 and 5 (attrs 36 and 37 are in word 1,
	 * at bit positions 36-32=4 and 37-32=5).
	 */
	static uint32_t bm_words[2] = { 0, (1U << 4) | (1U << 5) };

	GETATTR4args *ga = &slot->nfs_argop4_u.opgetattr;

	ga->attr_request.bitmap4_len = 2;
	ga->attr_request.bitmap4_val = bm_words;

	ret = mds_compound_send(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	nfs_resop4 *res = mds_compound_result(&mc, 2);

	if (!res || res->nfs_resop4_u.opgetattr.status != NFS4_OK) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	GETATTR4resok *resok =
		&res->nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
	fattr4 *fa = &resok->obj_attributes;

	/*
	 * The attr_vals contains XDR-encoded values for the bits set
	 * in attrmask.  We requested only owner and owner_group
	 * (both utf8str_cs), so the payload is: [owner] [owner_group].
	 * Check which bits are actually returned.
	 */
	const char *p = fa->attr_vals.attrlist4_val;
	size_t remaining = fa->attr_vals.attrlist4_len;

	bool has_owner = (fa->attrmask.bitmap4_len > 1 &&
			  (fa->attrmask.bitmap4_val[1] & (1U << 4)));
	bool has_owner_group = (fa->attrmask.bitmap4_len > 1 &&
				(fa->attrmask.bitmap4_val[1] & (1U << 5)));

	if (has_owner && owner && owner_size > 0) {
		p = parse_utf8str(p, remaining, owner, owner_size);
		if (!p) {
			mds_compound_fini(&mc);
			return -EINVAL;
		}
		remaining = fa->attr_vals.attrlist4_len -
			    (p - fa->attr_vals.attrlist4_val);
	} else if (owner) {
		owner[0] = '\0';
	}

	if (has_owner_group && owner_group && owner_group_size > 0) {
		p = parse_utf8str(p, remaining, owner_group, owner_group_size);
		if (!p) {
			mds_compound_fini(&mc);
			return -EINVAL;
		}
	} else if (owner_group) {
		owner_group[0] = '\0';
	}

	mds_compound_fini(&mc);
	return 0;
}

/* ------------------------------------------------------------------ */
/* CLONE -- server-side reflink from SAVED_FH (source) to CURRENT_FH   */
/* ------------------------------------------------------------------ */

/*
 * mds_file_clone - copy src into dst using the server's CLONE op.
 *
 * The compound is: SEQUENCE + PUTFH(src) + SAVEFH + PUTFH(dst) + CLONE.
 * src becomes SAVED_FH (the source of the copy); dst becomes CURRENT_FH
 * (the destination).
 *
 * count=0 means "to end of file" per RFC 7862 S15.13.
 */
int mds_file_clone(struct mds_session *ms, struct mds_file *src,
		   struct mds_file *dst, uint64_t src_offset,
		   uint64_t dst_offset, uint64_t count)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	/* SEQUENCE + PUTFH(src) + SAVEFH + PUTFH(dst) + CLONE = 5 ops */
	ret = mds_compound_init(&mc, 5, "clone");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	/* PUTFH(src): make src the current FH so SAVEFH captures it. */
	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = src->mf_fh;

	/* SAVEFH: copy current FH (src) into the saved FH slot. */
	slot = mds_compound_add_op(&mc, OP_SAVEFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	/* PUTFH(dst): make dst the current FH (CLONE destination). */
	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = dst->mf_fh;

	/* CLONE: reflink from SAVED_FH (src) into CURRENT_FH (dst). */
	slot = mds_compound_add_op(&mc, OP_CLONE);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	CLONE4args *ca = &slot->nfs_argop4_u.opclone;
	memcpy(&ca->cl_src_stateid, &src->mf_stateid, sizeof(stateid4));
	memcpy(&ca->cl_dst_stateid, &dst->mf_stateid, sizeof(stateid4));
	ca->cl_src_offset = src_offset;
	ca->cl_dst_offset = dst_offset;
	ca->cl_count = count;

	ret = mds_compound_send(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	nfs_resop4 *res = mds_compound_result(&mc, 4);

	if (!res || res->nfs_resop4_u.opclone.cl_status != NFS4_OK) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	mds_compound_fini(&mc);
	return 0;
}

/* ------------------------------------------------------------------ */
/* EXCHANGE_RANGE -- atomically swap byte ranges between two files      */
/* ------------------------------------------------------------------ */

/*
 * mds_file_exchange_range - atomically swap byte ranges between src and dst.
 *
 * The compound is: SEQUENCE + PUTFH(src) + SAVEFH + PUTFH(dst) +
 * EXCHANGE_RANGE.  src becomes SAVED_FH; dst becomes CURRENT_FH.
 *
 * The swap is self-inverse: calling this twice with the same arguments
 * returns both files to their original state.  count=0 means "to end of
 * the larger of the two files" per draft-haynes-nfsv4-swap.
 */
int mds_file_exchange_range(struct mds_session *ms, struct mds_file *src,
			    struct mds_file *dst, uint64_t src_offset,
			    uint64_t dst_offset, uint64_t count)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	/* SEQUENCE + PUTFH(src) + SAVEFH + PUTFH(dst) + EXCHANGE_RANGE = 5 ops */
	ret = mds_compound_init(&mc, 5, "exchange_range");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	/* PUTFH(src): make src the current FH so SAVEFH captures it. */
	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = src->mf_fh;

	/* SAVEFH: copy current FH (src) into the saved FH slot. */
	slot = mds_compound_add_op(&mc, OP_SAVEFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	/* PUTFH(dst): make dst the current FH (EXCHANGE_RANGE destination). */
	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = dst->mf_fh;

	/* EXCHANGE_RANGE: atomically swap ranges between SAVED_FH and CURRENT_FH. */
	slot = mds_compound_add_op(&mc, OP_EXCHANGE_RANGE);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	EXCHANGE_RANGE4args *era = &slot->nfs_argop4_u.opexchange_range;
	memcpy(&era->era_src_stateid, &src->mf_stateid, sizeof(stateid4));
	memcpy(&era->era_dst_stateid, &dst->mf_stateid, sizeof(stateid4));
	era->era_src_offset = src_offset;
	era->era_dst_offset = dst_offset;
	era->era_count = count;

	ret = mds_compound_send(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	nfs_resop4 *res = mds_compound_result(&mc, 4);

	if (!res || res->nfs_resop4_u.opexchange_range.err_status != NFS4_OK) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	mds_compound_fini(&mc);
	return 0;
}

/* ------------------------------------------------------------------ */
/* SETATTR -- set owner and/or owner_group strings                      */
/* ------------------------------------------------------------------ */

int mds_file_setattr_owner(struct mds_session *ms, struct mds_file *mf,
			   const char *owner, const char *owner_group)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	/* SEQUENCE + PUTFH + SETATTR = 3 ops */
	ret = mds_compound_init(&mc, 3, "setattr");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = mf->mf_fh;

	slot = mds_compound_add_op(&mc, OP_SETATTR);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	SETATTR4args *sa = &slot->nfs_argop4_u.opsetattr;

	/* Zero stateid = no delegation. */
	memset(&sa->stateid, 0, sizeof(stateid4));

	/*
	 * Build fattr4: bitmap + attr_vals for owner and/or owner_group.
	 * XDR format: each is a utf8str_cs = opaque<> = [4-byte len][data][pad].
	 */
	uint32_t bm_val[2] = { 0, 0 };
	char attr_buf[1024];
	char *ap = attr_buf;

	if (owner) {
		uint32_t len = (uint32_t)strlen(owner);
		uint32_t len_net = htonl(len);

		memcpy(ap, &len_net, 4);
		ap += 4;
		memcpy(ap, owner, len);
		ap += len;

		uint32_t pad = ((len + 3) & ~3u) - len;

		if (pad > 0) {
			memset(ap, 0, pad);
			ap += pad;
		}
		bm_val[1] |= (1U << 4); /* FATTR4_OWNER */
	}

	if (owner_group) {
		uint32_t len = (uint32_t)strlen(owner_group);
		uint32_t len_net = htonl(len);

		memcpy(ap, &len_net, 4);
		ap += 4;
		memcpy(ap, owner_group, len);
		ap += len;

		uint32_t pad = ((len + 3) & ~3u) - len;

		if (pad > 0) {
			memset(ap, 0, pad);
			ap += pad;
		}
		bm_val[1] |= (1U << 5); /* FATTR4_OWNER_GROUP */
	}

	size_t attr_len = (size_t)(ap - attr_buf);

	static uint32_t bm_words_set[2];

	bm_words_set[0] = bm_val[0];
	bm_words_set[1] = bm_val[1];

	sa->obj_attributes.attrmask.bitmap4_len = 2;
	sa->obj_attributes.attrmask.bitmap4_val = bm_words_set;
	sa->obj_attributes.attr_vals.attrlist4_len = (u_int)attr_len;
	sa->obj_attributes.attr_vals.attrlist4_val = attr_buf;

	ret = mds_compound_send(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	nfs_resop4 *res = mds_compound_result(&mc, 2);

	if (!res || res->nfs_resop4_u.opsetattr.status != NFS4_OK) {
		nfsstat4 st = res ? res->nfs_resop4_u.opsetattr.status : 0;

		fprintf(stderr, "SETATTR failed: status=%u\n", st);
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	mds_compound_fini(&mc);
	return 0;
}
