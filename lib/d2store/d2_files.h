/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef REFFS_D2_FILES_H
#define REFFS_D2_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "d2_format.h"

enum d2_io_point {
	D2_IO_PAYLOAD_WRITTEN = 1,
	D2_IO_PAYLOAD_DURABLE = 2,
	D2_IO_WAL_WRITTEN = 3,
	D2_IO_WAL_DURABLE = 4,
	D2_IO_SUPER_WRITTEN = 5,
	D2_IO_SUPER_DURABLE = 6,
};

enum d2_tail_class {
	D2_TAIL_CLEAN = 1,
	D2_TAIL_ZERO = 2,
	D2_TAIL_INCOMPLETE = 3,
	D2_TAIL_OVERLONG = 4,
	D2_TAIL_UNPARSEABLE = 5,
	D2_TAIL_BAD_COMPLETE = 6,
};

struct d2_binding {
	uint8_t store_uuid[16];
	uint8_t export_uuid[16];
	uint8_t fs_uuid[16];
	uint64_t root_dev;
	uint64_t root_ino;
	uint8_t wal_uuid[16];
	uint8_t payload_uuid[16];
};

struct d2_provision {
	uint8_t export_uuid[16];
	const uint8_t *binding_token;
	uint32_t binding_token_len;
	uint64_t capacity_wal_bytes;
	uint64_t capacity_payload_bytes;
};

struct d2_rebind {
	uint8_t expected_store_uuid[16];
	uint8_t expected_export_uuid[16];
	uint64_t expected_root_ino;
	const uint8_t *binding_token;
	uint32_t binding_token_len;
};

struct d2_scan_result {
	enum d2_tail_class tail_class;
	uint64_t valid_bytes;
	uint64_t truncated_bytes;
	uint64_t last_lsn;
	uint64_t last_incarnation;
	uint64_t payload_cursor;
	uint32_t record_count;
};

struct d2_files;

typedef bool (*d2_scan_fn)(const struct d2_wal_header *header,
			   const uint8_t *record, void *arg);
typedef void (*d2_io_hook_fn)(enum d2_io_point point, void *arg);

uint32_t d2_root_qualify(int dirfd, uint8_t fs_uuid[16], uint64_t *root_dev,
			 uint64_t *root_ino);
uint32_t d2_files_provision(int dirfd, const struct d2_provision *p,
			    struct d2_binding *binding, struct d2_files **out);
uint32_t d2_files_rebind(int dirfd, const struct d2_rebind *r,
			 struct d2_binding *binding, struct d2_files **out);
void d2_files_close(struct d2_files *files);
void d2_files_crash(struct d2_files *files);

void d2_files_set_io_hook(struct d2_files *files, d2_io_hook_fn hook,
			  void *arg);
const struct d2_superblock *d2_files_super(const struct d2_files *files);
uint64_t d2_files_next_lsn(const struct d2_files *files);
uint64_t d2_files_wal_cursor(const struct d2_files *files);
uint64_t d2_files_payload_cursor(const struct d2_files *files);
const struct d2_scan_result *d2_files_last_scan(const struct d2_files *files);

uint32_t d2_files_payload_append(struct d2_files *files,
				 const struct d2_payload_object *object,
				 uint64_t *offset);
uint32_t d2_files_wal_append(struct d2_files *files, const uint8_t *record,
			     size_t len);
uint32_t d2_files_super_update(struct d2_files *files, uint32_t state);
uint32_t d2_files_start(struct d2_files *files, uint32_t recovery_decision,
			uint64_t truncated_bytes, uint64_t payload_cursor,
			uint64_t live_wal_reserved,
			uint64_t live_payload_outstanding,
			uint64_t live_payload_staged);
uint32_t d2_files_scan(struct d2_files *files, d2_scan_fn fn, void *arg,
		       struct d2_scan_result *result, bool truncate_tail);
uint32_t d2_files_payload_read(struct d2_files *files, uint64_t offset,
			       struct d2_payload_object *object,
			       uint8_t **allocation);

#endif /* REFFS_D2_FILES_H */
