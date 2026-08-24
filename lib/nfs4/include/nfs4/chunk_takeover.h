/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef NFS4_CHUNK_TAKEOVER_H
#define NFS4_CHUNK_TAKEOVER_H

#include <stddef.h>
#include <stdint.h>

#define CHUNK_TAKEOVER_ED25519_PUBLIC_KEY_LEN 32
#define CHUNK_TAKEOVER_TOKEN_ID_LEN 16
#define CHUNK_TAKEOVER_ED25519_SIGNATURE_LEN 64
#define CHUNK_TAKEOVER_PROOF_MAX 4096

struct chunk_takeover_policy {
	const uint8_t *public_key;
	const char *principal;
	const char *scope;
	uint64_t now_sec;
	uint64_t skew_sec;
};

struct chunk_takeover_claim {
	uint64_t epoch;
	uint64_t issued_at;
	uint64_t expires_at;
	uint8_t token_id[CHUNK_TAKEOVER_TOKEN_ID_LEN];
};

/*
 * Verify the mandatory Ed25519/COSE_Sign1 takeover profile.  Replay-cache
 * insertion and epoch mutation remain the caller's serialized duties.
 */
int chunk_takeover_verify_proof(const uint8_t *proof, size_t proof_len,
				uint64_t expected_epoch,
				const struct chunk_takeover_policy *policy,
				struct chunk_takeover_claim *claim);

#endif /* NFS4_CHUNK_TAKEOVER_H */
