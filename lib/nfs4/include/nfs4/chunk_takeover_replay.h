/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef NFS4_CHUNK_TAKEOVER_REPLAY_H
#define NFS4_CHUNK_TAKEOVER_REPLAY_H

#include <stdint.h>

#include "reffs/settings.h"

#define CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN 16
#define CHUNK_TAKEOVER_REPLAY_MAX 4096

/*
 * Atomically claim a takeover token ID.  A return value of -EALREADY means
 * that the same profile, principal, and token ID was previously committed.
 * The caller must treat all other negative returns as fail-closed errors.
 * The claim is durable before this function returns success.
 */
int chunk_takeover_replay_claim(
	const char *state_dir, uint32_t profile, const char *principal,
	const uint8_t token_id[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN],
	uint64_t expires_at, uint64_t now_sec);

#endif /* NFS4_CHUNK_TAKEOVER_REPLAY_H */
