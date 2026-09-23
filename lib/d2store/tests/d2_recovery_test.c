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
#include <unistd.h>

#include "d1_types.h"
#include "d2_files.h"

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

static void clean_root(int dirfd)
{
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
}

static void setup_rebind(struct d2_rebind *r, const struct d2_binding *binding,
			 const uint8_t *token, size_t token_len)
{
	memcpy(r->expected_store_uuid, binding->store_uuid, 16);
	memcpy(r->expected_export_uuid, binding->export_uuid, 16);
	r->expected_root_ino = binding->root_ino;
	r->binding_token = token;
	r->binding_token_len = (uint32_t)token_len;
}

static bool control_record(struct d2_files *files, uint32_t subtype,
			   uint32_t body_len, uint8_t *record, size_t cap,
			   size_t *written)
{
	const struct d2_superblock *super = d2_files_super(files);
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };

	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, super->store_uuid, 16);
	memcpy(h.wal_uuid, super->wal_uuid, 16);
	h.lsn = d2_files_next_lsn(files);
	h.ds_incarnation = super->ds_incarnation;
	control.subtype = subtype;
	control.transition = D2_COMMITTED;
	control.status = D1_OK;
	memset(control.key.session, 0x71, 16);
	control.key.sequence = (uint32_t)h.lsn;
	d2_operation_key(control.key.session, 0, control.key.sequence, 0,
			 control.key.operation_key);
	control.body_len = body_len;
	if (subtype == D2_CTL_CERTIFICATE_INSTALL) {
		memset(control.body, 1, body_len);
	} else if (subtype == D2_CTL_EPISODE_CLEAR) {
		memset(control.body, 1, 16);
		control.body[23] = 1;
		control.body[31] = 16;
		memset(control.body + 32, 1, D1_CERTIFICATE_BYTES);
	}
	return d2_control_encode(&h, &control, record, cap, written);
}

int main(void)
{
	const char *root = getenv("D2_TEST_ROOT");
	struct d2_provision provision = { 0 };
	struct d2_rebind rebind = { 0 };
	struct d2_binding binding = { 0 };
	struct d2_scan_result scan;
	struct d2_start start_body;
	struct d2_wal_header header;
	struct d2_files *files = NULL;
	uint8_t token[16], tail[23], saved = 0, *overlong;
	uint8_t start[D2_START_RECORD_BYTES], forged[D2_START_RECORD_BYTES];
	uint32_t status;
	int dirfd, wal;
	ssize_t done;

	if (!root)
		return 77;
	dirfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0)
		return 77;
	clean_root(dirfd);
	memset(provision.export_uuid, 0x31, 16);
	memset(token, 0x52, sizeof(token));
	provision.binding_token = token;
	provision.binding_token_len = sizeof(token);
	provision.capacity_wal_bytes = D2_MIN_WAL_BYTES;
	provision.capacity_payload_bytes = 8u * 1024u * 1024u;
	check(d2_files_provision(dirfd, &provision, &binding, &files) == D1_OK,
	      "provision recovery fixture");
	if (!files)
		goto done;
	setup_rebind(&rebind, &binding, token, sizeof(token));
	d2_files_crash(files);
	files = NULL;
	wal = openat(dirfd, "wal", O_WRONLY | O_APPEND | O_CLOEXEC);
	memset(tail, 0xa5, sizeof(tail));
	check(wal >= 0 &&
		      write(wal, tail, sizeof(tail)) == (ssize_t)sizeof(tail) &&
		      fdatasync(wal) == 0,
	      "inject nonzero incomplete suffix");
	close(wal);
	check(d2_files_rebind(dirfd, &rebind, &binding, &files) == D1_OK,
	      "incomplete final suffix truncates");
	if (!files)
		goto done;
	scan = *d2_files_last_scan(files);
	check(scan.tail_class == D2_TAIL_INCOMPLETE &&
		      scan.truncated_bytes == sizeof(tail),
	      "tail classification records exact cut");
	d2_files_crash(files);
	files = NULL;
	wal = openat(dirfd, "wal", O_RDWR | O_CLOEXEC);
	check(wal >= 0 && pread(wal, &saved, 1, D2_START_RECORD_BYTES - 1) == 1,
	      "read complete-record CRC byte");
	saved ^= 1;
	check(pwrite(wal, &saved, 1, D2_START_RECORD_BYTES - 1) == 1 &&
		      fdatasync(wal) == 0,
	      "corrupt complete record");
	status = d2_files_rebind(dirfd, &rebind, &binding, &files);
	check(status != D1_OK && !files, "complete-record corruption fences");
	saved ^= 1;
	check(pwrite(wal, &saved, 1, D2_START_RECORD_BYTES - 1) == 1 &&
		      fdatasync(wal) == 0,
	      "restore complete record");
	close(wal);
	check(d2_files_rebind(dirfd, &rebind, &binding, &files) != D1_OK &&
		      !files,
	      "persisted fence survives byte restoration");
	clean_root(dirfd);
	memset(&binding, 0, sizeof(binding));
	check(d2_files_provision(dirfd, &provision, &binding, &files) == D1_OK,
	      "reprovision after administrative fence");
	if (!files)
		goto done;
	setup_rebind(&rebind, &binding, token, sizeof(token));
	d2_files_crash(files);
	files = NULL;
	wal = openat(dirfd, "wal", O_RDWR | O_CLOEXEC);
	check(wal >= 0 &&
		      pread(wal, start, sizeof(start), 0) ==
			      (ssize_t)sizeof(start) &&
		      d2_wal_header_decode(start, sizeof(start),
				   binding.store_uuid, binding.wal_uuid,
				   &header) &&
		      d2_start_decode(start, sizeof(start), &header, &start_body),
	      "decode START for identity fault");
	header.wal_uuid[0] ^= 1;
	check(d2_start_encode(&header, &start_body, forged) &&
		      pwrite(wal, forged, sizeof(forged), 0) ==
			      (ssize_t)sizeof(forged) &&
		      fdatasync(wal) == 0,
	      "inject CRC-valid WAL identity mismatch");
	check(d2_files_rebind(dirfd, &rebind, &binding, &files) != D1_OK &&
		      !files,
	      "WAL identity mismatch refuses rebind");
	check(pwrite(wal, start, sizeof(start), 0) == (ssize_t)sizeof(start) &&
		      fdatasync(wal) == 0,
	      "restore WAL identity");
	close(wal);
	rebind.expected_store_uuid[0] ^= 1;
	check(d2_files_rebind(dirfd, &rebind, &binding, &files) != D1_OK &&
		      !files,
	      "store identity mismatch refuses rebind");
	rebind.expected_store_uuid[0] ^= 1;
	rebind.expected_export_uuid[0] ^= 1;
	check(d2_files_rebind(dirfd, &rebind, &binding, &files) != D1_OK &&
		      !files,
	      "export identity mismatch refuses rebind");
	rebind.expected_export_uuid[0] ^= 1;
	rebind.expected_root_ino++;
	check(d2_files_rebind(dirfd, &rebind, &binding, &files) != D1_OK &&
		      !files,
	      "root identity mismatch refuses rebind");
	rebind.expected_root_ino--;
	token[0] ^= 1;
	check(d2_files_rebind(dirfd, &rebind, &binding, &files) != D1_OK &&
		      !files,
	      "binding-token mismatch refuses rebind");
	token[0] ^= 1;
	wal = openat(dirfd, "wal", O_WRONLY | O_APPEND | O_CLOEXEC);
	overlong = calloc(1, D2_MAX_WAL_RECORD + 1u);
	done = overlong && wal >= 0 ?
		       write(wal, overlong, D2_MAX_WAL_RECORD + 1u) :
		       -1;
	check(done == D2_MAX_WAL_RECORD + 1u && fdatasync(wal) == 0,
	      "inject overlong suffix");
	free(overlong);
	close(wal);
	check(d2_files_rebind(dirfd, &rebind, &binding, &files) != D1_OK &&
		      !files,
	      "overlong suffix fences before zero-tail rule");
	clean_root(dirfd);
	check(d2_files_provision(dirfd, &provision, &binding, &files) == D1_OK,
	      "provision low-headroom fixture");
	if (files) {
		uint8_t record[D2_MAX_RECORD_BYTES];
		size_t written;

		do {
			if (!control_record(files, D2_CTL_AUTHORITY_REVOKE, 28,
					    record, sizeof(record), &written)) {
				status = D1_INVALID;
				break;
			}
			status = d2_files_wal_append_floor(files, record, written,
							1184);
		} while (status == D1_OK);
		check(status == D1_NOSPC,
		      "ordinary append preserves ERROR completion promise");
		check(control_record(files, D2_CTL_CERTIFICATE_INSTALL, 56,
				     record, sizeof(record), &written) &&
			      written == 268 &&
			      d2_files_wal_append_floor(files, record, written,
							916) == D1_OK,
		      "certificate install spends its 268-byte promise");
		do {
			control_record(files, D2_CTL_AUTHORITY_REVOKE, 28,
				       record, sizeof(record), &written);
			status = d2_files_wal_append_floor(files, record, written,
							916);
		} while (status == D1_OK);
		check(status == D1_NOSPC &&
			      control_record(files, D2_CTL_EPISODE_CLEAR, 704,
					     record, sizeof(record), &written) &&
			      written == 916 &&
			      d2_files_wal_append_floor(files, record, written, 0) ==
				      D1_OK,
		      "episode clear spends its 916-byte promise");
	}

done:
	if (files)
		d2_files_close(files);
	close(dirfd);
	printf("D2 RECOVERY: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
