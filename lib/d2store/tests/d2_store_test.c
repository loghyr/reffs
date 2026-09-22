/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "d1_codec.h"
#include "d1_digest.h"
#include "d2_store.h"

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

static void fill(uint8_t *p, size_t len, uint8_t seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = (uint8_t)(seed + i);
}

static void write_request(struct d1_envelope *env, uint64_t sequence,
			  uint64_t index, uint32_t co_id,
			  const struct d1_guard *guard, const uint8_t *payload,
			  uint32_t len)
{
	uint32_t crc = d1_crc32c(payload, len);

	env->key.sequence = sequence;
	memset(&env->body, 0, sizeof(env->body));
	env->body.write.count = 1;
	env->body.write.stability = D1_FILE_SYNC;
	env->body.write.activate = true;
	env->body.write.entries[0].index = index;
	env->body.write.entries[0].owner.cohort.raw = 1;
	env->body.write.entries[0].owner.writer = 17;
	env->body.write.entries[0].owner.co_id = co_id;
	env->body.write.entries[0].guard_check = true;
	env->body.write.entries[0].expected = *guard;
	env->body.write.entries[0].payload = payload;
	env->body.write.entries[0].payload_len = len;
	env->body.write.entries[0].checksum.alg = D1_CKSUM_CRC32C;
	env->body.write.entries[0].checksum.len = 4;
	env->body.write.entries[0].checksum.digest[0] = (uint8_t)(crc >> 24);
	env->body.write.entries[0].checksum.digest[1] = (uint8_t)(crc >> 16);
	env->body.write.entries[0].checksum.digest[2] = (uint8_t)(crc >> 8);
	env->body.write.entries[0].checksum.digest[3] = (uint8_t)crc;
}

static bool entry_has_admission(int dirfd, uint64_t wal_bytes,
				const struct d2_binding *binding, uint64_t id)
{
	struct d2_wal_header header;
	struct d2_entry entry;
	uint8_t *wal;
	uint8_t expected[12] = { 'T', 'A', 'T', 'E' };
	uint64_t at = 0;
	unsigned int i;
	int fd = -1;
	bool found = false;

	for (i = 0; i < 8; i++)
		expected[4 + i] = (uint8_t)(id >> (56 - 8 * i));
	wal = malloc((size_t)wal_bytes);
	if (!wal)
		return false;
	fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
	if (fd < 0 ||
	    pread(fd, wal, (size_t)wal_bytes, 0) != (ssize_t)wal_bytes)
		goto out;
	while (at < wal_bytes &&
	       d2_wal_header_decode(wal + at, (size_t)(wal_bytes - at),
				    binding->store_uuid, binding->wal_uuid,
				    &header)) {
		if (header.family == D2_REC_ENTRY &&
		    d2_entry_decode(wal + at, header.total_bytes, &header,
				    &entry) &&
		    entry.admission.client_id == id &&
		    entry.admission.stateid_seqid == UINT32_C(0x44324253) &&
		    !memcmp(entry.admission.stateid_other, expected,
			    sizeof(expected)))
			found = true;
		at += header.total_bytes;
	}
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return found;
}

int main(void)
{
	const char *root = getenv("D2_TEST_ROOT");
	struct d2_store_config config = { 0 };
	struct d2_store_rebind reopen = { 0 };
	struct d2_binding binding = { 0 };
	struct d2_store *store = NULL;
	struct d1_objkey object = { 0 };
	struct d1_envelope env = { 0 };
	struct d1_envelope unsupported;
	struct d1_result result = { 0 };
	struct d1_result durable_result = { 0 };
	d1_admission_id admission;
	d1_version_id visible;
	struct d2_wal_header wal_header;
	struct d2_control control;
	struct d1_selection_spec selection = { .selection =
						       D1_SELECT_ORDINARY };
	struct d1_guard guard = { .never_written = true };
	struct d1_view *view = NULL;
	uint8_t payload[4096], replacement[4096], readback[4096], token[32];
	uint8_t registration[396];
	uint32_t read_len;
	uint64_t wal_bytes;
	int dirfd;

	if (!root)
		return 77;
	dirfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0)
		return 77;
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	fill(config.files.export_uuid, 16, 0x20);
	fill(token, sizeof(token), 0x40);
	config.files.binding_token = token;
	config.files.binding_token_len = sizeof(token);
	config.files.capacity_wal_bytes = D2_MIN_WAL_BYTES;
	config.files.capacity_payload_bytes = 64u * 1024u * 1024u;
	config.chunk_bytes = sizeof(payload);
	config.max_file_bytes = 64u * sizeof(payload);
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision fixed files");
	if (!store)
		goto done;
	fill(object.export_uuid.bytes, 16, 0x20);
	fill(object.object_uuid.bytes, 16, 0x60);
	admission = d2_store_admit(store, &object, 17,
				   D1_RIGHT_READ | D1_RIGHT_WRITE |
					   D1_RIGHT_SINGLE_WRITER);
	check(d1_admission_live(admission), "durable admission");
	{
		int wal_fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
		ssize_t got = wal_fd < 0 ? -1 :
					   pread(wal_fd, registration,
						 sizeof(registration),
						 D2_START_RECORD_BYTES);

		if (wal_fd >= 0)
			close(wal_fd);
		check(got == (ssize_t)sizeof(registration) &&
			      d2_wal_header_decode(
				      registration, sizeof(registration),
				      binding.store_uuid, binding.wal_uuid,
				      &wal_header) &&
			      d2_control_decode(registration,
						sizeof(registration),
						&wal_header, &control) &&
			      control.subtype == D2_CTL_FILE_REGISTER,
		      "file registration precedes object records");
	}
	fill(payload, sizeof(payload), 0x80);
	env.object = object;
	env.admission = admission;
	env.incarnation = d2_store_incarnation(store);
	fill(env.key.origin.bytes, 16, 0xa0);
	env.op = D1_OP_WRITE_BATCH;
	write_request(&env, 1, 0, 1, &guard, payload, sizeof(payload));
	check(d2_store_apply(store, &env, &result) == D1_OK,
	      "payload then WAL apply");
	check(entry_has_admission(dirfd, d2_store_wal_bytes(store), &binding,
				  admission.raw),
	      "ENTRY carries injective admission surrogate");
	check(result.entries[0].status == D1_OK &&
		      result.entries[0].disposition == D1_COMPLETED,
	      "exact result recorded");
	check(d2_store_visible(store, &object, 0, &visible),
	      "write is visible");
	check(d2_store_eof(store, &object) == sizeof(payload),
	      "visible EOF is derived");
	check(d2_store_view_open(store, &object, admission, &selection, 0,
				 sizeof(payload), &view) == D1_OK,
	      "read view pins visible payload");
	check(d2_store_guard(store, &object, 0, &guard), "visible guard reads");
	fill(replacement, sizeof(replacement), 0x90);
	write_request(&env, 2, 0, 2, &guard, replacement, sizeof(replacement));
	check(d2_store_apply(store, &env, &result) == D1_OK,
	      "replacement commits");
	check(d1_view_read(view, 0, readback, sizeof(readback), &read_len) ==
			      D1_OK &&
		      read_len == sizeof(readback) &&
		      !memcmp(readback, payload, sizeof(payload)),
	      "pin retains predecessor across replacement");
	d1_view_close(view);
	view = NULL;
	wal_bytes = d2_store_wal_bytes(store);
	check(d2_store_apply(store, &env, &result) == D1_OK &&
		      d2_store_wal_bytes(store) == wal_bytes,
	      "exact replay appends nothing");
	unsupported = env;
	unsupported.key.sequence = 99;
	unsupported.op = D1_OP_LEASE_REAP;
	memset(&unsupported.body, 0, sizeof(unsupported.body));
	unsupported.body.control.count = 1;
	unsupported.body.control.txns[0] = d2_store_txn_handle(store, 1);
	unsupported.body.control.old_admission = admission;
	check(d2_store_apply(store, &unsupported, &result) == D1_UNSUPPORTED &&
		      result.disposition == D1_UNRECORDED &&
		      d2_store_wal_bytes(store) == wal_bytes,
	      "unimplemented control is explicitly unsupported");
	guard = (struct d1_guard){ .never_written = true };
	write_request(&env, 3, 5, 3, &guard, payload, sizeof(payload));
	check(d2_store_apply(store, &env, &result) == D1_OK &&
		      d2_store_eof(store, &object) == 6u * sizeof(payload),
	      "sparse high chunk extends EOF");
	write_request(&env, 4, 3, 4, &guard, payload, sizeof(payload));
	d2_store_fail_next_index(store);
	check(d2_store_apply(store, &env, &result) == D1_OK &&
		      d2_store_eof(store, &object) == 6u * sizeof(payload),
	      "durable commit survives failed index publication");
	durable_result = result;
	d2_store_crash(store);
	store = NULL;
	memcpy(reopen.files.expected_store_uuid, binding.store_uuid, 16);
	memcpy(reopen.files.expected_export_uuid, binding.export_uuid, 16);
	reopen.files.expected_root_ino = binding.root_ino;
	reopen.files.binding_token = token;
	reopen.files.binding_token_len = sizeof(token);
	reopen.chunk_bytes = config.chunk_bytes;
	reopen.max_file_bytes = config.max_file_bytes;
	check(d2_store_rebind(dirfd, &reopen, &binding, &store) == D1_OK,
	      "fresh process rebinds fixed files");
	check(store && d2_store_visible(store, &object, 0, &visible),
	      "replay reconstructs visible version");
	check(store && d2_store_eof(store, &object) == 6u * sizeof(payload),
	      "replay reconstructs EOF");
	check(store && d2_store_visible(store, &object, 3, &visible) &&
		      d2_store_visible(store, &object, 5, &visible),
	      "replay reconstructs sparse visible map");
	if (store) {
		env.admission = d2_store_admission_handle(store, admission.raw);
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].version.raw ==
				      durable_result.entries[0].version.raw &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "restart returns exact durable receipt once");
	}
	if (store) {
		uint8_t byte;
		int payload_fd;

		check(d2_store_close(store) == D1_OK,
		      "close before payload corruption");
		store = NULL;
		payload_fd = openat(dirfd, "payload", O_RDWR | O_CLOEXEC);
		check(payload_fd >= 0 &&
			      pread(payload_fd, &byte, 1,
				    D2_PAYLOAD_ALIGN +
					    D2_PAYLOAD_HEADER_BYTES) == 1 &&
			      (++byte,
			       pwrite(payload_fd, &byte, 1,
				      D2_PAYLOAD_ALIGN +
					      D2_PAYLOAD_HEADER_BYTES) == 1) &&
			      fdatasync(payload_fd) == 0,
		      "corrupt a referenced payload object");
		if (payload_fd >= 0)
			close(payload_fd);
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_IO,
		      "scan fences a corrupt referenced payload object");
	}

done:
	if (store)
		d2_store_close(store);
	close(dirfd);
	printf("D2 STORE: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
