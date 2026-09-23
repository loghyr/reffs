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

static bool authority_precedes_entry(int dirfd, uint64_t wal_bytes,
				     const struct d2_binding *binding,
				     uint64_t admission_id)
{
	struct d2_wal_header header;
	struct d2_control control;
	struct d2_entry entry;
	uint8_t *wal;
	uint64_t at = 0;
	int fd = -1;
	bool trust = false, authority = false, ok = false;

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
		if (header.family == D2_REC_CONTROL &&
		    d2_control_decode(wal + at, header.total_bytes, &header,
				      &control) &&
		    control.admission_client_id == admission_id) {
			if (control.subtype == D2_CTL_TRUST_STATEID)
				trust = !authority;
			else if (control.subtype == D2_CTL_AUTHORITY_ADMIT)
				authority = trust;
		} else if (header.family == D2_REC_ENTRY &&
			   d2_entry_decode(wal + at, header.total_bytes,
					   &header, &entry) &&
			   entry.admission.client_id == admission_id) {
			ok = trust && authority;
			break;
		}
		at += header.total_bytes;
	}
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return ok;
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
	struct d1_envelope committed_rollback;
	struct d1_envelope expired;
	struct d1_envelope nopre;
	struct d1_envelope recovery;
	struct d1_envelope refused;
	struct d1_envelope unsupported;
	struct d1_result result = { 0 };
	struct d1_result durable_result = { 0 };
	struct d1_result refused_result = { 0 };
	d1_admission_id admission;
	d1_admission_id control_admission, fresh_admission, next_control,
		next_fresh;
	d1_custody_id custody;
	d1_postcond_id postcond;
	d1_txn_id staged_txn;
	d1_version_id predecessor, successor, visible;
	struct d2_wal_header wal_header;
	struct d2_control control;
	struct d1_selection_spec selection = { .selection =
						       D1_SELECT_ORDINARY };
	struct d1_guard guard = { .never_written = true };
	struct d1_view *view = NULL;
	uint8_t payload[4096], replacement[4096], readback[4096], token[32];
	uint8_t verifier[D1_VERIFIER_BYTES];
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
	{
		uint8_t persisted[sizeof(payload)];
		int payload_fd = openat(dirfd, "payload", O_RDONLY | O_CLOEXEC);

		check(payload_fd >= 0 &&
			      pread(payload_fd, persisted, sizeof(persisted),
				    D2_PAYLOAD_ALIGN +
					    D2_PAYLOAD_HEADER_BYTES) ==
				      (ssize_t)sizeof(persisted) &&
			      !memcmp(persisted, payload, sizeof(persisted)),
		      "payload object contains application bytes");
		if (payload_fd >= 0)
			close(payload_fd);
	}
	check(entry_has_admission(dirfd, d2_store_wal_bytes(store), &binding,
				  admission.raw),
	      "ENTRY carries injective admission surrogate");
	check(authority_precedes_entry(dirfd, d2_store_wal_bytes(store),
				       &binding, admission.raw),
	      "trust and covering authority precede admitted work");
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
	refused = env;
	write_request(&refused, 40, 0, 40,
		      &(struct d1_guard){ .never_written = true }, payload,
		      sizeof(payload));
	check(d2_store_apply(store, &refused, &refused_result) == D1_OK &&
		      refused_result.entries[0].status == D1_GUARDED,
	      "guard refusal is recorded without changing state");
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
		wal_bytes = d2_store_wal_bytes(store);
		refused.admission =
			d2_store_admission_handle(store, refused.admission.raw);
		check(d2_store_apply(store, &refused, &result) == D1_OK &&
			      result.entries[0].status == D1_GUARDED &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "restart returns exact refused receipt");
	}
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
		check(d2_store_visible(store, &object, 5, &predecessor) &&
			      d2_store_guard(store, &object, 5, &guard),
		      "committed rollback captures predecessor");
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_REPAIR |
						   D1_RIGHT_SINGLE_WRITER);
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		env.op = D1_OP_WRITE_BATCH;
		write_request(&env, 20, 5, 20, &guard, replacement,
			      sizeof(replacement));
		env.body.write.activate = false;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "committed rollback stages replacement");
		staged_txn = result.entries[0].txn;
		successor = result.entries[0].version;
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 21;
		env.op = D1_OP_FINALIZE_BATCH;
		env.body.lifecycle.range_begin = 5;
		env.body.lifecycle.range_end = 6;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = 5;
		env.body.lifecycle.entries[0].owner.cohort.raw = 1;
		env.body.lifecycle.entries[0].owner.writer = 17;
		env.body.lifecycle.entries[0].owner.co_id = 20;
		env.body.lifecycle.entries[0].txn = staged_txn;
		env.body.lifecycle.entries[0].predecessor_present = true;
		env.body.lifecycle.entries[0].predecessor = predecessor;
		d2_store_verifier(store, verifier);
		memcpy(env.body.lifecycle.prior_verifier, verifier,
		       sizeof(verifier));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_FINALIZED,
		      "replacement finalizes before custody rollback");
		env.key.sequence = 22;
		env.op = D1_OP_COMMIT_BATCH;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_COMMITTED,
		      "replacement commits before custody rollback");
		custody = d2_store_custody(store, successor);
		check(d1_custody_live(custody), "custody issue is durable");
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 23;
		env.op = D1_OP_ROLLBACK_BATCH;
		env.body.rollback.range_begin = 5;
		env.body.rollback.range_end = 6;
		env.body.rollback.count = 1;
		env.body.rollback.entries[0].index = 5;
		env.body.rollback.entries[0].owner.cohort.raw = 1;
		env.body.rollback.entries[0].owner.writer = 17;
		env.body.rollback.entries[0].owner.co_id = 20;
		env.body.rollback.entries[0].txn = staged_txn;
		env.body.rollback.entries[0].visible_present = true;
		env.body.rollback.entries[0].visible = successor;
		env.body.rollback.entries[0].predecessor_present = true;
		env.body.rollback.entries[0].predecessor = predecessor;
		env.body.rollback.entries[0].custody_present = true;
		env.body.rollback.entries[0].custody = custody;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_ROLLED_BACK &&
			      d2_store_visible(store, &object, 5, &visible) &&
			      visible.raw == predecessor.raw,
		      "custody rollback restores predecessor");
		committed_rollback = env;
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
				      D1_OK &&
			      d2_store_visible(store, &object, 5, &visible) &&
			      visible.raw == predecessor.raw,
		      "restart replays custody rollback");
		if (store) {
			committed_rollback.admission = d2_store_admission_handle(
				store, committed_rollback.admission.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &committed_rollback,
					     &result) == D1_OK &&
				      result.entries[0].phase ==
					      D2_ROLLED_BACK &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns committed rollback receipt");
		}
	}
	if (store) {
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 30, 20, 30, &guard, payload,
			      sizeof(payload));
		env.body.write.activate = false;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_PREPARED,
		      "recovery fixture leaves prepared work");
		staged_txn = result.entries[0].txn;
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "recovery fixture crosses an incarnation");
	}
	if (store) {
		control_admission =
			d2_store_admit(store, &object, 17, D1_RIGHT_CONTROL);
		fresh_admission =
			d2_store_admit(store, &object, 17,
				       D1_RIGHT_READ | D1_RIGHT_WRITE |
					       D1_RIGHT_SINGLE_WRITER);
		memset(&recovery, 0, sizeof(recovery));
		recovery.object = object;
		recovery.admission = control_admission;
		recovery.incarnation = d2_store_incarnation(store);
		fill(recovery.key.origin.bytes, 16, 0xc0);
		recovery.key.sequence = 1;
		recovery.op = D1_OP_RECOVERY_ADMIT;
		recovery.body.control.count = 1;
		recovery.body.control.txns[0] =
			d2_store_txn_handle(store, staged_txn.raw);
		recovery.body.control.old_admission =
			d2_store_admission_handle(store, admission.raw);
		recovery.body.control.new_admission_present = true;
		recovery.body.control.new_admission = fresh_admission;
		recovery.body.control.read_epoch_present = true;
		recovery.body.control.read_epoch = 0;
		check(d2_store_apply(store, &recovery, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "recovery admission rebinds prepared work");
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "recovery admission replays from portable controls");
	}
	if (store) {
		recovery.admission =
			d2_store_admission_handle(store, control_admission.raw);
		recovery.body.control.txns[0] =
			d2_store_txn_handle(store, staged_txn.raw);
		recovery.body.control.old_admission =
			d2_store_admission_handle(store, admission.raw);
		recovery.body.control.new_admission =
			d2_store_admission_handle(store, fresh_admission.raw);
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_apply(store, &recovery, &result) == D1_OK &&
			      result.entries[0].status == D1_OK &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "restart returns exact recovery admission receipt");
		next_control =
			d2_store_admit(store, &object, 17, D1_RIGHT_CONTROL);
		next_fresh = d2_store_admit(store, &object, 17,
					    D1_RIGHT_READ | D1_RIGHT_WRITE |
						    D1_RIGHT_SINGLE_WRITER);
		recovery.admission = next_control;
		recovery.incarnation = d2_store_incarnation(store);
		recovery.key.sequence = 2;
		recovery.body.control.old_admission =
			d2_store_admission_handle(store, fresh_admission.raw);
		recovery.body.control.new_admission = next_fresh;
		check(d2_store_apply(store, &recovery, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "later recovery uses the transaction's current admission");
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = next_fresh;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, 16, 0xd0);
		env.key.sequence = 1;
		env.op = D1_OP_FINALIZE_BATCH;
		env.body.lifecycle.range_begin = 20;
		env.body.lifecycle.range_end = 21;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = 20;
		env.body.lifecycle.entries[0].owner.cohort.raw = 1;
		env.body.lifecycle.entries[0].owner.writer = 17;
		env.body.lifecycle.entries[0].owner.co_id = 30;
		env.body.lifecycle.entries[0].txn =
			d2_store_txn_handle(store, staged_txn.raw);
		d2_store_verifier(store, verifier);
		memcpy(env.body.lifecycle.prior_verifier, verifier,
		       sizeof(verifier));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_FINALIZED,
		      "recovered admission resumes prepared work");
		recovery.key.sequence = 3;
		recovery.body.control.old_admission =
			d2_store_admission_handle(store, admission.raw);
		check(d2_store_apply(store, &recovery, &result) == D1_OK &&
			      result.entries[0].status == D1_OWNER_CONFLICT,
		      "recovery refusal is recorded without rebinding work");
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_REPAIR |
						   D1_RIGHT_SINGLE_WRITER);
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, 16, 0xe0);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 1, 22, 31, &guard, payload,
			      sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "postcondition fixture publishes a successor");
		staged_txn = result.entries[0].txn;
		successor = result.entries[0].version;
		custody = d2_store_custody(store, successor);
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 2;
		env.op = D1_OP_ROLLBACK_BATCH;
		env.body.rollback.range_begin = 22;
		env.body.rollback.range_end = 23;
		env.body.rollback.count = 1;
		env.body.rollback.entries[0].index = 22;
		env.body.rollback.entries[0].owner.cohort.raw = 1;
		env.body.rollback.entries[0].owner.writer = 17;
		env.body.rollback.entries[0].owner.co_id = 31;
		env.body.rollback.entries[0].txn = staged_txn;
		env.body.rollback.entries[0].visible_present = true;
		env.body.rollback.entries[0].visible = successor;
		env.body.rollback.entries[0].custody_present = true;
		env.body.rollback.entries[0].custody = custody;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_NO_PREDECESSOR &&
			      result.entries[0].postcond_present,
		      "no-predecessor rollback persists its postcondition group");
		postcond = result.entries[0].postcond;
		nopre = env;
	}
	if (store) {
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		guard = (struct d1_guard){ .never_written = true };
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		env.op = D1_OP_WRITE_BATCH;
		write_request(&env, 5, 6, 5, &guard, replacement,
			      sizeof(replacement));
		check(d2_store_apply(store, &env, &result) == D1_OK,
		      "post-restart write uses the next incarnation");
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 6, 7, 6, &guard, payload, sizeof(payload));
		env.body.write.activate = false;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_PREPARED,
		      "staged write persists as PREPARED");
		staged_txn = result.entries[0].txn;
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 7;
		env.op = D1_OP_FINALIZE_BATCH;
		env.body.lifecycle.range_begin = 7;
		env.body.lifecycle.range_end = 8;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = 7;
		env.body.lifecycle.entries[0].owner.cohort.raw = 1;
		env.body.lifecycle.entries[0].owner.writer = 17;
		env.body.lifecycle.entries[0].owner.co_id = 6;
		env.body.lifecycle.entries[0].txn = staged_txn;
		d2_store_verifier(store, verifier);
		memcpy(env.body.lifecycle.prior_verifier, verifier,
		       sizeof(verifier));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_FINALIZED,
		      "single member finalize persists without payload");
		env.key.sequence = 8;
		env.op = D1_OP_COMMIT_BATCH;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_COMMITTED &&
			      d2_store_visible(store, &object, 7, &visible),
		      "single member commit publishes staged payload");
		guard = (struct d1_guard){ .never_written = true };
		env.op = D1_OP_WRITE_BATCH;
		write_request(&env, 9, 8, 7, &guard, replacement,
			      sizeof(replacement));
		env.body.write.activate = false;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_PREPARED,
		      "rollback fixture stages private work");
		staged_txn = result.entries[0].txn;
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 10;
		env.op = D1_OP_ROLLBACK_BATCH;
		env.body.rollback.range_begin = 8;
		env.body.rollback.range_end = 9;
		env.body.rollback.count = 1;
		env.body.rollback.entries[0].index = 8;
		env.body.rollback.entries[0].owner.cohort.raw = 1;
		env.body.rollback.entries[0].owner.writer = 17;
		env.body.rollback.entries[0].owner.co_id = 7;
		env.body.rollback.entries[0].txn = staged_txn;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_ROLLED_BACK &&
			      !d2_store_visible(store, &object, 8, &visible),
		      "private rollback persists without payload");
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "second restart rebinds both incarnations");
		check(store && d2_store_visible(store, &object, 6, &visible),
		      "second restart publishes post-restart write");
		check(store && d2_store_visible(store, &object, 7, &visible),
		      "second restart replays finalize and commit");
		if (store) {
			check(!d2_store_visible(store, &object, 8, &visible),
			      "second restart replays private rollback");
			nopre.admission = d2_store_admission_handle(
				store, nopre.admission.raw);
			nopre.body.rollback.entries[0].txn = d2_store_txn_handle(
				store, nopre.body.rollback.entries[0].txn.raw);
			nopre.body.rollback.entries[0]
				.visible = d2_store_version_handle(
				store,
				nopre.body.rollback.entries[0].visible.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &nopre, &result) == D1_OK &&
				      result.entries[0].status ==
					      D1_NO_PREDECESSOR &&
				      result.entries[0].postcond.raw ==
					      postcond.raw &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns exact no-predecessor receipt");
			recovery.admission = d2_store_admission_handle(
				store, recovery.admission.raw);
			recovery.body.control.txns[0] = d2_store_txn_handle(
				store, recovery.body.control.txns[0].raw);
			recovery.body.control
				.old_admission = d2_store_admission_handle(
				store, recovery.body.control.old_admission.raw);
			recovery.body.control
				.new_admission = d2_store_admission_handle(
				store, recovery.body.control.new_admission.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &recovery, &result) ==
					      D1_OK &&
				      result.entries[0].status ==
					      D1_OWNER_CONFLICT &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns exact refused recovery receipt");
			env.admission =
				d2_store_admission_handle(store, admission.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].phase ==
					      D2_ROLLED_BACK &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns exact rollback receipt");
		}
	}
	if (store) {
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 11, 9, 8, &guard, payload, sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "expiry fixture records its admission");
		d2_store_expire(store, admission);
		write_request(&env, 12, 10, 9, &guard, payload,
			      sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_STALE_AUTH,
		      "durable lease expiry refuses later work");
		expired = env;
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		env.admission = admission;
		write_request(&env, 13, 11, 10, &guard, replacement,
			      sizeof(replacement));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "revocation fixture records its admission");
		d2_store_revoke(store, admission);
		write_request(&env, 14, 12, 11, &guard, replacement,
			      sizeof(replacement));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_STALE_AUTH,
		      "durable stateid revocation refuses later work");
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "liveness controls replay in LSN order");
		if (store) {
			env.admission =
				d2_store_admission_handle(store, admission.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status ==
					      D1_STALE_AUTH &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns receipt after stateid revocation");
			expired.admission = d2_store_admission_handle(
				store, expired.admission.raw);
			check(d2_store_apply(store, &expired, &result) ==
					      D1_OK &&
				      result.entries[0].status ==
					      D1_STALE_AUTH &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns receipt after lease expiry");
		}
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
