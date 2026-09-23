/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef REFFS_D2_FORMAT_H
#define REFFS_D2_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D2_FORMAT_VERSION 1u
#define D2_PROLOGUE_BYTES 36u
#define D2_WAL_HEADER_BYTES 72u
#define D2_SB_BODY_BYTES 320u
#define D2_SB_RECORD_BYTES 360u
#define D2_SB_SLOT_BYTES 4096u
#define D2_SUPER_BYTES 8192u
#define D2_PAYLOAD_HEADER_BYTES 136u
#define D2_PAYLOAD_FILE_HEADER_BYTES 80u
#define D2_PAYLOAD_ALIGN 4096u
#define D2_START_BODY_BYTES 121u
#define D2_START_RECORD_BYTES 197u
#define D2_CONTROL_PREFIX_BYTES 136u
#define D2_ENTRY_BODY_BYTES 460u
#define D2_ENTRY_RECORD_BYTES 536u
#define D2_POSTCOND_RECORD_BYTES 280u
#define D2_CERTIFICATE_RECORD_BYTES \
	(D2_WAL_HEADER_BYTES + D2_CONTROL_PREFIX_BYTES + 56u + 4u)
#define D2_EPISODE_CLEAR_MAX_RECORD_BYTES \
	(D2_WAL_HEADER_BYTES + D2_CONTROL_PREFIX_BYTES + 64u + \
	 40u * D2_MAX_BATCH_ENTRIES + 4u)
#define D2_COHORT_PREFIX_BYTES 256u
#define D2_COHORT_MEMBER_BYTES 244u
#define D2_MAX_CONTROL_BODY_BYTES 3076u
#define D2_MAX_RECORD_BYTES 4236u
#define D2_MAX_WAL_RECORD (1024u * 1024u)
#define D2_MAX_HANDLE_BYTES 128u
#define D2_MAX_CHECKSUM_BYTES 64u
#define D2_MAX_BATCH_ENTRIES 16u
#define D2_MAX_AUTHORITY_STATEIDS 64u
#define D2_MAX_COUNTED_ENTRIES 64u
#define D2_MAX_LIVE_TXNS 64u
#define D2_MIN_WAL_BYTES 3352780u
#define D2_RESTART_HEADROOM 51909u
#define D2_RESTART_SWEEP 51712u
#define D2_RECOVERY_HEADROOM 3322572u
#define D2_MIN_RESTARTS 64u
#define D2_MAX_INCARNATION 0x00ffffffu

#define D2_SB_MAGIC 0x44325342u
#define D2_WAL_MAGIC 0x4432574cu
#define D2_PAYLOAD_MAGIC 0x44325059u

enum d2_record_type {
	D2_SB_SLOT = 1,
	D2_PAYLOAD_DATA = 1,
	D2_PAYLOAD_FILE_HEADER = 2,
	D2_REC_START = 1,
	D2_REC_CONTROL = 2,
	D2_REC_ENTRY = 3,
	D2_REC_COHORT = 4,
};

enum d2_transition {
	D2_ADMITTED = 1,
	D2_PREPARED = 2,
	D2_FINALIZED = 3,
	D2_COMMITTED = 4,
	D2_ABORTED = 5,
	D2_ROLLED_BACK = 6,
	D2_REFUSED = 7,
	D2_UNLOCKED = 8,
};

enum d2_control_subtype {
	D2_CTL_FILE_REGISTER = 1,
	D2_CTL_FILE_TOMBSTONE = 2,
	D2_CTL_AUTHORITY_ADMIT = 3,
	D2_CTL_AUTHORITY_REVOKE = 4,
	D2_CTL_TRUST_STATEID = 5,
	D2_CTL_REVOKE_STATEID = 6,
	D2_CTL_LEASE_REAP = 7,
	D2_CTL_RECOVERY_ADMIT = 8,
	D2_CTL_EPISODE_MARK = 9,
	D2_CTL_EPISODE_CLEAR = 10,
	D2_CTL_CUSTODY = 11,
	D2_CTL_COORDINATOR_COMPLETION = 12,
	D2_CTL_EXPORT_TOMBSTONE = 13,
	D2_CTL_POSTCOND = 14,
	D2_CTL_LEASE_EXPIRE = 15,
	D2_CTL_CERTIFICATE_INSTALL = 16,
};

enum d2_super_state {
	D2_SB_CLEAN = 1,
	D2_SB_NEEDS_RECOVERY = 2,
	D2_SB_FENCED = 3,
	D2_SB_RETIRED = 4,
};

enum d2_extent_kind {
	D2_EXTENT_UNCHANGED = 1,
	D2_EXTENT_EXTEND = 2,
	D2_EXTENT_SHRINK = 3,
};

enum d2_recovery_decision {
	D2_RD_FIRST_PROVISION = 1u << 0,
	D2_RD_CLEAN_SCAN = 1u << 1,
	D2_RD_TRUNCATED_TAIL = 1u << 2,
	D2_RD_DISCARDED_UNSTABLE = 1u << 3,
	D2_RD_ADMIN_CLEARED_FENCE = 1u << 4,
	D2_RD_TRUNCATED_NONZERO_TAIL = 1u << 5,
};

struct d2_superblock {
	uint8_t store_uuid[16];
	uint64_t generation;
	uint8_t fs_uuid[16];
	uint64_t fs_dev_hint;
	uint16_t root_handle_type;
	uint16_t root_handle_len;
	uint8_t root_handle[D2_MAX_HANDLE_BYTES];
	uint8_t export_uuid[16];
	uint64_t ds_incarnation;
	uint64_t verifier_epoch;
	uint8_t write_verifier[8];
	uint8_t wal_uuid[16];
	uint8_t payload_uuid[16];
	uint8_t binding_token_digest[32];
	uint64_t wal_durable_lsn;
	uint64_t wal_durable_bytes;
	uint64_t payload_durable_bytes;
	uint64_t capacity_wal_bytes;
	uint64_t capacity_payload_bytes;
	uint32_t state;
	uint32_t format_floor;
};

struct d2_wal_header {
	uint16_t family;
	uint32_t total_bytes;
	uint8_t store_uuid[16];
	uint8_t wal_uuid[16];
	uint64_t lsn;
	uint64_t ds_incarnation;
};

struct d2_start {
	uint64_t ds_incarnation;
	uint64_t prev_incarnation;
	uint64_t verifier_epoch;
	bool verifier_changed;
	uint32_t recovery_decision;
	uint64_t truncated_bytes;
	uint64_t last_valid_lsn_before;
	uint64_t wal_append_cursor;
	uint64_t payload_append_cursor;
	uint64_t capacity_wal_bytes;
	uint64_t capacity_payload_bytes;
	uint8_t export_uuid[16];
	uint64_t live_txn_wal_reserved;
	uint64_t live_txn_payload_outstanding;
	uint64_t live_txn_payload_staged;
};

struct d2_payload_header {
	uint8_t store_uuid[16];
	uint8_t payload_uuid[16];
	uint8_t wal_uuid[16];
};

struct d2_payload_object {
	uint8_t store_uuid[16];
	uint64_t payload_object_id;
	uint32_t content_crc32c;
	uint32_t content_alg;
	uint32_t content_ck_len;
	uint8_t content_ck[D2_MAX_CHECKSUM_BYTES];
	uint32_t content_len;
	uint64_t object_bytes;
	const uint8_t *content;
};

struct d2_key_block {
	uint8_t session[16];
	uint32_t slot;
	uint32_t sequence;
	uint32_t compound_ordinal;
	uint8_t operation_key[32];
	uint8_t request_digest[32];
};

struct d2_admission_block {
	uint8_t session[16];
	uint8_t principal[16];
	uint8_t issuer[16];
	uint64_t authority_epoch;
	uint64_t fence_sequence;
	uint64_t lease_epoch;
	uint64_t client_id;
	uint32_t stateid_seqid;
	uint8_t stateid_other[12];
	uint32_t writer;
	uint32_t rights;
};

struct d2_control {
	uint32_t subtype;
	uint32_t transition;
	uint32_t status;
	struct d2_key_block key;
	uint8_t admission_issuer[16];
	uint64_t admission_authority_epoch;
	uint64_t admission_client_id;
	uint32_t body_len;
	uint8_t body[D2_MAX_CONTROL_BODY_BYTES];
};

struct d2_entry {
	uint32_t transition;
	uint32_t status;
	uint32_t disposition;
	uint32_t stability;
	uint32_t batch_ordinal;
	uint32_t batch_count;
	uint8_t file_key[32];
	uint64_t chunk_index;
	uint64_t txn_id;
	struct d2_key_block key;
	struct d2_admission_block admission;
	uint64_t owner_cohort;
	uint32_t owner_client_id;
	uint32_t owner_co_id;
	uint32_t generation;
	bool predecessor_present;
	uint64_t predecessor_object_id;
	uint32_t predecessor_generation;
	bool postcond_present;
	uint64_t postcond_id;
	uint64_t payload_object_id;
	uint64_t payload_object_offset;
	uint32_t payload_content_len;
	uint32_t extent_kind;
	uint64_t extent_high_water;
	uint64_t extent_highest_index;
	uint64_t index_generation;
	uint8_t result_verifier[8];
	bool result_activated;
	uint64_t result_visible_object_id;
	uint32_t result_effective_len;
	uint32_t result_ck_alg;
	uint32_t result_ck_len;
	uint8_t result_ck[D2_MAX_CHECKSUM_BYTES];
	bool result_guard_never_written;
	uint32_t result_guard_generation;
	uint32_t result_guard_writer;
};

struct d2_cohort_member {
	uint32_t repair_mode;
	uint8_t file_key[32];
	uint64_t chunk_index;
	uint64_t member_txn_id;
	uint64_t owner_cohort;
	uint32_t owner_client_id;
	uint32_t owner_co_id;
	uint64_t custody_id;
	bool postcond_present;
	uint64_t postcond_id;
	uint64_t successor_object_id;
	bool predecessor_present;
	uint64_t predecessor_object_id;
	uint32_t predecessor_generation;
	uint64_t payload_object_id;
	uint64_t payload_object_offset;
	uint32_t payload_content_len;
	bool staged;
	uint32_t member_status;
	uint32_t extent_kind;
	uint64_t extent_high_water;
	uint64_t extent_highest_index;
	uint64_t result_visible_object_id;
	uint32_t result_effective_len;
	uint32_t result_ck_alg;
	uint32_t result_ck_len;
	uint8_t result_ck[D2_MAX_CHECKSUM_BYTES];
	bool result_guard_never_written;
	uint32_t result_guard_generation;
	uint32_t result_guard_writer;
};

struct d2_cohort {
	uint32_t transition;
	uint32_t status;
	uint32_t disposition;
	uint64_t cohort_id;
	uint8_t episode_uuid[16];
	uint32_t flags;
	struct d2_key_block key;
	struct d2_admission_block admission;
	uint64_t index_generation;
	uint8_t result_verifier[8];
	uint32_t member_count;
	struct d2_cohort_member members[D2_MAX_BATCH_ENTRIES];
};

uint32_t d2_crc32c_domain(const char *domain, const void *buf, size_t len);
void d2_hash_domain(const char *domain, const void *buf, size_t len,
		    uint8_t out[32]);
void d2_file_key(const void *handle, uint32_t len, uint8_t out[32]);
void d2_operation_key(const uint8_t session[16], uint32_t slot,
		      uint32_t sequence, uint32_t ordinal, uint8_t out[32]);

bool d2_super_encode(const struct d2_superblock *sb,
		     uint8_t out[D2_SB_SLOT_BYTES]);
bool d2_super_decode(const uint8_t in[D2_SB_SLOT_BYTES],
		     const uint8_t expected_store[16],
		     struct d2_superblock *sb);
bool d2_payload_header_encode(const struct d2_payload_header *h,
			      uint8_t out[D2_PAYLOAD_ALIGN]);
bool d2_payload_header_decode(const uint8_t in[D2_PAYLOAD_ALIGN],
			      const uint8_t expected_store[16],
			      struct d2_payload_header *h);
uint64_t d2_payload_object_bytes(uint32_t content_len);
bool d2_payload_encode(const struct d2_payload_object *o, uint8_t *out,
		       size_t cap, size_t *written);
bool d2_payload_decode(const uint8_t *in, size_t len,
		       const uint8_t expected_store[16],
		       struct d2_payload_object *o);
bool d2_payload_decode_content(const uint8_t *in, size_t len,
			       const uint8_t expected_store[16],
			       struct d2_payload_object *o, bool *content_ok);

bool d2_wal_header_decode(const uint8_t *in, size_t len,
			  const uint8_t expected_store[16],
			  const uint8_t expected_wal[16],
			  struct d2_wal_header *h);
bool d2_start_encode(const struct d2_wal_header *h, const struct d2_start *s,
		     uint8_t out[D2_START_RECORD_BYTES]);
bool d2_start_decode(const uint8_t *record, size_t len,
		     const struct d2_wal_header *h, struct d2_start *s);
bool d2_control_encode(const struct d2_wal_header *h,
		       const struct d2_control *c, uint8_t *out, size_t cap,
		       size_t *written);
bool d2_control_decode(const uint8_t *record, size_t len,
		       const struct d2_wal_header *h, struct d2_control *c);
bool d2_entry_encode(const struct d2_wal_header *h, const struct d2_entry *e,
		     uint8_t out[D2_ENTRY_RECORD_BYTES]);
bool d2_entry_decode(const uint8_t *record, size_t len,
		     const struct d2_wal_header *h, struct d2_entry *e);
bool d2_cohort_encode(const struct d2_wal_header *h, const struct d2_cohort *c,
		      uint8_t *out, size_t cap, size_t *written);
bool d2_cohort_decode(const uint8_t *record, size_t len,
		      const struct d2_wal_header *h, struct d2_cohort *c);

#endif /* REFFS_D2_FORMAT_H */
