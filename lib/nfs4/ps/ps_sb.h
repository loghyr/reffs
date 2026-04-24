/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_SB_H
#define _REFFS_PS_SB_H

#include <stdint.h>

#include "ps_state.h" /* for PS_MAX_FH_SIZE */

/*
 * Per-proxy-SB upstream binding.
 *
 * Slice 2e-iii-c introduces the data shape.  Slice 2e-iii-d will add
 * the `void *sb_proxy_binding` field to struct super_block and attach
 * one of these to every proxy SB created by reffsd startup + on-demand
 * re-discovery.  Op handlers then determine "is this a proxy SB?" with
 * a single pointer load and, if non-NULL, forward upstream using the
 * listener and FH the binding carries.
 *
 * Fields:
 *
 *   psb_listener_id
 *     Matches ps_listener_state.pls_listener_id.  Used to look up the
 *     owning listener's MDS session + cached root FH.
 *
 *   psb_mds_fh / psb_mds_fh_len
 *     The upstream's FH for this export's root -- the anchor for
 *     every future LOOKUP below this SB.  Discovered by
 *     ps_discovery_walk_path() at startup and cached here so op
 *     handlers do not re-walk on every compound.
 *
 * Ownership: the binding is owned by the super_block that references
 * it.  ps_sb_binding_free() is the single release point; slice 2e-iii-d
 * will call it from super_block_release().  Non-proxy SBs carry a
 * NULL sb_proxy_binding and never invoke the free path.
 *
 * See .claude/design/proxy-server.md phase 2 "Proxy SB".
 */
struct ps_sb_binding {
	uint32_t psb_listener_id;
	uint32_t psb_mds_fh_len;
	uint8_t psb_mds_fh[PS_MAX_FH_SIZE];
};

/*
 * Allocate a new binding with the given upstream coordinates.
 * Returns NULL on OOM or if any argument is invalid:
 *
 *   listener_id == 0     (reserved for native)
 *   fh == NULL
 *   fh_len == 0          (an MDS anchor FH is required)
 *   fh_len > PS_MAX_FH_SIZE
 *
 * The caller owns the returned binding and MUST eventually release
 * it via ps_sb_binding_free().
 */
struct ps_sb_binding *ps_sb_binding_alloc(uint32_t listener_id,
					  const uint8_t *fh, uint32_t fh_len);

/*
 * Release a binding previously returned by ps_sb_binding_alloc().
 * NULL-safe, matching the project convention for other free / put
 * helpers (inode_put, super_block_put, ps_mount_free_exports).
 */
void ps_sb_binding_free(struct ps_sb_binding *b);

#endif /* _REFFS_PS_SB_H */
