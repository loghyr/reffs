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

/*
 * Which repair a cohort member is.  Section 7 lets one local cohort
 * carry both, explicitly tagged per member, so the tag is part of the
 * request and not derived from the state the member names.
 */
enum d1_repair_mode {
	D1_REPAIR_ERROR = 1,
	D1_REPAIR_NOPRE = 2,
};

/*
 * The cross-DS completion certificate clear_error requires.  It is an
 * opaque fixed-width blob this model only ever compares: what issues it
 * is outside D1, and a fixture stands in for that issuer.
 */
#define D1_CERTIFICATE_BYTES 32u

/* An uninterpreted identity. */
struct d1_uuid {
	uint8_t bytes[D1_UUID_BYTES];
};

/*
 * A handle: one canonical value, one domain, one issuing instance.
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
 * Distinct C types alone did not close either hole.  The structs had
 * the same layout and public fields, so a memcpy from an admission
 * into a version, or a compound literal assembled from an admission's
 * fields, made a version the compiler had refused to make; and the
 * issuing store's UUID is a durable name for a store rather than a
 * name for one live instance of it, so two stores opened at once under
 * one UUID -- which this model does deliberately, a source and a
 * pristine replay target -- aliased each other's handles.
 *
 * So the canonical value keeps its u64 and the C model keeps three
 * separate things apart:
 *
 *   the canonical identity  the raw u64, the only part encoded;
 *   the handle domain       which table the value names, as a runtime
 *                           kind every resolver checks before it
 *                           chooses a table;
 *   the runtime issuer      which live store object issued or adopted
 *                           the value, as a process-local instance
 *                           token.
 *
 * Neither the kind nor the token reaches the encoding, and neither is
 * durable: an in-memory handle does not survive a process restart, and
 * nothing in a journal names either one.  The token is not a store
 * identity -- the UUID still is -- it is API provenance, and it exists
 * because two live objects of one UUID have separate locks, tables and
 * counters and can evolve apart.
 *
 * The canonical value stays a public field, because it is the one part
 * of a handle a caller legitimately reads and compares -- it is what a
 * receipt carries and what two histories agree about.  The kind and the
 * token are private, named with a leading underscore, and hidden in the
 * sense that matters: they survive an ordinary copy of the whole value
 * and they are checked before the handle selects anything, so a memcpy
 * of an admission into a version carries the admission's kind and names
 * nothing, and a literal assembled from an admission's exposed value
 * carries no kind at all and names nothing either.
 *
 * Zero is absent, as it always was, and an absent handle has no domain
 * and no issuer.
 */

/*
 * The handle domains the store issues.  The owner's cohort is not among
 * them: it names the caller's own cohort, the store never issues one
 * and nothing resolves one against a table, so its C type is the whole
 * of its domain.  A repair cohort is a different thing wearing a
 * similar word -- the store opens it, keeps its staged vector and
 * resolves it against a table -- so it is a handle like the rest.
 */
enum d1_handle_kind {
	D1_HANDLE_NONE = 0,
	D1_HANDLE_ADMISSION = 1,
	D1_HANDLE_TXN = 2,
	D1_HANDLE_VERSION = 3,
	D1_HANDLE_CUSTODY = 4,
	D1_HANDLE_REPAIR = 5,
	D1_HANDLE_POSTCOND = 6,
};

typedef struct d1_admission_id {
	uint64_t raw;
	/*
	 * Which table this value may name; see d1_handle_kind.  Zero for
	 * a handle nothing typed: an absent one, a decoder's output, or
	 * bytes copied out of another domain's handle, all of which name
	 * nothing anywhere.
	 */
	uint32_t _kind;
	/*
	 * The live store object that issued or adopted this value, or
	 * zero for one no live store has -- an absent handle, or a value
	 * a decoder has just read out of a log and replay has not yet
	 * bound to the store it is rebuilding.
	 */
	uint64_t _instance;
} d1_admission_id;

typedef struct d1_txn_id {
	uint64_t raw;
	/*
	 * Which table this value may name; see d1_handle_kind.  Zero for
	 * a handle nothing typed: an absent one, a decoder's output, or
	 * bytes copied out of another domain's handle, all of which name
	 * nothing anywhere.
	 */
	uint32_t _kind;
	/*
	 * The live store object that issued or adopted this value, or
	 * zero for one no live store has -- an absent handle, or a value
	 * a decoder has just read out of a log and replay has not yet
	 * bound to the store it is rebuilding.
	 */
	uint64_t _instance;
} d1_txn_id;

typedef struct d1_version_id {
	uint64_t raw;
	/*
	 * Which table this value may name; see d1_handle_kind.  Zero for
	 * a handle nothing typed: an absent one, a decoder's output, or
	 * bytes copied out of another domain's handle, all of which name
	 * nothing anywhere.
	 */
	uint32_t _kind;
	/*
	 * The live store object that issued or adopted this value, or
	 * zero for one no live store has -- an absent handle, or a value
	 * a decoder has just read out of a log and replay has not yet
	 * bound to the store it is rebuilding.
	 */
	uint64_t _instance;
} d1_version_id;

typedef struct d1_custody_id {
	uint64_t raw;
	/*
	 * Which table this value may name; see d1_handle_kind.  Zero for
	 * a handle nothing typed: an absent one, a decoder's output, or
	 * bytes copied out of another domain's handle, all of which name
	 * nothing anywhere.
	 */
	uint32_t _kind;
	/*
	 * The live store object that issued or adopted this value, or
	 * zero for one no live store has -- an absent handle, or a value
	 * a decoder has just read out of a log and replay has not yet
	 * bound to the store it is rebuilding.
	 */
	uint64_t _instance;
} d1_custody_id;

/*
 * The cohort is a typed handle too, and the one the store never issues:
 * it names the caller's own cohort, so it has no domain to confuse and
 * no issuer to check, and only its type keeps it apart from the others.
 */
typedef struct d1_cohort_id {
	uint64_t raw;
} d1_cohort_id;

/*
 * A repair cohort, opened by begin_repair and named by every later
 * member of the same repair.  The store issues it, keeps the exact
 * vector it captured, and resolves it against its own table, so it
 * carries a domain and an issuer like every other handle it issues.
 */
typedef struct d1_repair_id {
	uint64_t raw;
	uint32_t _kind;
	uint64_t _instance;
} d1_repair_id;

/*
 * The postcondition a refused rollback leaves behind.
 *
 * Section 4 makes it part of the rollback result, and section 7 has
 * NOPRE repair "consume a retained postcondition bound to that
 * unchanged successor".  The store mints it, keeps what it was bound
 * to, and resolves it against its own table, so it is a handle like
 * the rest.
 */
typedef struct d1_postcond_id {
	uint64_t raw;
	uint32_t _kind;
	uint64_t _instance;
} d1_postcond_id;

/*
 * The canonical value of a handle: the one part of it the wire and the
 * journal carry, and the only part two stores that ran the same history
 * agree about.
 */
static inline uint64_t d1_admission_raw(d1_admission_id id)
{
	return id.raw;
}

static inline uint64_t d1_txn_raw(d1_txn_id id)
{
	return id.raw;
}

static inline uint64_t d1_version_raw(d1_version_id id)
{
	return id.raw;
}

static inline uint64_t d1_custody_raw(d1_custody_id id)
{
	return id.raw;
}

static inline uint64_t d1_repair_raw(d1_repair_id id)
{
	return id.raw;
}

static inline uint64_t d1_postcond_raw(d1_postcond_id id)
{
	return id.raw;
}

/*
 * The absent handle of each kind.  Zero names nothing and is issued by
 * nobody, so this is the one raw value a caller may make a handle out
 * of: it can be compared and it can be passed, and it resolves to no
 * row in any store.
 */
static inline d1_admission_id d1_admission_none(void)
{
	d1_admission_id id = { 0, D1_HANDLE_NONE, 0 };

	return id;
}

static inline d1_txn_id d1_txn_none(void)
{
	d1_txn_id id = { 0, D1_HANDLE_NONE, 0 };

	return id;
}

static inline d1_version_id d1_version_none(void)
{
	d1_version_id id = { 0, D1_HANDLE_NONE, 0 };

	return id;
}

static inline d1_custody_id d1_custody_none(void)
{
	d1_custody_id id = { 0, D1_HANDLE_NONE, 0 };

	return id;
}

static inline d1_repair_id d1_repair_none(void)
{
	d1_repair_id id = { 0, D1_HANDLE_NONE, 0 };

	return id;
}

static inline d1_postcond_id d1_postcond_none(void)
{
	d1_postcond_id id = { 0, D1_HANDLE_NONE, 0 };

	return id;
}

/*
 * Whether a handle names anything: zero is absent, everywhere.
 *
 * This asks about the canonical value alone, because that is the
 * question the canonical form can answer -- a decoder reading a log
 * produces a handle with no issuer, and it is still a request to name
 * something.  Whether the thing it names is in this store is the
 * resolver's question, not this one.
 */
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

static inline bool d1_repair_live(d1_repair_id id)
{
	return id.raw != 0;
}

static inline bool d1_postcond_live(d1_postcond_id id)
{
	return id.raw != 0;
}

/*
 * Whether two handles are the same handle: the same value, of the same
 * domain, from the same live store.  Two stores' first admissions are
 * both one and are not the same admission, and neither are a store's
 * and its replay target's.
 *
 * This is the identity of a live C handle, not of a logical one.  Two
 * stores that ran the same history hold the same canonical values under
 * different tokens, so an oracle asking whether two histories agree
 * compares the raw values -- which is what the log carries and all it
 * carries.
 */
static inline bool d1_admission_eq(d1_admission_id a, d1_admission_id b)
{
	return a.raw == b.raw && a._kind == b._kind &&
	       a._instance == b._instance;
}

static inline bool d1_txn_eq(d1_txn_id a, d1_txn_id b)
{
	return a.raw == b.raw && a._kind == b._kind &&
	       a._instance == b._instance;
}

static inline bool d1_version_eq(d1_version_id a, d1_version_id b)
{
	return a.raw == b.raw && a._kind == b._kind &&
	       a._instance == b._instance;
}

static inline bool d1_custody_eq(d1_custody_id a, d1_custody_id b)
{
	return a.raw == b.raw && a._kind == b._kind &&
	       a._instance == b._instance;
}

static inline bool d1_repair_eq(d1_repair_id a, d1_repair_id b)
{
	return a.raw == b.raw && a._kind == b._kind &&
	       a._instance == b._instance;
}

static inline bool d1_postcond_eq(d1_postcond_id a, d1_postcond_id b)
{
	return a.raw == b.raw && a._kind == b._kind &&
	       a._instance == b._instance;
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
