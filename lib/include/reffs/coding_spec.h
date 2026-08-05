/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * reffs_coding_spec -- per-export encoding descriptor.
 *
 * Carries the encoding selection + (k, m) geometry that the MDS uses
 * to drive LAYOUTGET-time layout-segment creation and the
 * `ffm_coding_type` choice the FFv2 layout body advertises to
 * clients.
 *
 * The numeric values of `enum reffs_encoding_type` are aligned with
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

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdbool.h>
#include <stdint.h>

/*
 * Encoding type identifier.  Values intentionally match
 * FFV2_ENCODING_* on the wire so the translation from
 * sb-level descriptor to ffm_coding_type is the identity
 * cast.  Keep in sync with lib/xdr/nfsv42_xdr.x.
 *
 * SNAPRAID_CAUCHY and ISA_L_RS were removed from the FFv2
 * standards-track registry on 2026-07-30 due to StreamScale
 * US 8,683,296 patent exposure on their SIMD split-table
 * Galois-field multiply.  Reffs retains them for bench and
 * internal comparison work, gated by
 * REFFS_ENABLE_PRIVATE_ENCODINGS at configure time; when the
 * flag is set they occupy private-range wire codepoints
 * (0x8001, 0x8002) that will never collide with an
 * IETF-registered value.
 */
enum reffs_encoding_type {
	REFFS_ENCODING_PASSTHROUGH = 0x1,
	REFFS_ENCODING_MOJETTE_SYSTEMATIC = 0x2,
	REFFS_ENCODING_MOJETTE_NON_SYSTEMATIC = 0x3,
	REFFS_ENCODING_RS_VANDERMONDE = 0x4,
	REFFS_ENCODING_REPLICATED = 0x5,
	REFFS_ENCODING_XOR_PARITY = 0x6,
	REFFS_ENCODING_LINUX_MD_RAID = 0x7,
#ifdef REFFS_ENABLE_PRIVATE_ENCODINGS
	REFFS_ENCODING_HAMMERSPACE_SNAPRAID_CAUCHY = 0x8001,
	REFFS_ENCODING_HAMMERSPACE_ISA_L_RS = 0x8002,
#endif
};

/*
 * Per-export encoding descriptor.
 *
 * cs_k -- number of data shards.  Must be in [1, LAYOUT_SEG_MAX_FILES].
 * cs_m -- number of parity shards.  Must be in [0, LAYOUT_SEG_MAX_FILES - cs_k].
 *         cs_m == 0 implies PASSTHROUGH (no parity, no encoding).
 *
 * cs_encoding_type -- one of REFFS_ENCODING_*.  For PASSTHROUGH cs_m
 *                  must be zero; for any other encoding cs_m must be
 *                  positive.  The TOML parser and the
 *                  SB_SET_DEFAULT_CODING probe handler both
 *                  enforce these invariants.
 */
struct reffs_coding_spec {
	enum reffs_encoding_type cs_encoding_type;
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
	return cs->cs_encoding_type == 0 && cs->cs_k == 0 && cs->cs_m == 0;
}

#endif /* _REFFS_CODING_SPEC_H */
