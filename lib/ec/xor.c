/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * XOR single-parity encoding (RAID-5 style).
 *
 * Systematic: k data shards preserved verbatim; one parity shard =
 * XOR of all k data shards.  Any single missing shard (data or
 * parity) recovers by XORing the surviving shards.
 *
 * No Galois-field math -- pure byte-wise XOR runs at memory
 * bandwidth on any CPU.  The MTI floor with actual erasure
 * recovery per Christoph's IETF-126 remark: "complete non-brainer
 * that's trivial to support" without buying into a GF(2^8)
 * polynomial choice.
 *
 * Storage: (k+1)/k * payload.  Fault tolerance: any single shard
 * loss.  See ~/Documents/reffs-docs/ffv2-encoding-menu.md
 * FFV2_ENCODING_XOR_PARITY (proposed 0x7) for the wire-encoding
 * design + MTI rationale.
 *
 * Caps: k in [1, 254] (m is always 1); k+m = k+1 <= 255 matches
 * the same conservative shard-count ceiling ec_rs uses.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/ec.h"

#define XOR_MAX_DATA 254

static int xor_encode(struct ec_encoding *encoding, uint8_t **data,
		      uint8_t **parity, size_t shard_len)
{
	int k = encoding->ec_k;

	if (shard_len == 0)
		return 0;

	memcpy(parity[0], data[0], shard_len);
	for (int i = 1; i < k; i++)
		for (size_t p = 0; p < shard_len; p++)
			parity[0][p] ^= data[i][p];
	return 0;
}

static int xor_decode(struct ec_encoding *encoding, uint8_t **shards,
		      const bool *present, size_t shard_len)
{
	int k = encoding->ec_k;
	int n = k + 1;

	int missing = -1;
	int missing_count = 0;

	for (int i = 0; i < n; i++) {
		if (!present[i]) {
			missing = i;
			missing_count++;
		}
	}
	if (missing_count == 0)
		return 0;
	if (missing_count > 1)
		return -EIO;
	if (shard_len == 0)
		return 0;

	/*
	 * Recover shard[missing] = XOR of every present shard.  Works
	 * uniformly whether the missing slot is a data shard or the
	 * parity shard: sum-over-all-present == the missing entry
	 * because summing all n shards is 0 in GF(2).
	 */
	memset(shards[missing], 0, shard_len);
	for (int i = 0; i < n; i++) {
		if (i == missing)
			continue;
		for (size_t p = 0; p < shard_len; p++)
			shards[missing][p] ^= shards[i][p];
	}
	return 0;
}

static void xor_destroy(struct ec_encoding *encoding)
{
	free(encoding);
}

struct ec_encoding *ec_xor_create(int k)
{
	if (k < 1 || k > XOR_MAX_DATA)
		return NULL;

	struct ec_encoding *encoding = calloc(1, sizeof(*encoding));

	if (!encoding)
		return NULL;

	encoding->ec_name = "xor-parity";
	encoding->ec_k = k;
	encoding->ec_m = 1;
	encoding->ec_encode = xor_encode;
	encoding->ec_decode = xor_decode;
	encoding->ec_shard_size = NULL; /* uniform: all shards shard_len */
	encoding->ec_destroy = xor_destroy;
	encoding->ec_private = NULL;
	return encoding;
}
