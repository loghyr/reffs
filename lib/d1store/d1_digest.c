/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: CRC32, CRC32C and SHA-256.
 *
 * They are implemented here rather than borrowed so that the model's
 * golden fixtures are reproducible from this source alone, and so that
 * a checksum domain cannot drift when a dependency changes.  All three
 * are covered by known-answer tests beside this file.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "d1_digest.h"

static uint32_t d1_crc_reflected(uint32_t poly, const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t crc = 0xffffffffu;
	size_t i;
	int bit;

	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (poly & (~(crc & 1u) + 1u));
	}
	return crc ^ 0xffffffffu;
}

uint32_t d1_crc32(const void *data, size_t len)
{
	return d1_crc_reflected(0xedb88320u, data, len);
}

uint32_t d1_crc32c(const void *data, size_t len)
{
	return d1_crc_reflected(0x82f63b78u, data, len);
}

struct d1_sha256_ctx {
	uint32_t h[8];
	uint64_t bits;
	uint8_t block[64];
	size_t used;
};

static const uint32_t d1_sha256_k[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
	0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
	0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
	0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
	0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
	0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
	0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
	0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
	0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
	0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t d1_ror(uint32_t v, unsigned int n)
{
	return (v >> n) | (v << (32u - n));
}

static void d1_sha256_block(struct d1_sha256_ctx *ctx, const uint8_t *p)
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	unsigned int i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) |
		       ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
	for (i = 16; i < 64; i++) {
		uint32_t s0 = d1_ror(w[i - 15], 7) ^ d1_ror(w[i - 15], 18) ^
			      (w[i - 15] >> 3);
		uint32_t s1 = d1_ror(w[i - 2], 17) ^ d1_ror(w[i - 2], 19) ^
			      (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = ctx->h[0];
	b = ctx->h[1];
	c = ctx->h[2];
	d = ctx->h[3];
	e = ctx->h[4];
	f = ctx->h[5];
	g = ctx->h[6];
	h = ctx->h[7];
	for (i = 0; i < 64; i++) {
		uint32_t s1 = d1_ror(e, 6) ^ d1_ror(e, 11) ^ d1_ror(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + s1 + ch + d1_sha256_k[i] + w[i];
		uint32_t s0 = d1_ror(a, 2) ^ d1_ror(a, 13) ^ d1_ror(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = s0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	ctx->h[0] += a;
	ctx->h[1] += b;
	ctx->h[2] += c;
	ctx->h[3] += d;
	ctx->h[4] += e;
	ctx->h[5] += f;
	ctx->h[6] += g;
	ctx->h[7] += h;
}

static void d1_sha256_init(struct d1_sha256_ctx *ctx)
{
	static const uint32_t iv[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
					0xa54ff53au, 0x510e527fu, 0x9b05688cu,
					0x1f83d9abu, 0x5be0cd19u };

	memcpy(ctx->h, iv, sizeof(iv));
	ctx->bits = 0;
	ctx->used = 0;
}

static void d1_sha256_update(struct d1_sha256_ctx *ctx, const void *data,
			     size_t len)
{
	const uint8_t *p = data;

	ctx->bits += (uint64_t)len * 8u;
	while (len) {
		size_t take = sizeof(ctx->block) - ctx->used;

		if (take > len)
			take = len;
		memcpy(ctx->block + ctx->used, p, take);
		ctx->used += take;
		p += take;
		len -= take;
		if (ctx->used == sizeof(ctx->block)) {
			d1_sha256_block(ctx, ctx->block);
			ctx->used = 0;
		}
	}
}

static void d1_sha256_final(struct d1_sha256_ctx *ctx,
			    uint8_t out[D1_DIGEST_BYTES])
{
	uint64_t bits = ctx->bits;
	uint8_t tail[8];
	unsigned int i;

	d1_sha256_update(ctx, "\x80", 1);
	while (ctx->used != 56) {
		d1_sha256_update(ctx, "\x00", 1);
	}
	for (i = 0; i < 8; i++)
		tail[i] = (uint8_t)(bits >> (56 - 8 * i));
	d1_sha256_update(ctx, tail, sizeof(tail));
	for (i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(ctx->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)ctx->h[i];
	}
}

void d1_sha256(const void *data, size_t len, uint8_t out[D1_DIGEST_BYTES])
{
	struct d1_sha256_ctx ctx;

	d1_sha256_init(&ctx);
	d1_sha256_update(&ctx, data, len);
	d1_sha256_final(&ctx, out);
}

void d1_request_digest(const void *data, size_t len,
		       uint8_t out[D1_DIGEST_BYTES])
{
	struct d1_sha256_ctx ctx;

	d1_sha256_init(&ctx);
	d1_sha256_update(&ctx, D1_DIGEST_DOMAIN, D1_DIGEST_DOMAIN_BYTES);
	d1_sha256_update(&ctx, data, len);
	d1_sha256_final(&ctx, out);
}

bool d1_checksum_compute(uint32_t alg, const void *data, size_t len,
			 struct d1_checksum *out)
{
	uint32_t value;

	switch (alg) {
	case D1_CKSUM_CRC32:
		value = d1_crc32(data, len);
		break;
	case D1_CKSUM_CRC32C:
		value = d1_crc32c(data, len);
		break;
	default:
		return false;
	}
	out->alg = alg;
	out->len = sizeof(out->digest);
	out->digest[0] = (uint8_t)(value >> 24);
	out->digest[1] = (uint8_t)(value >> 16);
	out->digest[2] = (uint8_t)(value >> 8);
	out->digest[3] = (uint8_t)value;
	return true;
}

bool d1_checksum_verify(const struct d1_checksum *sum, const void *data,
			size_t len)
{
	struct d1_checksum want;

	if (sum->len != sizeof(sum->digest))
		return false;
	if (!d1_checksum_compute(sum->alg, data, len, &want))
		return false;
	return memcmp(want.digest, sum->digest, sizeof(want.digest)) == 0;
}
