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

/*
 * Maximum path depth (number of "/"-separated components) the walker
 * accepts in a single compound.  The compound emits 3 + depth ops
 * (SEQUENCE + PUTROOTFH + LOOKUP*depth + GETFH), and the client
 * session negotiated ca_maxoperations = 16 at CREATE_SESSION time
 * (see lib/nfs4/client/mds_session.c).  Depth 13 saturates that
 * budget without overflowing the upstream-accepted bound -- a
 * compliant server returns NFS4ERR_TOO_MANY_OPS if we exceed it.
 * Real upstream paths are typically 1-3 components deep; 13 is
 * generous without needing a session-attrs bump.
 */
#define PS_DISCOVERY_MAX_DEPTH 13

/*
 * Per-component length cap (RFC 8881 does not mandate one but most
 * filesystems impose a 255-byte max on a single name).  Exceeded
 * components return -E2BIG rather than silently truncating.
 */
#define PS_DISCOVERY_COMPONENT_MAX 255

/*
 * Walk from the MDS root to `path`, returning the FH of the final
 * component.  Sends a single compound:
 *
 *   SEQUENCE + PUTROOTFH + LOOKUP c1 + LOOKUP c2 + ... + GETFH
 *
 * `path` must be absolute and NUL-terminated.  "/" is treated as the
 * root (equivalent to ps_discovery_fetch_root_fh()).  Leading,
 * trailing, and doubled slashes are tolerated; empty components are
 * skipped.
 *
 *   0        success
 *   -EINVAL  ms / path / fh_buf / fh_len_out is NULL, or path is not
 *            absolute
 *   -E2BIG   path has more than PS_DISCOVERY_MAX_DEPTH components, or
 *            a single component exceeds PS_DISCOVERY_COMPONENT_MAX
 *   -ENOSPC  buf_size is smaller than the returned FH
 *   -errno   RPC / compound failure, or any per-op status != NFS4_OK
 *            (typically -ENOENT when an intermediate component is
 *            missing on the upstream)
 */
int ps_discovery_walk_path(struct mds_session *ms, const char *path,
			   uint8_t *fh_buf, uint32_t buf_size,
			   uint32_t *fh_len_out);

struct ps_listener_state; /* forward: from ps_state.h */

/*
 * Run one discovery pass for a listener.  Chains:
 *
 *   MOUNT3 EXPORT proc against pls_upstream  -> list of paths
 *   per-path LOOKUP walk via pls_session     -> upstream FH
 *   ps_state_add_export()                    -> cache entry
 *
 * Individual per-path failures (walk returning -ENOENT, add
 * hitting PS_MAX_EXPORTS_PER_LISTENER, etc.) are logged and
 * skipped so a single broken export does not drop every other
 * path on the same upstream.  MOUNT3 enumeration failure
 * propagates -- the coordinator has nothing to do if the
 * upstream's mountd is unreachable.
 *
 * Preconditions (caller must satisfy):
 *   pls->pls_upstream is set (non-empty)
 *   pls->pls_session is non-NULL (reffsd opened it at startup)
 *
 * Returns:
 *   0        success (>= 0 exports cached; individual failures
 *            are only logged)
 *   -EINVAL  pls is NULL, or pls_upstream is empty
 *   -ENOTCONN  pls_session is NULL
 *   -errno   MOUNT3 RPC / call failure (nothing cached)
 *
 * Safe to call more than once for on-demand re-discovery; the
 * add-export path is update-in-place on duplicate paths.
 *
 * NOT_NOW_BROWN_COW: concurrent callers on the same listener.
 * ps_state_add_export() documents a single-writer discipline
 * (.claude/design/proxy-server.md).  Today the only caller is
 * reffsd startup (single-threaded), so the invariant holds.  Slice
 * 2e-iii-d will enable LOOKUP-triggered re-discovery from op-handler
 * worker threads; that slice MUST add a per-listener mutex held
 * across ps_discovery_run()'s body so two writers cannot race on
 * pls_exports[] / pls_nexports.  Tracked here so the gap is not
 * discovered at 2e-iii-d integration time.
 */
int ps_discovery_run(const struct ps_listener_state *pls);

#endif /* _REFFS_PS_DISCOVERY_H */
