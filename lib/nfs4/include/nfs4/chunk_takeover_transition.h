/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef NFS4_CHUNK_TAKEOVER_TRANSITION_H
#define NFS4_CHUNK_TAKEOVER_TRANSITION_H

#include <stdint.h>

#include "nfs4/chunk_takeover_replay.h"

struct chunk_takeover_transition {
	uint32_t profile;
	const char *principal;
	const uint8_t *token_id;
	uint64_t token_expires_at;
	uint64_t expected_prior_epoch;
	uint64_t new_epoch;
	uint64_t new_expires_at_ns;
	uint64_t issuer_clientid;
};

/*
 * Apply or recover one serialized takeover transition.  The journal is
 * removed only after the replay claim and epoch record are both durable.
 * A byte-identical reissue whose epoch advance is already visible returns
 * success without another state change.
 */
int chunk_takeover_transition_apply(
	const char *state_dir,
	const struct chunk_takeover_transition *transition, uint64_t now_sec);

#endif /* NFS4_CHUNK_TAKEOVER_TRANSITION_H */
