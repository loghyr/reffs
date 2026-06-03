/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * reffs_coding_spec -- per-export codec descriptor.
 *
 * Carries the codec selection + (k, m) geometry that the MDS uses
 * to drive LAYOUTGET-time layout-segment creation and the
 * `ffm_coding_type` choice the FFv2 layout body advertises to
 * clients.
 *
 * The numeric values of `enum reffs_codec_type` are aligned with
 * the FFv2 wire-protocol `ffv2_coding_type4` values (see
 * `draft-haynes-nfsv4-flexfiles-v2` and `lib/xdr/nfsv42_xdr.x`
 * around `FFV2_ENCODING_PASSTHROUGH = 0x1`).  The alignment is
 * verified by a _Static_assert at the translation site in
 * `lib/nfs4/server/layout.c` so the layering stays one-way:
 * `lib/include/reffs/` does not pull in XDR-generated headers,
 * but the NFSv4 server code that DOES include both ensures the
 * values remain in lock-step.
 *
 * See `.claude/design/per-export-default-coding.md` for the full
 * design, including the TOML format ("rs:K+M" etc.), persistence
 * in `sb_registry_entry`, and the runway-target invariant the
 * LAYOUTGET dispatch must honour.
 */

#ifndef _REFFS_CODING_SPEC_H
#define _REFFS_CODING_SPEC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Codec type identifier.  Values intentionally match
 * FFV2_ENCODING_* on the wire so the translation from
 * sb-level descriptor to ffm_coding_type is the identity
 * cast.  Keep in sync with lib/xdr/nfsv42_xdr.x.
 */
enum reffs_codec_type {
	REFFS_CODEC_PASSTHROUGH = 0x1,
	REFFS_CODEC_MOJETTE_SYSTEMATIC = 0x2,
	REFFS_CODEC_MOJETTE_NON_SYSTEMATIC = 0x3,
	REFFS_CODEC_RS_VANDERMONDE = 0x4,
	REFFS_CODEC_MIRRORED = 0x5,
};

/*
 * Per-export codec descriptor.
 *
 * cs_k -- number of data shards.  Must be in [1, LAYOUT_SEG_MAX_FILES].
 * cs_m -- number of parity shards.  Must be in [0, LAYOUT_SEG_MAX_FILES - cs_k].
 *         cs_m == 0 implies PASSTHROUGH (no parity, no encoding).
 *
 * cs_codec_type -- one of REFFS_CODEC_*.  For PASSTHROUGH cs_m
 *                  must be zero; for any other codec cs_m must be
 *                  positive.  The TOML parser and the
 *                  SB_SET_DEFAULT_CODING probe handler both
 *                  enforce these invariants.
 */
struct reffs_coding_spec {
	enum reffs_codec_type cs_codec_type;
	uint16_t cs_k;
	uint16_t cs_m;
};

/*
 * A zero-initialised reffs_coding_spec is interpreted by the
 * LAYOUTGET dispatch as "no explicit default" and falls back to
 * PASSTHROUGH with k = ss_layout_width (the server-wide knob,
 * NOT per-sb -- see per-export-default-coding.md "Backward
 * compatibility").  Registry entries that pre-date this slice
 * load with sre_default_coding all-zero and hit this path.
 */
static inline bool
reffs_coding_spec_is_unset(const struct reffs_coding_spec *cs)
{
	return cs->cs_codec_type == 0 && cs->cs_k == 0 && cs->cs_m == 0;
}

#endif /* _REFFS_CODING_SPEC_H */
