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
#include <string.h>

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
 * A typed identifier, and the store that issued it.
 *
 * Section 3 makes version, transaction, cohort, custody, episode, pin
 * and admission handles disjoint typed IDs, and one u64 is what the
 * canonical form carries for each of them.  One u64 was also all the C
 * model carried, and that was two holes rather than one.  Every store
 * starts each of its counters at one, so a handle from one store
 * resolved against another store's table of the same kind and
 * authorized work there; and a lookup chose its table from the
 * parameter the number arrived in, so an admission passed where a
 * version belongs was resolved as a version.
 *
 * So the canonical value keeps its u64 and gains two things that never
 * reach the encoding.  The type is the C type: these are different
 * structs, and neither the compiler nor a lookup will take one for
 * another.  The provenance is the issuing store's UUID, which is this
 * model's own name for a store -- the same name a journal carries and a
 * rebuild checks, so a handle survives a reopen of the store that
 * issued it and is refused by any other.  It is an exact identity, not
 * a hash or a partition of the value: nothing is derived, nothing can
 * collide that the model does not already treat as one store, and
 * there is no counter to wrap or restart.
 *
 * Zero is absent, as it always was, and an absent handle has no issuer.
 */
typedef struct d1_admission_id {
	uint64_t raw;
	/* The issuing store's UUID; zero when the handle is absent. */
	struct d1_uuid store;
} d1_admission_id;

typedef struct d1_txn_id {
	uint64_t raw;
	/* The issuing store's UUID; zero when the handle is absent. */
	struct d1_uuid store;
} d1_txn_id;

typedef struct d1_version_id {
	uint64_t raw;
	/* The issuing store's UUID; zero when the handle is absent. */
	struct d1_uuid store;
} d1_version_id;

typedef struct d1_custody_id {
	uint64_t raw;
	/* The issuing store's UUID; zero when the handle is absent. */
	struct d1_uuid store;
} d1_custody_id;

/*
 * The cohort is a typed handle too, and the one the store never issues:
 * it names the caller's own cohort, so it has no issuer and only its
 * type keeps it apart from the others.
 */
typedef struct d1_cohort_id {
	uint64_t raw;
} d1_cohort_id;

/*
 * The absent handle of each kind.  Zero names nothing and is issued by
 * nobody, so this is the one raw value a caller may make a handle out
 * of: it can be compared and it can be passed, and it resolves to no
 * row in any store.
 */
static inline d1_admission_id d1_admission_none(void)
{
	d1_admission_id id = { 0, { { 0 } } };

	return id;
}

static inline d1_txn_id d1_txn_none(void)
{
	d1_txn_id id = { 0, { { 0 } } };

	return id;
}

static inline d1_version_id d1_version_none(void)
{
	d1_version_id id = { 0, { { 0 } } };

	return id;
}

static inline d1_custody_id d1_custody_none(void)
{
	d1_custody_id id = { 0, { { 0 } } };

	return id;
}

/* Whether a handle names anything: zero is absent, everywhere. */
static inline bool d1_admission_live(d1_admission_id id)
{
	return id.raw != 0;
}

static inline bool d1_txn_live(d1_txn_id id)
{
	return id.raw != 0;
}

static inline bool d1_version_live(d1_version_id id)
{
	return id.raw != 0;
}

static inline bool d1_custody_live(d1_custody_id id)
{
	return id.raw != 0;
}

static inline bool d1_cohort_live(d1_cohort_id id)
{
	return id.raw != 0;
}

/*
 * Whether two handles are the same handle: the same value, issued by
 * the same store.  Two stores' first admissions are both one and are
 * not the same admission.
 */
static inline bool d1_admission_eq(d1_admission_id a, d1_admission_id b)
{
	return a.raw == b.raw &&
	       memcmp(a.store.bytes, b.store.bytes, D1_UUID_BYTES) == 0;
}

static inline bool d1_txn_eq(d1_txn_id a, d1_txn_id b)
{
	return a.raw == b.raw &&
	       memcmp(a.store.bytes, b.store.bytes, D1_UUID_BYTES) == 0;
}

static inline bool d1_version_eq(d1_version_id a, d1_version_id b)
{
	return a.raw == b.raw &&
	       memcmp(a.store.bytes, b.store.bytes, D1_UUID_BYTES) == 0;
}

static inline bool d1_custody_eq(d1_custody_id a, d1_custody_id b)
{
	return a.raw == b.raw &&
	       memcmp(a.store.bytes, b.store.bytes, D1_UUID_BYTES) == 0;
}

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
	d1_cohort_id cohort;
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
