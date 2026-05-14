/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_SHORTCIRCUIT_H
#define _REFFS_PS_SHORTCIRCUIT_H

#include <stddef.h>
#include <stdint.h>

struct ps_listener_state; /* fwd: ps_state.h */

/*
 * Install the short-circuit dispatch on `pls`.  Sets the
 * pls_sc_write_fn / pls_sc_read_fn function pointers so the
 * Phase 5 dispatch hook in ec_pipeline.c can route through them
 * without a static reference to ps_shortcircuit_write / _read.
 *
 * Layering note: ps_shortcircuit.c lives in libreffs_nfs4_ps_sb.la
 * (depends on lib/fs).  ec_pipeline.c lives in libreffs_nfs4_ps.la
 * (no lib/fs dependency).  ec_demo links libreffs_nfs4_ps.la but
 * NOT libreffs_nfs4_ps_sb.la and NOT lib/fs.  The install hook is
 * the one-way bridge: reffsd + PS unit tests call it after
 * ps_state_register; ec_demo never does, and the dispatch hook
 * falls through to the RPC path because the function pointers
 * stay NULL.
 *
 * `pls == NULL` is a no-op (callers may invoke unconditionally
 * after ps_state_find).
 */
void ps_shortcircuit_install(struct ps_listener_state *pls);

/*
 * Phase 5 short-circuit helpers.  When the EC pipeline's per-mirror
 * dispatch hook sees em_local == true (set by ps_local_addr_match
 * after deviceinfo resolution), these helpers replace the RPC call
 * with a direct data-backend access against the co-resident DS sb.
 *
 * The wire FH layout is `struct network_file_handle` (24 packed
 * bytes: version + reserved + sb_id + ino).  The helper decodes the
 * FH, looks up the local sb via the unscoped super_block_find()
 * (the DS sb is shared across listeners), pins the target inode,
 * and drives sb->sb_ops->db_read / db_write directly.
 *
 * Returns:
 *   0          -- success (write); read sets *nread on success
 *   -EINVAL    -- NULL / short / version-mismatched FH
 *   -ESTALE    -- sb_id or ino does not resolve to a live local
 *                 object (caller should propagate to the upstream
 *                 LAYOUTERROR machinery just as it would on the
 *                 RPC path's NFS4ERR_STALE)
 *   other      -- backend I/O error from db_read / db_write
 *
 * The helpers are synchronous -- they hold an active inode ref
 * across the db_io call and drop it before returning.  No RCU
 * read-side critical section spans the I/O.
 *
 * `block_offset` is the byte offset within the file (the same
 * `offset` units the upstream RPC path uses; the helpers do not
 * scale by block size).
 *
 * Per-request cred check: forwarded_uid / forwarded_gid carry the
 * synthetic AUTH_SYS credentials the layout granted to the upstream
 * client.  The helpers compare them against the local file's stored
 * i_uid / i_gid (the synthetic-fenced values the MDS chmod'd onto
 * the DS file at runway-pop time) and return -EACCES on mismatch
 * BEFORE touching the data block.  This mirrors what the RPC path's
 * NFSv3 / CHUNK access check would have rejected.  Callers pass the
 * same uid/gid pair the layout-decode placed on
 * struct ec_mirror.em_uid / em_gid; root-squash (if any) is
 * expected to have already transformed those values upstream so a
 * forwarded uid=0 against a fenced file decodes here as a mismatch.
 *
 * NOT_NOW_BROWN_COW (slice 5.4): trust-stateid table lookup.  Once
 * trust-stateid Phase 1 ships on the PS, the layout stateid MUST
 * be checked here against the local DS sb's trust table; until
 * then the table is empty and every short-circuit accepts.
 */
int ps_shortcircuit_write(const uint8_t *fh, uint32_t fh_len,
			  uint64_t block_offset, const uint8_t *data,
			  size_t data_len, uint32_t forwarded_uid,
			  uint32_t forwarded_gid);

int ps_shortcircuit_read(const uint8_t *fh, uint32_t fh_len,
			 uint64_t block_offset, size_t buf_len, uint8_t *buf,
			 uint32_t *nread, uint32_t forwarded_uid,
			 uint32_t forwarded_gid);

#endif /* _REFFS_PS_SHORTCIRCUIT_H */
