/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Pure-striping encoding -- identity encode/decode, no redundancy.
 *
 * Used for benchmarking parallel I/O throughput without any coding
 * overhead.  Data is split into k equal stripes across k DSes.
 * There are no parity shards (m=0).
 */

#include <errno.h>
#include <stdlib.h>

#include "reffs/ec.h"

static int stripe_encode(struct ec_encoding *encoding __attribute__((unused)),
			 uint8_t **data __attribute__((unused)),
			 uint8_t **parity __attribute__((unused)),
			 size_t shard_len __attribute__((unused)))
{
	/* No parity to compute. */
	return 0;
}

static int stripe_decode(struct ec_encoding *encoding __attribute__((unused)),
			 uint8_t **shards __attribute__((unused)),
			 const bool *present,
			 size_t shard_len __attribute__((unused)))
{
	/* Any missing stripe is unrecoverable (m=0). */
	for (int i = 0; i < encoding->ec_k; i++) {
		if (!present[i])
			return -EIO;
	}
	return 0;
}

struct ec_encoding *ec_stripe_create(int k)
{
	struct ec_encoding *encoding;

	if (k < 1 || k > 255)
		return NULL;

	encoding = calloc(1, sizeof(*encoding));
	if (!encoding)
		return NULL;

	encoding->ec_name = "stripe";
	encoding->ec_k = k;
	encoding->ec_m = 0;
	encoding->ec_encode = stripe_encode;
	encoding->ec_decode = stripe_decode;

	return encoding;
}
