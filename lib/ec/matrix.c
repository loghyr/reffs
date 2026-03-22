/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Matrix operations over GF(2^8) for erasure coding.
 *
 * Reference: Peterson & Weldon, "Error-Correcting Codes" (1972), Ch. 7.
 */

#include "matrix.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "gf.h"

struct gf_matrix *gf_matrix_create(int rows, int cols)
{
	struct gf_matrix *m;

	if (rows <= 0 || cols <= 0)
		return NULL;

	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;

	m->data = calloc((size_t)rows * cols, 1);
	if (!m->data) {
		free(m);
		return NULL;
	}

	m->rows = rows;
	m->cols = cols;
	return m;
}

void gf_matrix_destroy(struct gf_matrix *m)
{
	if (!m)
		return;
	free(m->data);
	free(m);
}

int gf_matrix_mul(const struct gf_matrix *a, const struct gf_matrix *b,
		  struct gf_matrix *out)
{
	if (a->cols != b->rows)
		return -EINVAL;
	if (out->rows != a->rows || out->cols != b->cols)
		return -EINVAL;

	for (int r = 0; r < a->rows; r++) {
		for (int c = 0; c < b->cols; c++) {
			uint8_t sum = 0;

			for (int i = 0; i < a->cols; i++)
				sum = gf_add(sum,
					     gf_mul(gf_matrix_get(a, r, i),
						    gf_matrix_get(b, i, c)));
			gf_matrix_set(out, r, c, sum);
		}
	}

	return 0;
}

int gf_matrix_invert(const struct gf_matrix *m, struct gf_matrix *out)
{
	int n = m->rows;
	struct gf_matrix *work = NULL;
	int ret = 0;

	if (n != m->cols || n != out->rows || n != out->cols)
		return -EINVAL;

	/* Work on a copy augmented conceptually with the identity. */
	work = gf_matrix_create(n, n);
	if (!work)
		return -ENOMEM;
	memcpy(work->data, m->data, (size_t)n * n);

	/* Initialize out as identity. */
	memset(out->data, 0, (size_t)n * n);
	for (int i = 0; i < n; i++)
		gf_matrix_set(out, i, i, 1);

	/* Forward elimination with partial pivoting. */
	for (int col = 0; col < n; col++) {
		/* Find pivot. */
		int pivot = -1;

		for (int row = col; row < n; row++) {
			if (gf_matrix_get(work, row, col) != 0) {
				pivot = row;
				break;
			}
		}

		if (pivot < 0) {
			ret = -EINVAL; /* singular */
			goto out_free;
		}

		/* Swap rows if needed. */
		if (pivot != col) {
			for (int j = 0; j < n; j++) {
				uint8_t tmp;

				tmp = gf_matrix_get(work, col, j);
				gf_matrix_set(work, col, j,
					      gf_matrix_get(work, pivot, j));
				gf_matrix_set(work, pivot, j, tmp);

				tmp = gf_matrix_get(out, col, j);
				gf_matrix_set(out, col, j,
					      gf_matrix_get(out, pivot, j));
				gf_matrix_set(out, pivot, j, tmp);
			}
		}

		/* Scale pivot row so pivot element becomes 1. */
		uint8_t inv = gf_inv(gf_matrix_get(work, col, col));

		for (int j = 0; j < n; j++) {
			gf_matrix_set(work, col, j,
				      gf_mul(gf_matrix_get(work, col, j),
					     inv));
			gf_matrix_set(out, col, j,
				      gf_mul(gf_matrix_get(out, col, j),
					     inv));
		}

		/* Eliminate column in all other rows. */
		for (int row = 0; row < n; row++) {
			uint8_t factor;

			if (row == col)
				continue;
			factor = gf_matrix_get(work, row, col);
			if (factor == 0)
				continue;
			for (int j = 0; j < n; j++) {
				gf_matrix_set(
					work, row, j,
					gf_sub(gf_matrix_get(work, row, j),
					       gf_mul(factor,
						      gf_matrix_get(work, col,
								    j))));
				gf_matrix_set(
					out, row, j,
					gf_sub(gf_matrix_get(out, row, j),
					       gf_mul(factor,
						      gf_matrix_get(out, col,
								    j))));
			}
		}
	}

out_free:
	gf_matrix_destroy(work);
	return ret;
}

struct gf_matrix *gf_matrix_vandermonde(int rows, int cols)
{
	struct gf_matrix *m = gf_matrix_create(rows, cols);

	if (!m)
		return NULL;

	for (int r = 0; r < rows; r++)
		for (int c = 0; c < cols; c++)
			gf_matrix_set(m, r, c, gf_pow((uint8_t)r, (uint8_t)c));

	return m;
}
