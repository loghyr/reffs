/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: journal framing, as section 9 fixes it.
 *
 * The simulator keeps an append vector and a SEPARATE durable length.
 * Flush advances that length, and only ever to a whole record boundary.
 * That separation is the whole point: bytes past the durable length are
 * a torn suffix the writer never claimed, while anything wrong inside
 * the durable prefix is corruption of data the writer did claim, and
 * the two must not be confused.
 *
 * So a reader has three outcomes, not two.  It consumed the prefix
 * exactly; or it found the prefix malformed and fails closed, including
 * in its final record; or it has another record to hand back.  There is
 * no outcome in which a completed record quietly disappears.
 *
 * This is a declared failure model for tests, not a claim about real
 * disk atomicity.
 */

#ifndef REFFS_D1_JOURNAL_H
#define REFFS_D1_JOURNAL_H

#include "d1_types.h"

#define D1_JOURNAL_MAGIC 0x44314a31u /* "D1J1" */
/*
 * The record format this build writes and the only one it reads.
 *
 * Two, not one: the D1b slice added a repair cohort and a postcondition
 * to the entry result, a request digest to the Envelope control record,
 * five canonical fields to the repair request, and a COHORT record
 * family that a format-1 reader has no arm for.  None of that is
 * readable as format 1 and none of format 1 is readable as this, so the
 * number says so rather than leaving a reader to decode one as the
 * other.  Nothing is deployed, so there is no migration to write; what
 * there is to do is refuse the other version, which is what a reader
 * that checks this does.
 */
#define D1_JOURNAL_FORMAT 2u
#define D1_JOURNAL_HEADER_BYTES 56u
#define D1_JOURNAL_TRAILER_BYTES 4u

struct d1_journal {
	uint8_t *buf;
	/* Appended bytes, whether or not they are claimed durable. */
	size_t len;
	/* Claimed durable bytes; always a whole record boundary. */
	size_t durable;
	/* The next LSN as of the last flush, so a rollback can restore it. */
	uint64_t durable_lsn;
	size_t cap;
	/* Strictly increasing from one, globally across the log. */
	uint64_t next_lsn;
	/* The incarnation every record after the latest START carries. */
	uint64_t incarnation;
	struct d1_uuid store_uuid;
	/*
	 * Fixture fault state.  Deliberately not in the log: a fault is
	 * something that happened to this run, not a fact about the stored
	 * data, so it can neither replay nor survive.
	 */
	/*
	 * Zero is disarmed; n means the nth append from now fails.  A
	 * count rather than a flag so a test can aim at one member of a
	 * batch instead of only at its first.
	 */
	uint32_t fail_append_in;
	bool fail_next_flush;
};

bool d1_journal_init(struct d1_journal *j, const struct d1_uuid *store_uuid);
void d1_journal_fini(struct d1_journal *j);

/*
 * Append one record at the current incarnation and the next LSN.
 * Returns false when a fault is armed, the log cannot grow, or a
 * counter would overflow.  The log is unchanged either way, so a
 * refused append leaves no torn record behind and consumes no LSN.
 */
bool d1_journal_append(struct d1_journal *j, uint32_t type, const uint8_t *body,
		       uint32_t len);

/*
 * Adopt an existing durable prefix and continue its LSNs.  A reopen
 * writes on top of the history it rebuilt rather than starting a new
 * log beside it.
 */
bool d1_journal_adopt(struct d1_journal *j, const uint8_t *log, size_t durable,
		      uint64_t next_lsn);

/* Claim everything appended so far as durable.  Whole records only. */
bool d1_journal_flush(struct d1_journal *j);

/*
 * Discard everything appended since the last successful flush, and give
 * back the LSNs those records took.
 *
 * This is the enclosing event's undo, not the byte vector's own policy:
 * an append that is never claimed is a legitimate thing for a buffer to
 * be holding, but an event whose reducer has been undone must not be
 * left there for the next flush to claim.  It never moves the durable
 * length, so a record that was successfully claimed is never discarded.
 */
void d1_journal_rollback(struct d1_journal *j);

/* The incarnation subsequent records carry; START sets it. */
void d1_journal_set_incarnation(struct d1_journal *j, uint64_t incarnation);

/* What a read of the durable prefix found. */
enum d1_journal_read {
	/* Another whole, intact record is available. */
	D1_JOURNAL_RECORD = 1,
	/* The durable prefix was consumed exactly. */
	D1_JOURNAL_CLEAN_END = 2,
	/*
	 * Something inside the durable prefix is not a whole intact
	 * record.  The writer claimed these bytes, so this fails closed;
	 * it is never reported as a clean end.
	 */
	D1_JOURNAL_CORRUPT = 3,
};

struct d1_journal_cursor {
	const uint8_t *buf;
	/* The durable length supplied by the caller, never the append length. */
	size_t durable;
	size_t at;
	const struct d1_uuid *expect_uuid;
	uint64_t last_lsn;
};

/*
 * @expect_uuid is an argument, not something taken from the log: a
 * record is not trusted to say which store it belongs to.
 */
void d1_journal_cursor_init(struct d1_journal_cursor *c, const uint8_t *buf,
			    size_t durable, const struct d1_uuid *expect_uuid);

enum d1_journal_read d1_journal_next(struct d1_journal_cursor *c,
				     uint32_t *type, uint64_t *lsn,
				     uint64_t *incarnation,
				     const uint8_t **body, uint32_t *len);

#endif /* REFFS_D1_JOURNAL_H */
