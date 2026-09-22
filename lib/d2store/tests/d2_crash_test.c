/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "d1_digest.h"
#include "d2_store.h"

struct cut {
	enum d2_io_point target;
};

static unsigned int checks;
static unsigned int failures;

static void check(bool value, const char *name)
{
	checks++;
	if (!value) {
		failures++;
		fprintf(stderr, "not ok %u - %s\n", checks, name);
	}
}

static void cut_hook(enum d2_io_point point, void *arg)
{
	const struct cut *cut = arg;

	if (point == cut->target)
		_exit(100 + point);
}

static void clean_root(int dirfd)
{
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
}

static void fill_checksum(struct d1_write_entry *entry, const uint8_t *payload,
			  uint32_t len)
{
	uint32_t crc = d1_crc32c(payload, len);

	entry->checksum.alg = D1_CKSUM_CRC32C;
	entry->checksum.len = 4;
	entry->checksum.digest[0] = (uint8_t)(crc >> 24);
	entry->checksum.digest[1] = (uint8_t)(crc >> 16);
	entry->checksum.digest[2] = (uint8_t)(crc >> 8);
	entry->checksum.digest[3] = (uint8_t)crc;
}

int main(void)
{
	const char *root = getenv("D2_TEST_ROOT");
	const enum d2_io_point points[] = {
		D2_IO_PAYLOAD_WRITTEN,
		D2_IO_PAYLOAD_DURABLE,
		D2_IO_WAL_WRITTEN,
		D2_IO_WAL_DURABLE,
	};
	uint8_t token[16], payload[4096];
	unsigned int i;
	int dirfd;

	if (!root)
		return 77;
	dirfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0)
		return 77;
	memset(token, 0x62, sizeof(token));
	memset(payload, 0x93, sizeof(payload));
	for (i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
		struct d2_store_config config = { 0 };
		struct d2_store_rebind reopen = { 0 };
		struct d2_binding binding = { 0 };
		struct d2_store *store = NULL;
		struct d1_objkey object = { 0 };
		struct d1_envelope env = { 0 };
		struct d1_result result;
		d1_admission_id admission;
		d1_version_id visible;
		struct cut cut = { .target = points[i] };
		pid_t pid;
		int child_status;

		clean_root(dirfd);
		memset(config.files.export_uuid, 0x21, 16);
		config.files.binding_token = token;
		config.files.binding_token_len = sizeof(token);
		config.files.capacity_wal_bytes = D2_MIN_WAL_BYTES;
		config.files.capacity_payload_bytes = 32u * 1024u * 1024u;
		config.chunk_bytes = sizeof(payload);
		config.max_file_bytes = 64u * sizeof(payload);
		check(d2_store_provision(dirfd, &config, &binding, &store) ==
			      D1_OK,
		      "cut fixture provisions");
		if (!store)
			continue;
		memset(object.export_uuid.bytes, 0x21, 16);
		memset(object.object_uuid.bytes, 0x44, 16);
		admission = d2_store_admit(store, &object, 9,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		env.object = object;
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		memset(env.key.origin.bytes, 0x71, 16);
		env.key.sequence = 1;
		env.op = D1_OP_WRITE_BATCH;
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		env.body.write.activate = true;
		env.body.write.entries[0].owner.cohort.raw = 1;
		env.body.write.entries[0].owner.writer = 9;
		env.body.write.entries[0].owner.co_id = 1;
		env.body.write.entries[0].guard_check = true;
		env.body.write.entries[0].expected.never_written = true;
		env.body.write.entries[0].payload = payload;
		env.body.write.entries[0].payload_len = sizeof(payload);
		fill_checksum(&env.body.write.entries[0], payload,
			      sizeof(payload));
		pid = fork();
		if (pid == 0) {
			d2_store_set_io_hook(store, cut_hook, &cut);
			(void)d2_store_apply(store, &env, &result);
			_exit(2);
		}
		check(pid > 0 && waitpid(pid, &child_status, 0) == pid &&
			      WIFEXITED(child_status) &&
			      WEXITSTATUS(child_status) == 100 + points[i],
		      "child exits at requested durability barrier");
		d2_store_crash(store);
		store = NULL;
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		reopen.files.binding_token = token;
		reopen.files.binding_token_len = sizeof(token);
		reopen.chunk_bytes = config.chunk_bytes;
		reopen.max_file_bytes = config.max_file_bytes;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "fresh process recovers barrier cut");
		if (points[i] <= D2_IO_PAYLOAD_DURABLE)
			check(store && !d2_store_visible(store, &object, 0,
							 &visible),
			      "payload-only cut publishes nothing");
		else
			check(store && d2_store_visible(store, &object, 0,
							&visible),
			      "process-visible complete WAL record replays");
		if (store)
			d2_store_close(store);
	}
	close(dirfd);
	printf("D2 CRASH: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
