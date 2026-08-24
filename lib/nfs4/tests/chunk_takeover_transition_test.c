/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nfs4/chunk_epoch.h"
#include "nfs4/chunk_takeover_transition.h"

struct transition_worker {
	const char *state_dir;
	struct chunk_takeover_transition transition;
	int result;
};

static void *run_transition(void *arg)
{
	struct transition_worker *worker = arg;

	worker->result = chunk_takeover_transition_apply(
		worker->state_dir, &worker->transition, 100);
	return NULL;
}

static void test_concurrent_winner(void)
{
	char state_dir[] = "/tmp/reffs-takeover-transition-concurrent-XXXXXX";
	static const uint8_t token_a[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN] = {
		0xa0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
	};
	static const uint8_t token_b[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN] = {
		0xb0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
	};
	struct chunk_mds_epoch initial = {
		.epoch = 7,
		.expires_at_ns = 1000,
		.issuer_clientid = 11,
	};
	struct chunk_mds_epoch current;
	struct transition_worker workers[2] = {
		{
			.transition = {
				.profile = 1,
				.principal = "mds@REALM",
				.token_id = token_a,
				.token_expires_at = 110,
				.expected_prior_epoch = 7,
				.new_epoch = 8,
				.new_expires_at_ns = 2000,
				.issuer_clientid = 22,
			},
		},
		{
			.transition = {
				.profile = 1,
				.principal = "mds@REALM",
				.token_id = token_b,
				.token_expires_at = 110,
				.expected_prior_epoch = 7,
				.new_epoch = 9,
				.new_expires_at_ns = 3000,
				.issuer_clientid = 33,
			},
		},
	};
	pthread_t threads[2];
	unsigned int winners = 0;
	unsigned int losers = 0;

	assert(mkdtemp(state_dir));
	assert(chunk_mds_epoch_persist(state_dir, &initial) == 0);
	for (size_t i = 0; i < 2; i++) {
		workers[i].state_dir = state_dir;
		assert(pthread_create(&threads[i], NULL, run_transition,
				      &workers[i]) == 0);
	}
	for (size_t i = 0; i < 2; i++) {
		assert(pthread_join(threads[i], NULL) == 0);
		if (workers[i].result == 0)
			winners++;
		else if (workers[i].result == -ESTALE)
			losers++;
	}
	assert(winners == 1);
	assert(losers == 1);
	assert(chunk_mds_epoch_load(state_dir, &current) == 0);
	assert(current.epoch == 8 || current.epoch == 9);
}

static void test_persistence_failure_recovery(void)
{
	char state_dir[] = "/tmp/reffs-takeover-transition-persist-XXXXXX";
	char temp_path[512];
	static const uint8_t token_id[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN] = {
		0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
	};
	struct chunk_mds_epoch initial = {
		.epoch = 20,
		.expires_at_ns = 1000,
		.issuer_clientid = 11,
	};
	struct chunk_mds_epoch current;
	struct chunk_takeover_transition transition = {
		.profile = 1,
		.principal = "mds@REALM",
		.token_id = token_id,
		.token_expires_at = 110,
		.expected_prior_epoch = 20,
		.new_epoch = 21,
		.new_expires_at_ns = 2000,
		.issuer_clientid = 22,
	};

	assert(mkdtemp(state_dir));
	assert(chunk_mds_epoch_persist(state_dir, &initial) == 0);
	assert(snprintf(temp_path, sizeof(temp_path), "%s/chunk_mds_epoch.tmp",
			state_dir) < (int)sizeof(temp_path));
	assert(mkdir(temp_path, 0700) == 0);
	assert(chunk_takeover_transition_apply(state_dir, &transition, 100) <
	       0);
	assert(chunk_mds_epoch_load(state_dir, &current) == 0);
	assert(current.epoch == initial.epoch);
	assert(access(temp_path, F_OK) == 0);
	assert(rmdir(temp_path) == 0);
	assert(chunk_takeover_transition_apply(state_dir, &transition, 101) ==
	       0);
	assert(chunk_mds_epoch_load(state_dir, &current) == 0);
	assert(current.epoch == transition.new_epoch);
}

static void test_malformed_journal_fails_closed(void)
{
	char state_dir[] = "/tmp/reffs-takeover-transition-journal-XXXXXX";
	char journal_path[512];
	static const uint8_t token_id[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN] = {
		0xd0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
	};
	struct chunk_mds_epoch initial = {
		.epoch = 30,
		.expires_at_ns = 1000,
		.issuer_clientid = 11,
	};
	struct chunk_mds_epoch current;
	struct chunk_takeover_transition transition = {
		.profile = 1,
		.principal = "mds@REALM",
		.token_id = token_id,
		.token_expires_at = 110,
		.expected_prior_epoch = 30,
		.new_epoch = 31,
		.new_expires_at_ns = 2000,
		.issuer_clientid = 22,
	};
	FILE *journal;

	assert(mkdtemp(state_dir));
	assert(chunk_mds_epoch_persist(state_dir, &initial) == 0);
	assert(snprintf(journal_path, sizeof(journal_path),
			"%s/chunk_takeover_journal",
			state_dir) < (int)sizeof(journal_path));
	journal = fopen(journal_path, "w");
	assert(journal != NULL);
	assert(fputs("old prototype state", journal) >= 0);
	assert(fclose(journal) == 0);
	assert(chunk_takeover_transition_apply(state_dir, &transition, 100) <
	       0);
	assert(chunk_mds_epoch_load(state_dir, &current) == 0);
	assert(current.epoch == initial.epoch);
}

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

	test_concurrent_winner();
	test_persistence_failure_recovery();
	test_malformed_journal_fails_closed();

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
	/* A lost renewal response is also idempotent after journal cleanup. */
	assert(chunk_takeover_transition_apply(state_dir, &transition, 101) ==
	       0);
	transition.issuer_clientid = 45;
	assert(chunk_takeover_transition_apply(state_dir, &transition, 101) ==
	       -EALREADY);
	transition.issuer_clientid = 44;

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
