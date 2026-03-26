/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Mojette transform — core forward and inverse algorithms.
 *
 * Clean-room implementation from published academic sources only:
 *
 *   Guédon J.-P. (ed.), "The Mojette Transform: Theory and
 *   Applications", ISTE/Wiley, 2009.
 *
 *   Normand N., Guédon J.-P., Philippe O., Barba D., "Controlled
 *   Redundancy for Image Coding and High-Speed Transmission",
 *   Proc. SPIE Visual Communications and Image Processing, 1996.
 *
 *   Normand N., Kingston A., Évenou P., "A Geometry Driven
 *   Reconstruction Algorithm for the Mojette Transform", DGCI 2006,
 *   LNCS 4245, pp. 122-133.
 *
 *   Katz M., "Questions of Uniqueness and Resolution in
 *   Reconstruction from Projections", Springer, 1978.
 *
 *   Parrein B., Normand N., Guédon J.-P., "Multiple Description
 *   Coding Using Exact Discrete Radon Transform", IEEE DCC, 2001.
 *
 * Arithmetic is unsigned 64-bit wrapping (mod 2^64).  No Galois
 * field operations.
 *
 * SIMD acceleration (AArch64 NEON) for directions with |p|=1, q=1:
 *
 *   p=+1: bin b = col + (off-row), so adjacent columns map to adjacent
 *         bins in ascending order.  The row loop becomes a plain
 *         sequential vector add (vaddq_u64), unrolled 4-wide.
 *
 *   p=-1: bin b = (off-row) - col, so adjacent columns map to
 *         adjacent bins in descending order.  Pairs of bins are loaded
 *         ascending, the two grid lanes are swapped (vextq_u64), and
 *         the result is stored back — again sequential, 4-wide.
 *
 * The StreamScale patent (US 8,683,296) covers SIMD-accelerated
 * Galois field arithmetic.  Mojette uses no GF operations; these
 * NEON paths are plain integer addition and are unaffected.
 *
 * Directions with |p|>1 produce stride-|p| scatter into bins; no
 * SIMD benefit is available and the scalar path is used.
 */

#include "mojette.h"

#ifdef __aarch64__
#include <arm_neon.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Projection lifecycle                                                */
/* ------------------------------------------------------------------ */

struct moj_projection *moj_projection_create(int nbins)
{
	struct moj_projection *p;

	p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;

	p->mp_bins = calloc((size_t)nbins, sizeof(uint64_t));
	if (!p->mp_bins) {
		free(p);
		return NULL;
	}

	p->mp_nbins = nbins;
	return p;
}

void moj_projection_destroy(struct moj_projection *proj)
{
	if (!proj)
		return;
	free(proj->mp_bins);
	free(proj);
}

/* ------------------------------------------------------------------ */
/* Direction generation                                                */
/* ------------------------------------------------------------------ */

int moj_directions_generate(int n, struct moj_direction **dirs)
{
	struct moj_direction *d;

	d = calloc((size_t)n, sizeof(*d));
	if (!d)
		return -ENOMEM;

	/*
	 * Generate n directions with q=1 and non-zero p values,
	 * roughly symmetric around 0.
	 *
	 * For n=4: p = {-2, -1, 1, 2}.
	 * For n=6: p = {-3, -2, -1, 1, 2, 3}.
	 *
	 * p=0 is excluded because direction (0,1) maps all P pixels
	 * in a row to the same bin, making individual pixel recovery
	 * impossible with the iterative corner-peeling algorithm.
	 */
	int half = n / 2;
	int idx = 0;

	for (int i = half; i >= 1; i--) {
		d[idx].md_p = -i;
		d[idx].md_q = 1;
		idx++;
	}
	for (int i = 1; idx < n; i++) {
		d[idx].md_p = i;
		d[idx].md_q = 1;
		idx++;
	}

	*dirs = d;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Katz criterion                                                      */
/* ------------------------------------------------------------------ */

bool moj_katz_check(const struct moj_direction *dirs, int n, int P, int Q)
{
	int sum_abs_p = 0;
	int sum_abs_q = 0;

	for (int i = 0; i < n; i++) {
		int ap = dirs[i].md_p;
		int aq = dirs[i].md_q;

		sum_abs_p += ap < 0 ? -ap : ap;
		sum_abs_q += aq < 0 ? -aq : aq;
	}

	return sum_abs_p >= P || sum_abs_q >= Q;
}

/* ------------------------------------------------------------------ */
/* Forward Mojette transform                                           */
/* ------------------------------------------------------------------ */

#ifdef __aarch64__

/*
 * moj_fwd_row_p1 — accumulate one grid row into bins for p=+1, q=1.
 *
 * For p=+1: b = col + (off-row), so bin indices are sequential and
 * ascending.  dst points to bins[off-row]; we add src[0..P-1] into
 * dst[0..P-1] using 4-wide NEON (two 128-bit vectors per iteration).
 */
static void moj_fwd_row_p1(const uint64_t *__restrict__ src, int P,
			   uint64_t *__restrict__ dst)
{
	int col = 0;

	for (; col + 4 <= P; col += 4) {
		uint64x2_t d0 = vld1q_u64(dst + col);
		uint64x2_t d1 = vld1q_u64(dst + col + 2);
		uint64x2_t s0 = vld1q_u64(src + col);
		uint64x2_t s1 = vld1q_u64(src + col + 2);

		vst1q_u64(dst + col, vaddq_u64(d0, s0));
		vst1q_u64(dst + col + 2, vaddq_u64(d1, s1));
	}
	for (; col < P; col++)
		dst[col] += src[col];
}

/*
 * moj_fwd_row_pm1 — accumulate one grid row into bins for p=-1, q=1.
 *
 * For p=-1: b = (off-row) - col, so bin indices are sequential and
 * descending.  dst points to bins[off-row].
 *
 * For a pair at col and col+1:
 *   dst[-col]   += src[col]
 *   dst[-col-1] += src[col+1]
 *
 * Load bins as {dst[-col-1], dst[-col]} (ascending in memory), load
 * grid as {src[col], src[col+1]}, swap grid lanes with vextq_u64,
 * then add and store.  Unrolled 4-wide.
 */
static void moj_fwd_row_pm1(const uint64_t *__restrict__ src, int P,
			    uint64_t *__restrict__ dst)
{
	int col = 0;

	for (; col + 4 <= P; col += 4) {
		/* Load two ascending bin pairs (each covers 2 columns). */
		uint64x2_t b0 = vld1q_u64(dst - col - 1);
		uint64x2_t b1 = vld1q_u64(dst - col - 3);

		/* Load two sequential grid pairs. */
		uint64x2_t g0 = vld1q_u64(src + col);
		uint64x2_t g1 = vld1q_u64(src + col + 2);

		/*
		 * Swap lanes so {src[col], src[col+1]} becomes
		 * {src[col+1], src[col]}, matching the reversed bin order:
		 *   b0 = {bins[-col-1], bins[-col]}  ← add {src[col+1], src[col]}
		 *   b1 = {bins[-col-3], bins[-col-2]} ← add {src[col+3], src[col+2]}
		 */
		vst1q_u64(dst - col - 1, vaddq_u64(b0, vextq_u64(g0, g0, 1)));
		vst1q_u64(dst - col - 3, vaddq_u64(b1, vextq_u64(g1, g1, 1)));
	}
	for (; col + 2 <= P; col += 2) {
		uint64x2_t bv = vld1q_u64(dst - col - 1);
		uint64x2_t gv = vld1q_u64(src + col);

		vst1q_u64(dst - col - 1, vaddq_u64(bv, vextq_u64(gv, gv, 1)));
	}
	for (; col < P; col++)
		dst[-col] += src[col];
}

#endif /* __aarch64__ */

void moj_forward(const uint64_t *__restrict__ grid, int P, int Q,
		 const struct moj_direction *dirs, int n,
		 struct moj_projection **projs)
{
	for (int i = 0; i < n; i++) {
		int p = dirs[i].md_p;
		int q = dirs[i].md_q;
		int off = moj_bin_offset(p, q, P, Q);
		struct moj_projection *proj = projs[i];

		memset(proj->mp_bins, 0,
		       (size_t)proj->mp_nbins * sizeof(uint64_t));

#ifdef __aarch64__
		/*
		 * Fast path for |p|=1, q=1: bin accesses are sequential
		 * (ascending for p=+1, descending for p=-1), enabling
		 * full 4-wide NEON vectorisation.
		 */
		if (q == 1 && (p == 1 || p == -1)) {
			for (int row = 0; row < Q; row++) {
				const uint64_t *src = grid + row * P;
				uint64_t *dst = proj->mp_bins + (off - row);

				if (p == 1)
					moj_fwd_row_p1(src, P, dst);
				else
					moj_fwd_row_pm1(src, P, dst);
			}
			continue;
		}
#endif
		/* General scalar path — handles any (p, q). */
		for (int row = 0; row < Q; row++) {
			for (int col = 0; col < P; col++) {
				int b = col * p - row * q + off;

				proj->mp_bins[b] += grid[row * P + col];
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Inverse Mojette transform (corner-peeling)                          */
/* ------------------------------------------------------------------ */

/*
 * Efficient corner-peeling with precomputed contributor counts.
 *
 * For each projection, maintain a count of how many unknown pixels
 * map to each bin.  When a count reaches 1, the bin is reconstructible:
 * its value (after subtracting known contributions) IS the unknown
 * pixel.  After recovering a pixel, decrement the contributor count
 * in every projection that includes it.
 *
 * This avoids the O(P*Q) inner scan per bin that a naive approach
 * would require.
 */

int moj_inverse(uint64_t *grid, int P, int Q, const struct moj_direction *dirs,
		int n, struct moj_projection **projs)
{
	int total = P * Q;
	int recovered = 0;
	int ret = -EIO;

	memset(grid, 0, (size_t)total * sizeof(uint64_t));

	/* Track which pixels have been reconstructed. */
	bool *known = calloc((size_t)total, sizeof(bool));

	if (!known)
		return -ENOMEM;

	/*
	 * Precompute bin offsets per projection direction.
	 */
	int *offsets = calloc((size_t)n, sizeof(int));

	if (!offsets) {
		free(known);
		return -ENOMEM;
	}

	for (int i = 0; i < n; i++)
		offsets[i] = moj_bin_offset(dirs[i].md_p, dirs[i].md_q, P, Q);

	/*
	 * Allocate per-projection contributor count arrays.
	 * count[i][b] = number of unknown pixels contributing to
	 * bin b of projection i.
	 */
	int **count = calloc((size_t)n, sizeof(int *));

	if (!count)
		goto out_offsets;

	for (int i = 0; i < n; i++) {
		count[i] = calloc((size_t)projs[i]->mp_nbins, sizeof(int));
		if (!count[i])
			goto out_count;
	}

	/* Initialize counts: every pixel contributes to one bin per projection. */
	for (int row = 0; row < Q; row++) {
		for (int col = 0; col < P; col++) {
			for (int i = 0; i < n; i++) {
				int b = col * dirs[i].md_p -
					row * dirs[i].md_q + offsets[i];

				count[i][b]++;
			}
		}
	}

	/*
	 * Iterative corner-peeling.  Each pass scans all bins of all
	 * projections looking for singleton bins (count == 1).
	 */
	while (recovered < total) {
		bool progress = false;

		for (int i = 0; i < n; i++) {
			int p = dirs[i].md_p;
			int q = dirs[i].md_q;
			int off = offsets[i];
			struct moj_projection *proj = projs[i];

			for (int b = 0; b < proj->mp_nbins; b++) {
				if (count[i][b] != 1)
					continue;

				/*
				 * Find the sole unknown pixel that
				 * contributes to this bin.
				 */
				int u_row = -1, u_col = -1;

				for (int row = 0; row < Q; row++) {
					for (int col = 0; col < P; col++) {
						if (known[row * P + col])
							continue;
						int bb =
							col * p - row * q + off;
						if (bb == b) {
							u_row = row;
							u_col = col;
							goto found;
						}
					}
				}
				continue;
found:;
				/*
				 * The bin value IS the pixel value (all
				 * other contributors have been subtracted
				 * as they were recovered).
				 */
				uint64_t val = proj->mp_bins[b];

				grid[u_row * P + u_col] = val;
				known[u_row * P + u_col] = true;
				recovered++;
				progress = true;

				/*
				 * Subtract the recovered pixel from every
				 * projection and decrement their counts.
				 */
				for (int j = 0; j < n; j++) {
					int bj = u_col * dirs[j].md_p -
						 u_row * dirs[j].md_q +
						 offsets[j];

					projs[j]->mp_bins[bj] -= val;
					count[j][bj]--;
				}
			}
		}

		if (!progress)
			break;
	}

	ret = recovered == total ? 0 : -EIO;

out_count:
	for (int i = 0; i < n; i++)
		free(count[i]);
	free(count);
out_offsets:
	free(offsets);
	free(known);
	return ret;
}
