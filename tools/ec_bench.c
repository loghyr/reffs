/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * ec_bench -- encoding-only microbenchmark for lib/ec/ backends.
 *
 * Times ec_encode + ec_decode-with-erasures on RS Vandermonde and
 * SnapRAID Cauchy across a small (k, m, shard_size) sweep.  Reports
 * throughput in MB/s per encoding per cell.
 *
 * Motivation: an IETF-126 pre-meeting review flagged the RS
 * numbers as "bad".  The existing ec_benchmark.sh paths bake in
 * reffsd + NFS RPC + TLS + layout-fetch cost, so a throughput
 * regression / advantage on the encoding side is buried under
 * transport noise.  This tool isolates the encoding call from
 * every non-arithmetic cost -- the same shape moj_bench uses for
 * Mojette.
 *
 * Also answers the "run ec_demo on local files as a baseline"
 * review ask at a scope small enough to iterate on: same
 * pointer-array + heap-shard shape ec_pipeline uses, no file I/O,
 * no transport, no configuration.
 *
 * Usage:
 *   ec_bench [--iters N] [--warmup N] [--sizes SIZE[,SIZE...]]
 *
 * Times are per-iteration wall-clock, throughput is
 * (k * shard_size) / t_encode for encode and
 * (k * shard_size) / t_decode for decode -- reporting the byte
 * count of the recovered data payload, not shards-plus-parity.
 *
 * Links only against libreffs_ec.la (no reffs runtime, no NFS,
 * no autotools DS setup).
 */

#include "reffs/ec.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_ITERS 200
#define DEFAULT_WARMUP 5

/* Default sweep -- override with --sizes.  Multiples of 64 (SnapRAID
 * requires it; RS accepts any size, so 64-multiples keep both paths
 * comparable at the same shard_size). */
static const size_t DEFAULT_SIZES[] = { 4096, 16384, 65536, 262144, 1048576, 0 };

/* (k, m) pairs to sweep -- RAID-6 shape, k=6 m=2 for wider stripes,
 * k=8 m=4 for max-parity Cauchy-only coverage. */
struct geom {
	int k;
	int m;
};
static const struct geom GEOMS[] = { { 4, 1 }, { 4, 2 }, { 6, 2 },
				     { 8, 1 }, { 8, 4 }, { 0, 0 } };

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Fill data shards with a deterministic pattern. */
static void fill_shards(uint8_t **data, int k, size_t shard_len)
{
	for (int i = 0; i < k; i++)
		for (size_t j = 0; j < shard_len; j++)
			data[i][j] = (uint8_t)((i * 37 + j * 7 + 13) & 0xff);
}

/* MB/s of *data payload* recovered/encoded, given nanoseconds
 * elapsed and (k * shard_len) bytes of payload. */
static double mb_per_sec(int k, size_t shard_len, uint64_t elapsed_ns)
{
	if (elapsed_ns == 0)
		return 0.0;
	double bytes = (double)k * (double)shard_len;
	double secs = (double)elapsed_ns / 1e9;

	return (bytes / secs) / (1024.0 * 1024.0);
}

/* Time encode over `iters` runs.  Returns min ns per iter. */
static uint64_t time_encode(struct ec_encoding *c, uint8_t **data,
			    uint8_t **parity, size_t shard_len, int iters,
			    int warmup)
{
	for (int i = 0; i < warmup; i++)
		c->ec_encode(c, data, parity, shard_len);

	uint64_t best = UINT64_MAX;

	for (int i = 0; i < iters; i++) {
		uint64_t t0 = now_ns();

		c->ec_encode(c, data, parity, shard_len);
		uint64_t elapsed = now_ns() - t0;

		if (elapsed < best)
			best = elapsed;
	}
	return best;
}

/* Time decode with shard[erase_idx] dropped.  All shards are set up
 * fresh each iter (memcpy from snapshot) so consecutive iters do not
 * observe already-recovered state. */
static uint64_t time_decode(struct ec_encoding *c, uint8_t **shards,
			    uint8_t **snapshot, int k, int m, int erase_idx,
			    size_t shard_len, int iters, int warmup)
{
	int n = k + m;
	bool *present = calloc(n, sizeof(*present));

	for (int i = 0; i < n; i++)
		present[i] = true;
	present[erase_idx] = false;

	/* Warmup */
	for (int w = 0; w < warmup; w++) {
		for (int i = 0; i < n; i++)
			memcpy(shards[i], snapshot[i], shard_len);
		memset(shards[erase_idx], 0, shard_len);
		c->ec_decode(c, shards, present, shard_len);
	}

	uint64_t best = UINT64_MAX;

	for (int i = 0; i < iters; i++) {
		for (int j = 0; j < n; j++)
			memcpy(shards[j], snapshot[j], shard_len);
		memset(shards[erase_idx], 0, shard_len);

		uint64_t t0 = now_ns();

		c->ec_decode(c, shards, present, shard_len);
		uint64_t elapsed = now_ns() - t0;

		if (elapsed < best)
			best = elapsed;
	}
	free(present);
	return best;
}

/*
 * Query per-shard byte size for an encoder.  Encoders with
 * uniform shard sizes (RS Vandermonde, SnapRAID, ISA-L, XOR,
 * Linux md) leave ec_shard_size NULL and every shard is
 * shard_len bytes.  Non-systematic Mojette (and systematic
 * Mojette's parity shards) use ec_shard_size to report larger
 * projection-sized buffers.  Round up to 64-byte alignment
 * for aligned_alloc.
 */
static size_t bench_shard_bytes(struct ec_encoding *c, int shard_idx,
				size_t data_shard_len)
{
	size_t s = c->ec_shard_size ?
			   c->ec_shard_size(c, shard_idx, data_shard_len) :
			   data_shard_len;

	return (s + 63) & ~(size_t)63;
}

static void bench_one(const char *label, struct ec_encoding *c, int k, int m,
		      size_t shard_len, int iters, int warmup)
{
	int n = k + m;
	uint8_t **data = calloc(k, sizeof(*data));
	uint8_t **parity = calloc(m, sizeof(*parity));
	uint8_t **shards = calloc(n, sizeof(*shards));
	uint8_t **snapshot = calloc(n, sizeof(*snapshot));

	for (int i = 0; i < k; i++) {
		size_t sz = bench_shard_bytes(c, i, shard_len);

		data[i] = aligned_alloc(64, sz);
		shards[i] = aligned_alloc(64, sz);
		snapshot[i] = aligned_alloc(64, sz);
	}
	for (int i = 0; i < m; i++) {
		size_t sz = bench_shard_bytes(c, k + i, shard_len);

		parity[i] = aligned_alloc(64, sz);
		shards[k + i] = aligned_alloc(64, sz);
		snapshot[k + i] = aligned_alloc(64, sz);
	}

	fill_shards(data, k, shard_len);

	uint64_t enc_ns =
		time_encode(c, data, parity, shard_len, iters, warmup);

	/* Snapshot the encoded shards so decode sees an authentic input.
	 * Data shards are shard_len; parity shards use the encoder's
	 * ec_shard_size (Mojette non-systematic can be larger). */
	for (int i = 0; i < k; i++)
		memcpy(snapshot[i], data[i], shard_len);
	for (int i = 0; i < m; i++) {
		size_t sz = c->ec_shard_size ?
				    c->ec_shard_size(c, k + i, shard_len) :
				    shard_len;

		memcpy(snapshot[k + i], parity[i], sz);
	}

	/* Decode with one data shard missing (index 1 -- avoid 0 which
	 * often triggers a fast-path in some encodings). */
	uint64_t dec_ns = time_decode(c, shards, snapshot, k, m, 1, shard_len,
				      iters, warmup);

	printf("%-14s k=%d m=%d size=%7zu  enc %6.1f MB/s (%6" PRIu64
	       " ns)  dec %6.1f MB/s (%6" PRIu64 " ns)\n",
	       label, k, m, shard_len, mb_per_sec(k, shard_len, enc_ns), enc_ns,
	       mb_per_sec(k, shard_len, dec_ns), dec_ns);

	for (int i = 0; i < k; i++) {
		free(data[i]);
		free(shards[i]);
		free(snapshot[i]);
	}
	for (int i = 0; i < m; i++) {
		free(parity[i]);
		free(shards[k + i]);
		free(snapshot[k + i]);
	}
	free(data);
	free(parity);
	free(shards);
	free(snapshot);
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: ec_bench [--iters N] [--warmup N] [--sizes SIZE[,SIZE...]]\n"
		"\n"
		"Microbenchmark of RS Vandermonde and SnapRAID Cauchy encoding.\n"
		"No file I/O, no NFS.  Isolates arithmetic cost.\n"
		"\n"
		"  --iters N     iterations per cell (default %d)\n"
		"  --warmup N    warmup iters before timing (default %d)\n"
		"  --sizes S,S,S comma-separated shard sizes in bytes, each a multiple of 64\n"
		"                (default 4096,16384,65536,262144,1048576)\n",
		DEFAULT_ITERS, DEFAULT_WARMUP);
}

int main(int argc, char **argv)
{
	int iters = DEFAULT_ITERS;
	int warmup = DEFAULT_WARMUP;
	size_t user_sizes[32];
	int n_user_sizes = 0;
	const size_t *sizes = DEFAULT_SIZES;

	static struct option opts[] = {
		{ "iters", required_argument, NULL, 'i' },
		{ "warmup", required_argument, NULL, 'w' },
		{ "sizes", required_argument, NULL, 's' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "i:w:s:h", opts, NULL)) != -1) {
		switch (opt) {
		case 'i':
			iters = atoi(optarg);
			break;
		case 'w':
			warmup = atoi(optarg);
			break;
		case 's': {
			char *tok, *rest = optarg;

			while ((tok = strtok_r(rest, ",", &rest))) {
				if (n_user_sizes >= 31)
					break;
				long v = strtol(tok, NULL, 0);

				if (v <= 0 || (v & 63)) {
					fprintf(stderr,
						"ec_bench: shard size %ld must be a positive multiple of 64\n",
						v);
					return 1;
				}
				user_sizes[n_user_sizes++] = (size_t)v;
			}
			user_sizes[n_user_sizes] = 0;
			sizes = user_sizes;
			break;
		}
		case 'h':
		default:
			usage();
			return opt == 'h' ? 0 : 1;
		}
	}

	printf("ec_bench: iters=%d warmup=%d\n", iters, warmup);
	printf("Reporting MB/s of data payload (k * shard_size) per iteration,\n"
	       "using min-of-iters wall clock.  Higher is better.\n\n");

	for (const struct geom *g = GEOMS; g->k > 0; g++) {
		struct ec_encoding *rs = ec_rs_create(g->k, g->m);
		struct ec_encoding *sr = ec_snapraid_create(g->k, g->m);
		struct ec_encoding *il = ec_isa_l_create(g->k, g->m);
		/* XOR is single-parity only; run it only at m=1 cells.
		 * NULL here just means "skip the XOR row for this geom". */
		struct ec_encoding *xr = (g->m == 1) ? ec_xor_create(g->k) :
						       NULL;
		struct ec_encoding *md =
			(g->m == 2) ? ec_linux_md_create(g->k) : NULL;
		struct ec_encoding *ms = ec_mojette_sys_create(g->k, g->m);
		struct ec_encoding *mn = ec_mojette_nonsys_create(g->k, g->m);

		if (!rs || !sr || !il) {
			fprintf(stderr,
				"ec_bench: skip k=%d m=%d (create returned NULL: rs=%p sr=%p il=%p)\n",
				g->k, g->m, (void *)rs, (void *)sr, (void *)il);
			if (rs)
				ec_encoding_destroy(rs);
			if (sr)
				ec_encoding_destroy(sr);
			if (il)
				ec_encoding_destroy(il);
			if (xr)
				ec_encoding_destroy(xr);
			if (md)
				ec_encoding_destroy(md);
			if (ms)
				ec_encoding_destroy(ms);
			if (mn)
				ec_encoding_destroy(mn);
			continue;
		}
		for (const size_t *sz = sizes; *sz; sz++) {
			bench_one("rs-vand", rs, g->k, g->m, *sz, iters,
				  warmup);
			bench_one("snapraid-cauchy", sr, g->k, g->m, *sz, iters,
				  warmup);
			bench_one("isa-l-rs", il, g->k, g->m, *sz, iters,
				  warmup);
			if (xr)
				bench_one("xor-parity", xr, g->k, 1, *sz, iters,
					  warmup);
			if (md)
				bench_one("linux-md-raid6", md, g->k, 2, *sz,
					  iters, warmup);
			if (ms)
				bench_one("mojette-sys", ms, g->k, g->m, *sz,
					  iters, warmup);
			if (mn)
				bench_one("mojette-nonsys", mn, g->k, g->m, *sz,
					  iters, warmup);
		}
		ec_encoding_destroy(rs);
		ec_encoding_destroy(sr);
		ec_encoding_destroy(il);
		if (xr)
			ec_encoding_destroy(xr);
		if (md)
			ec_encoding_destroy(md);
		if (ms)
			ec_encoding_destroy(ms);
		if (mn)
			ec_encoding_destroy(mn);
		printf("\n");
	}
	return 0;
}
