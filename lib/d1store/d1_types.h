/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: frozen numeric tags, limits and value types.
 *
 * This is a standalone userspace model of a Flex Files v2 data-server
 * store.  It is not the reffs store, not a wire format, and not a
 * kernel interface.  Nothing here is linked into reffs's server paths.
 *
 * Every enumeration below is encoded on the wire of the model's own
 * canonical form as a big-endian u32, and every value is frozen: a tag
 * may be added, never renumbered, because recorded journals and golden
 * fixtures decode by number.  The golden fixtures in the tests beside
 * this header are the record of those numbers.
 */

#ifndef REFFS_D1_TYPES_H
#define REFFS_D1_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Uninterpreted 16-byte identity, never parsed by the model. */
#define D1_UUID_BYTES 16

/* The verifier a START event publishes and lifecycle calls carry. */
#define D1_VERIFIER_BYTES 8

/* SHA-256 of the domain-separated canonical envelope. */
#define D1_DIGEST_BYTES 32

/*
 * Geometry and batch limits.  These are the model's declared bounds, not
 * a protocol maximum: every addition and multiplication that can reach
 * them is checked before it is performed.
 */
#define D1_CHUNK_BYTES_MIN 1u
#define D1_CHUNK_BYTES_MAX 1048576u
#define D1_BATCH_ENTRIES_MIN 1u
#define D1_BATCH_ENTRIES_MAX 16u
#define D1_BATCH_PAYLOAD_MAX (4u * 1024u * 1024u)
#define D1_JOURNAL_RECORD_MAX (8u * 1024u * 1024u)

/* Status of one entry or one whole operation. */
enum d1_status {
	D1_OK = 1,
	D1_INVALID = 2,
	D1_GUARDED = 3,
	D1_OWNER_CONFLICT = 4,
	D1_STALE_AUTH = 5,
	D1_BAD_PHASE = 6,
	D1_NO_PREDECESSOR = 7,
	D1_QUARANTINED = 8,
	D1_CHECKSUM = 9,
	D1_NOSPC = 10,
	D1_IO = 11,
	D1_REPLAY_CONFLICT = 12,
	/*
	 * Entry points this slice does not implement answer with this and
	 * mutate nothing.  It is a model tag, not a draft status.
	 */
	D1_UNSUPPORTED = 13,
	/*
	 * A logical close with views or calls still outstanding, or a
	 * destruction of a store that has not been closed.  A model tag
	 * rather than a draft status: section 6 names the condition, and
	 * the draft's status list has no word for it.
	 */
	D1_BUSY = 14,
};

/*
 * Whether the result was recorded.  A semantic error that reached a
 * receipt is COMPLETED; an inability to reserve or append one is
 * UNRECORDED and changes no state.
 */
enum d1_disposition {
	D1_COMPLETED = 1,
	D1_UNRECORDED = 2,
};

/* Transaction phase.  ADMITTED belongs to repair before its payload. */
enum d1_phase {
	D1_PHASE_ADMITTED = 1,
	D1_PHASE_PREPARED = 2,
	D1_PHASE_FINALIZED = 3,
	D1_PHASE_COMMITTED = 4,
	D1_PHASE_ABORTED = 5,
	D1_PHASE_ROLLED_BACK = 6,
};

/* Transaction mode.  A REPAIR member is refused by ordinary lifecycle. */
enum d1_mode {
	D1_MODE_ORDINARY = 1,
	D1_MODE_REPAIR = 2,
};

/*
 * Requested stability.  All three are persisted as strongly as
 * FILE_SYNC by this model; the request still selects activation
 * eligibility, so it is recorded rather than normalised away.
 */
enum d1_stability {
	D1_UNSTABLE = 1,
	D1_DATA_SYNC = 2,
	D1_FILE_SYNC = 3,
};

/* Read selection. */
enum d1_selection {
	D1_SELECT_ORDINARY = 1,
	D1_SELECT_OWNER = 2,
};

/* Payload checksum algorithms, four big-endian digest bytes each. */
enum d1_checksum_alg {
	D1_CKSUM_CRC32 = 1,
	D1_CKSUM_CRC32C = 2,
};

/* Journal record types, as they appear in the framed header. */
enum d1_record_type {
	D1_REC_START = 1,
	D1_REC_CONTROL = 2,
	D1_REC_ENTRY = 3,
	D1_REC_COHORT = 4,
};

/* Operation body tags of the typed sum an Envelope carries. */
enum d1_op {
	D1_OP_WRITE_BATCH = 1,
	D1_OP_FINALIZE_BATCH = 2,
	D1_OP_COMMIT_BATCH = 3,
	D1_OP_ROLLBACK_BATCH = 4,
	D1_OP_MARK_ERROR = 5,
	D1_OP_BEGIN_REPAIR = 6,
	D1_OP_PREPARE_REPAIR = 7,
	D1_OP_FINALIZE_REPAIR = 8,
	D1_OP_COMMIT_REPAIR = 9,
	D1_OP_ABORT_REPAIR = 10,
	D1_OP_CLEAR_ERROR = 11,
	D1_OP_UNLOCK = 12,
	D1_OP_RECOVERY_ADMIT = 13,
	D1_OP_LEASE_REAP = 14,
};

/* Rights an admission handle grants, validated per operation. */
enum d1_right {
	D1_RIGHT_READ = 0x00000001u,
	D1_RIGHT_WRITE = 0x00000002u,
	D1_RIGHT_REPAIR = 0x00000004u,
	D1_RIGHT_CONTROL = 0x00000008u,
	D1_RIGHT_SINGLE_WRITER = 0x00000010u,
};

/*
 * Writer IDs 0 and 0xffffffff are reserved and never issued by the
 * fixture authority; a request bearing one is malformed.
 */
#define D1_WRITER_RESERVED_LOW 0x00000000u
#define D1_WRITER_RESERVED_HIGH 0xffffffffu

/* An uninterpreted identity. */
struct d1_uuid {
	uint8_t bytes[D1_UUID_BYTES];
};

/*
 * A typed identifier.  Zero means absent everywhere, which is why
 * presence is carried by an option tag rather than by testing for zero.
 */
typedef uint64_t d1_id_t;

/* The CAS guard of one chunk: (generation, writer), plus never-written. */
struct d1_guard {
	uint32_t generation;
	uint32_t writer;
	/*
	 * A chunk that has never been written is tagged, not inferred from
	 * generation zero: zero is a valid generation after the first
	 * successful write.
	 */
	bool never_written;
};

/* The client-supplied owner triple of one chunk write. */
struct d1_owner {
	uint64_t cohort;
	uint32_t writer;
	uint32_t co_id;
};

/* Operation key: fixture-allocated origin, never-wrapping sequence. */
struct d1_opkey {
	struct d1_uuid origin;
	uint64_t sequence;
	uint32_t ordinal;
};

/* A tagged payload checksum. */
struct d1_checksum {
	uint32_t alg;
	uint32_t len;
	uint8_t digest[4];
};

/* Object key. */
struct d1_objkey {
	struct d1_uuid export_uuid;
	struct d1_uuid object_uuid;
};

#endif /* REFFS_D1_TYPES_H */
