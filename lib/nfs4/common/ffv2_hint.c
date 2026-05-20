/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * FFv2 codec-negotiation hint helpers.  See the header for the
 * full contract; this TU is the shared implementation that both
 * the client (mds_layout.c) and the server (layout.c) link
 * against.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>
#include <rpc/xdr.h>

#include "nfsv42_xdr.h"

#include "nfs4/ffv2_hint.h"

int ffv2_hint_pack(const uint32_t *supported_types, uint32_t n_supported_types,
		   char **out_body, uint32_t *out_len)
{
	const size_t bufsz = 256;

	if (!out_body || !out_len)
		return -EINVAL;
	if (!supported_types || n_supported_types == 0)
		return -EINVAL;

	char *body = calloc(1, bufsz);

	if (!body)
		return -ENOMEM;

	ffv2_layouthint4 lh;

	memset(&lh, 0, sizeof(lh));
	lh.fflh_supported_types.fflh_supported_types_len = n_supported_types;
	lh.fflh_supported_types.fflh_supported_types_val =
		(ffv2_coding_type4 *)supported_types;

	XDR xdrs;

	xdrmem_create(&xdrs, body, bufsz, XDR_ENCODE);
	if (!xdr_ffv2_layouthint4(&xdrs, &lh)) {
		xdr_destroy(&xdrs);
		free(body);
		return -EOVERFLOW;
	}
	uint32_t used = xdr_getpos(&xdrs);
	xdr_destroy(&xdrs);

	*out_body = body;
	*out_len = used;
	return 0;
}

uint32_t ffv2_hint_pick(const layouthint4 *hint, layouttype4 layout_type)
{
	if (layout_type != LAYOUT4_FLEX_FILES_V2)
		return FFV2_ENCODING_PASSTHROUGH;
	if (!hint || hint->loh_type != LAYOUT4_FLEX_FILES_V2)
		return FFV2_ENCODING_PASSTHROUGH;
	if (hint->loh_body.loh_body_len == 0 || !hint->loh_body.loh_body_val)
		return FFV2_ENCODING_PASSTHROUGH;

	ffv2_layouthint4 lh;
	XDR xdrs;

	memset(&lh, 0, sizeof(lh));
	xdrmem_create(&xdrs, hint->loh_body.loh_body_val,
		      hint->loh_body.loh_body_len, XDR_DECODE);
	bool_t ok = xdr_ffv2_layouthint4(&xdrs, &lh);
	xdr_destroy(&xdrs);

	uint32_t picked = FFV2_ENCODING_PASSTHROUGH;

	if (ok && lh.fflh_supported_types.fflh_supported_types_len > 0) {
		uint32_t first =
			lh.fflh_supported_types.fflh_supported_types_val[0];
		switch (first) {
		case FFV2_ENCODING_PASSTHROUGH:
		case FFV2_ENCODING_MOJETTE_SYSTEMATIC:
		case FFV2_ENCODING_MOJETTE_NON_SYSTEMATIC:
		case FFV2_ENCODING_RS_VANDERMONDE:
		case FFV2_ENCODING_MIRRORED:
			picked = first;
			break;
		default:
			/* unknown enum value, leave PASSTHROUGH */
			break;
		}
	}

	xdr_free((xdrproc_t)xdr_ffv2_layouthint4, &lh);
	return picked;
}
