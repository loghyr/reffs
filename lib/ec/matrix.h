/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Matrix operations over GF(2^8).
 *
 * All matrices are row-major: element [r][c] is at data[r * cols + c].
 * Reference: Peterson & Weldon, "Error-Correcting Codes" (1972).
 */

#ifndef _REFFS_EC_MATRIX_H
#define _REFFS_EC_MATRIX_H

#include <stdint.h>

struct gf_matrix {
	int rows;
	int cols;
	uint8_t *data;
};

struct gf_matrix *gf_matrix_create(int rows, int cols);
void gf_matrix_destroy(struct gf_matrix *m);

static inline uint8_t gf_matrix_get(const struct gf_matrix *m, int r, int c)
{
	return m->data[r * m->cols + c];
}

static inline void gf_matrix_set(struct gf_matrix *m, int r, int c, uint8_t val)
{
	m->data[r * m->cols + c] = val;
}

/*
 * gf_matrix_mul -- multiply a * b, store result in out.
 * out must be pre-allocated with a->rows x b->cols dimensions.
 * a->cols must equal b->rows.  Returns 0 or -EINVAL.
 */
int gf_matrix_mul(const struct gf_matrix *a, const struct gf_matrix *b,
		  struct gf_matrix *out);

/*
 * gf_matrix_invert -- compute the inverse of a square matrix.
 * Uses Gaussian elimination with partial pivoting over GF(2^8).
 * Returns 0 on success, -EINVAL if singular or non-square.
 * The result is written to out (must be pre-allocated, same dims as m).
 */
int gf_matrix_invert(const struct gf_matrix *m, struct gf_matrix *out);

/*
 * gf_matrix_vandermonde -- construct a rows x cols Vandermonde matrix
 * over GF(2^8), keyed on non-zero evaluation points.
 * Element [i][j] = (i+1)^j (so alpha_i = i+1, taking values 1..rows).
 * The all-non-zero-alpha choice guarantees any k distinct rows form an
 * invertible k x k Vandermonde, which is the MDS-property foundation
 * of the Reed-Solomon Vandermonde encoding defined in
 * draft-haynes-nfsv4-flexfiles-v2.
 * Caller MUST ensure rows <= 255 (255 non-zero points fit in
 * GF(2^8) \ {0}) and cols <= 256.
 */
struct gf_matrix *gf_matrix_vandermonde(int rows, int cols);

#endif /* _REFFS_EC_MATRIX_H */
