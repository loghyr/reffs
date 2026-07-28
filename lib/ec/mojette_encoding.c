/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Mojette erasure encoding -- systematic and non-systematic wrappers.
 *
 * Wraps the core Mojette transform (mojette.c) into the ec_encoding
 * interface for use by the EC demo client.
 *
 * Systematic: data shards are grid rows (verbatim), parity shards are
 * projections.  On decode, subtract known rows from projections and
 * inverse-solve for missing rows.
 *
 * Non-systematic: all k+m shards are projections.  Encode overwrites
 * data[] with the first k projections.  Any k projections suffice
 * for decode via full inverse Mojette.
 */

#include "reffs/ec.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "mojette.h"

struct moj_encoding_private {
	struct moj_direction *mcp_dirs; /* k+m directions */
	int mcp_n; /* k + m */
	bool mcp_systematic;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Derive grid column count P from shard_len and element size.
 * shard_len is the byte size of one data shard (= one grid row).
 */
static int grid_P(size_t shard_len)
{
	return (int)(shard_len / sizeof(uint64_t));
}

/* ------------------------------------------------------------------ */
/* Systematic encode / decode                                          */
/* ------------------------------------------------------------------ */

static int mojette_sys_encode(struct ec_encoding *encoding, uint8_t **data,
			      uint8_t **parity, size_t shard_len)
{
	struct moj_encoding_private *mcp = encoding->ec_private;
	int k = encoding->ec_k;
	int m = encoding->ec_m;
	int P = grid_P(shard_len);

	/*
	 * data[0..k-1] are grid rows.  Pass them directly as row
	 * pointers -- moj_forward_rows XORs each row into projection
	 * bins without materializing a contiguous grid.
	 */
	const uint64_t *rows[k];

	for (int i = 0; i < k; i++)
		rows[i] = (const uint64_t *)data[i];

	/*
	 * We only need the last m projections (the parity directions).
	 * Wrap parity[i] buffers directly -- ec_shard_size guarantees
	 * they are nbins*8 bytes, so moj_forward_impl's memset+XOR
	 * writes land exactly where the caller expects the encoded
	 * shard.  No intermediate allocation, no post-encode memcpy.
	 */
	struct moj_projection projs[m];
	struct moj_projection *proj_ptrs[m];

	for (int i = 0; i < m; i++) {
		int dir_idx = k + i;
		int nbins = moj_projection_size(mcp->mcp_dirs[dir_idx].md_p,
						mcp->mcp_dirs[dir_idx].md_q, P,
						k);

		projs[i].mp_bins = (uint64_t *)parity[i];
		projs[i].mp_nbins = nbins;
		proj_ptrs[i] = &projs[i];
	}

	moj_forward_rows(rows, P, k, mcp->mcp_dirs + k, m, proj_ptrs);
	return 0;
}

static int mojette_sys_decode(struct ec_encoding *encoding, uint8_t **shards,
			      const bool *present, size_t shard_len)
{
	struct moj_encoding_private *mcp = encoding->ec_private;
	int k = encoding->ec_k;
	int m = encoding->ec_m;
	int n = k + m;
	int P = grid_P(shard_len);

	/* Count losses. */
	int missing_data = 0;
	int total_missing = 0;

	for (int i = 0; i < n; i++) {
		if (!present[i])
			total_missing++;
		if (i < k && !present[i])
			missing_data++;
	}

	if (total_missing > m)
		return -EIO;
	if (total_missing == 0)
		return 0;
	if (missing_data == 0) {
		/* Only parity missing -- re-encode to regenerate. */
		uint8_t **parity = shards + k;

		return mojette_sys_encode(encoding, shards, parity, shard_len);
	}

	/*
	 * Some data rows are missing.  Build the full P*k grid with
	 * known rows pre-filled, missing rows zeroed, and call the
	 * sparse-failures inverse.  The DGCI sweep handles known
	 * rows naturally during the diagonal walk; corner-peeling
	 * pre-subtracts known contributions internally.
	 */
	int n_avail_proj = 0;

	for (int i = k; i < n; i++) {
		if (present[i])
			n_avail_proj++;
	}

	if (n_avail_proj < missing_data)
		return -EIO;

	/* Use exactly missing_data parity projections (n == n_missing). */
	int n_inv = missing_data;
	/* VLA declared before any goto target -- C forbids goto over VLA. */
	uint64_t *rows[k];
	struct moj_direction *sub_dirs =
		calloc((size_t)n_inv, sizeof(*sub_dirs));
	struct moj_projection **sub_projs =
		calloc((size_t)n_inv, sizeof(*sub_projs));
	int *missing_rows = calloc((size_t)missing_data, sizeof(int));

	if (!sub_dirs || !sub_projs || !missing_rows) {
		free(missing_rows);
		free(sub_projs);
		free(sub_dirs);
		return -ENOMEM;
	}

	int pidx = 0;
	int ret = -ENOMEM;

	for (int i = 0; i < m && pidx < n_inv; i++) {
		if (!present[k + i])
			continue;

		int dir_idx = k + i;

		sub_dirs[pidx] = mcp->mcp_dirs[dir_idx];

		int nbins = moj_projection_size(sub_dirs[pidx].md_p,
						sub_dirs[pidx].md_q, P, k);

		sub_projs[pidx] = moj_projection_create(nbins);
		if (!sub_projs[pidx])
			goto out;

		/*
		 * Load FULL parity bins (no pre-subtraction here).  Peel
		 * writes back into these during decode, so we cannot alias
		 * the shards buffer directly (see slice-R4-decode-scoping.md
		 * for the R.4.2 feasibility finding).
		 */
		memcpy(sub_projs[pidx]->mp_bins, shards[k + i],
		       (size_t)nbins * sizeof(uint64_t));
		pidx++;
	}

	/*
	 * R.4.1 fastpath: alias rows[] directly at the on-wire shard
	 * buffers.  Present rows carry the caller's data (read by the
	 * inverse init pass); missing rows receive recovered pixels
	 * in place (written by peel/gd).  No full_grid, no memcpy of
	 * present rows in, no memcpy of recovered rows out.
	 *
	 * Missing rows MUST be zero on entry -- moj_inverse_gd_sparse_rows
	 * XORs pre-existing bytes into the pixel accumulator during the
	 * DGCI walk (see mojette.c:1112 etc.).  peel_sparse does not read
	 * missing rows on init, so it tolerates any initial content, but
	 * the shared contract requires zero.  Callers of ec_pipeline reuse
	 * shard buffers across stripes, so `shards[missing[i]]` can carry
	 * leftover bytes from a previous chunk.  memset the missing shards
	 * to zero before calling the inverse.
	 */
	int midx = 0;

	for (int i = 0; i < k; i++) {
		rows[i] = (uint64_t *)shards[i];
		if (!present[i]) {
			memset(shards[i], 0, shard_len);
			missing_rows[midx++] = i;
		}
	}

	ret = moj_inverse_sparse_rows(rows, P, k, sub_dirs, n_inv, sub_projs,
				      missing_rows, missing_data);

out:
	free(missing_rows);
	for (int i = 0; i < pidx; i++)
		moj_projection_destroy(sub_projs[i]);
	free(sub_projs);
	free(sub_dirs);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Non-systematic encode / decode                                      */
/* ------------------------------------------------------------------ */

static int mojette_nonsys_encode(struct ec_encoding *encoding, uint8_t **data,
				 uint8_t **parity, size_t shard_len)
{
	struct moj_encoding_private *mcp = encoding->ec_private;
	int k = encoding->ec_k;
	int m = encoding->ec_m;
	int P = grid_P(shard_len);

	/* Row pointers into caller-supplied data buffers. */
	const uint64_t *rows[k];

	for (int i = 0; i < k; i++)
		rows[i] = (const uint64_t *)data[i];

	/*
	 * The m parity projections can wrap parity[i] directly (different
	 * buffer from rows[]).  The k "data" projections cannot: they
	 * would overwrite data[i] which aliases rows[i], and
	 * moj_forward_impl's memset would zero the row before it is XORed
	 * into projection 0.  Compute them into temp buffers then copy
	 * out at the end.  Splitting into two moj_forward_rows calls
	 * keeps both concerns clean.
	 */

	/* --- parity projections: direct wrap, no memcpy after --- */
	struct moj_projection parity_projs[m];
	struct moj_projection *parity_ptrs[m];

	for (int i = 0; i < m; i++) {
		int dir_idx = k + i;
		int nbins = moj_projection_size(mcp->mcp_dirs[dir_idx].md_p,
						mcp->mcp_dirs[dir_idx].md_q, P,
						k);

		parity_projs[i].mp_bins = (uint64_t *)parity[i];
		parity_projs[i].mp_nbins = nbins;
		parity_ptrs[i] = &parity_projs[i];
	}
	moj_forward_rows(rows, P, k, mcp->mcp_dirs + k, m, parity_ptrs);

	/*
	 * --- data projections: temp buffers to avoid rows[i]==data[i]
	 * aliasing; memcpy back afterwards.  data[] holding the input
	 * is only safe to overwrite once the transform has finished
	 * reading from it, so this copy is load-bearing.
	 */
	struct moj_projection data_projs[k];
	struct moj_projection *data_ptrs[k];
	uint64_t *data_bufs[k];

	for (int i = 0; i < k; i++) {
		int nbins = moj_projection_size(mcp->mcp_dirs[i].md_p,
						mcp->mcp_dirs[i].md_q, P, k);
		size_t proj_bytes = (size_t)nbins * sizeof(uint64_t);

		data_bufs[i] = malloc(proj_bytes);
		if (!data_bufs[i]) {
			for (int j = 0; j < i; j++)
				free(data_bufs[j]);
			return -ENOMEM;
		}
		data_projs[i].mp_bins = data_bufs[i];
		data_projs[i].mp_nbins = nbins;
		data_ptrs[i] = &data_projs[i];
	}
	moj_forward_rows(rows, P, k, mcp->mcp_dirs, k, data_ptrs);

	for (int i = 0; i < k; i++) {
		size_t proj_bytes =
			(size_t)data_projs[i].mp_nbins * sizeof(uint64_t);
		memcpy(data[i], data_bufs[i], proj_bytes);
		free(data_bufs[i]);
	}
	return 0;
}

static int mojette_nonsys_decode(struct ec_encoding *encoding, uint8_t **shards,
				 const bool *present, size_t shard_len)
{
	struct moj_encoding_private *mcp = encoding->ec_private;
	int k = encoding->ec_k;
	int m = encoding->ec_m;
	int n = k + m;
	int P = grid_P(shard_len);

	/* Count available shards. */
	int avail = 0;

	for (int i = 0; i < n; i++) {
		if (present[i])
			avail++;
	}

	if (avail < k)
		return -EIO;

	/* Select first k available projections. */
	struct moj_direction *sub_dirs = calloc((size_t)k, sizeof(*sub_dirs));
	struct moj_projection **sub_projs =
		calloc((size_t)k, sizeof(*sub_projs));

	if (!sub_dirs || !sub_projs) {
		free(sub_dirs);
		free(sub_projs);
		return -ENOMEM;
	}

	int sidx = 0;
	int ret = -ENOMEM;

	for (int i = 0; i < n && sidx < k; i++) {
		if (!present[i])
			continue;

		sub_dirs[sidx] = mcp->mcp_dirs[i];

		int nbins = moj_projection_size(sub_dirs[sidx].md_p,
						sub_dirs[sidx].md_q, P, k);

		sub_projs[sidx] = moj_projection_create(nbins);
		if (!sub_projs[sidx])
			goto out;

		memcpy(sub_projs[sidx]->mp_bins, shards[i],
		       (size_t)nbins * sizeof(uint64_t));
		sidx++;
	}

	/* Full inverse to recover original grid. */
	uint64_t *grid = calloc((size_t)P * k, sizeof(uint64_t));

	if (!grid)
		goto out;

	ret = moj_inverse(grid, P, k, sub_dirs, k, sub_projs);

	if (ret == 0) {
		/* Place original data rows into shards[0..k-1]. */
		for (int i = 0; i < k; i++)
			memcpy(shards[i], grid + (size_t)i * P, shard_len);
	}

	free(grid);

out:
	for (int i = 0; i < sidx; i++)
		moj_projection_destroy(sub_projs[i]);
	free(sub_projs);
	free(sub_dirs);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Shard size callback                                                 */
/* ------------------------------------------------------------------ */

static size_t mojette_shard_size(struct ec_encoding *encoding, int shard_idx,
				 size_t data_shard_len)
{
	struct moj_encoding_private *mcp = encoding->ec_private;
	int k = encoding->ec_k;
	int P = grid_P(data_shard_len);
	int dir_idx = shard_idx;

	if (mcp->mcp_systematic && shard_idx < k)
		return data_shard_len;

	return (size_t)moj_projection_size(mcp->mcp_dirs[dir_idx].md_p,
					   mcp->mcp_dirs[dir_idx].md_q, P, k) *
	       sizeof(uint64_t);
}

/* ------------------------------------------------------------------ */
/* Destroy                                                             */
/* ------------------------------------------------------------------ */

static void mojette_destroy(struct ec_encoding *encoding)
{
	struct moj_encoding_private *mcp = encoding->ec_private;

	if (mcp) {
		free(mcp->mcp_dirs);
		free(mcp);
	}
	free(encoding);
}

/* ------------------------------------------------------------------ */
/* Create                                                              */
/* ------------------------------------------------------------------ */

static struct ec_encoding *mojette_create(int k, int m, bool systematic)
{
	struct ec_encoding *encoding = NULL;
	struct moj_encoding_private *mcp = NULL;
	int n = k + m;

	if (k < 1 || m < 1)
		return NULL;
	/*
	 * Encoder wrappers use k/m-sized stack VLAs (row-pointer arrays,
	 * projection descriptor arrays, temp buffer arrays -- see
	 * mojette_sys_encode + mojette_nonsys_encode).  MDS-provided
	 * layout values reach here unvalidated by ec_pipeline; enforce a
	 * hard cap so a malformed layout can't blow the stack via ~24 MB
	 * of VLAs before any allocation failure surfaces.  Real
	 * deployments never approach this ceiling (typical stripe widths
	 * are k <= 16, m <= 4).
	 */
	if (n > MOJ_MAX_STRIPE_WIDTH)
		return NULL;

	encoding = calloc(1, sizeof(*encoding));
	if (!encoding)
		return NULL;

	mcp = calloc(1, sizeof(*mcp));
	if (!mcp)
		goto fail;

	if (moj_directions_generate(n, &mcp->mcp_dirs) < 0)
		goto fail;

	mcp->mcp_n = n;
	mcp->mcp_systematic = systematic;

	encoding->ec_name = systematic ? "mojette-systematic" :
					 "mojette-non-systematic";
	encoding->ec_k = k;
	encoding->ec_m = m;
	encoding->ec_encode = systematic ? mojette_sys_encode :
					   mojette_nonsys_encode;
	encoding->ec_decode = systematic ? mojette_sys_decode :
					   mojette_nonsys_decode;
	encoding->ec_shard_size = mojette_shard_size;
	encoding->ec_destroy = mojette_destroy;
	encoding->ec_private = mcp;

	return encoding;

fail:
	if (mcp)
		free(mcp->mcp_dirs);
	free(mcp);
	free(encoding);
	return NULL;
}

struct ec_encoding *ec_mojette_sys_create(int k, int m)
{
	return mojette_create(k, m, true);
}

struct ec_encoding *ec_mojette_nonsys_create(int k, int m)
{
	return mojette_create(k, m, false);
}
