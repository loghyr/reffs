/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef DS_RENEWAL_INTERNAL_H
#define DS_RENEWAL_INTERNAL_H

#include <stdint.h>

struct dstore;

/*
 * Internal surface of ds_renewal.c -- exposed for whitebox unit
 * tests of the per-dstore tick decision tree.  Mirrors the
 * lib/nfs4/ps/ps_renewal_internal.h pattern: in production, the
 * renewal thread (ds_renewal.c worker) calls renewal_tick_one via
 * a snapshot taken inside ds_renewal_one_tick; the unit tests call
 * it directly with a synthesised struct dstore so the
 * decision-tree branches (local skip, in-backoff skip, dispatch by
 * protocol, no-session reconnect attempt, NFSv3 NULL keep-alive)
 * can be exercised without a live DS.
 *
 * The counters here track which branch fired and let tests assert
 * on the path taken.  v3_renewed / v3_failed split out the NFSv3
 * NULL path from the NFSv4 SEQUENCE path because the two share
 * ctx->renewed / ctx->failed only at the dispatcher level; the
 * NFSv3 path has its own success/failure ledger.
 */
struct renewal_tick_ctx {
	uint32_t renewed;
	uint32_t failed;
	uint32_t skipped_no_session;
	uint32_t skipped_local;
	uint32_t reconnect_attempted;
	uint32_t reconnect_succeeded;
	uint32_t reconnect_skipped_backoff;
	uint32_t v3_renewed;
	uint32_t v3_failed;
};

/*
 * Named ds_renewal_tick_one (not renewal_tick_one) because
 * lib/nfs4/ps/ps_renewal.c already exports a symbol called
 * renewal_tick_one with a DIFFERENT signature
 * (const struct ps_listener_state *, void *).  When the dstore
 * test binary links both libreffs_dstore.la and libreffs_nfs4_ps.la,
 * the Linux ELF dynamic loader resolves the call against the PS
 * symbol -- a wrong-type call that SEGVs as soon as the PS code
 * dereferences the dstore pointer as a struct ps_listener_state.
 * Mach-O on macOS uses different resolution and silently picks
 * the dstore symbol, so the bug was invisible on local builds and
 * surfaced only on garbo's ASan run.  Prefixed name is the cheap
 * fix; no two-namespace cleanup needed across the keep-alive code.
 */
void ds_renewal_tick_one(struct dstore *ds, struct renewal_tick_ctx *ctx);

#endif /* DS_RENEWAL_INTERNAL_H */
