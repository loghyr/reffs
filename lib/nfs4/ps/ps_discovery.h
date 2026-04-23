/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_DISCOVERY_H
#define _REFFS_PS_DISCOVERY_H

#include <stdint.h>

struct mds_session; /* lib/nfs4/client/ec_client.h */

/*
 * Fetch the MDS's root filehandle via the PS's existing session.
 *
 * Sends SEQUENCE + PUTROOTFH + GETFH and copies the returned FH into
 * `fh_buf` (up to `buf_size` bytes).  On success writes the FH length
 * into `*fh_len_out` and returns 0.
 *
 * This is the first discovery step the PS runs after opening its
 * MDS-facing session: proves the session can round-trip a compound
 * and stashes the anchor FH for future LOOKUP walks.  Returns:
 *
 *   0        success
 *   -EINVAL  ms / fh_buf / fh_len_out is NULL
 *   -ENOSPC  buf_size is smaller than the returned FH
 *   -errno   RPC / compound failure (mds_compound_send or non-OK
 *            status on PUTROOTFH or GETFH)
 *
 * See `.claude/design/proxy-server.md` phase 2, "Discovery".
 */
int ps_discovery_fetch_root_fh(struct mds_session *ms, uint8_t *fh_buf,
			       uint32_t buf_size, uint32_t *fh_len_out);

#endif /* _REFFS_PS_DISCOVERY_H */
