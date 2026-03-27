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
#elif defined(__AVX2__)
#include <immintrin.h> /* AVX2 — 256-bit, Haswell+ */
#elif defined(__SSE2__)
#include <emmintrin.h> /* SSE2 — baseline on all x86_64 */
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Force-scalar toggle                                                 */
/* ------------------------------------------------------------------ */

static _Atomic bool moj_scalar_only;

void moj_force_scalar(bool force)
{
	moj_scalar_only = force;
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

#if defined(__SSE2__) || defined(__AVX2__)

/*
 * moj_fwd_row_p1_sse2 — sequential ascending bins, SSE2 (x86_64).
 *
 * Identical logic to the NEON p=+1 path but using 128-bit SSE2
 * intrinsics: _mm_add_epi64 on pairs loaded with _mm_loadu_si128.
 * Unrolled 4-wide (two 128-bit ops per iteration).
 */
static void moj_fwd_row_p1_sse2(const uint64_t *__restrict__ src, int P,
				uint64_t *__restrict__ dst)
{
	int col = 0;

	for (; col + 4 <= P; col += 4) {
		__m128i d0 = _mm_loadu_si128((const __m128i *)(dst + col));
		__m128i d1 = _mm_loadu_si128((const __m128i *)(dst + col + 2));
		__m128i s0 = _mm_loadu_si128((const __m128i *)(src + col));
		__m128i s1 = _mm_loadu_si128((const __m128i *)(src + col + 2));

		_mm_storeu_si128((__m128i *)(dst + col), _mm_add_epi64(d0, s0));
		_mm_storeu_si128((__m128i *)(dst + col + 2),
				 _mm_add_epi64(d1, s1));
	}
	for (; col < P; col++)
		dst[col] += src[col];
}

/*
 * moj_fwd_row_pm1_sse2 — sequential descending bins, SSE2 (x86_64).
 *
 * Identical logic to the NEON p=-1 path.  Two 64-bit lanes are swapped
 * with _mm_shuffle_epi32(v, 0x4E): the constant 0x4E = _MM_SHUFFLE(1,0,3,2)
 * moves 32-bit dwords [2,3,0,1], which swaps the two 64-bit halves.
 * This is the SSE2 equivalent of vextq_u64(v, v, 1).
 */
static void moj_fwd_row_pm1_sse2(const uint64_t *__restrict__ src, int P,
				 uint64_t *__restrict__ dst)
{
	int col = 0;

	for (; col + 4 <= P; col += 4) {
		__m128i b0 = _mm_loadu_si128((const __m128i *)(dst - col - 1));
		__m128i b1 = _mm_loadu_si128((const __m128i *)(dst - col - 3));
		__m128i g0 = _mm_loadu_si128((const __m128i *)(src + col));
		__m128i g1 = _mm_loadu_si128((const __m128i *)(src + col + 2));

		_mm_storeu_si128((__m128i *)(dst - col - 1),
				 _mm_add_epi64(b0,
					       _mm_shuffle_epi32(g0, 0x4E)));
		_mm_storeu_si128((__m128i *)(dst - col - 3),
				 _mm_add_epi64(b1,
					       _mm_shuffle_epi32(g1, 0x4E)));
	}
	for (; col + 2 <= P; col += 2) {
		__m128i bv = _mm_loadu_si128((const __m128i *)(dst - col - 1));
		__m128i gv = _mm_loadu_si128((const __m128i *)(src + col));

		_mm_storeu_si128((__m128i *)(dst - col - 1),
				 _mm_add_epi64(bv,
					       _mm_shuffle_epi32(gv, 0x4E)));
	}
	for (; col < P; col++)
		dst[-col] += src[col];
}

#endif /* __SSE2__ || __AVX2__ */

#ifdef __AVX2__

/*
 * moj_fwd_row_p1_avx2 — sequential ascending bins, AVX2 (x86_64).
 *
 * 256-bit (4 x uint64_t) per iteration, double the width of SSE2.
 * Same logic: load bins, load grid, add, store.
 */
static void moj_fwd_row_p1_avx2(const uint64_t *__restrict__ src, int P,
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
				    _mm256_add_epi64(d0, s0));
		_mm256_storeu_si256((__m256i *)(dst + col + 4),
				    _mm256_add_epi64(d1, s1));
	}
	for (; col + 4 <= P; col += 4) {
		__m256i dv = _mm256_loadu_si256((const __m256i *)(dst + col));
		__m256i sv = _mm256_loadu_si256((const __m256i *)(src + col));

		_mm256_storeu_si256((__m256i *)(dst + col),
				    _mm256_add_epi64(dv, sv));
	}
	/* Delegate remaining columns to the SSE2 p1 handler. */
	if (col < P)
		moj_fwd_row_p1_sse2(src + col, P - col, dst + col);
}

/*
 * moj_fwd_row_pm1_avx2 — sequential descending bins, AVX2 (x86_64).
 *
 * For p=-1 the bins descend: dst[-col] += src[col].  Within a 4-element
 * AVX2 vector, we need to reverse the element order.  Load grid as
 * {g[0], g[1], g[2], g[3]}, reverse to {g[3], g[2], g[1], g[0]} with
 * _mm256_permute4x64_epi64(v, 0x1B), then add to the ascending bin
 * vector at dst[-col-3..dst[-col].
 *
 * 0x1B = _MM_SHUFFLE(0,1,2,3): lane 0←3, 1←2, 2←1, 3←0.
 */
static void moj_fwd_row_pm1_avx2(const uint64_t *__restrict__ src, int P,
				 uint64_t *__restrict__ dst)
{
	int col = 0;

	for (; col + 8 <= P; col += 8) {
		__m256i b0 =
			_mm256_loadu_si256((const __m256i *)(dst - col - 3));
		__m256i b1 =
			_mm256_loadu_si256((const __m256i *)(dst - col - 7));
		__m256i g0 = _mm256_loadu_si256((const __m256i *)(src + col));
		__m256i g1 =
			_mm256_loadu_si256((const __m256i *)(src + col + 4));

		_mm256_storeu_si256(
			(__m256i *)(dst - col - 3),
			_mm256_add_epi64(b0,
					 _mm256_permute4x64_epi64(g0, 0x1B)));
		_mm256_storeu_si256(
			(__m256i *)(dst - col - 7),
			_mm256_add_epi64(b1,
					 _mm256_permute4x64_epi64(g1, 0x1B)));
	}
	for (; col + 4 <= P; col += 4) {
		__m256i bv =
			_mm256_loadu_si256((const __m256i *)(dst - col - 3));
		__m256i gv = _mm256_loadu_si256((const __m256i *)(src + col));

		_mm256_storeu_si256(
			(__m256i *)(dst - col - 3),
			_mm256_add_epi64(bv,
					 _mm256_permute4x64_epi64(gv, 0x1B)));
	}
	/* Delegate remaining columns to the SSE2 pm1 handler. */
	if (col < P)
		moj_fwd_row_pm1_sse2(src + col, P - col, dst - col);
}

#endif /* __AVX2__ */

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
		 * SIMD fast path for |p|=1, q=1.
		 *
		 * Bin indices are sequential (ascending for p=+1, descending
		 * for p=-1), so each row reduces to a plain vector add with
		 * no scatter.  Dispatch order: AVX2 > SSE2 > NEON > scalar.
		 * Bypassed entirely when moj_force_scalar(true) is set.
		 */
#if defined(__aarch64__) || defined(__SSE2__) || defined(__AVX2__)
		if (!moj_scalar_only && q == 1 && (p == 1 || p == -1)) {
			for (int row = 0; row < Q; row++) {
				const uint64_t *src = grid + row * P;
				uint64_t *dst = proj->mp_bins + (off - row);

#ifdef __aarch64__
				if (p == 1)
					moj_fwd_row_p1(src, P, dst);
				else
					moj_fwd_row_pm1(src, P, dst);
#elif defined(__AVX2__)
				if (p == 1)
					moj_fwd_row_p1_avx2(src, P, dst);
				else
					moj_fwd_row_pm1_avx2(src, P, dst);
#else /* __SSE2__ */
				if (p == 1)
					moj_fwd_row_p1_sse2(src, P, dst);
				else
					moj_fwd_row_pm1_sse2(src, P, dst);
#endif
			}
			continue;
		}
#endif /* __aarch64__ || __SSE2__ || __AVX2__ */
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
