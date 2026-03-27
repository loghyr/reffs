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
	 * data[0..k-1]:   input data buffers.  For uniform codecs (RS),
	 *     each buffer is shard_len bytes.  For non-systematic codecs
	 *     that produce variable-size projections (Mojette nonsys),
	 *     each buffer MUST be allocated to ec_shard_size(codec, i,
	 *     shard_len) bytes — the codec overwrites data[i] with
	 *     projection output that may be larger than shard_len.
	 * parity[0..m-1]: output parity buffers, each allocated to
	 *     ec_shard_size(codec, k+i, shard_len) bytes.
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

	/*
	 * ec_shard_size -- return the byte size of shard i.
	 *
	 * For uniform codecs (RS), all shards are data_shard_len bytes;
	 * set this to NULL to use the default.
	 *
	 * For variable-size codecs (Mojette), parity shards may be
	 * larger than data shards.  The callback returns the actual
	 * byte size for shard i given the data shard length.
	 */
	size_t (*ec_shard_size)(struct ec_codec *codec, int shard_idx,
				size_t data_shard_len);

	/* Release codec-specific private state.  Called by ec_codec_destroy. */
	void (*ec_destroy)(struct ec_codec *codec);

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
 * ec_mojette_sys_create -- create a systematic Mojette codec.
 *
 * k: number of data rows (grid height Q).
 * m: number of extra projections (parity).
 *
 * Data shards are grid rows.  Parity shards are projections.
 * Returns NULL on failure.
 */
struct ec_codec *ec_mojette_sys_create(int k, int m);

/*
 * ec_mojette_nonsys_create -- create a non-systematic Mojette codec.
 *
 * k: number of data rows (grid height Q).
 * m: number of extra projections (parity).
 *
 * All k+m output shards are projections.  Encode overwrites data[]
 * with the first k projections.  Any k shards suffice for decode.
 * Returns NULL on failure.
 */
struct ec_codec *ec_mojette_nonsys_create(int k, int m);

/*
 * ec_stripe_create -- create a pure-striping codec (no redundancy).
 *
 * k: number of stripe segments (1..255).
 * m must be 0.  Encode and decode are identity operations.
 * Used for benchmarking parallel I/O throughput without coding overhead.
 */
struct ec_codec *ec_stripe_create(int k);

/*
 * ec_codec_destroy -- release a codec and all internal state.
 */
void ec_codec_destroy(struct ec_codec *codec);

#endif /* _REFFS_EC_H */
