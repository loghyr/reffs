/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>
#include <zlib.h>

#include "blake3.h"
#include "nfs4/chunk_checksum.h"

static void put_be32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}

static void put_be64(uint8_t *out, uint64_t value)
{
	for (unsigned int i = 0; i < 8; i++)
		out[i] = (uint8_t)(value >> (56 - i * 8));
}

static uint32_t get_le32(const uint8_t *data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint32_t checksum_crc32(const uint8_t *data, size_t len)
{
	uLong crc = crc32(0L, Z_NULL, 0);

	while (len > 0) {
		uInt count = len > UINT_MAX ? UINT_MAX : (uInt)len;

		crc = crc32(crc, data, count);
		data += count;
		len -= count;
	}
	return (uint32_t)crc;
}

/* Reflected Castagnoli polynomial, four bits per table step. */
static uint32_t checksum_crc32c(const uint8_t *data, size_t len)
{
	static const uint32_t table[16] = {
		0x00000000, 0x105ec76f, 0x20bd8ede, 0x30e349b1,
		0x417b1dbc, 0x5125dad3, 0x61c69362, 0x7198540d,
		0x82f63b78, 0x92a8fc17, 0xa24bb5a6, 0xb21572c9,
		0xc38d26c4, 0xd3d3e1ab, 0xe330a81a, 0xf36e6f75,
	};
	uint32_t crc = UINT32_MAX;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		crc = table[crc & 0xf] ^ (crc >> 4);
		crc = table[crc & 0xf] ^ (crc >> 4);
	}
	return crc ^ UINT32_MAX;
}

static void checksum_fletcher4(const uint8_t *data, size_t len, uint8_t *value)
{
	uint64_t a = 0;
	uint64_t b = 0;
	uint64_t c = 0;
	uint64_t d = 0;

	for (size_t i = 0; i < len; i += 4) {
		a += get_le32(data + i);
		b += a;
		c += b;
		d += c;
	}
	put_be64(value, a);
	put_be64(value + 8, b);
	put_be64(value + 16, c);
	put_be64(value + 24, d);
}

bool chunk_checksum_supported(uint32_t algorithm)
{
	return chunk_checksum_expected_len(algorithm) >= 0;
}

int chunk_checksum_compute(uint32_t algorithm, const uint8_t *data, size_t len,
			   uint8_t *value, uint32_t *value_len)
{
	int expected_len = chunk_checksum_expected_len(algorithm);
	static const uint8_t empty = 0;

	if (!value_len || expected_len < 0)
		return expected_len < 0 ? -EOPNOTSUPP : -EINVAL;
	if (len > 0 && !data)
		return -EINVAL;
	if (expected_len > 0 && !value)
		return -EINVAL;
	if (algorithm == CHECKSUM_ALG_FLETCHER4 && len % 4 != 0)
		return -EINVAL;
	if (!data)
		data = &empty;

	switch (algorithm) {
	case CHECKSUM_ALG_NONE:
		break;
	case CHECKSUM_ALG_CRC32:
		put_be32(value, checksum_crc32(data, len));
		break;
	case CHECKSUM_ALG_CRC32C:
		put_be32(value, checksum_crc32c(data, len));
		break;
	case CHECKSUM_ALG_FLETCHER4:
		checksum_fletcher4(data, len, value);
		break;
	case CHECKSUM_ALG_SHA256:
		if (!SHA256(data, len, value))
			return -EIO;
		break;
	case CHECKSUM_ALG_SHA512:
		if (!SHA512(data, len, value))
			return -EIO;
		break;
	case CHECKSUM_ALG_BLAKE3: {
		blake3_hasher hasher;

		blake3_hasher_init(&hasher);
		blake3_hasher_update(&hasher, data, len);
		blake3_hasher_finalize(&hasher, value, BLAKE3_OUT_LEN);
		break;
	}
	default:
		return -EOPNOTSUPP;
	}
	*value_len = (uint32_t)expected_len;
	return 0;
}

int chunk_checksum_pack_data(checksum4 *out, uint32_t algorithm,
			     const uint8_t *data, size_t len)
{
	uint8_t value[64];
	uint32_t value_len;
	int ret;

	if (!out)
		return -EINVAL;
	ret = chunk_checksum_compute(algorithm, data, len, value, &value_len);
	if (ret)
		return ret;
	return chunk_checksum_pack(out, algorithm, value, value_len);
}

int chunk_checksum_verify(const checksum4 *checksum, const uint8_t *data,
			  size_t len)
{
	uint8_t value[64];
	uint32_t value_len;
	int expected_len;
	int ret;

	if (!checksum)
		return -EINVAL;
	expected_len = chunk_checksum_expected_len(checksum->cs_algorithm);
	if (expected_len < 0 ||
	    checksum->cs_value.cs_value_len != (uint32_t)expected_len)
		return -EINVAL;
	if (expected_len > 0 && !checksum->cs_value.cs_value_val)
		return -EINVAL;
	ret = chunk_checksum_compute(checksum->cs_algorithm, data, len, value,
				     &value_len);
	if (ret)
		return ret;
	if (value_len == 0)
		return 0;
	return memcmp(value, checksum->cs_value.cs_value_val, value_len) == 0 ?
		       0 :
		       -EBADMSG;
}
