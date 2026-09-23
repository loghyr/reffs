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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "d1_digest.h"
#include "d2_store.h"

struct cut {
	enum d2_io_point target;
	unsigned int occurrence;
	unsigned int seen;
};

struct io_counts {
	unsigned int point[D2_IO_SUPER_DURABLE + 1];
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
	struct cut *cut = arg;

	if (point == cut->target && ++cut->seen == cut->occurrence)
		_exit(100 + point);
}

static void count_hook(enum d2_io_point point, void *arg)
{
	struct io_counts *counts = arg;

	if ((unsigned int)point <= D2_IO_SUPER_DURABLE)
		counts->point[point]++;
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

static bool repair_log_atomic(int dirfd, const struct d2_binding *binding)
{
	struct stat st;
	struct d2_wal_header h;
	struct d2_cohort cohort;
	uint8_t *wal = NULL;
	uint64_t at = 0;
	unsigned int prepared = 0;
	bool ok = false;
	int fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);

	if (fd < 0 || fstat(fd, &st) < 0 || st.st_size <= 0)
		goto out;
	wal = malloc((size_t)st.st_size);
	if (!wal || pread(fd, wal, (size_t)st.st_size, 0) != st.st_size)
		goto out;
	while (at < (uint64_t)st.st_size &&
	       d2_wal_header_decode(wal + at, (size_t)st.st_size - at,
				    binding->store_uuid, binding->wal_uuid, &h)) {
		if (h.family == D2_REC_COHORT &&
		    d2_cohort_decode(wal + at, h.total_bytes, &h, &cohort) &&
		    cohort.transition == D2_PREPARED) {
			prepared++;
			if (cohort.member_count != 2 || !cohort.members[0].staged ||
			    !cohort.members[1].staged ||
			    !cohort.members[0].payload_object_id ||
			    !cohort.members[1].payload_object_id)
				goto out;
		}
		at += h.total_bytes;
	}
	ok = at == (uint64_t)st.st_size && prepared <= 1;
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return ok;
}

static void repair_cut(int dirfd, const uint8_t *token,
		       enum d2_io_point point, unsigned int occurrence,
		       const uint8_t *payload, const uint8_t *replacement)
{
	struct d2_store_config config = { 0 };
	struct d2_store_rebind reopen = { 0 };
	struct d2_binding binding = { 0 };
	struct d2_store *store = NULL;
	struct d1_objkey object = { 0 };
	struct d1_envelope env = { 0 };
	struct d1_result result;
	struct d1_guard guard = { .never_written = true };
	d1_admission_id admission;
	d1_version_id first, second, visible;
	d1_custody_id first_custody, second_custody;
	d1_episode_id episode;
	d1_repair_id repair;
	d1_txn_id first_txn, second_txn;
	struct cut cut = { .target = point, .occurrence = occurrence };
	uint64_t wal_before;
	uint32_t phase, member_count;
	bool fail_wal = point == 0;
	pid_t pid;
	int child_status;

	clean_root(dirfd);
	memset(config.files.export_uuid, 0x21, 16);
	config.files.binding_token = token;
	config.files.binding_token_len = 16;
	config.files.capacity_wal_bytes = D2_MIN_WAL_BYTES;
	config.files.capacity_payload_bytes = 32u * 1024u * 1024u;
	config.chunk_bytes = 4096;
	config.max_file_bytes = 64u * 4096u;
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "repair cut fixture provisions");
	if (!store)
		return;
	memset(object.export_uuid.bytes, 0x21, 16);
	memset(object.object_uuid.bytes, 0x66, 16);
	admission = d2_store_admit(store, &object, 9,
				   D1_RIGHT_READ | D1_RIGHT_WRITE | D1_RIGHT_REPAIR |
					   D1_RIGHT_SINGLE_WRITER);
	env.object = object;
	env.admission = admission;
	env.incarnation = d2_store_incarnation(store);
	memset(env.key.origin.bytes, 0x74, 16);
	env.op = D1_OP_WRITE_BATCH;
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	env.body.write.activate = true;
	env.body.write.entries[0].owner.cohort.raw = 1;
	env.body.write.entries[0].owner.writer = 9;
	env.body.write.entries[0].owner.co_id = 1;
	env.body.write.entries[0].guard_check = true;
	env.body.write.entries[0].expected = guard;
	env.body.write.entries[0].payload = payload;
	env.body.write.entries[0].payload_len = 4096;
	fill_checksum(&env.body.write.entries[0], payload, 4096);
	env.key.sequence = 1;
	check(d2_store_apply(store, &env, &result) == D1_OK &&
		      result.entries[0].status == D1_OK,
	      "repair cut publishes first member");
	first = result.entries[0].version;
	first_custody = d2_store_custody(store, first);
	env.key.sequence = 2;
	env.body.write.entries[0].index = 1;
	env.body.write.entries[0].owner.co_id = 2;
	env.body.write.entries[0].payload = replacement;
	fill_checksum(&env.body.write.entries[0], replacement, 4096);
	check(d2_store_apply(store, &env, &result) == D1_OK &&
		      result.entries[0].status == D1_OK,
	      "repair cut publishes second member");
	second = result.entries[0].version;
	second_custody = d2_store_custody(store, second);
	memset(&env.body, 0, sizeof(env.body));
	env.key.sequence = 3;
	env.op = D1_OP_MARK_ERROR;
	env.body.repair.range_end = 2;
	env.body.repair.count = 2;
	for (unsigned int i = 0; i < 2; i++) {
		env.body.repair.entries[i].index = i;
		env.body.repair.entries[i].owner.cohort.raw = 2;
		env.body.repair.entries[i].owner.writer = 9;
		env.body.repair.entries[i].owner.co_id = 3 + i;
		env.body.repair.entries[i].custody_present = true;
		env.body.repair.entries[i].custody = i ? second_custody :
							   first_custody;
		env.body.repair.entries[i].successor_present = true;
		env.body.repair.entries[i].successor = i ? second : first;
	}
	check(d2_store_apply(store, &env, &result) == D1_OK &&
		      result.entries[0].status == D1_OK,
	      "repair cut marks two-member episode");
	episode = result.entries[0].episode;
	memset(&env.body, 0, sizeof(env.body));
	env.key.sequence = 4;
	env.op = D1_OP_BEGIN_REPAIR;
	env.body.repair.range_end = 2;
	env.body.repair.count = 2;
	env.body.repair.episode_present = true;
	env.body.repair.episode = episode;
	for (unsigned int i = 0; i < 2; i++) {
		env.body.repair.entries[i].index = i;
		env.body.repair.entries[i].owner.cohort.raw = 3;
		env.body.repair.entries[i].owner.writer = 9;
		env.body.repair.entries[i].owner.co_id = 5 + i;
		env.body.repair.entries[i].mode = D1_REPAIR_ERROR;
		env.body.repair.entries[i].custody_present = true;
		env.body.repair.entries[i].custody = i ? second_custody :
							   first_custody;
		env.body.repair.entries[i].successor_present = true;
		env.body.repair.entries[i].successor = i ? second : first;
	}
	check(d2_store_apply(store, &env, &result) == D1_OK &&
		      result.entries[0].status == D1_OK,
	      "repair cut admits two-member cohort");
	repair = result.entries[0].cohort;
	first_txn = result.entries[0].member_txn[0];
	second_txn = result.entries[0].member_txn[1];
	memset(&env.body, 0, sizeof(env.body));
	env.key.sequence = 5;
	env.op = D1_OP_PREPARE_REPAIR;
	env.body.repair.range_end = 2;
	env.body.repair.count = 2;
	env.body.repair.cohort_present = true;
	env.body.repair.cohort = repair;
	for (unsigned int i = 0; i < 2; i++) {
		env.body.repair.entries[i].index = i;
		env.body.repair.entries[i].owner.cohort.raw = 3;
		env.body.repair.entries[i].owner.writer = 9;
		env.body.repair.entries[i].owner.co_id = 5 + i;
		env.body.repair.entries[i].txn_present = true;
		env.body.repair.entries[i].txn = i ? second_txn : first_txn;
		env.body.repair.entries[i].payload_present = true;
		env.body.repair.entries[i].payload = i ? payload : replacement;
		env.body.repair.entries[i].payload_len = 4096;
		d1_checksum_compute(D1_CKSUM_CRC32C,
				    env.body.repair.entries[i].payload, 4096,
				    &env.body.repair.entries[i].checksum);
	}
	if (fail_wal) {
		env.body.repair.entries[0].checksum.digest[0] ^= 0xff;
		wal_before = d2_store_wal_bytes(store);
		d2_store_fail_next_wal_write(store);
		check(d2_store_apply(store, &env, &result) == D1_IO &&
			      result.disposition == D1_UNRECORDED,
		      "kept abort WAL failure is unrecorded");
		check(d2_store_repair_state(store, repair.raw, &phase,
					    &member_count) &&
			      phase == D2_ADMITTED && member_count == 2,
		      "kept abort WAL failure restores the live cohort");
	} else {
		pid = fork();
		if (pid == 0) {
			d2_store_set_io_hook(store, cut_hook, &cut);
			(void)d2_store_apply(store, &env, &result);
			_exit(2);
		}
		check(pid > 0 && waitpid(pid, &child_status, 0) == pid &&
			      WIFEXITED(child_status) &&
			      WEXITSTATUS(child_status) == 100 + point,
		      "repair child exits at selected barrier");
	}
	d2_store_crash(store);
	store = NULL;
	if (!fail_wal && point == D2_IO_WAL_DURABLE) {
		uint8_t byte;
		int payload_fd = openat(dirfd, "payload", O_RDWR | O_CLOEXEC);

		check(payload_fd >= 0 &&
			      pread(payload_fd, &byte, 1,
				    5u * D2_PAYLOAD_ALIGN +
					    D2_PAYLOAD_HEADER_BYTES) == 1 &&
			      (++byte,
			       pwrite(payload_fd, &byte, 1,
				      5u * D2_PAYLOAD_ALIGN +
					      D2_PAYLOAD_HEADER_BYTES) == 1) &&
			      fdatasync(payload_fd) == 0,
		      "damage staged repair payload after durable WAL");
		if (payload_fd >= 0)
			close(payload_fd);
	}
	memcpy(reopen.files.expected_store_uuid, binding.store_uuid, 16);
	memcpy(reopen.files.expected_export_uuid, binding.export_uuid, 16);
	reopen.files.expected_root_ino = binding.root_ino;
	reopen.files.binding_token = token;
	reopen.files.binding_token_len = 16;
	reopen.chunk_bytes = 4096;
	reopen.max_file_bytes = 64u * 4096u;
	check(d2_store_rebind(dirfd, &reopen, &binding, &store) == D1_OK &&
		      d2_store_visible(store, &object, 0, &visible) &&
		      visible.raw == first.raw &&
		      d2_store_visible(store, &object, 1, &visible) &&
		      visible.raw == second.raw,
	      "repair barrier recovery preserves both visible members");
	if (fail_wal)
		check(store && d2_store_wal_bytes(store) ==
				       wal_before + D2_START_RECORD_BYTES &&
			      d2_store_repair_state(store, repair.raw, &phase,
						    &member_count) &&
			      phase == D2_ADMITTED && member_count == 2,
		      "failed kept abort remains admitted across restart");
	check(repair_log_atomic(dirfd, &binding),
	      "repair barrier log contains no partial vector");
	if (store)
		d2_store_close(store);
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
	const struct cut repair_cuts[] = {
		{ .target = D2_IO_PAYLOAD_WRITTEN, .occurrence = 1 },
		{ .target = D2_IO_PAYLOAD_DURABLE, .occurrence = 1 },
		{ .target = D2_IO_PAYLOAD_WRITTEN, .occurrence = 2 },
		{ .target = D2_IO_PAYLOAD_DURABLE, .occurrence = 2 },
		{ .target = D2_IO_WAL_WRITTEN, .occurrence = 1 },
		{ .target = D2_IO_WAL_DURABLE, .occurrence = 1 },
	};
	uint8_t token[16], payload[4096], replacement[4096];
	unsigned int i;
	int dirfd;

	if (!root)
		return 77;
	dirfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0)
		return 77;
	memset(token, 0x62, sizeof(token));
	memset(payload, 0x93, sizeof(payload));
	memset(replacement, 0xa4, sizeof(replacement));
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
		struct cut cut = { .target = points[i], .occurrence = 1 };
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
	for (i = 0; i < sizeof(repair_cuts) / sizeof(repair_cuts[0]); i++)
		repair_cut(dirfd, token, repair_cuts[i].target,
			   repair_cuts[i].occurrence, payload, replacement);
	repair_cut(dirfd, token, 0, 0, payload, replacement);
	{
		struct d2_store_config config = { 0 };
		struct d2_store_rebind reopen = { 0 };
		struct d2_binding binding = { 0 };
		struct d2_store *store = NULL;
		struct d1_objkey object = { 0 };
		struct d1_envelope env = { 0 };
		struct d1_result result;
		d1_admission_id admission;
		d1_txn_id txn;
		d1_version_id successor;
		d1_custody_id custody;
		d1_version_id restored_successor;
		d1_postcond_id postcond;
		uint64_t restored_index, wal_before;
		bool consumed;
		bool restored;
		struct io_counts counts = { 0 };

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
		      "postcondition cut fixture provisions");
		memset(object.export_uuid.bytes, 0x21, 16);
		memset(object.object_uuid.bytes, 0x55, 16);
		admission = d2_store_admit(store, &object, 9,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_REPAIR |
						   D1_RIGHT_SINGLE_WRITER);
		env.object = object;
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		memset(env.key.origin.bytes, 0x72, 16);
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
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "postcondition cut publishes successor");
		txn = result.entries[0].txn;
		successor = result.entries[0].version;
		custody = d2_store_custody(store, successor);
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 2;
		env.op = D1_OP_ROLLBACK_BATCH;
		env.body.rollback.range_end = 1;
		env.body.rollback.count = 1;
		env.body.rollback.entries[0].owner.cohort.raw = 1;
		env.body.rollback.entries[0].owner.writer = 9;
		env.body.rollback.entries[0].owner.co_id = 1;
		env.body.rollback.entries[0].txn = txn;
		env.body.rollback.entries[0].visible_present = true;
		env.body.rollback.entries[0].visible = successor;
		env.body.rollback.entries[0].custody_present = true;
		env.body.rollback.entries[0].custody = custody;
		wal_before = d2_store_wal_bytes(store);
		d2_store_set_io_hook(store, count_hook, &counts);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_NO_PREDECESSOR &&
			      result.entries[0].postcond_present &&
			      d2_store_wal_bytes(store) ==
				      wal_before + D2_POSTCOND_RECORD_BYTES +
					      D2_ENTRY_RECORD_BYTES &&
			      counts.point[D2_IO_WAL_WRITTEN] == 1 &&
			      counts.point[D2_IO_WAL_DURABLE] == 1,
		      "postcondition and receipt use one append and flush");
		postcond = result.entries[0].postcond;
		d2_store_set_io_hook(store, NULL, NULL);
		d2_store_crash(store);
		store = NULL;
		{
			uint8_t zeros[D2_ENTRY_RECORD_BYTES] = { 0 };
			int wal = openat(dirfd, "wal", O_RDWR | O_CLOEXEC);

			check(wal >= 0 &&
				      pwrite(wal, zeros, sizeof(zeros),
					     (off_t)(wal_before +
						     D2_POSTCOND_RECORD_BYTES)) ==
					      (ssize_t)sizeof(zeros) &&
				      fdatasync(wal) == 0,
			      "truncate grouped result to its orphan postcondition");
			if (wal >= 0)
				close(wal);
		}
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid, 16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid, 16);
		reopen.files.expected_root_ino = binding.root_ino;
		reopen.files.binding_token = token;
		reopen.files.binding_token_len = sizeof(token);
		reopen.chunk_bytes = config.chunk_bytes;
		reopen.max_file_bytes = config.max_file_bytes;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) == D1_OK,
		      "orphan postcondition replays after crash");
		restored = d2_store_postcond(store, postcond.raw, &restored_index,
					     &restored_successor, &consumed);
		check(restored &&
			      restored_index == 0 &&
			      restored_successor.raw == successor.raw && !consumed,
		      "orphan postcondition remains available after restart");
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) == D1_OK &&
			      d2_store_postcond(store, postcond.raw, &restored_index,
						&restored_successor, &consumed) &&
			      restored_successor.raw == successor.raw && !consumed,
		      "orphan postcondition survives another incarnation");
		if (store)
			d2_store_close(store);
	}
	close(dirfd);
	printf("D2 CRASH: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
