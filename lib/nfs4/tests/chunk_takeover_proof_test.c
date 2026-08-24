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
		0x35, 0x3d, 0x35, 0xda, 0xa9, 0x6d, 0x88, 0x15,
		0xab, 0x2b, 0x10, 0x99, 0xd8, 0x1c, 0x0f, 0x68,
		0x3e, 0xbb, 0x0d, 0x7c, 0x73, 0x18, 0x22, 0x1f,
		0xe4, 0xfe, 0x18, 0x19, 0x99, 0xf3, 0x13, 0x13,
	};
	static const uint8_t signature[CHUNK_TAKEOVER_ED25519_SIGNATURE_LEN] = {
		0x43, 0xf1, 0x82, 0x3e, 0x31, 0x1a, 0xa2, 0x0f, 0xc8, 0x72,
		0xb9, 0x5c, 0x87, 0x9e, 0x8f, 0xe4, 0xe8, 0x49, 0xdf, 0x72,
		0xbe, 0x89, 0x2f, 0x39, 0x61, 0xee, 0xdd, 0x06, 0xfe, 0xd6,
		0xb6, 0x64, 0x0f, 0x51, 0xda, 0xe2, 0xf9, 0xb0, 0xef, 0xa5,
		0x99, 0xad, 0x07, 0xba, 0xf7, 0x1f, 0x10, 0xcc, 0x65, 0x73,
		0x3f, 0x55, 0x15, 0x7a, 0xcd, 0xa4, 0xae, 0xb5, 0x1a, 0x2e,
		0xf7, 0x4f, 0xd0, 0x06,
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
