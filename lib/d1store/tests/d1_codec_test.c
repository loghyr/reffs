/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * D1 storage model: the frozen tag table, the canonical encoding and the
 * three digest domains.
 *
 * The expected bytes here are written out literally rather than produced
 * by the code under test.  A fixture that asked the encoder what it
 * encodes would agree with any encoder, including a changed one; these
 * are the numbers a recorded journal is decoded by, so they are spelled
 * out and read by eye.
 */

#include <stdio.h>
#include <string.h>

#include "d1_codec.h"
#include "d1_control.h"
#include "d1_digest.h"
#include "d1_journal.h"

static unsigned int failures;

static void check(bool ok, const char *what)
{
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static void check_bytes(const void *got, size_t got_len, const void *want,
			size_t want_len, const char *what)
{
	if (got_len != want_len || memcmp(got, want, want_len) != 0) {
		const uint8_t *g = got;
		size_t i;

		failures++;
		fprintf(stderr, "FAIL: %s\n  got ", what);
		for (i = 0; i < got_len; i++)
			fprintf(stderr, "%02x", g[i]);
		fprintf(stderr, "\n  want ");
		for (i = 0; i < want_len; i++)
			fprintf(stderr, "%02x", ((const uint8_t *)want)[i]);
		fprintf(stderr, "\n");
	}
}

/* The frozen tag table.  A renumbering here is a format change. */
static void test_tag_table(void)
{
	check(D1_OK == 1 && D1_INVALID == 2 && D1_GUARDED == 3 &&
		      D1_OWNER_CONFLICT == 4 && D1_STALE_AUTH == 5 &&
		      D1_BAD_PHASE == 6 && D1_NO_PREDECESSOR == 7 &&
		      D1_QUARANTINED == 8 && D1_CHECKSUM == 9 &&
		      D1_NOSPC == 10 && D1_IO == 11 &&
		      D1_REPLAY_CONFLICT == 12 && D1_UNSUPPORTED == 13,
	      "status tags");
	check(D1_COMPLETED == 1 && D1_UNRECORDED == 2, "disposition tags");
	check(D1_PHASE_ADMITTED == 1 && D1_PHASE_PREPARED == 2 &&
		      D1_PHASE_FINALIZED == 3 && D1_PHASE_COMMITTED == 4 &&
		      D1_PHASE_ABORTED == 5 && D1_PHASE_ROLLED_BACK == 6,
	      "phase tags");
	check(D1_MODE_ORDINARY == 1 && D1_MODE_REPAIR == 2, "mode tags");
	check(D1_UNSTABLE == 1 && D1_DATA_SYNC == 2 && D1_FILE_SYNC == 3,
	      "stability tags");
	check(D1_SELECT_ORDINARY == 1 && D1_SELECT_OWNER == 2,
	      "selection tags");
	check(D1_CKSUM_CRC32 == 1 && D1_CKSUM_CRC32C == 2, "checksum tags");
	check(D1_REC_START == 1 && D1_REC_CONTROL == 2 && D1_REC_ENTRY == 3 &&
		      D1_REC_COHORT == 4,
	      "record tags");
	check(D1_OP_WRITE_BATCH == 1 && D1_OP_FINALIZE_BATCH == 2 &&
		      D1_OP_COMMIT_BATCH == 3 && D1_OP_ROLLBACK_BATCH == 4 &&
		      D1_OP_MARK_ERROR == 5 && D1_OP_BEGIN_REPAIR == 6 &&
		      D1_OP_PREPARE_REPAIR == 7 && D1_OP_FINALIZE_REPAIR == 8 &&
		      D1_OP_COMMIT_REPAIR == 9 && D1_OP_ABORT_REPAIR == 10 &&
		      D1_OP_CLEAR_ERROR == 11 && D1_OP_UNLOCK == 12 &&
		      D1_OP_RECOVERY_ADMIT == 13 && D1_OP_LEASE_REAP == 14,
	      "operation tags");
	check(D1_RIGHT_READ == 0x1u && D1_RIGHT_WRITE == 0x2u &&
		      D1_RIGHT_REPAIR == 0x4u && D1_RIGHT_CONTROL == 0x8u &&
		      D1_RIGHT_SINGLE_WRITER == 0x10u,
	      "rights flags");
	check(D1_CTL_ENVELOPE == 1 && D1_CTL_ADMIT == 2 &&
		      D1_CTL_REVOKE == 3 && D1_CTL_EXPIRE == 4 &&
		      D1_CTL_CUSTODY == 5 && D1_CTL_RELEASE == 6 &&
		      D1_CTL_CERTIFICATE == 7,
	      "control record kinds");
	check(D1_REPAIR_ERROR == 1 && D1_REPAIR_NOPRE == 2, "repair modes");
	/*
	 * The handle domains are not on the wire -- a handle encodes as
	 * its canonical value alone -- but they are frozen all the same:
	 * two domains that shared a number would let a value decoded for
	 * one be adopted as the other, which is the one mistake the
	 * runtime kind exists to refuse.
	 */
	check(D1_HANDLE_NONE == 0 && D1_HANDLE_ADMISSION == 1 &&
		      D1_HANDLE_TXN == 2 && D1_HANDLE_VERSION == 3 &&
		      D1_HANDLE_CUSTODY == 4 && D1_HANDLE_REPAIR == 5 &&
		      D1_HANDLE_POSTCOND == 6 && D1_HANDLE_EPISODE == 7,
	      "handle domains");
	/* Widths a record's shape depends on, and the format it is in. */
	check(D1_VERIFIER_BYTES == 8u && D1_DIGEST_BYTES == 32u &&
		      D1_CERTIFICATE_BYTES == 32u,
	      "frozen widths");
	check(D1_JOURNAL_FORMAT == 2u, "the record format this build writes");
	check(D1_JOURNAL_MAGIC == 0x44314a31u &&
		      D1_JOURNAL_HEADER_BYTES == 56u &&
		      D1_JOURNAL_TRAILER_BYTES == 4u,
	      "and the frame it writes it in");
}

/* Golden bytes for each primitive, written out by hand. */
static void test_golden_primitives(void)
{
	static const uint8_t want_scalars[] = {
		/* u8 0x7f */
		0x7f,
		/* u16 0x0102 */
		0x01,
		0x02,
		/* u32 0x01020304 */
		0x01,
		0x02,
		0x03,
		0x04,
		/* u64 0x0123456789abcdef */
		0x01,
		0x23,
		0x45,
		0x67,
		0x89,
		0xab,
		0xcd,
		0xef,
		/* bool true, bool false */
		0x01,
		0x00,
		/* bytes "hi" */
		0x00,
		0x00,
		0x00,
		0x02,
		'h',
		'i',
		/* bytes empty */
		0x00,
		0x00,
		0x00,
		0x00,
		/* option absent */
		0x00,
		/* option present 0x1122334455667788 */
		0x01,
		0x11,
		0x22,
		0x33,
		0x44,
		0x55,
		0x66,
		0x77,
		0x88,
	};
	uint8_t buf[64];
	struct d1_cursor c;

	d1_enc_init(&c, buf, sizeof(buf));
	d1_enc_u8(&c, 0x7f);
	d1_enc_u16(&c, 0x0102);
	d1_enc_u32(&c, 0x01020304u);
	d1_enc_u64(&c, UINT64_C(0x0123456789abcdef));
	d1_enc_bool(&c, true);
	d1_enc_bool(&c, false);
	d1_enc_bytes(&c, "hi", 2);
	d1_enc_bytes(&c, "", 0);
	d1_enc_opt_u64(&c, false, 0);
	d1_enc_opt_u64(&c, true, UINT64_C(0x1122334455667788));
	check(d1_cursor_ok(&c), "scalar encode");
	check_bytes(buf, c.len, want_scalars, sizeof(want_scalars),
		    "golden scalars");
}

static void test_golden_structs(void)
{
	static const uint8_t want[] = {
		/* objkey: export UUID then object UUID */
		0x00,
		0x01,
		0x02,
		0x03,
		0x04,
		0x05,
		0x06,
		0x07,
		0x08,
		0x09,
		0x0a,
		0x0b,
		0x0c,
		0x0d,
		0x0e,
		0x0f,
		0x10,
		0x11,
		0x12,
		0x13,
		0x14,
		0x15,
		0x16,
		0x17,
		0x18,
		0x19,
		0x1a,
		0x1b,
		0x1c,
		0x1d,
		0x1e,
		0x1f,
		/* opkey: origin UUID, sequence, ordinal */
		0x20,
		0x21,
		0x22,
		0x23,
		0x24,
		0x25,
		0x26,
		0x27,
		0x28,
		0x29,
		0x2a,
		0x2b,
		0x2c,
		0x2d,
		0x2e,
		0x2f,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x07,
		0x00,
		0x00,
		0x00,
		0x09,
		/* owner: cohort, writer, co_id */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x2a,
		0x00,
		0x00,
		0x00,
		0x0b,
		0x00,
		0x00,
		0x00,
		0x0c,
		/* guard: generation, writer, never_written=false */
		0x00,
		0x00,
		0x00,
		0x05,
		0x00,
		0x00,
		0x00,
		0x0b,
		0x00,
		/* checksum: alg CRC32C, 4 bytes 0xdeadbeef */
		0x00,
		0x00,
		0x00,
		0x02,
		0x00,
		0x00,
		0x00,
		0x04,
		0xde,
		0xad,
		0xbe,
		0xef,
	};
	struct d1_objkey obj;
	struct d1_opkey key;
	struct d1_owner owner = { .cohort = { 42 }, .writer = 11, .co_id = 12 };
	struct d1_guard guard = { .generation = 5,
				  .writer = 11,
				  .never_written = false };
	struct d1_checksum sum = { .alg = D1_CKSUM_CRC32C,
				   .len = 4,
				   .digest = { 0xde, 0xad, 0xbe, 0xef } };
	uint8_t buf[128];
	struct d1_cursor c;
	unsigned int i;

	for (i = 0; i < D1_UUID_BYTES; i++) {
		obj.export_uuid.bytes[i] = (uint8_t)i;
		obj.object_uuid.bytes[i] = (uint8_t)(0x10 + i);
		key.origin.bytes[i] = (uint8_t)(0x20 + i);
	}
	key.sequence = 7;
	key.ordinal = 9;

	d1_enc_init(&c, buf, sizeof(buf));
	d1_enc_objkey(&c, &obj);
	d1_enc_opkey(&c, &key);
	d1_enc_owner(&c, &owner);
	d1_enc_guard(&c, &guard);
	d1_enc_checksum(&c, &sum);
	check(d1_cursor_ok(&c), "struct encode");
	check_bytes(buf, c.len, want, sizeof(want), "golden structs");
}

/* Every primitive survives a round trip with its value intact. */
static void test_round_trip(void)
{
	struct d1_objkey obj, obj2;
	struct d1_opkey key, key2;
	struct d1_owner owner = { .cohort = { UINT64_MAX },
				  .writer = 1,
				  .co_id = 2 },
			owner2;
	struct d1_guard guard = { .generation = 0,
				  .writer = 0,
				  .never_written = true },
			guard2;
	struct d1_checksum sum = { .alg = D1_CKSUM_CRC32,
				   .len = 4,
				   .digest = { 1, 2, 3, 4 } },
			   sum2;
	uint8_t buf[128];
	struct d1_cursor c;
	bool present = false;
	uint64_t opt = 0;
	unsigned int i;

	for (i = 0; i < D1_UUID_BYTES; i++) {
		obj.export_uuid.bytes[i] = (uint8_t)(i * 3);
		obj.object_uuid.bytes[i] = (uint8_t)(i * 5);
		key.origin.bytes[i] = (uint8_t)(i * 7);
	}
	key.sequence = UINT64_MAX;
	key.ordinal = UINT32_MAX;

	d1_enc_init(&c, buf, sizeof(buf));
	d1_enc_objkey(&c, &obj);
	d1_enc_opkey(&c, &key);
	d1_enc_owner(&c, &owner);
	d1_enc_guard(&c, &guard);
	d1_enc_checksum(&c, &sum);
	d1_enc_opt_u64(&c, true, 99);
	check(d1_cursor_ok(&c), "round trip encode");

	d1_dec_init(&c, buf, c.len);
	check(d1_dec_objkey(&c, &obj2) && d1_dec_opkey(&c, &key2) &&
		      d1_dec_owner(&c, &owner2) && d1_dec_guard(&c, &guard2) &&
		      d1_dec_checksum(&c, &sum2) &&
		      d1_dec_opt_u64(&c, &present, &opt),
	      "round trip decode");
	check(d1_dec_finished(&c), "round trip consumed everything");
	/*
	 * Compared field by field.  These structures have padding the
	 * encoding never carries, so comparing their storage would compare
	 * bytes no encoder wrote and no decoder set.
	 */
	check(memcmp(obj.export_uuid.bytes, obj2.export_uuid.bytes,
		     D1_UUID_BYTES) == 0 &&
		      memcmp(obj.object_uuid.bytes, obj2.object_uuid.bytes,
			     D1_UUID_BYTES) == 0,
	      "objkey survives");
	check(memcmp(key.origin.bytes, key2.origin.bytes, D1_UUID_BYTES) == 0 &&
		      key.sequence == key2.sequence &&
		      key.ordinal == key2.ordinal,
	      "opkey survives");
	check(owner.cohort.raw == owner2.cohort.raw &&
		      owner.writer == owner2.writer &&
		      owner.co_id == owner2.co_id,
	      "owner survives");
	check(guard2.never_written && guard2.generation == 0,
	      "never-written guard survives");
	check(sum2.alg == sum.alg && sum2.len == sum.len &&
		      memcmp(sum2.digest, sum.digest, 4) == 0,
	      "checksum survives");
	check(present && opt == 99, "option survives");
}

/* Everything the decoder must refuse. */
static void test_decoder_refusals(void)
{
	static const uint8_t noncanonical_bool[] = { 0x02 };
	static const uint8_t noncanonical_option[] = { 0x02 };
	static const uint8_t oversize_bytes[] = { 0x00, 0x00, 0x00, 0x05, 'a',
						  'b',	'c',  'd',  'e' };
	static const uint8_t truncated_u32[] = { 0x00, 0x00 };
	static const uint8_t trailing_byte[] = { 0x00, 0x00, 0x00, 0x01, 0xff };
	struct d1_cursor c;
	const uint8_t *ref;
	uint32_t len;
	uint32_t v32;
	bool b;
	bool present;
	uint64_t opt;

	d1_dec_init(&c, noncanonical_bool, sizeof(noncanonical_bool));
	check(!d1_dec_bool(&c, &b) && !d1_cursor_ok(&c),
	      "a boolean that is neither 0 nor 1 is refused");

	d1_dec_init(&c, noncanonical_option, sizeof(noncanonical_option));
	check(!d1_dec_opt_u64(&c, &present, &opt) && !d1_cursor_ok(&c),
	      "an option tag that is neither 0 nor 1 is refused");

	d1_dec_init(&c, oversize_bytes, sizeof(oversize_bytes));
	check(!d1_dec_bytes_ref(&c, &ref, &len, 4) && !d1_cursor_ok(&c),
	      "a count past the declared limit is refused");

	d1_dec_init(&c, truncated_u32, sizeof(truncated_u32));
	check(!d1_dec_u32(&c, &v32) && !d1_cursor_ok(&c),
	      "a value running past the end is refused");

	d1_dec_init(&c, oversize_bytes, sizeof(oversize_bytes));
	check(d1_dec_bytes_ref(&c, &ref, &len, 8) && len == 5,
	      "a count within the limit decodes");
	check(d1_dec_finished(&c), "and consumes exactly its own bytes");

	/* One byte more than the value needs is not a finished decode. */
	d1_dec_init(&c, trailing_byte, sizeof(trailing_byte));
	check(d1_dec_u32(&c, &v32) && v32 == 1u, "the value decodes");
	check(!d1_dec_finished(&c), "a trailing byte leaves the decode short");

	/* An encode that runs out of room is sticky and yields nothing. */
	{
		uint8_t small[3];

		d1_enc_init(&c, small, sizeof(small));
		d1_enc_u32(&c, 1);
		check(!d1_cursor_ok(&c), "an encode past the end is refused");
		d1_enc_u8(&c, 1);
		check(!d1_cursor_ok(&c), "and stays refused");
	}
}

/* Checked arithmetic refuses rather than wraps. */
static void test_checked_arithmetic(void)
{
	uint64_t out64;
	size_t outsz;

	check(d1_add_u64(1, 2, &out64) && out64 == 3, "ordinary addition");
	check(!d1_add_u64(UINT64_MAX, 1, &out64), "addition overflow refused");
	check(d1_mul_u64(1u << 20, 1u << 20, &out64), "ordinary product");
	check(!d1_mul_u64(UINT64_MAX, 2, &out64), "product overflow refused");
	check(!d1_add_size(SIZE_MAX, 1, &outsz), "size overflow refused");
	/* index * chunk_bytes, the product the model checks most. */
	check(!d1_mul_u64(UINT64_MAX / 4096 + 1, 4096, &out64),
	      "chunk offset overflow refused");
}

/* Known answers, from outside this implementation. */
static void test_known_answers(void)
{
	static const uint8_t sha_abc[D1_DIGEST_BYTES] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
		0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
	};
	static const uint8_t sha_empty[D1_DIGEST_BYTES] = {
		0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
		0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
		0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
		0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
	};
	uint8_t got[D1_DIGEST_BYTES];
	struct d1_checksum sum;

	/* CRC32 and CRC32C of "123456789" are the standard check values. */
	check(d1_crc32("123456789", 9) == 0xcbf43926u, "CRC32 check value");
	check(d1_crc32c("123456789", 9) == 0xe3069283u, "CRC32C check value");
	check(d1_crc32("", 0) == 0u, "CRC32 of nothing");
	check(d1_crc32c("", 0) == 0u, "CRC32C of nothing");

	d1_sha256("abc", 3, got);
	check_bytes(got, sizeof(got), sha_abc, sizeof(sha_abc),
		    "SHA-256 of abc");
	d1_sha256("", 0, got);
	check_bytes(got, sizeof(got), sha_empty, sizeof(sha_empty),
		    "SHA-256 of nothing");

	/*
	 * The request digest is the domain bytes followed by the body, with
	 * no length prefix and no NUL, so it equals the plain hash of the
	 * concatenation.
	 */
	{
		static const char joined[] = "FFV2-D1-REQ-v1body";
		uint8_t direct[D1_DIGEST_BYTES];

		d1_request_digest("body", 4, got);
		d1_sha256(joined, sizeof(joined) - 1, direct);
		check_bytes(got, sizeof(got), direct, sizeof(direct),
			    "request digest is domain||body");
	}

	/* A tagged checksum carries four big-endian bytes. */
	check(d1_checksum_compute(D1_CKSUM_CRC32, "123456789", 9, &sum) &&
		      sum.len == 4 && sum.digest[0] == 0xcb &&
		      sum.digest[1] == 0xf4 && sum.digest[2] == 0x39 &&
		      sum.digest[3] == 0x26,
	      "CRC32 digest bytes are big-endian");
	check(d1_checksum_verify(&sum, "123456789", 9), "checksum verifies");
	check(!d1_checksum_verify(&sum, "12345678a", 9),
	      "a changed byte fails the checksum");
	check(!d1_checksum_compute(7, "x", 1, &sum),
	      "an unsupported algorithm is refused");
	sum.alg = D1_CKSUM_CRC32;
	sum.len = 3;
	check(!d1_checksum_verify(&sum, "123456789", 9),
	      "a wrong digest length is refused before comparison");
}

int main(void)
{
	test_tag_table();
	test_golden_primitives();
	test_golden_structs();
	test_round_trip();
	test_decoder_refusals();
	test_checked_arithmetic();
	test_known_answers();

	if (failures) {
		fprintf(stderr, "%u check(s) failed\n", failures);
		return 1;
	}
	printf("d1_codec_test: all checks passed\n");
	return 0;
}
