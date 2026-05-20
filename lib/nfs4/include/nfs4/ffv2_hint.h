/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * FFv2 codec-negotiation hint helpers.  Shared between the client
 * (mds_layout_get packs the hint into loga_layouthint) and the
 * server (nfs4_op_layoutget picks the coding type out of the hint
 * the client sent).  Single source of truth so both ends agree on
 * the wire format and the fallback rules.
 *
 * Wire surface: RFC 8881 S18.43 (loga_layouthint) +
 *   draft-haynes-nfsv4-flexfiles-v2 sec-codec-negotiation.
 *
 * Header lives in lib/nfs4/include/ alongside the rest of the
 * NFSv4-server private API; consumers include with <nfs4/ffv2_hint.h>.
 */

#ifndef _REFFS_NFS4_FFV2_HINT_H
#define _REFFS_NFS4_FFV2_HINT_H

#include <stdint.h>

#include "nfsv42_xdr.h"

/*
 * ffv2_hint_pack -- encode the caller's fflh_supported_types<>
 * preference list into a malloc'd opaque buffer suitable for
 * loga_layouthint.loh_body.
 *
 * On success: returns 0, stores the buffer + length in
 *   *out_body / *out_len; the caller owns and must free() the
 *   buffer.
 * On failure: returns -errno.  Callers that have nothing to hint
 *   should not invoke this helper (leave loh_body empty).
 *
 * The encoded body is `ffv2_layouthint4` with
 * fflh_supported_types<> populated from the caller's list and
 * fflh_preferred_protection left as a zero ffv2_data_protection4.
 */
int ffv2_hint_pack(const uint32_t *supported_types, uint32_t n_supported_types,
		   char **out_body, uint32_t *out_len);

/*
 * ffv2_hint_pick -- choose an FFv2 coding type for a fresh
 * LAYOUTGET, honouring the client's loga_layouthint when present.
 *
 * Inputs are taken directly from the LAYOUTGET4args layouthint4
 * field plus the requested layout type.  Returns an
 * ffv2_coding_type4 wire value.
 *
 * Failure modes all degrade to FFV2_ENCODING_PASSTHROUGH so
 * clients that have not learned to pack the hint, or send a
 * malformed / forward-compat one, keep getting the FFv1-compatible
 * default the MDS has always emitted:
 *
 *   - layout_type != LAYOUT4_FLEX_FILES_V2
 *   - hint == NULL or hint->loh_type != LAYOUT4_FLEX_FILES_V2
 *   - loh_body absent / empty
 *   - XDR decode failure on the body
 *   - empty fflh_supported_types<>
 *   - first supported entry names an unrecognised enum value
 */
uint32_t ffv2_hint_pick(const layouthint4 *hint, layouttype4 layout_type);

#endif /* _REFFS_NFS4_FFV2_HINT_H */
