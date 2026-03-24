/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Mojette transform — internal header.
 *
 * Clean-room implementation from published academic sources:
 *   - Guédon (2005), "The Mojette Transform: Theory and Applications"
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
 * Bins accumulate element sums modulo 2^64.
 */
struct moj_projection {
	uint64_t *mp_bins;
	int mp_nbins; /* B = |p|(Q-1) + |q|(P-1) + 1 */
};

/*
 * moj_projection_size -- compute the number of bins for a projection.
 *
 *   B(p, q, P, Q) = |p|(Q-1) + |q|(P-1) + 1
 */
static inline int moj_projection_size(int p, int q, int P, int Q)
{
	int abs_p = p < 0 ? -p : p;
	int abs_q = q < 0 ? -q : q;

	/*
	 * b = col*p - row*q ranges over |p|*(P-1) + |q|*(Q-1) + 1
	 * distinct values.  col spans P-1 steps scaled by |p|, row
	 * spans Q-1 steps scaled by |q|, plus 1 for the zero value.
	 */
	return abs_p * (P - 1) + abs_q * (Q - 1) + 1;
}

/*
 * moj_bin_offset -- compute the minimum bin index for a projection.
 *
 * Bin b receives pixel (row, col) when b = col*p - row*q.
 * The minimum value of b over 0<=row<Q, 0<=col<P determines
 * the offset to add so that array indices start at 0.
 *
 * Returns the value to ADD to (col*p - row*q) to get a 0-based index.
 */
static inline int moj_bin_offset(int p, int q, int P, int Q)
{
	int min_b = 0;

	/*
	 * b = col*p - row*q
	 * Minimize over col in [0, P-1] and row in [0, Q-1].
	 */
	if (p < 0)
		min_b += p * (P - 1); /* col*p minimized at col=P-1 */
	/* else p >= 0: col*p minimized at col=0, contributes 0 */

	if (q > 0)
		min_b -= q * (Q - 1); /* -row*q minimized at row=Q-1 */
	/* else q <= 0: -row*q minimized at row=0, contributes 0 */

	return -min_b; /* negate: this is the value to ADD */
}

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
 * For n=4: p = {-1, 0, 1, 2}
 * For n=6: p = {-2, -1, 0, 1, 2, 3}
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
void moj_forward(const uint64_t *grid, int P, int Q,
		 const struct moj_direction *dirs, int n,
		 struct moj_projection **projs);

/*
 * moj_inverse -- reconstruct a grid from projections.
 *
 * Corner-peeling algorithm: iteratively find bins that sum exactly
 * one unknown pixel, back-project, subtract from all projections.
 *
 * grid:     output P x Q matrix (zeroed on entry, filled on return).
 * P, Q:     grid dimensions.
 * dirs:     array of n directions corresponding to projs.
 * n:        number of available projections.
 * projs:    array of n projections.  Modified in place (bins are
 *           subtracted during reconstruction).
 *
 * Returns 0 on success, -EIO if reconstruction stalls (Katz criterion
 * not met).
 */
int moj_inverse(uint64_t *grid, int P, int Q, const struct moj_direction *dirs,
		int n, struct moj_projection **projs);

/*
 * moj_katz_check -- verify the Katz reconstruction criterion.
 *
 * Returns true if the direction set can reconstruct a P x Q grid.
 * Criterion: sum(|p_i|) >= P  OR  sum(|q_i|) >= Q.
 */
bool moj_katz_check(const struct moj_direction *dirs, int n, int P, int Q);

#endif /* _REFFS_MOJETTE_H */
