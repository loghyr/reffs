/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Intel ISA-L Reed-Solomon (Vandermonde) erasure encoding wrapper,
 * over the vendored ISA-L erasure_code/ portable-C subset in
 * lib/ec/isa-l-erasure/ (BSD-3-Clause -- see NOTICE.md).
 *
 * Wire-side: FFV2_ENCODING_ISA_L_RS (0x9, proposed).  Field GF(2^8),
 * primitive polynomial 0x1d (same field as RS_VANDERMONDE (0x4),
 * SNAPRAID_CAUCHY (0x6), LINUX_MD_RAID (0x8)).  Matrix construction
 * is ISA-L's gf_gen_rs_matrix: generator 2^(i*j), which does NOT
 * agree with reffs's own rs.c (2^(2^i * j)) or SnapRAID's Cauchy
 * point-choice; needs its own wire enum despite the shared field.
 *
 * The wrapper is portable-C uniformly for now.  Slice 7.3 will
 * re-vendor the SIMD tree behind an AC_CHECK_PROG(NASM) gate.
 * The public API entry points (ec_encode_data, ec_init_tables,
 * gf_gen_rs_matrix, gf_invert_matrix) come from ISA-L's
 * ec_base_aliases.c which routes them to the _base
 * implementations.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/ec.h"

#include "erasure_code.h"

/*
 * Use reffs's own gf_mul() from lib/ec/gf.c rather than ISA-L's,
 * to avoid a link-time duplicate-symbol collision (both libraries
 * export gf_mul / gf_inv).  Both implement GF(2^8) with primitive
 * polynomial 0x11d and generator=2, so results agree bit-for-bit.
 * ISA-L's ec_encode_data uses its own internal g_tbls (a 32-byte
 * SIMD-shaped table per parity row) and never calls gf_mul via
 * the public alias, so this substitution only affects our
 * matrix-build path in the decode step.
 */
#include "gf.h"

/*
 * ISA-L uses GF(2^8) so at most 254 shards total (255 is a
 * reserved value in the field construction).  k and m may each
 * be up to 254 - the-other.
 */
#define ISA_L_MAX_TOTAL 254

struct isa_l_private {
	/* Row-major (k+m) x k encode matrix, precomputed in create(). */
	unsigned char *encode_matrix;
	/* k*m*32 g_tbls the parity-row half of encode_matrix expands to,
	 * used by ec_encode_data() for the forward path. */
	unsigned char *g_tbls;
	/* Scratch: work buffers for decode.  Sized in create() so
	 * decode paths do not malloc on the hot path. */
	unsigned char *decode_matrix; /* m x k */
	unsigned char *invert_matrix; /* k x k */
	unsigned char *sub_matrix; /* k x k -- k surviving rows */
	unsigned char *decode_tbls; /* k*m*32 */
	unsigned char **recover_srcs; /* [k] */
	unsigned char **recover_outputs; /* [m] */
};

static int isa_l_encode(struct ec_encoding *encoding, uint8_t **data,
			uint8_t **parity, size_t shard_len)
{
	struct isa_l_private *sp = encoding->ec_private;
	int k = encoding->ec_k;
	int m = encoding->ec_m;

	if (shard_len == 0)
		return 0;
	if (parity == NULL || data == NULL)
		return -EINVAL;

	ec_encode_data((int)shard_len, k, m, sp->g_tbls, data, parity);
	return 0;
}

static int isa_l_decode(struct ec_encoding *encoding, uint8_t **shards,
			const bool *present, size_t shard_len)
{
	struct isa_l_private *sp = encoding->ec_private;
	int k = encoding->ec_k;
	int m = encoding->ec_m;
	int n = k + m;

	if (shard_len == 0)
		return 0;

	int nmiss = 0;
	int nmiss_data = 0;
	int miss_idx[ISA_L_MAX_TOTAL];

	for (int i = 0; i < n; i++) {
		if (!present[i]) {
			if (nmiss >= m)
				return -EIO;
			miss_idx[nmiss] = i;
			if (i < k)
				nmiss_data++;
			nmiss++;
		}
	}
	if (nmiss == 0)
		return 0;

	/*
	 * Build sub_matrix as k surviving rows from the encode
	 * matrix (pick the first k rows whose present[]==true).
	 * Invert to get the coefficients that reconstruct the
	 * original k data shards.
	 */
	int decode_index[ISA_L_MAX_TOTAL];
	int j = 0;

	for (int r = 0; r < n && j < k; r++) {
		if (present[r]) {
			for (int c = 0; c < k; c++)
				sp->sub_matrix[k * j + c] =
					sp->encode_matrix[k * r + c];
			decode_index[j] = r;
			j++;
		}
	}
	if (j < k)
		return -EIO;

	if (gf_invert_matrix(sp->sub_matrix, sp->invert_matrix, k) < 0)
		return -EIO;

	/*
	 * Two-phase reconstruction: first the missing DATA rows come
	 * from invert_matrix rows keyed by miss_idx[.] < k, then the
	 * missing PARITY rows come from (encode_matrix row for the
	 * missing parity) * invert_matrix.
	 */
	int drow = 0;

	for (int i = 0; i < nmiss_data; i++) {
		for (int c = 0; c < k; c++)
			sp->decode_matrix[k * drow + c] =
				sp->invert_matrix[k * miss_idx[i] + c];
		drow++;
	}
	for (int p = nmiss_data; p < nmiss; p++) {
		for (int i = 0; i < k; i++) {
			unsigned char s = 0;

			for (int c = 0; c < k; c++)
				s ^= gf_mul(
					sp->invert_matrix[c * k + i],
					sp->encode_matrix[k * miss_idx[p] + c]);
			sp->decode_matrix[k * drow + i] = s;
		}
		drow++;
	}

	ec_init_tables(k, nmiss, sp->decode_matrix, sp->decode_tbls);

	/* Wire the surviving input array + missing-output array. */
	for (int i = 0; i < k; i++)
		sp->recover_srcs[i] = shards[decode_index[i]];
	for (int i = 0; i < nmiss; i++)
		sp->recover_outputs[i] = shards[miss_idx[i]];

	ec_encode_data((int)shard_len, k, nmiss, sp->decode_tbls,
		       sp->recover_srcs, sp->recover_outputs);
	return 0;
}

static void isa_l_destroy(struct ec_encoding *encoding)
{
	struct isa_l_private *sp = encoding->ec_private;

	if (sp) {
		free(sp->encode_matrix);
		free(sp->g_tbls);
		free(sp->decode_matrix);
		free(sp->invert_matrix);
		free(sp->sub_matrix);
		free(sp->decode_tbls);
		free(sp->recover_srcs);
		free(sp->recover_outputs);
		free(sp);
	}
	free(encoding);
}

struct ec_encoding *ec_isa_l_create(int k, int m)
{
	if (k < 1 || m < 1 || k + m > ISA_L_MAX_TOTAL)
		return NULL;

	/*
	 * We borrow reffs's own gf_mul (from lib/ec/gf.c) for the
	 * decode-matrix build to avoid the link-time collision with
	 * ISA-L's copy.  That table is idempotent-init via
	 * pthread_once, so calling it here every create is safe and
	 * cheap.
	 */
	gf_init();

	struct ec_encoding *encoding = calloc(1, sizeof(*encoding));
	struct isa_l_private *sp = calloc(1, sizeof(*sp));

	if (!encoding || !sp)
		goto oom;

	int n = k + m;

	sp->encode_matrix = calloc((size_t)n * (size_t)k, 1);
	sp->g_tbls = calloc((size_t)k * (size_t)m * 32, 1);
	sp->decode_matrix = calloc((size_t)m * (size_t)k, 1);
	sp->invert_matrix = calloc((size_t)k * (size_t)k, 1);
	sp->sub_matrix = calloc((size_t)k * (size_t)k, 1);
	sp->decode_tbls = calloc((size_t)k * (size_t)m * 32, 1);
	sp->recover_srcs = calloc((size_t)k, sizeof(*sp->recover_srcs));
	sp->recover_outputs = calloc((size_t)m, sizeof(*sp->recover_outputs));
	if (!sp->encode_matrix || !sp->g_tbls || !sp->decode_matrix ||
	    !sp->invert_matrix || !sp->sub_matrix || !sp->decode_tbls ||
	    !sp->recover_srcs || !sp->recover_outputs)
		goto oom;

	gf_gen_rs_matrix(sp->encode_matrix, n, k);
	/* Expand parity rows (encode_matrix rows k..n-1) into g_tbls
	 * for the forward encode path. */
	ec_init_tables(k, m, &sp->encode_matrix[k * k], sp->g_tbls);

	encoding->ec_name = "isa-l-rs";
	encoding->ec_k = k;
	encoding->ec_m = m;
	encoding->ec_encode = isa_l_encode;
	encoding->ec_decode = isa_l_decode;
	encoding->ec_shard_size = NULL;
	encoding->ec_destroy = isa_l_destroy;
	encoding->ec_private = sp;
	return encoding;

oom:
	if (sp) {
		free(sp->encode_matrix);
		free(sp->g_tbls);
		free(sp->decode_matrix);
		free(sp->invert_matrix);
		free(sp->sub_matrix);
		free(sp->decode_tbls);
		free(sp->recover_srcs);
		free(sp->recover_outputs);
		free(sp);
	}
	free(encoding);
	return NULL;
}
