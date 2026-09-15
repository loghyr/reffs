/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: journal framing.
 *
 * The journal is an append-only byte log.  Every record carries its own
 * magic, type, sequence, length and CRC32C trailer, so a reader can
 * tell a whole intact record from a torn tail without being told where
 * the writer stopped -- which is what a reader after a crash actually
 * has.
 *
 * Reading stops at the first record that is not whole and intact.  That
 * point is the durable frontier: everything before it happened,
 * everything at or after it did not.
 */

#ifndef REFFS_D1_JOURNAL_H
#define REFFS_D1_JOURNAL_H

#include "d1_types.h"

/* magic(4) + type(4) + sequence(8) + length(4) */
#define D1_JOURNAL_HEADER_BYTES 20u
/* CRC32C over the header and the payload. */
#define D1_JOURNAL_TRAILER_BYTES 4u
#define D1_JOURNAL_MAGIC 0x44314a31u /* "D1J1" */

struct d1_journal {
	uint8_t *buf;
	size_t len;
	size_t cap;
	uint64_t next_seq;
	/*
	 * Fixture fault state.  It is deliberately not part of the log:
	 * a fault is a thing that happened to this run, not a fact about
	 * the stored data, so it cannot replay and cannot survive.
	 */
	bool fail_next;
};

bool d1_journal_init(struct d1_journal *j);
void d1_journal_fini(struct d1_journal *j);

/*
 * Append one record.  Returns false when the fixture has armed a
 * failure or the log cannot grow; the log is unchanged either way, so
 * a refused append leaves no torn record behind.
 */
bool d1_journal_append(struct d1_journal *j, uint32_t type,
		       const uint8_t *payload, uint32_t len);

/* Truncate to @len bytes, as a crash in the middle of a write would. */
void d1_journal_truncate(struct d1_journal *j, size_t len);

struct d1_journal_cursor {
	const uint8_t *buf;
	size_t len;
	size_t at;
};

void d1_journal_cursor_init(struct d1_journal_cursor *c, const uint8_t *buf,
			    size_t len);

/*
 * Advance to the next whole, intact record.  Returns false at the end
 * of the log and at the first record that is torn, mistyped or fails
 * its CRC -- the caller cannot tell those apart, and should not: both
 * mean the same thing, which is that the log ends here.
 */
bool d1_journal_next(struct d1_journal_cursor *c, uint32_t *type, uint64_t *seq,
		     const uint8_t **payload, uint32_t *len);

#endif /* REFFS_D1_JOURNAL_H */
