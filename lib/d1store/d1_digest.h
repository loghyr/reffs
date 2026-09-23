/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: the three digest domains it keeps apart.
 *
 * CRC32 and CRC32C are payload checksums a caller supplies and the
 * model validates against the exact bytes it accepted.  CRC32C is also
 * the journal record trailer, over a different input and written in a
 * different byte order; the two uses share an algorithm and nothing
 * else.  SHA-256 covers the canonical envelope of a request, under its
 * own ASCII domain, and never meets either.
 */

#ifndef REFFS_D1_DIGEST_H
#define REFFS_D1_DIGEST_H

#include <stddef.h>
#include <stdint.h>

#include "d1_types.h"

/* The bare ASCII domain of the request digest: no length, no NUL. */
#define D1_DIGEST_DOMAIN "FFV2-D1-REQ-v1"
#define D1_DIGEST_DOMAIN_BYTES 14

/* zlib CRC32, reflected polynomial 0xedb88320. */
uint32_t d1_crc32(const void *data, size_t len);

/* CRC32C, reflected polynomial 0x82f63b78, initial and final xor ~0. */
uint32_t d1_crc32c(const void *data, size_t len);

/* One-shot SHA-256. */
void d1_sha256(const void *data, size_t len, uint8_t out[D1_DIGEST_BYTES]);

/* SHA-256 of the domain bytes followed by @len bytes of @data. */
void d1_request_digest(const void *data, size_t len,
		       uint8_t out[D1_DIGEST_BYTES]);

/*
 * Compute the tagged checksum @alg over @data.  Returns false for an
 * algorithm this model does not support, before any use of the result.
 */
bool d1_checksum_compute(uint32_t alg, const void *data, size_t len,
			 struct d1_checksum *out);

/*
 * Whether @sum is well formed and matches @data.  A tag this model does
 * not support, or a length that is not this algorithm's, fails here
 * rather than being compared.
 */
bool d1_checksum_verify(const struct d1_checksum *sum, const void *data,
			size_t len);

#endif /* REFFS_D1_DIGEST_H */
