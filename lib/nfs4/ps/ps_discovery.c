/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "nfsv42_xdr.h"

#include "ec_client.h"
#include "ps_discovery.h"

int ps_discovery_fetch_root_fh(struct mds_session *ms, uint8_t *fh_buf,
			       uint32_t buf_size, uint32_t *fh_len_out)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !fh_buf || !fh_len_out)
		return -EINVAL;

	/* SEQUENCE + PUTROOTFH + GETFH = 3 ops */
	ret = mds_compound_init(&mc, 3, "ps-discover-root");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTROOTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	slot = mds_compound_add_op(&mc, OP_GETFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	ret = mds_compound_send(&mc, ms);
	if (ret)
		goto out;

	/* PUTROOTFH status (op index 1) */
	res = mds_compound_result(&mc, 1);
	if (!res || res->nfs_resop4_u.opputrootfh.status != NFS4_OK) {
		ret = -EREMOTEIO;
		goto out;
	}

	/* GETFH result (op index 2) */
	res = mds_compound_result(&mc, 2);
	if (!res || res->nfs_resop4_u.opgetfh.status != NFS4_OK) {
		ret = -EREMOTEIO;
		goto out;
	}

	GETFH4resok *fhresok = &res->nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

	/*
	 * Zero-length FH would be a broken MDS response (no meaningful
	 * anchor) and guards against memcpy(dst, NULL, 0) UB if the XDR
	 * decoder left nfs_fh4_val NULL on a 0-length read.
	 */
	if (fhresok->object.nfs_fh4_len == 0) {
		ret = -EREMOTEIO;
		goto out;
	}
	if (fhresok->object.nfs_fh4_len > buf_size) {
		ret = -ENOSPC;
		goto out;
	}

	memcpy(fh_buf, fhresok->object.nfs_fh4_val,
	       fhresok->object.nfs_fh4_len);
	*fh_len_out = fhresok->object.nfs_fh4_len;
	ret = 0;

out:
	mds_compound_fini(&mc);
	return ret;
}
