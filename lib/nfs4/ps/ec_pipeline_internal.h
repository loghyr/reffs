/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef EC_PIPELINE_INTERNAL_H
#define EC_PIPELINE_INTERNAL_H

/*
 * Whitebox surface for ec_pipeline.c.  Tests include this header
 * to drive the per-mirror CHUNK dispatch (ec_chunk_write /
 * ec_chunk_read) directly without standing up a full LAYOUTGET +
 * codec stack.  The Phase 5 short-circuit dispatch hook lives at
 * the very top of those functions; the partial-2-mirrors test
 * needs to exercise the hook with synthetic per-mirror em_local
 * flags, so the test allocates a struct ec_context on the stack
 * and calls the helper directly.
 *
 * Same precedent as ps_write_buffer_internal.h: the consumer is
 * in the same library, the helper is one-call-deep, and stacking
 * up accessors would be more code than just exposing the struct.
 * Production callers outside ec_pipeline.c do NOT include this
 * header.
 */

#include <stdint.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"
#include "ps_state.h"

/*
 * Mirror of the static struct ec_context in ec_pipeline.c.  Kept
 * in sync by code review -- if a field is added to ec_pipeline.c's
 * private definition without updating this header, the test
 * compilation will not catch the divergence (the test includes
 * this header, ec_pipeline.c does not).  The reviewer must check
 * both.
 *
 * The compiler does NOT verify struct identity across TUs -- C
 * does not have nominal struct typing.  ec_pipeline.c's local
 * `struct ec_context` and this declaration are independent type
 * declarations that happen to share a name; the linker resolves
 * pointer-to-struct references by name + binary layout.  Field
 * order / alignment must match exactly.
 */
/* Mirror of EC_CTX_MAX_MIRRORS in ec_pipeline.c -- field-layout
 * lock-step.  See the comment block above and the chunk-collision-
 * validation design doc for the cwa_guard CAS plumbing rationale. */
#define EC_CTX_MAX_MIRRORS 16

struct ec_context {
	struct mds_session *ctx_ms;
	struct mds_file ctx_file;
	struct ec_layout ctx_layout;
	struct ec_device *ctx_devs;
	struct ds_conn *ctx_conns; /* NFSv3 DS connections (v1) */
	/*
	 * NFSv4.2 DS sessions, one pointer per mirror.  Pointer (not
	 * inline struct) so that combined-mode dedup -- multiple
	 * mirrors resolving to the same DS host:port -- can SHARE a
	 * single mds_session via pointer copy instead of struct copy.
	 * The earlier struct-copy form gave each "mirror" its own
	 * slot_seqid counter on the same NFSv4.1 sessionid, which
	 * aliased into the server's DRC and silently no-op'd writes /
	 * reads for all but the first mirror.  Each unique connection
	 * is heap-allocated; duplicates share the pointer and are
	 * destroyed exactly once by ec_disconnect_all.
	 */
	struct mds_session **ctx_ds_sess;
	struct ec_codec *ctx_codec;
	uint32_t ctx_k;
	uint32_t ctx_m;
	struct ps_listener_state *ctx_pls;
	/*
	 * Track 1b Option C cwa_guard CAS plumbing -- see
	 * ec_pipeline.c's matching declaration.  Tests don't drive
	 * this surface today (the dispatch test passes NULL guards),
	 * but the layout must match exactly because pointer-to-struct
	 * references across this whitebox/internal boundary are
	 * resolved by binary layout, not C nominal typing.
	 */
	chunk_owner4 ctx_read_owners[EC_CTX_MAX_MIRRORS];
	bool ctx_read_owners_valid;
};

/*
 * Per-mirror CHUNK_WRITE.  Phase 5 short-circuit dispatch lives
 * at the top of this function (em_local && ctx_pls &&
 * pls_sc_write_fn).  Returns 0 on success, -errno on failure.
 * The dispatch path through pls_sc_write_fn returns whatever the
 * installed stub returns; the RPC path returns the ds_chunk_write
 * status (which falls through to mds_compound_send_with_auth).
 *
 * Public via this internal header for
 * ec_pipeline_dispatch_test.c.  ec_pipeline.c retains its single
 * definition; the test TU links against it through the
 * libreffs_nfs4_ps.la static archive.
 */
int ec_chunk_write(struct ec_context *ctx, int mirror_idx,
		   uint64_t block_offset, uint32_t chunk_sz, const uint8_t *src,
		   uint32_t wsz, uint32_t owner_id, const chunk_guard4 *guard);

/*
 * Per-mirror CHUNK_READ.  Mirrors ec_chunk_write -- same
 * dispatch hook at the top, same fall-through to ds_chunk_read
 * on the RPC path.  out_owners (optional, NULL = skip) lets
 * the caller capture each returned block's chunk_owner4 for
 * later cwa_guard CAS-checks on the matching write path.
 */
int ec_chunk_read(struct ec_context *ctx, int mirror_idx, uint64_t block_offset,
		  uint32_t nblk, uint8_t *shard, uint32_t rd_chunk_sz,
		  uint32_t *nread, chunk_owner4 *out_owners);

#endif /* EC_PIPELINE_INTERNAL_H */
