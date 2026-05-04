/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * moj_bench -- algorithm-level microbenchmark for the Mojette inverse.
 *
 * Times moj_inverse_peel vs moj_inverse_gd on the dense reduced-grid
 * shape (every row missing) and the sparse-failures shape (k data
 * rows, n_missing < k missing).  Reports min and median per cell over
 * RUNS iterations after WARMUP iterations.
 *
 * This is the algorithm-level comparison referenced by the gd
 * landing in 2026-05-04.  Unlike the system-level ec_benchmark.sh
 * which goes through the full reffsd MDS + DSes + NFS stack and is
 * dominated by RPC round-trip time, this microbenchmark isolates
 * the inverse-algorithm cost.
 *
 * Usage:
 *   moj_bench
 *
 * Times are in microseconds.  No external dependencies; links only
 * against libreffs_ec.la.
 */

#include "mojette.h"
#include "reffs/ec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RUNS 20
#define WARMUP 3

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static struct moj_projection **
alloc_projs(const struct moj_direction *dirs, int n, int P, int Q)
{
	struct moj_projection **projs = calloc((size_t)n, sizeof(*projs));

	for (int i = 0; i < n; i++)
		projs[i] = moj_projection_create(
			moj_projection_size(dirs[i].md_p, dirs[i].md_q, P, Q));
	return projs;
}

static void free_projs(struct moj_projection **projs, int n)
{
	for (int i = 0; i < n; i++)
		moj_projection_destroy(projs[i]);
	free(projs);
}

static void
bench_dense(const char *label, int P, int Q, int n,
	    const struct moj_direction *dirs)
{
	uint64_t *grid = calloc((size_t)P * Q, sizeof(uint64_t));
	uint64_t *recovered = calloc((size_t)P * Q, sizeof(uint64_t));

	for (int i = 0; i < P * Q; i++)
		grid[i] = (uint64_t)(i * 0x9E3779B97F4A7C15ULL + 0xDEADBEEF);

	uint64_t fwd_ns = 0;
	uint64_t peel[RUNS];
	uint64_t gd[RUNS];

	for (int run = 0; run < WARMUP + RUNS; run++) {
		struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

		uint64_t t0 = now_ns();

		moj_forward(grid, P, Q, dirs, n, projs);
		uint64_t t1 = now_ns();

		if (run >= WARMUP)
			fwd_ns += (t1 - t0);

		struct moj_projection **projs2 = alloc_projs(dirs, n, P, Q);

		moj_forward(grid, P, Q, dirs, n, projs2);

		t0 = now_ns();
		int rp = moj_inverse_peel(recovered, P, Q, dirs, n, projs);

		t1 = now_ns();
		if (run >= WARMUP)
			peel[run - WARMUP] = (t1 - t0);
		if (rp != 0)
			fprintf(stderr, "FAIL peel %s ret=%d\n", label, rp);

		t0 = now_ns();
		int rg = moj_inverse_gd(recovered, P, Q, dirs, n, projs2);

		t1 = now_ns();
		if (run >= WARMUP)
			gd[run - WARMUP] = (t1 - t0);
		if (rg != 0)
			fprintf(stderr, "FAIL gd %s ret=%d\n", label, rg);

		free_projs(projs, n);
		free_projs(projs2, n);
	}

	qsort(peel, RUNS, sizeof(uint64_t), cmp_u64);
	qsort(gd, RUNS, sizeof(uint64_t), cmp_u64);

	double fwd_us = (double)fwd_ns / 1000.0 / (double)RUNS;
	double peel_med = (double)peel[RUNS / 2] / 1000.0;
	double gd_med = (double)gd[RUNS / 2] / 1000.0;
	double peel_min = (double)peel[0] / 1000.0;
	double gd_min = (double)gd[0] / 1000.0;

	printf("%-32s P=%5d Q=%2d n=%2d  fwd=%9.1f us  peel=%9.1f / %9.1f us  gd=%8.1f / %8.1f us  speedup=%7.2fx\n",
	       label, P, Q, n, fwd_us, peel_min, peel_med, gd_min, gd_med,
	       peel_med / gd_med);

	free(recovered);
	free(grid);
}

static void
bench_sparse(const char *label, int P, int k, int m,
	     const struct moj_direction *dirs_n, const int *missing,
	     int n_missing)
{
	uint64_t *grid_orig = calloc((size_t)P * k, sizeof(uint64_t));

	for (int i = 0; i < P * k; i++)
		grid_orig[i] = (uint64_t)(i * 0x9E3779B97F4A7C15ULL + 0xCAFEBABE);

	struct moj_projection **parity = alloc_projs(dirs_n + k, m, P, k);

	moj_forward(grid_orig, P, k, dirs_n + k, m, parity);

	bool *missing_set = calloc((size_t)k, sizeof(bool));

	for (int i = 0; i < n_missing; i++)
		missing_set[missing[i]] = true;

	uint64_t peel[RUNS];
	uint64_t gd[RUNS];

	for (int run = 0; run < WARMUP + RUNS; run++) {
		uint64_t *grid_p = calloc((size_t)P * k, sizeof(uint64_t));
		uint64_t *grid_g = calloc((size_t)P * k, sizeof(uint64_t));

		for (int row = 0; row < k; row++) {
			if (!missing_set[row]) {
				memcpy(grid_p + (size_t)row * P,
				       grid_orig + (size_t)row * P,
				       (size_t)P * sizeof(uint64_t));
				memcpy(grid_g + (size_t)row * P,
				       grid_orig + (size_t)row * P,
				       (size_t)P * sizeof(uint64_t));
			}
		}

		struct moj_projection **projs_p =
			alloc_projs(dirs_n + k, m, P, k);
		struct moj_projection **projs_g =
			alloc_projs(dirs_n + k, m, P, k);

		for (int i = 0; i < m; i++) {
			memcpy(projs_p[i]->mp_bins, parity[i]->mp_bins,
			       (size_t)projs_p[i]->mp_nbins * sizeof(uint64_t));
			memcpy(projs_g[i]->mp_bins, parity[i]->mp_bins,
			       (size_t)projs_g[i]->mp_nbins * sizeof(uint64_t));
		}

		uint64_t t0 = now_ns();
		int rp = moj_inverse_peel_sparse(grid_p, P, k, dirs_n + k, m,
						 projs_p, missing,
						 n_missing);
		uint64_t t1 = now_ns();

		if (run >= WARMUP)
			peel[run - WARMUP] = (t1 - t0);
		if (rp != 0)
			fprintf(stderr, "FAIL peel_sparse %s ret=%d\n", label,
				rp);

		t0 = now_ns();
		int rg = moj_inverse_gd_sparse(grid_g, P, k, dirs_n + k, m,
					       projs_g, missing, n_missing);

		t1 = now_ns();
		if (run >= WARMUP)
			gd[run - WARMUP] = (t1 - t0);
		if (rg != 0)
			fprintf(stderr, "FAIL gd_sparse %s ret=%d\n", label,
				rg);

		free_projs(projs_p, m);
		free_projs(projs_g, m);
		free(grid_p);
		free(grid_g);
	}

	qsort(peel, RUNS, sizeof(uint64_t), cmp_u64);
	qsort(gd, RUNS, sizeof(uint64_t), cmp_u64);

	double peel_med = (double)peel[RUNS / 2] / 1000.0;
	double gd_med = (double)gd[RUNS / 2] / 1000.0;
	double peel_min = (double)peel[0] / 1000.0;
	double gd_min = (double)gd[0] / 1000.0;

	printf("%-32s P=%5d k=%2d m=%2d miss=%d  peel=%9.1f / %9.1f us  gd=%8.1f / %8.1f us  speedup=%7.2fx\n",
	       label, P, k, m, n_missing, peel_min, peel_med, gd_min, gd_med,
	       peel_med / gd_med);

	free(missing_set);
	free_projs(parity, m);
	free(grid_orig);
}

/* ------------------------------------------------------------------ */
/* Codec-level bench (RS vs Mojette-sys vs Mojette-nonsys, peel/gd)    */
/* ------------------------------------------------------------------ */

static void
bench_codec(const char *label, struct ec_codec *codec, int k, int m,
	    size_t shard_len, const int *missing, int n_missing,
	    bool force_gd)
{
	uint8_t *data[k];
	uint8_t *parity[m];
	uint8_t *orig[k];
	uint8_t *shards[k + m];
	bool present[k + m];

	/* In non-systematic codecs the data buffers are overwritten with
	 * projections that may differ in size across shards; take max
	 * over data shards and over parity shards.  NULL ec_shard_size
	 * means uniform shards (= shard_len). */
	size_t data_buf_len = shard_len;
	size_t parity_buf_len = shard_len;

	if (codec->ec_shard_size) {
		for (int i = 0; i < k; i++) {
			size_t s = codec->ec_shard_size(codec, i, shard_len);

			if (s > data_buf_len)
				data_buf_len = s;
		}
		for (int i = 0; i < m; i++) {
			size_t s = codec->ec_shard_size(codec, k + i,
							shard_len);

			if (s > parity_buf_len)
				parity_buf_len = s;
		}
	}

	/* aligned_alloc requires size to be a multiple of alignment. */
	size_t data_alloc = (data_buf_len + 63) & ~(size_t)63;
	size_t parity_alloc = (parity_buf_len + 63) & ~(size_t)63;

	for (int i = 0; i < k; i++) {
		data[i] = aligned_alloc(64, data_alloc);
		orig[i] = malloc(shard_len);
		for (size_t j = 0; j < shard_len; j++)
			orig[i][j] = (uint8_t)(i * 31 + j * 7 + 5);
		memcpy(data[i], orig[i], shard_len);
	}
	for (int i = 0; i < m; i++)
		parity[i] = aligned_alloc(64, parity_alloc);

	moj_force_gd(force_gd);

	uint64_t enc[RUNS];
	uint64_t dec[RUNS];

	for (int run = 0; run < WARMUP + RUNS; run++) {
		/* Restore data shards (encode may overwrite for nonsys). */
		for (int i = 0; i < k; i++) {
			memset(data[i], 0, data_alloc);
			memcpy(data[i], orig[i], shard_len);
		}

		uint64_t t0 = now_ns();

		codec->ec_encode(codec, data, parity, shard_len);
		uint64_t t1 = now_ns();

		if (run >= WARMUP)
			enc[run - WARMUP] = (t1 - t0);

		for (int i = 0; i < k; i++)
			shards[i] = data[i];
		for (int i = 0; i < m; i++)
			shards[k + i] = parity[i];
		for (int i = 0; i < k + m; i++)
			present[i] = true;

		for (int i = 0; i < n_missing; i++) {
			present[missing[i]] = false;
			memset(data[missing[i]], 0, shard_len);
		}

		t0 = now_ns();
		int rd = codec->ec_decode(codec, shards, present, shard_len);

		t1 = now_ns();
		if (run >= WARMUP)
			dec[run - WARMUP] = (t1 - t0);
		if (rd != 0)
			fprintf(stderr, "FAIL %s decode ret=%d\n", label, rd);

		/* Verify recovered data matches original. */
		for (int i = 0; i < n_missing; i++) {
			if (memcmp(data[missing[i]], orig[missing[i]],
				   shard_len) != 0) {
				fprintf(stderr,
					"FAIL %s verify shard %d\n",
					label, missing[i]);
			}
		}
	}

	moj_force_gd(false);

	qsort(enc, RUNS, sizeof(uint64_t), cmp_u64);
	qsort(dec, RUNS, sizeof(uint64_t), cmp_u64);

	double enc_med = (double)enc[RUNS / 2] / 1000.0;
	double dec_med = (double)dec[RUNS / 2] / 1000.0;
	double enc_min = (double)enc[0] / 1000.0;
	double dec_min = (double)dec[0] / 1000.0;

	printf("%-36s shard=%5zu  encode=%9.1f / %9.1f us  decode=%9.1f / %9.1f us\n",
	       label, shard_len, enc_min, enc_med, dec_min, dec_med);

	for (int i = 0; i < k; i++) {
		free(data[i]);
		free(orig[i]);
	}
	for (int i = 0; i < m; i++)
		free(parity[i]);
}

static void
bench_codec_set(int k, int m, size_t shard_len, const int *missing,
		int n_missing, const char *miss_label)
{
	struct ec_codec *rs = ec_rs_create(k, m);
	struct ec_codec *msys = ec_mojette_sys_create(k, m);
	struct ec_codec *mns = ec_mojette_nonsys_create(k, m);

	char buf[64];

	snprintf(buf, sizeof(buf), "RS %d+%d %s", k, m, miss_label);
	bench_codec(buf, rs, k, m, shard_len, missing, n_missing, false);

	snprintf(buf, sizeof(buf), "Mojette-sys %d+%d %s peel", k, m,
		 miss_label);
	bench_codec(buf, msys, k, m, shard_len, missing, n_missing, false);
	snprintf(buf, sizeof(buf), "Mojette-sys %d+%d %s gd", k, m,
		 miss_label);
	bench_codec(buf, msys, k, m, shard_len, missing, n_missing, true);

	snprintf(buf, sizeof(buf), "Mojette-nonsys %d+%d %s peel", k, m,
		 miss_label);
	bench_codec(buf, mns, k, m, shard_len, missing, n_missing, false);
	snprintf(buf, sizeof(buf), "Mojette-nonsys %d+%d %s gd", k, m,
		 miss_label);
	bench_codec(buf, mns, k, m, shard_len, missing, n_missing, true);

	ec_codec_destroy(rs);
	ec_codec_destroy(msys);
	ec_codec_destroy(mns);
}

int main(void)
{
	printf("Mojette inverse algorithm-level microbenchmark\n");
	printf("RUNS=%d (after %d warmup), times in us, format min / median\n\n",
	       RUNS, WARMUP);

	printf("=== dense (n_missing == Q, all rows unknown) ===\n");

	struct moj_direction d_4x4[] = {
		{ -2, 1 }, { -1, 1 }, { 1, 1 }, { 2, 1 }
	};
	struct moj_direction d_8x8[] = {
		{ -4, 1 }, { -3, 1 }, { -2, 1 }, { -1, 1 },
		{ 1, 1 },  { 2, 1 },  { 3, 1 },  { 4, 1 }
	};
	struct moj_direction d_2x2[] = { { -1, 1 }, { 1, 1 } };

	bench_dense("dense 16x4 (RS-4+2-ish)", 16, 4, 4, d_4x4);
	bench_dense("dense 64x4", 64, 4, 4, d_4x4);
	bench_dense("dense 512x4 (4K shard, k=4)", 512, 4, 4, d_4x4);
	bench_dense("dense 4096x4 (32K shard, k=4)", 4096, 4, 4, d_4x4);
	bench_dense("dense 64x8", 64, 8, 8, d_8x8);
	bench_dense("dense 512x8 (4K shard, k=8)", 512, 8, 8, d_8x8);
	bench_dense("dense 4096x8 (32K shard, k=8)", 4096, 8, 8, d_8x8);
	bench_dense("dense 3072x2 (24K codec demo)", 3072, 2, 2, d_2x2);

	printf("\n=== sparse (k=4, m=2, lose 2 data rows) ===\n");
	struct moj_direction d_n6[] = {
		{ -3, 1 }, { -2, 1 }, { -1, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 }
	};
	int miss03[] = { 0, 3 };
	int miss12[] = { 1, 2 };

	bench_sparse("sys k=4 m=2 P=512 lose{0,3}", 512, 4, 2, d_n6, miss03,
		     2);
	bench_sparse("sys k=4 m=2 P=512 lose{1,2}", 512, 4, 2, d_n6, miss12,
		     2);
	bench_sparse("sys k=4 m=2 P=4096 lose{0,3}", 4096, 4, 2, d_n6, miss03,
		     2);
	bench_sparse("sys k=4 m=2 P=4096 lose{1,2}", 4096, 4, 2, d_n6, miss12,
		     2);

	printf("\n=== sparse (k=8, m=2, lose 2 data rows) ===\n");
	struct moj_direction d_n10[] = {
		{ -5, 1 }, { -4, 1 }, { -3, 1 }, { -2, 1 }, { -1, 1 },
		{ 1, 1 },  { 2, 1 },  { 3, 1 },  { 4, 1 },  { 5, 1 }
	};
	int miss07[] = { 0, 7 };

	bench_sparse("sys k=8 m=2 P=512 lose{0,7}", 512, 8, 2, d_n10, miss07,
		     2);
	bench_sparse("sys k=8 m=2 P=4096 lose{0,7}", 4096, 8, 2, d_n10, miss07,
		     2);

	printf("\n=== codec-level (full encode + decode, no NFS) ===\n");
	int miss_4_03[] = { 0, 3 };
	int miss_4_12[] = { 1, 2 };
	int miss_8_07[] = { 0, 7 };
	int miss_one[] = { 0 };

	/* 4 KB shard, k=4 m=2 (typical reffs ec_demo default). */
	printf("\n-- 4+2, shard=4096 (default ec_demo), 1 data-shard loss --\n");
	bench_codec_set(4, 2, 4096, miss_one, 1, "lose{0}");
	printf("\n-- 4+2, shard=4096, 2 data-shard losses --\n");
	bench_codec_set(4, 2, 4096, miss_4_03, 2, "lose{0,3}");
	bench_codec_set(4, 2, 4096, miss_4_12, 2, "lose{1,2}");

	/* 32 KB shard, k=4 m=2 (larger payload). */
	printf("\n-- 4+2, shard=32768, 2 data-shard losses --\n");
	bench_codec_set(4, 2, 32768, miss_4_03, 2, "lose{0,3}");

	/* 4 KB shard, k=8 m=2. */
	printf("\n-- 8+2, shard=4096, 2 data-shard losses --\n");
	bench_codec_set(8, 2, 4096, miss_8_07, 2, "lose{0,7}");

	/* 32 KB shard, k=8 m=2. */
	printf("\n-- 8+2, shard=32768, 2 data-shard losses --\n");
	bench_codec_set(8, 2, 32768, miss_8_07, 2, "lose{0,7}");

	return 0;
}
