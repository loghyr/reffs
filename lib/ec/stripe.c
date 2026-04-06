/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Pure-striping codec -- identity encode/decode, no redundancy.
 *
 * Used for benchmarking parallel I/O throughput without any coding
 * overhead.  Data is split into k equal stripes across k DSes.
 * There are no parity shards (m=0).
 */

#include <errno.h>
#include <stdlib.h>

#include "reffs/ec.h"

static int stripe_encode(struct ec_codec *codec __attribute__((unused)),
			 uint8_t **data __attribute__((unused)),
			 uint8_t **parity __attribute__((unused)),
			 size_t shard_len __attribute__((unused)))
{
	/* No parity to compute. */
	return 0;
}

static int stripe_decode(struct ec_codec *codec __attribute__((unused)),
			 uint8_t **shards __attribute__((unused)),
			 const bool *present,
			 size_t shard_len __attribute__((unused)))
{
	/* Any missing stripe is unrecoverable (m=0). */
	for (int i = 0; i < codec->ec_k; i++) {
		if (!present[i])
			return -EIO;
	}
	return 0;
}

struct ec_codec *ec_stripe_create(int k)
{
	struct ec_codec *codec;

	if (k < 1 || k > 255)
		return NULL;

	codec = calloc(1, sizeof(*codec));
	if (!codec)
		return NULL;

	codec->ec_name = "stripe";
	codec->ec_k = k;
	codec->ec_m = 0;
	codec->ec_encode = stripe_encode;
	codec->ec_decode = stripe_decode;

	return codec;
}
