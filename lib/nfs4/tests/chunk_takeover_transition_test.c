/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nfs4/chunk_epoch.h"
#include "nfs4/chunk_takeover_transition.h"

int main(void)
{
	char state_dir[] = "/tmp/reffs-takeover-transition-XXXXXX";
	static const uint8_t token_id[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	};
	static const uint8_t other_token[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN] = {
		15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
	};
	static const uint8_t renewal_token[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN] = {
		0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87,
		0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f,
	};
	struct chunk_mds_epoch initial = {
		.epoch = 7,
		.expires_at_ns = 1000,
		.issuer_clientid = 11,
	};
	struct chunk_mds_epoch current;
	struct chunk_takeover_transition transition = {
		.profile = 1,
		.principal = "mds@REALM",
		.token_id = token_id,
		.token_expires_at = 110,
		.expected_prior_epoch = 7,
		.new_epoch = 8,
		.new_expires_at_ns = 2000,
		.issuer_clientid = 22,
	};

	assert(mkdtemp(state_dir));
	assert(chunk_mds_epoch_persist(state_dir, &initial) == 0);
	assert(chunk_takeover_transition_apply(state_dir, &transition, 100) ==
	       0);
	assert(chunk_mds_epoch_load(state_dir, &current) == 0);
	assert(current.epoch == 8 && current.expires_at_ns == 2000 &&
	       current.issuer_clientid == 22);

	/* A lost response is recovered without another epoch mutation. */
	assert(chunk_takeover_transition_apply(state_dir, &transition, 101) ==
	       0);

	/* A new proof may renew the currently active incarnation. */
	transition.token_id = renewal_token;
	transition.token_expires_at = 120;
	transition.expected_prior_epoch = 8;
	transition.new_epoch = 8;
	transition.new_expires_at_ns = 4000;
	transition.issuer_clientid = 44;
	assert(chunk_takeover_transition_apply(state_dir, &transition, 101) ==
	       0);
	assert(chunk_mds_epoch_load(state_dir, &current) == 0);
	assert(current.epoch == 8 && current.expires_at_ns == 4000 &&
	       current.issuer_clientid == 44);

	/* Reuse of the first token for a new advance is rejected. */
	transition.token_id = token_id;
	transition.token_expires_at = 110;
	transition.expected_prior_epoch = 8;
	transition.new_epoch = 9;
	transition.new_expires_at_ns = 3000;
	transition.issuer_clientid = 33;
	assert(chunk_takeover_transition_apply(state_dir, &transition, 101) ==
	       -EALREADY);

	/* Replay classification precedes a stale-epoch result. */
	transition.expected_prior_epoch = 7;
	transition.new_epoch = 9;
	transition.token_id = token_id;
	assert(chunk_takeover_transition_apply(state_dir, &transition, 101) ==
	       -EALREADY);

	transition.expected_prior_epoch = 7;
	transition.new_epoch = 9;
	transition.token_id = other_token;
	assert(chunk_takeover_transition_apply(state_dir, &transition, 101) ==
	       -ESTALE);

	return 0;
}
