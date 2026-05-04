/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Mojette transform -- internal header.
 *
 * Clean-room implementation from published academic sources:
 *   - Guedon (2005), "The Mojette Transform: Theory and Applications"
 *   - Normand et al. (2006), "How and Why the Mojette Transform Works"
 *   - Katz (1978), reconstruction criterion
 *   - Parrein et al. (2001), q_i=1 direction selection
 *
 * NOT derived from any existing implementation (RozoFS or otherwise).
 */

#ifndef _REFFS_MOJETTE_H
#define _REFFS_MOJETTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Direction vector for a Mojette projection.
 * By convention q is always 1 (per Parrein 2001) to limit
 * projection size overhead.
 */
struct moj_direction {
	int md_p;
	int md_q; /* always 1 in practice */
};

/*
 * A single 1D projection along one direction.
 * Bins accumulate element sums in (GF(2)^64, XOR).
 */
struct moj_projection {
	uint64_t *mp_bins;
	int mp_nbins; /* B = |p|(Q-1) + |q|(P-1) + 1 */
};

/*
 * moj_projection_size -- compute the number of bins for a projection.
 *
 * Paper / HCF convention: b = row*p + col*q - off.
 *   row*p ranges over |p|*(Q-1) (row spans Q-1 steps scaled by |p|).
 *   col*q ranges over |q|*(P-1) (col spans P-1 steps scaled by |q|).
 *   B = |p|*(Q-1) + |q|*(P-1) + 1.
 */
static inline int moj_projection_size(int p, int q, int P, int Q)
{
	int abs_p = p < 0 ? -p : p;
	int abs_q = q < 0 ? -q : q;

	return abs_p * (Q - 1) + abs_q * (P - 1) + 1;
}

/*
 * moj_bin_offset -- compute the offset to subtract from row*p + col*q
 * to get a 0-based bin index.
 *
 * Bin formula (paper / HCF convention): b = row*p + col*q - off.
 * The minimum value of (row*p + col*q) over 0<=row<Q, 0<=col<P is
 * negative-or-zero; off is that minimum (also negative-or-zero) so
 * that subtracting it shifts the array indices to start at 0.
 *
 * Returns the value to SUBTRACT from (row*p + col*q) -- i.e., the
 * minimum of (row*p + col*q) (negative or zero).
 */
static inline int moj_bin_offset(int p, int q, int P, int Q)
{
	int min_b = 0;

	/*
	 * b = row*p + col*q
	 * Minimize over row in [0, Q-1] and col in [0, P-1].
	 */
	if (p < 0)
		min_b += p * (Q - 1); /* row*p minimized at row=Q-1 */
	/* else p >= 0: row*p minimized at row=0, contributes 0 */

	if (q < 0)
		min_b += q * (P - 1); /* col*q minimized at col=P-1 */
	/* else q >= 0: col*q minimized at col=0, contributes 0 */

	return min_b; /* negative or zero -- subtract from row*p + col*q */
}

/*
 * moj_force_scalar -- force the scalar (non-SIMD) forward transform path.
 *
 * When set to true, moj_forward() bypasses NEON/SSE2/AVX2 dispatch and
 * uses the general scalar loop for all directions.  Useful for benchmarking
 * SIMD vs scalar overhead.  Default is false (SIMD enabled).
 */
void moj_force_scalar(bool force);

/*
 * moj_projection_create -- allocate a projection with nbins bins, zeroed.
 * Returns NULL on allocation failure.
 */
struct moj_projection *moj_projection_create(int nbins);

/*
 * moj_projection_destroy -- free a projection and its bins.
 * NULL-safe.
 */
void moj_projection_destroy(struct moj_projection *proj);

/*
 * moj_directions_generate -- generate n directions with q=1 and
 * p values centered around 0.
 *
 * For n=4: p = {-2, -1, 1, 2}
 * For n=6: p = {-3, -2, -1, 1, 2, 3}
 *
 * Returns 0 on success, -ENOMEM on failure.
 * Caller must free(*dirs) when done.
 */
int moj_directions_generate(int n, struct moj_direction **dirs);

/*
 * moj_forward -- compute the forward Mojette transform.
 *
 * grid:  row-major P x Q matrix of uint64_t elements.
 *        grid[row * P + col] is element (row, col).
 * P, Q:  grid dimensions (columns, rows).
 * dirs:  array of n directions.
 * n:     number of projections to compute.
 * projs: output array of n projections (caller-allocated via
 *        moj_projection_create).  Bins are zeroed on entry and
 *        filled on return.
 */
void moj_forward(const uint64_t *__restrict__ grid, int P, int Q,
		 const struct moj_direction *dirs, int n,
		 struct moj_projection **projs);

/*
 * moj_inverse_sparse -- partially-known-grid dispatcher.
 *
 * Wraps moj_inverse_peel_sparse / moj_inverse_gd_sparse, choosing
 * the algorithm based on the moj_force_gd flag (gd if set and
 * shape permits, peel otherwise).
 */
int moj_inverse_sparse(uint64_t *grid, int P, int Q,
		       const struct moj_direction *dirs, int n,
		       struct moj_projection **projs,
		       const int *missing, int n_missing);

/*
 * moj_inverse -- reconstruct a grid from projections.
 *
 * Dispatcher.  Default routes to moj_inverse_peel (corner-peeling).
 * When moj_force_gd(true) is set, validates the direction shape
 * (q==1 for all dirs, n==Q) and routes to moj_inverse_gd; falls
 * back to moj_inverse_peel if the shape rejects.
 *
 * grid:     output P x Q matrix (zeroed on entry, filled on return).
 * P, Q:     grid dimensions.
 * dirs:     array of n directions corresponding to projs.
 * n:        number of available projections.
 * projs:    array of n projections.  Modified in place (bins are
 *           XORed during reconstruction).
 *
 * Returns 0 on success, -EIO if reconstruction stalls (Katz criterion
 * not met for peel; shape-check failure routes to peel).
 */
int moj_inverse(uint64_t *grid, int P, int Q, const struct moj_direction *dirs,
		int n, struct moj_projection **projs);

/*
 * moj_inverse_peel -- corner-peeling inverse Mojette transform.
 *
 * Iteratively finds bins with exactly one unknown contributor, back-
 * projects, XORs out the recovered pixel from all projections.  Works
 * for any direction set that satisfies Katz; does not require q==1
 * or n==Q.
 *
 * Same parameter semantics as moj_inverse.
 */
int moj_inverse_peel(uint64_t *grid, int P, int Q,
		     const struct moj_direction *dirs, int n,
		     struct moj_projection **projs);

/*
 * moj_inverse_peel_sparse -- corner-peeling inverse on a partially
 * known grid.
 *
 * grid is pre-filled with known data rows (rows NOT in missing[]),
 * and zeros in missing rows; missing[] lists the n_missing row
 * indices that the caller wants reconstructed.  Bins in projs[] are
 * the FULL forward bins (including known-row contributions); this
 * function XORs the known-row contributions out internally.
 *
 * Returns 0 on success, -EIO on stall, -ENOMEM on alloc failure.
 */
int moj_inverse_peel_sparse(uint64_t *grid, int P, int Q,
			    const struct moj_direction *dirs, int n,
			    struct moj_projection **projs,
			    const int *missing, int n_missing);

/*
 * moj_inverse_gd -- geometry-driven inverse Mojette transform.
 *
 * Implements Normand-Kingston-Evenou DGCI 2006: a one-shot sweep
 * that recovers pixels in a deterministic geometric order, without
 * scanning bins for singletons.  Faster than corner-peeling for
 * dense-failures geometries.
 *
 * Constraints (caller responsibility — the dispatcher checks):
 *   - md_q == 1 for every direction
 *   - n == Q (every row of the reduced grid is a failure)
 *
 * Same parameter semantics as moj_inverse.
 */
int moj_inverse_gd(uint64_t *grid, int P, int Q,
		   const struct moj_direction *dirs, int n,
		   struct moj_projection **projs);

/*
 * moj_inverse_gd_sparse -- geometry-driven inverse on a partially
 * known grid.
 *
 * grid is pre-filled with known data rows (rows NOT in missing[]),
 * and zeros in missing rows.  missing[] is sorted ascending and
 * lists n_missing rows to reconstruct.  Bins are FULL forward bins
 * (no pre-subtraction needed -- the DGCI sweep ordering handles
 * known-row contributions naturally during the diagonal walk).
 *
 * Constraints: q==1 for every direction, n == n_missing.
 *
 * Returns 0 on success, -EINVAL on shape rejection, -ENOMEM on alloc.
 */
int moj_inverse_gd_sparse(uint64_t *grid, int P, int Q,
			  const struct moj_direction *dirs, int n,
			  struct moj_projection **projs,
			  const int *missing, int n_missing);

/*
 * moj_force_gd -- enable / disable geometry-driven inverse dispatch.
 *
 * When set to true, moj_inverse() routes to moj_inverse_gd if the
 * direction shape (q==1, n==Q) permits, otherwise falls back to
 * moj_inverse_peel.  Default is false (always peel).
 */
void moj_force_gd(bool force);

/*
 * moj_katz_check -- verify the Katz reconstruction criterion.
 *
 * Returns true if the direction set can reconstruct a P x Q grid.
 * Criterion: sum(|p_i|) >= P  OR  sum(|q_i|) >= Q.
 */
bool moj_katz_check(const struct moj_direction *dirs, int n, int P, int Q);

#endif /* _REFFS_MOJETTE_H */
