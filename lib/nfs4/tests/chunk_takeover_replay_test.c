/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nfs4/chunk_takeover_replay.h"

int main(void)
{
	char state_dir[] = "/tmp/reffs-takeover-replay-XXXXXX";
	static const uint8_t token_id[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	};
	static const uint8_t other_token[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN] = {
		15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
	};
	char path[512];
	int fd;

	assert(mkdtemp(state_dir));
	assert(chunk_takeover_replay_claim(state_dir, 1, "mds@REALM", token_id,
					   110, 100) == 0);
	assert(chunk_takeover_replay_claim(state_dir, 1, "mds@REALM", token_id,
					   110, 100) == -EALREADY);
	assert(chunk_takeover_replay_claim(state_dir, 1, "mds@REALM",
					   other_token, 120, 111) == 0);
	/* Expired entries are pruned, so the original token can be reused. */
	assert(chunk_takeover_replay_claim(state_dir, 1, "mds@REALM", token_id,
					   130, 111) == 0);
	assert(chunk_takeover_replay_claim(state_dir, 2, "mds@REALM", token_id,
					   140, 112) == 0);
	assert(chunk_takeover_replay_claim(state_dir, 1, "other@REALM",
					   token_id, 150, 112) == 0);

	/* A malformed persisted record fails closed. */
	assert(snprintf(path, sizeof(path), "%s/chunk_takeover_replay",
			state_dir) < (int)sizeof(path));
	fd = open(path, O_WRONLY | O_TRUNC);
	assert(fd >= 0);
	assert(write(fd, "bad", 3) == 3);
	assert(close(fd) == 0);
	assert(chunk_takeover_replay_claim(state_dir, 3, "mds@REALM",
					   other_token, 160, 112) < 0);

	unlink(path);
	assert(snprintf(path, sizeof(path), "%s/chunk_takeover_replay.lock",
			state_dir) < (int)sizeof(path));
	unlink(path);
	rmdir(state_dir);
	return 0;
}
