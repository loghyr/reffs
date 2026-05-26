/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Helpers for the wire checksum4 type carried by CHUNK_WRITE,
 * CHUNK_WRITE_REPAIR, and CHUNK_READ.
 *
 * The implementation currently supports CHECKSUM_ALG_CRC32 only.
 * Other algorithms are defined in the IANA registry but not yet
 * computed; emitters use CRC32 and validators reject anything else
 * with NFS4ERR_INVAL.
 *
 * Wire byte order for cs_value is big-endian (network byte order),
 * consistent with XDR conventions.  For CRC32 the value is exactly
 * 4 octets.
 */

#ifndef _REFFS_NFS4_CHUNK_CHECKSUM_H
#define _REFFS_NFS4_CHUNK_CHECKSUM_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"

/*
 * Decode a checksum4 carrying CRC32.  Returns NFS4_OK and sets
 * *out_crc on success; returns NFS4ERR_INVAL if the algorithm is
 * not CHECKSUM_ALG_CRC32 or the value length is not 4.
 */
static inline nfsstat4 chunk_checksum_unpack_crc32(const checksum4 *cs,
						   uint32_t *out_crc)
{
	if (cs->cs_algorithm != CHECKSUM_ALG_CRC32)
		return NFS4ERR_INVAL;
	if (cs->cs_value.cs_value_len != 4 || cs->cs_value.cs_value_val == NULL)
		return NFS4ERR_INVAL;
	const uint8_t *v = (const uint8_t *)cs->cs_value.cs_value_val;

	*out_crc = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
		   ((uint32_t)v[2] << 8) | (uint32_t)v[3];
	return NFS4_OK;
}

/*
 * Encode a CRC32 into a checksum4.  Allocates a 4-byte cs_value
 * buffer; release is handled by the XDR free path on the response
 * side, or by the caller on the request side (since the caller
 * owns the request struct).  Returns 0 on success, -ENOMEM on
 * allocation failure.
 */
static inline int chunk_checksum_pack_crc32(checksum4 *out, uint32_t crc)
{
	uint8_t *buf = calloc(1, 4);

	if (!buf)
		return -ENOMEM;
	buf[0] = (uint8_t)(crc >> 24);
	buf[1] = (uint8_t)(crc >> 16);
	buf[2] = (uint8_t)(crc >> 8);
	buf[3] = (uint8_t)(crc);
	out->cs_algorithm = CHECKSUM_ALG_CRC32;
	out->cs_value.cs_value_len = 4;
	out->cs_value.cs_value_val = (char *)buf;
	return 0;
}

/*
 * Return the registered cs_value byte length for the named algorithm,
 * or -1 for unknown algorithms.  Used by step-8 wire-validation in
 * CHUNK_WRITE to reject malformed inputs before any compute-side work
 * (the algorithms whose value length is not the IANA-registered size
 * are by definition not the algorithm they claim to be).
 *
 * BLAKE3 produces variable-length output; reffs locks to the default
 * 32 bytes per the spec recommendation.  Future deployments wanting
 * longer outputs need a second canonical length here.
 */
static inline int chunk_checksum_expected_len(uint32_t algorithm)
{
	switch (algorithm) {
	case CHECKSUM_ALG_NONE:
		return 0;
	case CHECKSUM_ALG_CRC32:
		return 4;
	case CHECKSUM_ALG_CRC32C:
		return 4;
	case CHECKSUM_ALG_FLETCHER4:
		return 8;
	case CHECKSUM_ALG_SHA256:
		return 32;
	case CHECKSUM_ALG_SHA512:
		return 64;
	case CHECKSUM_ALG_BLAKE3:
		return 32;
	default:
		return -1;
	}
}

/*
 * Encode an arbitrary algorithm + raw value bytes into a checksum4.
 * Allocates a fresh cs_value buffer of `len` bytes.  Caller-owned on
 * the request side; XDR-free-owned on the response side.  Returns 0
 * on success, -ENOMEM on allocation failure, -EINVAL if len > 0 and
 * value is NULL.
 */
static inline int chunk_checksum_pack(checksum4 *out, uint32_t algorithm,
				      const uint8_t *value, uint32_t len)
{
	if (len > 0 && value == NULL)
		return -EINVAL;
	uint8_t *buf = NULL;

	if (len > 0) {
		buf = calloc(1, len);
		if (!buf)
			return -ENOMEM;
		memcpy(buf, value, len);
	}
	out->cs_algorithm = algorithm;
	out->cs_value.cs_value_len = len;
	out->cs_value.cs_value_val = (char *)buf;
	return 0;
}

#endif /* _REFFS_NFS4_CHUNK_CHECKSUM_H */
