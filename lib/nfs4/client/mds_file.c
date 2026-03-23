/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * File operations for the EC demo client.
 *
 * PUTROOTFH + OPEN (CLAIM_NULL) to open/create a file, CLOSE to
 * release the open stateid.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"

/* ------------------------------------------------------------------ */
/* OPEN                                                                */
/* ------------------------------------------------------------------ */

int mds_file_open(struct mds_session *ms, const char *path, struct mds_file *mf)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	memset(mf, 0, sizeof(*mf));

	/* SEQUENCE + PUTROOTFH + OPEN + GETFH = 4 ops */
	ret = mds_compound_init(&mc, 4, "open");
	if (ret)
		return ret;

	/* Op 0: SEQUENCE */
	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	/* Op 1: PUTROOTFH */
	slot = mds_compound_add_op(&mc, OP_PUTROOTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	/* Op 2: OPEN (CLAIM_NULL — open by name relative to current FH) */
	slot = mds_compound_add_op(&mc, OP_OPEN);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	OPEN4args *open_args = &slot->nfs_argop4_u.opopen;

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
	/* No initial attributes — let the server use defaults. */
	memset(&open_args->openhow.openflag4_u.how.createhow4_u.createattrs, 0,
	       sizeof(fattr4));

	open_args->claim.claim = CLAIM_NULL;
	open_args->claim.open_claim4_u.file.utf8string_val = (char *)path;
	open_args->claim.open_claim4_u.file.utf8string_len = strlen(path);

	/* Op 3: GETFH — get the filehandle for the opened file. */
	slot = mds_compound_add_op(&mc, OP_GETFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	ret = mds_compound_send(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	/* Extract open stateid from OPEN result (op index 2). */
	nfs_resop4 *open_res = mds_compound_result(&mc, 2);

	if (!open_res || open_res->nfs_resop4_u.opopen.status != NFS4_OK) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	OPEN4resok *resok = &open_res->nfs_resop4_u.opopen.OPEN4res_u.resok4;
	memcpy(&mf->mf_stateid, &resok->stateid, sizeof(stateid4));

	/* Extract filehandle from GETFH result (op index 3). */
	nfs_resop4 *getfh_res = mds_compound_result(&mc, 3);

	if (!getfh_res || getfh_res->nfs_resop4_u.opgetfh.status != NFS4_OK) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	GETFH4resok *fhresok =
		&getfh_res->nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

	mf->mf_fh.nfs_fh4_len = fhresok->object.nfs_fh4_len;
	mf->mf_fh.nfs_fh4_val = malloc(fhresok->object.nfs_fh4_len);
	if (!mf->mf_fh.nfs_fh4_val) {
		mds_compound_fini(&mc);
		return -ENOMEM;
	}
	memcpy(mf->mf_fh.nfs_fh4_val, fhresok->object.nfs_fh4_val,
	       fhresok->object.nfs_fh4_len);

	mds_compound_fini(&mc);
	return 0;
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

	/* PUTFH — set current FH to the file. */
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
