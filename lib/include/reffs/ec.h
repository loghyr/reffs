/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Erasure coding codec interface.
 *
 * Designed for swappability: Reed-Solomon is the first implementation,
 * but any codec that satisfies this interface can be plugged in.
 */

#ifndef _REFFS_EC_H
#define _REFFS_EC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ec_codec {
	const char *ec_name;

	int ec_k; /* data shards */
	int ec_m; /* parity shards */

	/*
	 * ec_encode -- produce m parity shards from k data shards.
	 *
	 * data[0..k-1]:   input data buffers, each shard_len bytes.
	 * parity[0..m-1]: output parity buffers, each shard_len bytes
	 *                 (caller-allocated).
	 * Returns 0 on success, negative errno on failure.
	 */
	int (*ec_encode)(struct ec_codec *codec, uint8_t **data,
			 uint8_t **parity, size_t shard_len);

	/*
	 * ec_decode -- reconstruct missing shards.
	 *
	 * shards[0..k+m-1]: buffers for all shards.  Present shards
	 *     contain valid data; missing shards are allocated but
	 *     contents are undefined.
	 * present[0..k+m-1]: true if shard[i] contains valid data.
	 * shard_len: byte length of each shard.
	 *
	 * On success, all missing shards are reconstructed in place.
	 * Returns 0 on success, -EIO if too many shards are missing
	 * (more than m), negative errno on other failures.
	 */
	int (*ec_decode)(struct ec_codec *codec, uint8_t **shards,
			 const bool *present, size_t shard_len);

	void *ec_private; /* codec-specific state */
};

/*
 * ec_rs_create -- create a Reed-Solomon codec instance.
 *
 * k: number of data shards (1..254).
 * m: number of parity shards (1..254, k+m <= 255).
 *
 * Returns an initialized codec, or NULL on failure.
 * The caller must call ec_codec_destroy() when done.
 */
struct ec_codec *ec_rs_create(int k, int m);

/*
 * ec_codec_destroy -- release a codec and all internal state.
 */
void ec_codec_destroy(struct ec_codec *codec);

#endif /* _REFFS_EC_H */
