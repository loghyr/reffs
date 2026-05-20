/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Mirror codec -- N replicas, no parity transform.
 *
 * Encoding: data[0] is the source chunk; data[1..k-1] receive
 * a verbatim copy of data[0].  Every replica's shard ends up
 * carrying the same bytes.
 *
 * Decoding: any one shard whose `present[i]` is true is
 * authoritative; copy it into the missing slots so the caller
 * sees every slot populated.  Returns -EIO only if every shard
 * is missing.
 *
 * The codec layer presumes the caller has allocated all k
 * shard buffers to the same length (shard_len).  Unlike the
 * pipeline's striped data layout (which points each data[i]
 * at a different offset in a padded source buffer), MIRRORED
 * requires every data[i] / shards[i] to occupy a separate
 * allocation: the encode step would otherwise smash data[0]'s
 * neighbours.  The pipeline integration that drives this
 * codec must lay shards out accordingly; see the comment
 * around ec_create_codec for the current wiring status.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/ec.h"

static int mirror_encode(struct ec_codec *codec, uint8_t **data,
			 uint8_t **parity __attribute__((unused)),
			 size_t shard_len)
{
	if (!data || !data[0])
		return -EINVAL;

	for (int i = 1; i < codec->ec_k; i++) {
		if (!data[i])
			return -EINVAL;
		if (data[i] == data[0])
			continue; /* aliased buffer, nothing to copy */
		memcpy(data[i], data[0], shard_len);
	}
	return 0;
}

static int mirror_decode(struct ec_codec *codec, uint8_t **shards,
			 const bool *present, size_t shard_len)
{
	int src = -1;

	for (int i = 0; i < codec->ec_k; i++) {
		if (present[i]) {
			src = i;
			break;
		}
	}
	if (src < 0)
		return -EIO; /* no replica survived */

	for (int i = 0; i < codec->ec_k; i++) {
		if (present[i])
			continue;
		if (!shards[i])
			return -EINVAL;
		if (shards[i] == shards[src])
			continue;
		memcpy(shards[i], shards[src], shard_len);
	}
	return 0;
}

struct ec_codec *ec_mirror_create(int k)
{
	struct ec_codec *codec;

	/*
	 * k here is the replica count (N).  Cap matches the other
	 * codecs' (k + m) <= 255 ceiling -- mirroring has m = 0 so
	 * the effective ceiling on k is the same 255.
	 */
	if (k < 1 || k > 255)
		return NULL;

	codec = calloc(1, sizeof(*codec));
	if (!codec)
		return NULL;

	codec->ec_name = "mirror";
	codec->ec_k = k;
	codec->ec_m = 0;
	codec->ec_encode = mirror_encode;
	codec->ec_decode = mirror_decode;

	return codec;
}
