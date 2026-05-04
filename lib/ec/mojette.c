/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Mojette transform -- core forward and inverse algorithms.
 *
 * Clean-room implementation from published academic sources only:
 *
 *   Guedon J.-P. (ed.), "The Mojette Transform: Theory and
 *   Applications", ISTE/Wiley, 2009.
 *
 *   Normand N., Guedon J.-P., Philippe O., Barba D., "Controlled
 *   Redundancy for Image Coding and High-Speed Transmission",
 *   Proc. SPIE Visual Communications and Image Processing, 1996.
 *
 *   Normand N., Kingston A., Evenou P., "A Geometry Driven
 *   Reconstruction Algorithm for the Mojette Transform", DGCI 2006,
 *   LNCS 4245, pp. 122-133.
 *
 *   Katz M., "Questions of Uniqueness and Resolution in
 *   Reconstruction from Projections", Springer, 1978.
 *
 *   Parrein B., Normand N., Guedon J.-P., "Multiple Description
 *   Coding Using Exact Discrete Radon Transform", IEEE DCC, 2001.
 *
 * Bin formula (paper / HCF convention): b = row*p + col*q - off.
 * Pixels accumulate into bins via XOR.  With q=1 (the conventional
 * choice, per Parrein 2001), b varies by 1 per col step regardless
 * of p -- so the inner loop XORs a row's worth of pixels into a
 * sequential run of bins, and SIMD is fully effective for ANY p.
 *
 * Arithmetic is bitwise XOR over (GF(2)^64): each bin accumulates
 * contributing pixels with `^=`, and the inverse subtraction is the
 * same XOR.  XOR has no carry chain, scales straightforwardly to
 * 128-/256-/512-bit SIMD lanes as a single bit-parallel unit, and
 * matches the broader Mojette literature.
 *
 * SIMD acceleration (AArch64 NEON, x86_64 SSE2, x86_64 AVX2):
 *
 *   For each row r and direction (p, q=1), bin pointer is
 *   `prj->bins + r*p - off`; the inner loop XORs grid[r][0..P-1]
 *   into bins[bin_ptr .. bin_ptr+P-1] sequentially.  One ascending
 *   helper per ISA covers all directions.
 *
 * The StreamScale patent (US 8,683,296) covers SIMD-accelerated
 * Galois-field MULTIPLICATION (GF(2^8) by tabled split-shuffles).
 * Mojette uses only XOR, which is the trivial group operation in
 * (GF(2)^64, +) and is unaffected by that patent.
 */

#include "mojette.h"

#ifdef __aarch64__
#include <arm_neon.h>
#elif defined(__AVX2__)
#include <immintrin.h> /* AVX2 -- 256-bit, Haswell+ */
#elif defined(__SSE2__)
#include <emmintrin.h> /* SSE2 -- baseline on all x86_64 */
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Force-scalar / force-gd toggles                                     */
/* ------------------------------------------------------------------ */

static _Atomic bool moj_scalar_only;
static _Atomic bool moj_gd_enabled;

void moj_force_scalar(bool force)
{
	moj_scalar_only = force;
}

void moj_force_gd(bool force)
{
	moj_gd_enabled = force;
}

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

/*
 * Single ascending-bins SIMD helper per ISA.  Paper bin formula
 * `b = row*p + col*q - off` with q=1 makes b sequential in col for
 * any p, so one helper covers every direction.
 */

#ifdef __aarch64__

static void moj_fwd_row_seq(const uint64_t *__restrict__ src, int P,
			    uint64_t *__restrict__ dst)
{
	int col = 0;

	for (; col + 4 <= P; col += 4) {
		uint64x2_t d0 = vld1q_u64(dst + col);
		uint64x2_t d1 = vld1q_u64(dst + col + 2);
		uint64x2_t s0 = vld1q_u64(src + col);
		uint64x2_t s1 = vld1q_u64(src + col + 2);

		vst1q_u64(dst + col, veorq_u64(d0, s0));
		vst1q_u64(dst + col + 2, veorq_u64(d1, s1));
	}
	for (; col < P; col++)
		dst[col] ^= src[col];
}

#elif defined(__SSE2__) || defined(__AVX2__)

#ifdef __AVX2__

static void moj_fwd_row_seq(const uint64_t *__restrict__ src, int P,
			    uint64_t *__restrict__ dst)
{
	int col = 0;

	for (; col + 8 <= P; col += 8) {
		__m256i d0 = _mm256_loadu_si256((const __m256i *)(dst + col));
		__m256i d1 =
			_mm256_loadu_si256((const __m256i *)(dst + col + 4));
		__m256i s0 = _mm256_loadu_si256((const __m256i *)(src + col));
		__m256i s1 =
			_mm256_loadu_si256((const __m256i *)(src + col + 4));

		_mm256_storeu_si256((__m256i *)(dst + col),
				    _mm256_xor_si256(d0, s0));
		_mm256_storeu_si256((__m256i *)(dst + col + 4),
				    _mm256_xor_si256(d1, s1));
	}
	for (; col + 4 <= P; col += 4) {
		__m256i dv = _mm256_loadu_si256((const __m256i *)(dst + col));
		__m256i sv = _mm256_loadu_si256((const __m256i *)(src + col));

		_mm256_storeu_si256((__m256i *)(dst + col),
				    _mm256_xor_si256(dv, sv));
	}
	for (; col + 2 <= P; col += 2) {
		__m128i dv = _mm_loadu_si128((const __m128i *)(dst + col));
		__m128i sv = _mm_loadu_si128((const __m128i *)(src + col));

		_mm_storeu_si128((__m128i *)(dst + col),
				 _mm_xor_si128(dv, sv));
	}
	for (; col < P; col++)
		dst[col] ^= src[col];
}

#else /* __SSE2__ only */

static void moj_fwd_row_seq(const uint64_t *__restrict__ src, int P,
			    uint64_t *__restrict__ dst)
{
	int col = 0;

	for (; col + 4 <= P; col += 4) {
		__m128i d0 = _mm_loadu_si128((const __m128i *)(dst + col));
		__m128i d1 = _mm_loadu_si128((const __m128i *)(dst + col + 2));
		__m128i s0 = _mm_loadu_si128((const __m128i *)(src + col));
		__m128i s1 = _mm_loadu_si128((const __m128i *)(src + col + 2));

		_mm_storeu_si128((__m128i *)(dst + col), _mm_xor_si128(d0, s0));
		_mm_storeu_si128((__m128i *)(dst + col + 2),
				 _mm_xor_si128(d1, s1));
	}
	for (; col < P; col++)
		dst[col] ^= src[col];
}

#endif /* __AVX2__ */

#endif /* __aarch64__ / __SSE2__ / __AVX2__ */

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

		/*
		 * SIMD fast path for q=1.
		 *
		 * Paper bin formula `b = row*p + col*q - off` with q=1 gives
		 * b sequential in col for any p; bin pointer for row r is
		 * `bins + r*p - off`, then we XOR a row's worth of pixels in.
		 * Bypassed when moj_force_scalar(true) is set.
		 */
#if defined(__aarch64__) || defined(__SSE2__) || defined(__AVX2__)
		if (!moj_scalar_only && q == 1) {
			for (int row = 0; row < Q; row++) {
				const uint64_t *src = grid + (size_t)row * P;
				uint64_t *dst = proj->mp_bins + (row * p - off);

				moj_fwd_row_seq(src, P, dst);
			}
			continue;
		}
#endif /* __aarch64__ || __SSE2__ || __AVX2__ */
		/* General scalar path -- handles any (p, q). */
		for (int row = 0; row < Q; row++) {
			for (int col = 0; col < P; col++) {
				int b = row * p + col * q - off;

				proj->mp_bins[b] ^= grid[row * P + col];
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
 * its value (after XORing out known contributions) IS the unknown
 * pixel.  After recovering a pixel, decrement the contributor count
 * in every projection that includes it.
 *
 * This avoids the O(P*Q) inner scan per bin that a naive approach
 * would require.
 */

int moj_inverse_peel(uint64_t *grid, int P, int Q,
		     const struct moj_direction *dirs, int n,
		     struct moj_projection **projs)
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
				int b = row * dirs[i].md_p +
					col * dirs[i].md_q - offsets[i];

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
							row * p + col * q - off;
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
				 * other contributors have been XORed out
				 * as they were recovered).
				 */
				uint64_t val = proj->mp_bins[b];

				grid[u_row * P + u_col] = val;
				known[u_row * P + u_col] = true;
				recovered++;
				progress = true;

				/*
				 * XOR the recovered pixel out of every
				 * projection and decrement their counts.
				 */
				for (int j = 0; j < n; j++) {
					int bj = u_row * dirs[j].md_p +
						 u_col * dirs[j].md_q -
						 offsets[j];

					projs[j]->mp_bins[bj] ^= val;
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

/* ------------------------------------------------------------------ */
/* Corner-peeling on a partially known grid                            */
/* ------------------------------------------------------------------ */

/*
 * moj_inverse_peel_sparse -- corner-peeling on a P*Q grid where some
 * rows are pre-filled (known) and the rest are missing.
 *
 * grid: pre-filled with known rows, missing rows zero.
 * missing[]: sorted ascending; rows[missing[i]] are unknown.
 *
 * Bins are FULL forward bins (containing XOR of all rows including
 * known ones).  We pre-subtract known-row contributions so bin
 * contents reflect only unknown pixels, then run standard peel.
 */
int moj_inverse_peel_sparse(uint64_t *grid, int P, int Q,
			    const struct moj_direction *dirs, int n,
			    struct moj_projection **projs,
			    const int *missing, int n_missing)
{
	int total = P * Q;
	int recovered = 0;
	int ret = -EIO;
	bool *missing_row = NULL;
	bool *known = NULL;
	int *offsets = NULL;
	int **count = NULL;

	if (!missing || n_missing < 1 || n_missing > Q)
		return -EINVAL;

	missing_row = calloc((size_t)Q, sizeof(bool));
	known = calloc((size_t)total, sizeof(bool));
	offsets = calloc((size_t)n, sizeof(int));
	if (!missing_row || !known || !offsets) {
		ret = -ENOMEM;
		goto out;
	}

	for (int i = 0; i < n_missing; i++) {
		if (missing[i] < 0 || missing[i] >= Q) {
			ret = -EINVAL;
			goto out;
		}
		missing_row[missing[i]] = true;
	}

	/* Pixels in non-missing rows are already known. */
	int n_unknown = 0;

	for (int row = 0; row < Q; row++) {
		if (missing_row[row])
			continue;
		for (int col = 0; col < P; col++)
			known[row * P + col] = true;
	}
	n_unknown = n_missing * P;
	recovered = total - n_unknown;

	for (int i = 0; i < n; i++)
		offsets[i] = moj_bin_offset(dirs[i].md_p, dirs[i].md_q, P, Q);

	count = calloc((size_t)n, sizeof(int *));
	if (!count) {
		ret = -ENOMEM;
		goto out;
	}
	for (int i = 0; i < n; i++) {
		count[i] = calloc((size_t)projs[i]->mp_nbins, sizeof(int));
		if (!count[i]) {
			ret = -ENOMEM;
			goto out_count;
		}
	}

	/*
	 * Initialize: for each (row, col, i),
	 *   if known: XOR pixel out of bin (so bin = XOR of unknowns).
	 *   else:     count[i][b]++.
	 */
	for (int row = 0; row < Q; row++) {
		for (int col = 0; col < P; col++) {
			bool is_known = !missing_row[row];

			for (int i = 0; i < n; i++) {
				int b = row * dirs[i].md_p +
					col * dirs[i].md_q - offsets[i];

				if (is_known)
					projs[i]->mp_bins[b] ^=
						grid[row * P + col];
				else
					count[i][b]++;
			}
		}
	}

	/* Iterative peel. */
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

				int u_row = -1, u_col = -1;

				for (int row = 0; row < Q; row++) {
					if (!missing_row[row])
						continue;
					for (int col = 0; col < P; col++) {
						if (known[row * P + col])
							continue;
						int bb =
							row * p + col * q - off;
						if (bb == b) {
							u_row = row;
							u_col = col;
							goto found;
						}
					}
				}
				continue;
found:;
				uint64_t val = proj->mp_bins[b];

				grid[u_row * P + u_col] = val;
				known[u_row * P + u_col] = true;
				recovered++;
				progress = true;

				for (int j = 0; j < n; j++) {
					int bj = u_row * dirs[j].md_p +
						 u_col * dirs[j].md_q -
						 offsets[j];

					projs[j]->mp_bins[bj] ^= val;
					count[j][bj]--;
				}
			}
		}

		if (!progress)
			break;
	}

	ret = recovered == total ? 0 : -EIO;

out_count:
	if (count) {
		for (int i = 0; i < n; i++)
			free(count[i]);
		free(count);
	}
out:
	free(offsets);
	free(known);
	free(missing_row);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Geometry-driven inverse (DGCI 2006)                                 */
/* ------------------------------------------------------------------ */

/*
 * moj_inverse_gd -- geometry-driven reconstruction.
 *
 * Reference:
 *   Normand N., Kingston A., Evenou P., "A Geometry Driven
 *   Reconstruction Algorithm for the Mojette Transform",
 *   DGCI 2006, LNCS 4245, pp. 122-133.
 *
 * Operates on a P*Q grid where every row is unknown (np == Q,
 * failures = {0, 1, ..., Q-1} dense).  Reffs's codec layer always
 * presents this dense-failures shape, so the sparse-failures
 * correction in the k_offsets recurrence collapses to zero and is
 * dropped.
 *
 * Sort projections by slope (md_p / md_q) descending via an index
 * array; the caller's dirs[] / projs[] are not mutated.  Bin
 * formula: b = row*p + col*q - off (paper / HCF convention).
 *
 * Sweep: for k from -max(k_off[0], k_off[np-1]) to P - m_off, and
 * for each sorted projection l, recover pixel (y=l, x=k+k_off[l])
 * by walking the projection's line through that pixel and XORing
 * already-recovered neighbours, then XORing the bin value.
 */

struct moj_gd_pair {
	int p;
	int q;
	int idx;
};

static int moj_gd_pair_cmp(const void *a, const void *b)
{
	const struct moj_gd_pair *pa = a;
	const struct moj_gd_pair *pb = b;
	double sa = (double)pa->p / (double)pa->q;
	double sb = (double)pb->p / (double)pb->q;

	/* Descending by p/q. */
	if (sa > sb)
		return -1;
	if (sa < sb)
		return 1;
	return 0;
}

int moj_inverse_gd(uint64_t *grid, int P, int Q,
		   const struct moj_direction *dirs, int n,
		   struct moj_projection **projs)
{
	int np = n;
	int ret = -EIO;
	struct moj_gd_pair *pairs = NULL;
	int *sorted_idx = NULL;
	int *sorted_p = NULL;
	int *off = NULL;
	int *k_off = NULL;

	if (np != Q || P < 1 || Q < 1)
		return -EINVAL;

	for (int i = 0; i < np; i++) {
		if (dirs[i].md_q != 1)
			return -EINVAL;
	}

	pairs = calloc((size_t)np, sizeof(*pairs));
	sorted_idx = calloc((size_t)np, sizeof(int));
	sorted_p = calloc((size_t)np, sizeof(int));
	off = calloc((size_t)np, sizeof(int));
	k_off = calloc((size_t)np, sizeof(int));
	if (!pairs || !sorted_idx || !sorted_p || !off || !k_off) {
		ret = -ENOMEM;
		goto out;
	}

	memset(grid, 0, (size_t)P * (size_t)Q * sizeof(uint64_t));

	/* Sort by slope p/q descending via an index array. */
	for (int i = 0; i < np; i++) {
		pairs[i].p = dirs[i].md_p;
		pairs[i].q = dirs[i].md_q;
		pairs[i].idx = i;
	}
	qsort(pairs, (size_t)np, sizeof(*pairs), moj_gd_pair_cmp);
	for (int l = 0; l < np; l++) {
		sorted_idx[l] = pairs[l].idx;
		sorted_p[l] = pairs[l].p;
		off[l] = moj_bin_offset(dirs[pairs[l].idx].md_p,
					dirs[pairs[l].idx].md_q, P, Q);
	}

	/*
	 * Compute k_offsets in sorted order.  Failures = {0..np-1}
	 * (dense), so failures[i+1] - failures[i] - 1 = 0 in the
	 * recurrence and the sparse correction is dropped.
	 *
	 *   s_minus = sum max(0, -p_l) for interior l in [1, np-2]
	 *   s_plus  = sum max(0,  p_l) for interior l in [1, np-2]
	 *   k_off[np-1] = max(max(0, -p_{np-1}) + s_minus,
	 *                     max(0,  p_{np-1}) + s_plus)
	 *   k_off[i]    = k_off[i+1] + p_{i+1}            for i = np-2..0
	 */
	int s_minus = 0;
	int s_plus = 0;

	for (int i = 1; i < np - 1; i++) {
		int p = sorted_p[i];

		s_minus += p < 0 ? -p : 0;
		s_plus += p > 0 ? p : 0;
	}

	if (np == 1) {
		int p0 = sorted_p[0];

		k_off[0] = p0 < 0 ? -p0 : p0;
	} else {
		int p_last = sorted_p[np - 1];
		int neg_term = (p_last < 0 ? -p_last : 0) + s_minus;
		int pos_term = (p_last > 0 ? p_last : 0) + s_plus;

		k_off[np - 1] = neg_term > pos_term ? neg_term : pos_term;
		for (int i = np - 2; i >= 0; i--)
			k_off[i] = k_off[i + 1] + sorted_p[i + 1];
	}

	int m_off = k_off[0];

	for (int l = 1; l < np; l++) {
		if (k_off[l] < m_off)
			m_off = k_off[l];
	}

	int k_lo_neg = k_off[0];
	int k_hi_neg = k_off[np - 1];
	int k_lo = -(k_lo_neg > k_hi_neg ? k_lo_neg : k_hi_neg);
	int k_hi = P - m_off;

	for (int k = k_lo; k < k_hi; k++) {
		for (int l = 0; l < np; l++) {
			int x = k + k_off[l];
			int y = l;

			if (x < 0 || x >= P)
				continue;

			int orig = sorted_idx[l];
			int p = sorted_p[l];
			uint64_t pixel = 0;

			if (p != 0) {
				/*
				 * Walk along the line through (y, x) for
				 * slope (p, q=1).  The line satisfies
				 * row*p + col = const.  Stepping row by -1
				 * keeps it constant iff col steps by +p;
				 * stepping row by +1 iff col steps by -p.
				 */
				int xtop = x;
				int ytop = y;

				while (ytop > 0 && xtop + p >= 0 &&
				       xtop + p < P) {
					xtop += p;
					ytop -= 1;
					pixel ^= grid[ytop * P + xtop];
				}
				int xdn = x;
				int ydn = y;

				while (ydn < Q - 1 && xdn - p >= 0 &&
				       xdn - p < P) {
					xdn -= p;
					ydn += 1;
					pixel ^= grid[ydn * P + xdn];
				}
			} else {
				/*
				 * p == 0: column-sum projection.  Bin
				 * b = col - off contains XOR of all
				 * pixels in column x; recover (y, x) by
				 * XORing all OTHER pixels in column x.
				 */
				for (int i = 0; i < Q; i++) {
					if (i != y)
						pixel ^= grid[i * P + x];
				}
			}

			int b = y * p + x - off[l];

			grid[y * P + x] = projs[orig]->mp_bins[b] ^ pixel;
		}
	}

	ret = 0;

out:
	free(k_off);
	free(off);
	free(sorted_p);
	free(sorted_idx);
	free(pairs);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Geometry-driven inverse on a partially known grid (sparse failures) */
/* ------------------------------------------------------------------ */

/*
 * moj_inverse_gd_sparse -- DGCI on a P*Q grid where some rows are
 * known and the rest are missing.
 *
 * grid is pre-filled with known data rows; missing rows are zero.
 * missing[] (sorted ascending) lists n_missing rows to recover.
 * Bins are FULL forward bins (no pre-subtraction); the sweep
 * ordering ensures all OTHER pixels on each line are either known
 * or already recovered when the algorithm reads them from grid[].
 */
int moj_inverse_gd_sparse(uint64_t *grid, int P, int Q,
			  const struct moj_direction *dirs, int n,
			  struct moj_projection **projs,
			  const int *missing, int n_missing)
{
	int np = n;
	int ret = -EIO;
	struct moj_gd_pair *pairs = NULL;
	int *sorted_idx = NULL;
	int *sorted_p = NULL;
	int *off = NULL;
	int *k_off = NULL;

	if (np != n_missing || P < 1 || Q < 1 || n_missing < 1 ||
	    n_missing > Q)
		return -EINVAL;

	for (int i = 0; i < np; i++) {
		if (dirs[i].md_q != 1)
			return -EINVAL;
	}
	for (int i = 0; i < n_missing; i++) {
		if (missing[i] < 0 || missing[i] >= Q)
			return -EINVAL;
		if (i > 0 && missing[i] <= missing[i - 1])
			return -EINVAL; /* must be strictly ascending */
	}

	pairs = calloc((size_t)np, sizeof(*pairs));
	sorted_idx = calloc((size_t)np, sizeof(int));
	sorted_p = calloc((size_t)np, sizeof(int));
	off = calloc((size_t)np, sizeof(int));
	k_off = calloc((size_t)np, sizeof(int));
	if (!pairs || !sorted_idx || !sorted_p || !off || !k_off) {
		ret = -ENOMEM;
		goto out;
	}

	/* Sort by slope p/q descending via an index array. */
	for (int i = 0; i < np; i++) {
		pairs[i].p = dirs[i].md_p;
		pairs[i].q = dirs[i].md_q;
		pairs[i].idx = i;
	}
	qsort(pairs, (size_t)np, sizeof(*pairs), moj_gd_pair_cmp);
	for (int l = 0; l < np; l++) {
		sorted_idx[l] = pairs[l].idx;
		sorted_p[l] = pairs[l].p;
		off[l] = moj_bin_offset(dirs[pairs[l].idx].md_p,
					dirs[pairs[l].idx].md_q, P, Q);
	}

	/*
	 * Compute k_offsets in sorted order with the sparse-failures
	 * correction term `(missing[i+1] - missing[i] - 1) * p_{i+1}`.
	 */
	int s_minus = 0;
	int s_plus = 0;

	for (int i = 1; i < np - 1; i++) {
		int p = sorted_p[i];

		s_minus += p < 0 ? -p : 0;
		s_plus += p > 0 ? p : 0;
	}

	if (np == 1) {
		int p0 = sorted_p[0];

		k_off[0] = p0 < 0 ? -p0 : p0;
	} else {
		int p_last = sorted_p[np - 1];
		int neg_term = (p_last < 0 ? -p_last : 0) + s_minus;
		int pos_term = (p_last > 0 ? p_last : 0) + s_plus;

		k_off[np - 1] = neg_term > pos_term ? neg_term : pos_term;
		for (int i = np - 2; i >= 0; i--) {
			k_off[i] = k_off[i + 1] + sorted_p[i + 1];
			int gap = missing[i + 1] - missing[i] - 1;

			if (gap > 0) {
				int corr = gap * sorted_p[i + 1];

				for (int j = i + 1; j < np; j++)
					k_off[j] -= corr;
			}
		}
	}

	int m_off = k_off[0];

	for (int l = 1; l < np; l++) {
		if (k_off[l] < m_off)
			m_off = k_off[l];
	}

	int k_lo_neg = k_off[0];
	int k_hi_neg = k_off[np - 1];
	int k_lo = -(k_lo_neg > k_hi_neg ? k_lo_neg : k_hi_neg);
	int k_hi = P - m_off;

	for (int k = k_lo; k < k_hi; k++) {
		for (int l = 0; l < np; l++) {
			int x = k + k_off[l];
			int y = missing[l];

			if (x < 0 || x >= P)
				continue;

			int orig = sorted_idx[l];
			int p = sorted_p[l];
			uint64_t pixel = 0;

			if (p != 0) {
				int xtop = x;
				int ytop = y;

				while (ytop > 0 && xtop + p >= 0 &&
				       xtop + p < P) {
					xtop += p;
					ytop -= 1;
					pixel ^= grid[ytop * P + xtop];
				}
				int xdn = x;
				int ydn = y;

				while (ydn < Q - 1 && xdn - p >= 0 &&
				       xdn - p < P) {
					xdn -= p;
					ydn += 1;
					pixel ^= grid[ydn * P + xdn];
				}
			} else {
				/* p==0: column-sum projection. */
				for (int i = 0; i < Q; i++) {
					if (i != y)
						pixel ^= grid[i * P + x];
				}
			}

			int b = y * p + x - off[l];

			grid[y * P + x] = projs[orig]->mp_bins[b] ^ pixel;
		}
	}

	ret = 0;

out:
	free(k_off);
	free(off);
	free(sorted_p);
	free(sorted_idx);
	free(pairs);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

/*
 * moj_inverse -- routes to peel or gd.
 *
 * Default: corner-peeling (always works for any direction set
 * satisfying Katz).  When moj_force_gd(true) is set AND the direction
 * shape (q==1 for all dirs, n==Q) permits, routes to moj_inverse_gd.
 * If the shape rejects or gd returns -ENOSYS, falls back to peel
 * transparently.
 *
 * The shape check `q==1 && n==Q` makes Katz `Σ|q_i| = Q` trivially
 * satisfied, so no separate Katz call is needed in the dispatch
 * path.
 */
int moj_inverse(uint64_t *grid, int P, int Q, const struct moj_direction *dirs,
		int n, struct moj_projection **projs)
{
	if (atomic_load_explicit(&moj_gd_enabled, memory_order_relaxed) &&
	    n == Q) {
		bool ok = true;

		for (int i = 0; i < n; i++) {
			if (dirs[i].md_q != 1) {
				ok = false;
				break;
			}
		}
		if (ok) {
			int ret = moj_inverse_gd(grid, P, Q, dirs, n, projs);

			if (ret != -ENOSYS && ret != -EINVAL)
				return ret;
			/* Fall through to peel on shape rejection. */
		}
	}
	return moj_inverse_peel(grid, P, Q, dirs, n, projs);
}

/*
 * moj_inverse_sparse -- partially-known-grid dispatcher.
 *
 * If moj_force_gd is set and shape permits (q==1 for all dirs,
 * n == n_missing), routes to gd_sparse; falls back to peel_sparse
 * if shape rejects.  Default: peel_sparse.
 */
int moj_inverse_sparse(uint64_t *grid, int P, int Q,
		       const struct moj_direction *dirs, int n,
		       struct moj_projection **projs,
		       const int *missing, int n_missing)
{
	if (atomic_load_explicit(&moj_gd_enabled, memory_order_relaxed) &&
	    n == n_missing) {
		bool ok = true;

		for (int i = 0; i < n; i++) {
			if (dirs[i].md_q != 1) {
				ok = false;
				break;
			}
		}
		if (ok) {
			int ret = moj_inverse_gd_sparse(grid, P, Q, dirs, n,
							projs, missing,
							n_missing);

			if (ret != -ENOSYS && ret != -EINVAL)
				return ret;
		}
	}
	return moj_inverse_peel_sparse(grid, P, Q, dirs, n, projs, missing,
				       n_missing);
}
