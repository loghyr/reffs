/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nfs4/chunk_takeover.h"

static size_t put_head(uint8_t *buf, size_t off, uint8_t major, size_t len)
{
	assert(len <= UINT8_MAX);
	if (len < 24)
		buf[off++] = (uint8_t)((major << 5) | len);
	else {
		buf[off++] = (uint8_t)((major << 5) | 24);
		buf[off++] = (uint8_t)len;
	}
	return off;
}

static size_t make_payload(uint8_t *buf)
{
	static const uint8_t token_id[CHUNK_TAKEOVER_TOKEN_ID_LEN] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	};
	size_t off = 0;

	buf[off++] = 0xa6;
	buf[off++] = 1;
	buf[off++] = 0x69;
	memcpy(buf + off, "mds@REALM", 9);
	off += 9;
	buf[off++] = 2;
	buf[off++] = 7;
	buf[off++] = 3;
	buf[off++] = 0x67;
	memcpy(buf + off, "ds.test", 7);
	off += 7;
	buf[off++] = 4;
	buf[off++] = 0xc1;
	buf[off++] = 0x18;
	buf[off++] = 100;
	buf[off++] = 5;
	buf[off++] = 0xc1;
	buf[off++] = 0x18;
	buf[off++] = 110;
	buf[off++] = 6;
	off = put_head(buf, off, 2, sizeof(token_id));
	memcpy(buf + off, token_id, sizeof(token_id));
	return off + sizeof(token_id);
}

int main(void)
{
	static const uint8_t public_key[CHUNK_TAKEOVER_ED25519_PUBLIC_KEY_LEN] = {
		0xc7, 0xa4, 0x02, 0x5d, 0x9b, 0x18, 0x00, 0x2a,
		0x03, 0x12, 0xa6, 0x3a, 0x56, 0xf3, 0xd1, 0x01,
		0xc0, 0x01, 0x6c, 0x42, 0xfc, 0xda, 0xe8, 0x1d,
		0x81, 0x7d, 0x8f, 0x52, 0x98, 0xe3, 0xf2, 0x45,
	};
	static const uint8_t signature[CHUNK_TAKEOVER_ED25519_SIGNATURE_LEN] = {
		0x3d, 0x12, 0x95, 0xcb, 0x1d, 0xb2, 0xd9, 0x39, 0x96, 0xc8,
		0x85, 0x19, 0x79, 0xd0, 0x35, 0x41, 0x83, 0xcf, 0x80, 0x92,
		0x1d, 0xb6, 0xc0, 0x0f, 0xaa, 0xe9, 0x1c, 0x98, 0xe0, 0xba,
		0xc0, 0x6a, 0x40, 0x3e, 0x99, 0x93, 0xfa, 0x65, 0xfa, 0xa0,
		0x7b, 0x8d, 0xdb, 0xe9, 0x7f, 0xa1, 0x19, 0x84, 0xe9, 0x70,
		0xa8, 0xa3, 0xf0, 0xa1, 0xa4, 0x3b, 0xf0, 0x79, 0x76, 0x3f,
		0xc6, 0x6d, 0x2d, 0x0b,
	};
	uint8_t payload[128], proof[256];
	static const uint8_t protected[] = { 0xa1, 1, 0x27 };
	size_t payload_len, off = 0;
	struct chunk_takeover_policy policy = {
		.public_key = public_key,
		.principal = "mds@REALM",
		.scope = "ds.test",
		.now_sec = 105,
		.skew_sec = 5,
	};
	struct chunk_takeover_claim claim;

	payload_len = make_payload(payload);
	proof[off++] = 0x84;
	proof[off++] = 0x43;
	memcpy(proof + off, protected, sizeof(protected));
	off += sizeof(protected);
	proof[off++] = 0xa0;
	off = put_head(proof, off, 2, payload_len);
	memcpy(proof + off, payload, payload_len);
	off += payload_len;
	off = put_head(proof, off, 2, sizeof(signature));
	memcpy(proof + off, signature, sizeof(signature));
	off += sizeof(signature);

	assert(chunk_takeover_verify_proof(proof, off, 7, &policy, &claim) ==
	       0);
	assert(claim.epoch == 7 && claim.issued_at == 100 &&
	       claim.expires_at == 110);
	proof[off - 1] ^= 1;
	assert(chunk_takeover_verify_proof(proof, off, 7, &policy, &claim) < 0);
	puts("takeover proof vector: PASS");
	return 0;
}
