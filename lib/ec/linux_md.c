/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Linux md/raid6 P+Q double-parity encoding wrapper, over the
 * vendored raid6 library (lib/ec/linux-md-raid/, GPL-2.0-or-later --
 * see NOTICE.md).
 *
 * Wire-side: FFV2_ENCODING_LINUX_MD_RAID (0x8, proposed).  Field
 * GF(2^8) with primitive polynomial 0x1d.  Fixed at double-parity
 * (m=2): P = XOR of all data, Q = 2^i-weighted sum of data_i in
 * GF(2^8).  Single-parity (m=1) is covered by XOR_PARITY (0x7),
 * whose wire format is identical to Linux md's P-only case.
 *
 * See ~/Documents/reffs-docs/ffv2-encoding-menu.md
 * FFV2_ENCODING_LINUX_MD_RAID for the MTI-candidate rationale and
 * the wire-compat verification owed against SnapRAID's first two
 * Cauchy rows.
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/ec.h"

/*
 * pq.h defines an inline raid6_get_zero_page() that returns
 * `void *` from a `const char[]` global -- fine under the -w in
 * the vendored sub-library, but flagged as
 * -Wincompatible-pointer-types-discards-qualifiers here.  Only
 * suppress that specific warning around the include; do not
 * touch the vendored header.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored \
	"-Wincompatible-pointer-types-discards-qualifiers"
#include "linux-md-raid/linux/raid/pq.h"
#pragma GCC diagnostic pop

#define LINUX_MD_MAX_DATA 253 /* k + 2 <= 255 for size + P + Q indexing */

/*
 * The vendored raid6 SIMD dispatchers (sse2, avx2, avx512, neon)
 * are hand-unrolled around n >= 4 disks (2 data + P + Q or more).
 * At k=1, n=3 disks the SSE2/AVX* paths read past the k-th data
 * shard's end and either segfault or produce wrong bytes (mana's
 * scalar int64x2 path handles it fine; shadow's avx2x2 does not).
 * k=1 has no practical workload -- a single data shard with P and
 * Q is just a triple-mirror -- so cap the wrapper at k >= 2 and
 * let callers use ec_mirror_create() for the degenerate case.
 */
#define LINUX_MD_MIN_DATA 2

/*
 * Userspace shim.  The vendored recov.c dereferences the kernel
 * symbol raid6_empty_zero_page (the shared zero page) when its
 * raid6_get_zero_page() is called at algo-selection time.  In
 * userspace there is no such global -- provide one page of zeros
 * here to satisfy the linker, page-aligned per pq.h's PAGE_SIZE.
 */
const char raid6_empty_zero_page[4096] __attribute__((aligned(4096)));

/*
 * raid6_select_algo() populates the raid6_call global dispatch
 * table and the raid6_2data_recov / raid6_datap_recov function
 * pointers.  It must run once before any encode/decode.  Wrapped
 * in pthread_once so concurrent ec_linux_md_create() calls do
 * not race on the init.
 */
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void linux_md_global_init(void)
{
	raid6_select_algo();
}

static int linux_md_encode(struct ec_encoding *encoding, uint8_t **data,
			   uint8_t **parity, size_t shard_len)
{
	int k = encoding->ec_k;

	if (shard_len == 0)
		return 0;
	if (parity == NULL)
		return -EINVAL;

	/*
	 * raid6_call.gen_syndrome takes an array of (k+2) pointers,
	 * with parity slots at ptrs[k] = P and ptrs[k+1] = Q.  Build
	 * the array from data[0..k-1] + parity[0..1].
	 */
	int n = k + 2;
	void **ptrs = calloc((size_t)n, sizeof(void *));

	if (!ptrs)
		return -ENOMEM;
	for (int i = 0; i < k; i++)
		ptrs[i] = data[i];
	ptrs[k] = parity[0];
	ptrs[k + 1] = parity[1];

	raid6_call.gen_syndrome(n, shard_len, ptrs);
	free(ptrs);
	return 0;
}

/*
 * Regenerate parity[0] (= P = XOR of all data).  Used when only
 * P is missing and Linux md's raid6_dual_recov "data+Q" case
 * ("NOT IMPLEMENTED - equivalent to RAID-5") does not apply.
 */
static void xor_all_data_into(uint8_t *out, uint8_t **data, int k,
			      size_t shard_len)
{
	memcpy(out, data[0], shard_len);
	for (int i = 1; i < k; i++)
		for (size_t p = 0; p < shard_len; p++)
			out[p] ^= data[i][p];
}

/*
 * Recover data[missing_data] using P and the surviving data
 * shards.  P XOR (all surviving data) == the missing data shard,
 * because P is defined as XOR of all data.
 */
static void raid5_style_data_recover(uint8_t *out, uint8_t **data, int k,
				     int missing_data, const uint8_t *p,
				     size_t shard_len)
{
	memcpy(out, p, shard_len);
	for (int i = 0; i < k; i++) {
		if (i == missing_data)
			continue;
		for (size_t x = 0; x < shard_len; x++)
			out[x] ^= data[i][x];
	}
}

static int linux_md_decode(struct ec_encoding *encoding, uint8_t **shards,
			   const bool *present, size_t shard_len)
{
	int k = encoding->ec_k;
	int n = k + 2;
	int p_idx = k;

	/* q_idx is implicit at k+1; the else-branch below handles the
	 * b == q_idx case without naming it. */

	if (shard_len == 0)
		return 0;

	int miss_count = 0;
	int miss[2] = { -1, -1 };

	for (int i = 0; i < n; i++) {
		if (!present[i]) {
			if (miss_count < 2)
				miss[miss_count] = i;
			miss_count++;
		}
	}
	if (miss_count == 0)
		return 0;
	if (miss_count > 2)
		return -EIO;

	/* Assemble the ptrs[] array the raid6 library expects. */
	void **ptrs = calloc((size_t)n, sizeof(void *));

	if (!ptrs)
		return -ENOMEM;
	for (int i = 0; i < n; i++)
		ptrs[i] = shards[i];

	if (miss_count == 1) {
		int m0 = miss[0];

		if (m0 < k) {
			/* One data missing: RAID-5 XOR against P. */
			uint8_t **data = (uint8_t **)shards;

			raid5_style_data_recover(shards[m0], data, k, m0,
						 shards[p_idx], shard_len);
		} else if (m0 == p_idx) {
			/* P missing: recompute XOR of data into shards[p]. */
			xor_all_data_into(shards[p_idx], (uint8_t **)shards, k,
					  shard_len);
		} else {
			/* Q missing: regen syndrome overwrites P and Q;
			 * P is already correct so its overwrite is a no-op. */
			raid6_call.gen_syndrome(n, shard_len, ptrs);
		}
		free(ptrs);
		return 0;
	}

	/* miss_count == 2 -- five sub-cases. */
	int a = miss[0];
	int b = miss[1];

	if (a >= p_idx) {
		/* Both parities missing (P + Q).  Regenerate from data. */
		raid6_call.gen_syndrome(n, shard_len, ptrs);
	} else if (b < p_idx) {
		/* Two data missing.  raid6_2data_recov -- both parities intact. */
		raid6_2data_recov(n, shard_len, a, b, ptrs);
	} else if (b == p_idx) {
		/* One data + P missing.  raid6_datap_recov reconstructs
		 * data[a] using Q.  Then regenerate P. */
		raid6_datap_recov(n, shard_len, a, ptrs);
		xor_all_data_into(shards[p_idx], (uint8_t **)shards, k,
				  shard_len);
	} else {
		/* One data + Q missing (b == q_idx).  raid6_dual_recov's
		 * "data+Q" branch is NOT IMPLEMENTED in the vendored
		 * recov.c ("equivalent to RAID-5" comment), so do it by
		 * hand: recover data[a] from P, then regenerate Q via
		 * gen_syndrome (P is correct so its overwrite is a no-op). */
		uint8_t **data = (uint8_t **)shards;

		raid5_style_data_recover(shards[a], data, k, a, shards[p_idx],
					 shard_len);
		raid6_call.gen_syndrome(n, shard_len, ptrs);
	}
	free(ptrs);
	return 0;
}

static void linux_md_destroy(struct ec_encoding *encoding)
{
	free(encoding);
}

struct ec_encoding *ec_linux_md_create(int k)
{
	if (k < LINUX_MD_MIN_DATA || k > LINUX_MD_MAX_DATA)
		return NULL;

	pthread_once(&g_init_once, linux_md_global_init);

	struct ec_encoding *encoding = calloc(1, sizeof(*encoding));

	if (!encoding)
		return NULL;

	encoding->ec_name = "linux-md-raid6";
	encoding->ec_k = k;
	encoding->ec_m = 2;
	encoding->ec_encode = linux_md_encode;
	encoding->ec_decode = linux_md_decode;
	encoding->ec_shard_size = NULL;
	encoding->ec_destroy = linux_md_destroy;
	encoding->ec_private = NULL;
	return encoding;
}
