/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Mojette erasure codec — systematic and non-systematic wrappers.
 *
 * Wraps the core Mojette transform (mojette.c) into the ec_codec
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

struct moj_codec_private {
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

static int mojette_sys_encode(struct ec_codec *codec, uint8_t **data,
			      uint8_t **parity, size_t shard_len)
{
	struct moj_codec_private *mcp = codec->ec_private;
	int k = codec->ec_k;
	int m = codec->ec_m;
	int P = grid_P(shard_len);

	/*
	 * data[0..k-1] are grid rows.  Assemble them into a contiguous
	 * grid for the forward transform.
	 */
	uint64_t *grid = calloc((size_t)(P * k), sizeof(uint64_t));

	if (!grid)
		return -ENOMEM;

	for (int i = 0; i < k; i++)
		memcpy(grid + i * P, data[i], shard_len);

	/*
	 * We only need the last m projections (the parity directions).
	 * The first k directions correspond to data rows in a
	 * systematic code; we skip them.
	 */
	struct moj_projection *projs[m]; /* VLA, m is small */

	for (int i = 0; i < m; i++) {
		int dir_idx = k + i;
		int nbins = moj_projection_size(mcp->mcp_dirs[dir_idx].md_p,
						mcp->mcp_dirs[dir_idx].md_q, P,
						k);

		projs[i] = moj_projection_create(nbins);
		if (!projs[i]) {
			for (int j = 0; j < i; j++)
				moj_projection_destroy(projs[j]);
			free(grid);
			return -ENOMEM;
		}
	}

	moj_forward(grid, P, k, mcp->mcp_dirs + k, m, projs);

	/* Copy projections into parity buffers, zero-padded. */
	for (int i = 0; i < m; i++) {
		size_t proj_bytes =
			(size_t)projs[i]->mp_nbins * sizeof(uint64_t);
		memcpy(parity[i], projs[i]->mp_bins, proj_bytes);
		moj_projection_destroy(projs[i]);
	}

	free(grid);
	return 0;
}

static int mojette_sys_decode(struct ec_codec *codec, uint8_t **shards,
			      const bool *present, size_t shard_len)
{
	struct moj_codec_private *mcp = codec->ec_private;
	int k = codec->ec_k;
	int m = codec->ec_m;
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
		/* Only parity missing — re-encode to regenerate. */
		uint8_t **parity = shards + k;

		return mojette_sys_encode(codec, shards, parity, shard_len);
	}

	/*
	 * Some data rows are missing.  Collect available projections
	 * (parity shards) and subtract the contributions of known
	 * data rows, then inverse-solve for missing rows.
	 */
	int n_avail_proj = 0;

	for (int i = k; i < n; i++) {
		if (present[i])
			n_avail_proj++;
	}

	if (n_avail_proj < missing_data)
		return -EIO;

	/*
	 * Build projections from parity shards.  Subtract known rows'
	 * contributions so that only missing rows remain.
	 */
	struct moj_direction *sub_dirs =
		calloc((size_t)n_avail_proj, sizeof(*sub_dirs));
	struct moj_projection **sub_projs =
		calloc((size_t)n_avail_proj, sizeof(*sub_projs));

	if (!sub_dirs || !sub_projs) {
		free(sub_dirs);
		free(sub_projs);
		return -ENOMEM;
	}

	int pidx = 0;
	int ret = -ENOMEM;

	for (int i = 0; i < m; i++) {
		if (!present[k + i])
			continue;
		if (pidx >= n_avail_proj)
			break;

		int dir_idx = k + i;

		sub_dirs[pidx] = mcp->mcp_dirs[dir_idx];

		int nbins = moj_projection_size(sub_dirs[pidx].md_p,
						sub_dirs[pidx].md_q, P, k);

		sub_projs[pidx] = moj_projection_create(nbins);
		if (!sub_projs[pidx])
			goto out;

		/* Load projection bins from parity shard. */
		memcpy(sub_projs[pidx]->mp_bins, shards[k + i],
		       (size_t)nbins * sizeof(uint64_t));

		/* Subtract known data rows' contributions. */
		int p = sub_dirs[pidx].md_p;
		int q = sub_dirs[pidx].md_q;
		int off = moj_bin_offset(p, q, P, k);

		for (int row = 0; row < k; row++) {
			if (!present[row])
				continue;
			const uint64_t *row_data =
				(const uint64_t *)shards[row];

			for (int col = 0; col < P; col++) {
				int b = col * p - row * q + off;

				sub_projs[pidx]->mp_bins[b] -= row_data[col];
			}
		}
		pidx++;
	}

	/*
	 * Build a reduced grid with only the missing rows.  Map
	 * missing row indices to consecutive rows in the sub-grid.
	 */
	int *missing_rows = calloc((size_t)missing_data, sizeof(int));

	if (!missing_rows)
		goto out;

	int midx = 0;

	for (int i = 0; i < k; i++) {
		if (!present[i])
			missing_rows[midx++] = i;
	}

	/*
	 * Remap directions for the reduced grid.  The reduced grid
	 * has P columns and missing_data rows.  Each projection has
	 * already had known rows subtracted.  We need to use only
	 * missing_data projections (Katz: sum(|q|) >= missing_data).
	 *
	 * Use at most missing_data projections for the inverse.
	 */
	int n_inv = missing_data;
	uint64_t *reduced_grid = calloc((size_t)(P * n_inv), sizeof(uint64_t));

	if (!reduced_grid) {
		free(missing_rows);
		goto out;
	}

	/*
	 * Remap the projections for the reduced grid.  The bin
	 * equation for the full grid was b = col*p - row*q + off.
	 * For the reduced grid, we need to re-index the missing rows
	 * as 0, 1, ..., missing_data-1.  Create new projections with
	 * only the contributions from missing rows.
	 */
	struct moj_projection **inv_projs =
		calloc((size_t)n_inv, sizeof(*inv_projs));

	if (!inv_projs) {
		free(reduced_grid);
		free(missing_rows);
		goto out;
	}

	for (int i = 0; i < n_inv; i++) {
		int nbins = moj_projection_size(sub_dirs[i].md_p,
						sub_dirs[i].md_q, P, n_inv);

		inv_projs[i] = moj_projection_create(nbins);
		if (!inv_projs[i]) {
			for (int j = 0; j < i; j++)
				moj_projection_destroy(inv_projs[j]);
			free(inv_projs);
			free(reduced_grid);
			free(missing_rows);
			goto out;
		}

		/*
		 * Re-project the subtracted bins into the reduced grid's
		 * bin space.  Missing row j in the full grid becomes
		 * row j in the reduced grid.
		 */
		int p = sub_dirs[i].md_p;
		int q = sub_dirs[i].md_q;
		int full_off = moj_bin_offset(p, q, P, k);
		int red_off = moj_bin_offset(p, q, P, n_inv);

		for (int mr = 0; mr < n_inv; mr++) {
			int full_row = missing_rows[mr];

			for (int col = 0; col < P; col++) {
				int full_b = col * p - full_row * q + full_off;
				int red_b = col * p - mr * q + red_off;

				inv_projs[i]->mp_bins[red_b] +=
					sub_projs[i]->mp_bins[full_b];
			}
		}
	}

	/* Inverse on the reduced grid. */
	ret = moj_inverse(reduced_grid, P, n_inv, sub_dirs, n_inv, inv_projs);

	if (ret == 0) {
		/* Copy recovered rows back into shards. */
		for (int i = 0; i < n_inv; i++)
			memcpy(shards[missing_rows[i]], reduced_grid + i * P,
			       shard_len);
	}

	for (int i = 0; i < n_inv; i++)
		moj_projection_destroy(inv_projs[i]);
	free(inv_projs);
	free(reduced_grid);
	free(missing_rows);

out:
	for (int i = 0; i < pidx; i++)
		moj_projection_destroy(sub_projs[i]);
	free(sub_projs);
	free(sub_dirs);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Non-systematic encode / decode                                      */
/* ------------------------------------------------------------------ */

static int mojette_nonsys_encode(struct ec_codec *codec, uint8_t **data,
				 uint8_t **parity, size_t shard_len)
{
	struct moj_codec_private *mcp = codec->ec_private;
	int k = codec->ec_k;
	int m = codec->ec_m;
	int n = k + m;
	int P = grid_P(shard_len);

	/* Assemble grid from data rows. */
	uint64_t *grid = calloc((size_t)(P * k), sizeof(uint64_t));

	if (!grid)
		return -ENOMEM;

	for (int i = 0; i < k; i++)
		memcpy(grid + i * P, data[i], shard_len);

	/* Compute all n projections. */
	struct moj_projection **projs =
		calloc((size_t)n, sizeof(struct moj_projection *));

	if (!projs) {
		free(grid);
		return -ENOMEM;
	}

	for (int i = 0; i < n; i++) {
		int nbins = moj_projection_size(mcp->mcp_dirs[i].md_p,
						mcp->mcp_dirs[i].md_q, P, k);

		projs[i] = moj_projection_create(nbins);
		if (!projs[i]) {
			for (int j = 0; j < i; j++)
				moj_projection_destroy(projs[j]);
			free(projs);
			free(grid);
			return -ENOMEM;
		}
	}

	moj_forward(grid, P, k, mcp->mcp_dirs, n, projs);
	free(grid);

	/*
	 * First k projections go into data[] (overwrites input).
	 * Remaining m go into parity[].
	 */
	for (int i = 0; i < k; i++) {
		size_t proj_bytes =
			(size_t)projs[i]->mp_nbins * sizeof(uint64_t);

		memcpy(data[i], projs[i]->mp_bins, proj_bytes);
		moj_projection_destroy(projs[i]);
	}

	for (int i = 0; i < m; i++) {
		size_t proj_bytes =
			(size_t)projs[k + i]->mp_nbins * sizeof(uint64_t);

		memcpy(parity[i], projs[k + i]->mp_bins, proj_bytes);
		moj_projection_destroy(projs[k + i]);
	}

	free(projs);
	return 0;
}

static int mojette_nonsys_decode(struct ec_codec *codec, uint8_t **shards,
				 const bool *present, size_t shard_len)
{
	struct moj_codec_private *mcp = codec->ec_private;
	int k = codec->ec_k;
	int m = codec->ec_m;
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
	uint64_t *grid = calloc((size_t)(P * k), sizeof(uint64_t));

	if (!grid)
		goto out;

	ret = moj_inverse(grid, P, k, sub_dirs, k, sub_projs);

	if (ret == 0) {
		/* Place original data rows into shards[0..k-1]. */
		for (int i = 0; i < k; i++)
			memcpy(shards[i], grid + i * P, shard_len);
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

static size_t mojette_shard_size(struct ec_codec *codec, int shard_idx,
				 size_t data_shard_len)
{
	struct moj_codec_private *mcp = codec->ec_private;
	int k = codec->ec_k;
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

static void mojette_destroy(struct ec_codec *codec)
{
	struct moj_codec_private *mcp = codec->ec_private;

	if (mcp) {
		free(mcp->mcp_dirs);
		free(mcp);
	}
	free(codec);
}

/* ------------------------------------------------------------------ */
/* Create                                                              */
/* ------------------------------------------------------------------ */

static struct ec_codec *mojette_create(int k, int m, bool systematic)
{
	struct ec_codec *codec = NULL;
	struct moj_codec_private *mcp = NULL;
	int n = k + m;

	if (k < 1 || m < 1)
		return NULL;

	codec = calloc(1, sizeof(*codec));
	if (!codec)
		return NULL;

	mcp = calloc(1, sizeof(*mcp));
	if (!mcp)
		goto fail;

	if (moj_directions_generate(n, &mcp->mcp_dirs) < 0)
		goto fail;

	mcp->mcp_n = n;
	mcp->mcp_systematic = systematic;

	codec->ec_name = systematic ? "mojette-systematic" :
				      "mojette-non-systematic";
	codec->ec_k = k;
	codec->ec_m = m;
	codec->ec_encode = systematic ? mojette_sys_encode :
					mojette_nonsys_encode;
	codec->ec_decode = systematic ? mojette_sys_decode :
					mojette_nonsys_decode;
	codec->ec_shard_size = mojette_shard_size;
	codec->ec_destroy = mojette_destroy;
	codec->ec_private = mcp;

	return codec;

fail:
	if (mcp)
		free(mcp->mcp_dirs);
	free(mcp);
	free(codec);
	return NULL;
}

struct ec_codec *ec_mojette_sys_create(int k, int m)
{
	return mojette_create(k, m, true);
}

struct ec_codec *ec_mojette_nonsys_create(int k, int m)
{
	return mojette_create(k, m, false);
}
