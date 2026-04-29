/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Slice 6c-x.1: proxy_stateid value primitives.
 * Tests proxy_stateid_alloc / extract_boot_seq / is_stale /
 * other_eq from lib/nfs4/include/nfs4/proxy_stateid.h.
 *
 * The lookup table that resolves proxy_stateid -> migration record
 * is part of slice 6c-x.2; tests for the table-level Rule 6 lifecycle
 * (drain, dual-index consistency, find/remove race) live with that
 * slice.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "nfs4/proxy_stateid.h"
#include "libreffs_test.h"

/* ------------------------------------------------------------------ */
/* Allocator                                                           */
/* ------------------------------------------------------------------ */

/*
 * Two consecutive allocs at the same boot_seq return different
 * other[12] values.  This is the load-bearing uniqueness property:
 * the migration record's stateid-keyed lookup depends on collision
 * being rare to negligible at the volume of expected migrations.
 */
START_TEST(test_alloc_unique_within_boot)
{
	stateid4 a, b;

	ck_assert_int_eq(proxy_stateid_alloc(0x0427, &a), 0);
	ck_assert_int_eq(proxy_stateid_alloc(0x0427, &b), 0);

	/*
	 * Boot prefix matches; opaque tail differs.  Compare only the
	 * opaque portion explicitly so a future slip in the prefix
	 * width does not mask the underlying-uniqueness check.
	 */
	ck_assert_mem_eq(&a.other[PROXY_STATEID_BOOT_SEQ_OFF],
			 &b.other[PROXY_STATEID_BOOT_SEQ_OFF],
			 PROXY_STATEID_BOOT_SEQ_LEN);
	ck_assert_mem_ne(&a.other[PROXY_STATEID_OPAQUE_OFF],
			 &b.other[PROXY_STATEID_OPAQUE_OFF],
			 PROXY_STATEID_OPAQUE_LEN);
}
END_TEST

/*
 * The reserved bytes (offsets 2-3) are zero on emit per the
 * draft.  Forward-compat for boot_seq widening.
 */
START_TEST(test_alloc_reserved_bytes_zero)
{
	stateid4 s;
	uint8_t zero[PROXY_STATEID_RESERVED_LEN] = { 0 };

	ck_assert_int_eq(proxy_stateid_alloc(0x1234, &s), 0);
	ck_assert_mem_eq(&s.other[PROXY_STATEID_RESERVED_OFF], zero,
			 PROXY_STATEID_RESERVED_LEN);
}
END_TEST

/*
 * Initial seqid is 1 -- matches the convention used by other
 * server-issued stateids in the codebase and gives the renewal
 * path a non-zero baseline to bump from.
 */
START_TEST(test_alloc_initial_seqid_is_one)
{
	stateid4 s;

	ck_assert_int_eq(proxy_stateid_alloc(0x0042, &s), 0);
	ck_assert_uint_eq(s.seqid, 1);
}
END_TEST

/*
 * Boot prefix encodes in big-endian byte order: the most-significant
 * byte is at offset 0.  This is wire-visible (the stateid is a
 * fixed 12-byte opaque on the wire); changing endianness would
 * silently break STALE detection across reboots.
 */
START_TEST(test_alloc_boot_seq_big_endian)
{
	stateid4 s;

	ck_assert_int_eq(proxy_stateid_alloc(0x04AB, &s), 0);
	/*
	 * Cast through uint8_t before the unsigned compare: stateid4.other
	 * is char[12] and char is signed on x86/x86_64, so other[1] = 0xAB
	 * sign-extends to a huge uint64_t in ck_assert_uint_eq's internal
	 * promotion if read as a bare char.  Bit pattern is what we care
	 * about.
	 */
	ck_assert_uint_eq((uint8_t)s.other[0], 0x04);
	ck_assert_uint_eq((uint8_t)s.other[1], 0xAB);
}
END_TEST

/*
 * Bad input: NULL out pointer -> -EINVAL.  Defensive only; the
 * single production caller (PROXY_PROGRESS reply builder, slice
 * 6c-y) always passes a valid pointer, but the wire-error contract
 * for upstream callers is undefined without this check.
 */
START_TEST(test_alloc_null_out_returns_einval)
{
	ck_assert_int_eq(proxy_stateid_alloc(0x0427, NULL), -EINVAL);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Boot-seq extraction + stale detection                               */
/* ------------------------------------------------------------------ */

/*
 * Extract returns the encoded boot_seq exactly.  Round-trip via
 * alloc to validate the encode/decode pair.
 */
START_TEST(test_extract_round_trip)
{
	stateid4 s;
	uint16_t seqs[] = { 0x0001, 0x00FF, 0x0100, 0x04AB,
			    0x7FFF, 0xFFFF, 0x0000 };

	for (size_t i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
		ck_assert_int_eq(proxy_stateid_alloc(seqs[i], &s), 0);
		ck_assert_uint_eq(proxy_stateid_extract_boot_seq(&s), seqs[i]);
	}
}
END_TEST

/*
 * is_stale: a freshly-minted stateid for the current boot is NOT
 * stale.  A stateid minted for any other boot_seq IS stale.
 *
 * Load-bearing for the wire-error contract: the PROXY_DONE / CANCEL
 * handler distinguishes NFS4ERR_STALE_STATEID (this case) from
 * NFS4ERR_BAD_STATEID (current-boot but no record found) at this
 * exact check.
 */
START_TEST(test_is_stale_current_boot_not_stale)
{
	stateid4 s;

	ck_assert_int_eq(proxy_stateid_alloc(0x0427, &s), 0);
	ck_assert(!proxy_stateid_is_stale(&s, 0x0427));
}
END_TEST

START_TEST(test_is_stale_prior_boot_is_stale)
{
	stateid4 s;

	ck_assert_int_eq(proxy_stateid_alloc(0x0426, &s), 0);
	ck_assert(proxy_stateid_is_stale(&s, 0x0427));
}
END_TEST

START_TEST(test_is_stale_future_boot_is_stale)
{
	/*
	 * Forward-compat: a stateid whose boot prefix is GREATER than
	 * the current boot_seq (could happen if a record from a
	 * future-but-now-rolled-back deployment is replayed) is also
	 * stale.  is_stale is "not equal", not "less than".
	 */
	stateid4 s;

	ck_assert_int_eq(proxy_stateid_alloc(0x0428, &s), 0);
	ck_assert(proxy_stateid_is_stale(&s, 0x0427));
}
END_TEST

START_TEST(test_is_stale_null_input_is_stale)
{
	ck_assert(proxy_stateid_is_stale(NULL, 0x0427));
}
END_TEST

/* ------------------------------------------------------------------ */
/* Other equality                                                      */
/* ------------------------------------------------------------------ */

START_TEST(test_other_eq_same_bytes_eq)
{
	stateid4 a, b;

	ck_assert_int_eq(proxy_stateid_alloc(0x0427, &a), 0);
	memcpy(&b, &a, sizeof(b));
	/*
	 * Mutate seqid to confirm other_eq compares only the opaque
	 * portion.  Two stateids with identical other but different
	 * seqid should still compare as equal -- the seqid renewal
	 * path bumps seqid without changing other.
	 */
	b.seqid = a.seqid + 5;
	ck_assert(proxy_stateid_other_eq(&a, &b));
}
END_TEST

START_TEST(test_other_eq_different_bytes_neq)
{
	stateid4 a, b;

	ck_assert_int_eq(proxy_stateid_alloc(0x0427, &a), 0);
	ck_assert_int_eq(proxy_stateid_alloc(0x0427, &b), 0);
	ck_assert(!proxy_stateid_other_eq(&a, &b));
}
END_TEST

START_TEST(test_other_eq_null_inputs_neq)
{
	stateid4 a;

	ck_assert_int_eq(proxy_stateid_alloc(0x0427, &a), 0);
	ck_assert(!proxy_stateid_other_eq(NULL, &a));
	ck_assert(!proxy_stateid_other_eq(&a, NULL));
	ck_assert(!proxy_stateid_other_eq(NULL, NULL));
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *proxy_stateid_suite(void)
{
	Suite *s = suite_create("proxy_stateid");
	TCase *tc = tcase_create("proxy_stateid");

	tcase_add_test(tc, test_alloc_unique_within_boot);
	tcase_add_test(tc, test_alloc_reserved_bytes_zero);
	tcase_add_test(tc, test_alloc_initial_seqid_is_one);
	tcase_add_test(tc, test_alloc_boot_seq_big_endian);
	tcase_add_test(tc, test_alloc_null_out_returns_einval);
	tcase_add_test(tc, test_extract_round_trip);
	tcase_add_test(tc, test_is_stale_current_boot_not_stale);
	tcase_add_test(tc, test_is_stale_prior_boot_is_stale);
	tcase_add_test(tc, test_is_stale_future_boot_is_stale);
	tcase_add_test(tc, test_is_stale_null_input_is_stale);
	tcase_add_test(tc, test_other_eq_same_bytes_eq);
	tcase_add_test(tc, test_other_eq_different_bytes_neq);
	tcase_add_test(tc, test_other_eq_null_inputs_neq);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(proxy_stateid_suite(), NULL, NULL);
}
