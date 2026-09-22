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

int main(void)
{
	const char *root = getenv("D2_TEST_ROOT");
	struct d2_provision provision = { 0 };
	struct d2_rebind rebind = { 0 };
	struct d2_binding binding = { 0 };
	struct d2_scan_result scan;
	struct d2_files *files = NULL;
	uint8_t token[16], tail[23], saved = 0, *overlong;
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
	rebind.expected_store_uuid[0] ^= 1;
	check(d2_files_rebind(dirfd, &rebind, &binding, &files) != D1_OK &&
		      !files,
	      "store identity mismatch refuses rebind");
	rebind.expected_store_uuid[0] ^= 1;
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

done:
	if (files)
		d2_files_close(files);
	close(dirfd);
	printf("D2 RECOVERY: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
