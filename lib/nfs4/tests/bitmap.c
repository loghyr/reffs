/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <check.h>
#include <stdlib.h>

#include "attr.h"
#include "nfs4_test_harness.h"

/*
 * supported_attributes is defined in fattr4.c and populated by
 * nfs4_attribute_init().  Declare it here so the nfs4_attribute_init/fini
 * tests can inspect it directly without adding a getter function.
 */
extern bitmap4 *supported_attributes;

int nfs4_attribute_init(void);
int nfs4_attribute_fini(void);

/* ------------------------------------------------------------------ */
/* Tests: bitmap4_init / bitmap4_destroy                               */
/* ------------------------------------------------------------------ */

START_TEST(test_bitmap_init_destroy)
{
	bitmap4 bm;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);
	ck_assert_ptr_nonnull(bm.bitmap4_val);
	ck_assert_uint_eq(bm.bitmap4_len,
			  BITMAP4_WORDS_FOR_MAX(FATTR4_ATTRIBUTE_MAX));

	bitmap4_destroy(&bm);
	ck_assert_ptr_null(bm.bitmap4_val);
	ck_assert_uint_eq(bm.bitmap4_len, 0);
}
END_TEST

START_TEST(test_bitmap_init_null)
{
	ck_assert_int_eq(bitmap4_init(NULL, FATTR4_ATTRIBUTE_MAX), -EINVAL);
}
END_TEST

START_TEST(test_bitmap_init_zeroed)
{
	bitmap4 bm;
	u_int i;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);
	for (i = 0; i < bm.bitmap4_len; i++)
		ck_assert_uint_eq(bm.bitmap4_val[i], 0);

	bitmap4_destroy(&bm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Tests: bitmap4_zero                                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_bitmap_zero)
{
	bitmap4 bm;
	u_int i;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);
	bitmap4_attribute_set(&bm, FATTR4_TYPE);
	bitmap4_attribute_set(&bm, FATTR4_MODE);
	bitmap4_attribute_set(&bm, FATTR4_UNCACHEABLE);

	bitmap4_zero(&bm);
	for (i = 0; i < bm.bitmap4_len; i++)
		ck_assert_uint_eq(bm.bitmap4_val[i], 0);

	bitmap4_destroy(&bm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Tests: bitmap4_wrap (stack-allocated storage)                       */
/* ------------------------------------------------------------------ */

START_TEST(test_bitmap_wrap)
{
	uint32_t storage[SUPPORTED_ATTR_WORDS];
	bitmap4 bm;

	memset(storage, 0, sizeof(storage));
	bitmap4_wrap(&bm, storage, SUPPORTED_ATTR_WORDS);

	ck_assert(bitmap4_attribute_set(&bm, FATTR4_FILEID));
	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_FILEID));

	bitmap4_attribute_clear(&bm, FATTR4_FILEID);
	ck_assert(!bitmap4_attribute_is_set(&bm, FATTR4_FILEID));
	/* bitmap4_destroy() must NOT be called on a wrapped bitmap. */
}
END_TEST

/* ------------------------------------------------------------------ */
/* Tests: set / clear / is_set — basic round-trip                      */
/* ------------------------------------------------------------------ */

START_TEST(test_bitmap_set_clear_isset)
{
	bitmap4 bm;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);

	ck_assert(bitmap4_attribute_set(&bm, FATTR4_MODE));
	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_MODE));

	ck_assert(bitmap4_attribute_clear(&bm, FATTR4_MODE));
	ck_assert(!bitmap4_attribute_is_set(&bm, FATTR4_MODE));

	bitmap4_destroy(&bm);
}
END_TEST

/*
 * Setting one bit must not disturb any other bit in the same word.
 */
START_TEST(test_bitmap_set_isolation)
{
	bitmap4 bm;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);

	bitmap4_attribute_set(&bm, FATTR4_TYPE); /* bit 1 */
	bitmap4_attribute_set(&bm, FATTR4_FILEID); /* bit 22 */

	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_TYPE));
	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_FILEID));

	bitmap4_attribute_clear(&bm, FATTR4_TYPE);
	ck_assert(!bitmap4_attribute_is_set(&bm, FATTR4_TYPE));
	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_FILEID));

	bitmap4_destroy(&bm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Tests: word-boundary attributes                                     */
/*                                                                     */
/* Word boundaries (all attribute numbers are in word N = attr / 32): */
/*   bit 31  — last bit of word 0                                      */
/*   bit 32  — first bit of word 1                                     */
/*   bit 63  — last bit of word 1  (FATTR4_LAYOUT_HINT)               */
/*   bit 64  — first bit of word 2 (FATTR4_LAYOUT_TYPES)              */
/* ------------------------------------------------------------------ */

START_TEST(test_bitmap_word0_last_bit)
{
	bitmap4 bm;
	/* bit 31 is the highest bit of word 0 */
	const uint32_t last_w0 = 31U;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);

	ck_assert(bitmap4_attribute_set(&bm, last_w0));
	ck_assert(bitmap4_attribute_is_set(&bm, last_w0));
	ck_assert_uint_eq(bm.bitmap4_val[1], 0); /* word 1 untouched */

	ck_assert(bitmap4_attribute_clear(&bm, last_w0));
	ck_assert(!bitmap4_attribute_is_set(&bm, last_w0));
	ck_assert_uint_eq(bm.bitmap4_val[0], 0);

	bitmap4_destroy(&bm);
}
END_TEST

START_TEST(test_bitmap_word1_first_bit)
{
	bitmap4 bm;
	/* bit 32 is the lowest bit of word 1 */
	const uint32_t first_w1 = 32U;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);

	ck_assert(bitmap4_attribute_set(&bm, first_w1));
	ck_assert(bitmap4_attribute_is_set(&bm, first_w1));
	ck_assert_uint_eq(bm.bitmap4_val[0], 0); /* word 0 untouched */

	bitmap4_destroy(&bm);
}
END_TEST

START_TEST(test_bitmap_word1_last_bit)
{
	bitmap4 bm;
	/*
	 * FATTR4_LAYOUT_HINT is the last bit of word 1 (bit 63).
	 * It is cleared in nfs4_attribute_init(), so use a fresh bitmap
	 * to test the mechanics independently of policy.
	 */
	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);

	ck_assert(bitmap4_attribute_set(&bm, FATTR4_LAYOUT_HINT));
	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_LAYOUT_HINT));
	ck_assert_uint_eq(bm.bitmap4_val[2], 0); /* word 2 untouched */

	ck_assert(bitmap4_attribute_clear(&bm, FATTR4_LAYOUT_HINT));
	ck_assert(!bitmap4_attribute_is_set(&bm, FATTR4_LAYOUT_HINT));

	bitmap4_destroy(&bm);
}
END_TEST

START_TEST(test_bitmap_word2_first_bit)
{
	bitmap4 bm;
	/*
	 * FATTR4_LAYOUT_TYPES is the first bit of word 2 (bit 64).
	 * Also cleared in nfs4_attribute_init(); test mechanics only.
	 */
	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);

	ck_assert(bitmap4_attribute_set(&bm, FATTR4_LAYOUT_TYPES));
	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_LAYOUT_TYPES));
	ck_assert_uint_eq(bm.bitmap4_val[1], 0); /* word 1 untouched */

	bitmap4_destroy(&bm);
}
END_TEST

/*
 * Set the last bit of word 1 and the first bit of word 2 simultaneously;
 * clearing one must not disturb the other.
 */
START_TEST(test_bitmap_straddle_word1_word2)
{
	bitmap4 bm;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);

	ck_assert(bitmap4_attribute_set(&bm, FATTR4_LAYOUT_HINT)); /* bit 63 */
	ck_assert(bitmap4_attribute_set(&bm, FATTR4_LAYOUT_TYPES)); /* bit 64 */

	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_LAYOUT_HINT));
	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_LAYOUT_TYPES));

	ck_assert(bitmap4_attribute_clear(&bm, FATTR4_LAYOUT_HINT));
	ck_assert(!bitmap4_attribute_is_set(&bm, FATTR4_LAYOUT_HINT));
	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_LAYOUT_TYPES));

	bitmap4_destroy(&bm);
}
END_TEST

START_TEST(test_bitmap_max_attr)
{
	bitmap4 bm;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);

	ck_assert(bitmap4_attribute_set(&bm, FATTR4_UNCACHEABLE));
	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_UNCACHEABLE));

	ck_assert(bitmap4_attribute_clear(&bm, FATTR4_UNCACHEABLE));
	ck_assert(!bitmap4_attribute_is_set(&bm, FATTR4_UNCACHEABLE));

	bitmap4_destroy(&bm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Tests: out-of-range access                                          */
/* ------------------------------------------------------------------ */

START_TEST(test_bitmap_out_of_range_read)
{
	bitmap4 bm;
	const uint32_t out_of_range =
		BITMAP4_WORDS_FOR_MAX(FATTR4_ATTRIBUTE_MAX) * 32U;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);

	/*
	 * Poison: if the bounds check is broken and out_of_range falls
	 * inside the bitmap, set() would succeed and is_set() would
	 * return true.  A correct implementation returns false because
	 * out_of_range is in word 3, which was never allocated.
	 */
	bitmap4_attribute_set(&bm, out_of_range);
	ck_assert(!bitmap4_attribute_is_set(&bm, out_of_range));

	bitmap4_destroy(&bm);
}
END_TEST

START_TEST(test_bitmap_out_of_range_write)
{
	bitmap4 bm;
	u_int i;
	const uint32_t out_of_range =
		BITMAP4_WORDS_FOR_MAX(FATTR4_ATTRIBUTE_MAX) * 32U;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);

	/*
	 * A broken bounds check would write into unallocated memory and
	 * return true.  Verify set() returns false and no word is dirtied.
	 */
	ck_assert(!bitmap4_attribute_set(&bm, out_of_range));
	for (i = 0; i < bm.bitmap4_len; i++)
		ck_assert_uint_eq(bm.bitmap4_val[i], 0);

	bitmap4_destroy(&bm);
}
END_TEST

START_TEST(test_bitmap_out_of_range_clear)
{
	bitmap4 bm;
	const uint32_t out_of_range =
		BITMAP4_WORDS_FOR_MAX(FATTR4_ATTRIBUTE_MAX) * 32U;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);
	bitmap4_attribute_set(&bm, FATTR4_UNCACHEABLE);

	ck_assert(!bitmap4_attribute_clear(&bm, out_of_range));
	ck_assert(bitmap4_attribute_is_set(&bm, FATTR4_UNCACHEABLE));

	bitmap4_destroy(&bm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Tests: bitmap4_attribute_is_supported_and_set                       */
/*                                                                     */
/* Simulates "client sends request bitmap, server checks against its   */
/* supported bitmap."                                                  */
/* ------------------------------------------------------------------ */

/*
 * Client sends exactly one word (only low 32 attributes).
 * Attributes above bit 31 must read as absent from the request.
 */
START_TEST(test_bitmap_client_one_word)
{
	bitmap4 supported, client_req;
	uint32_t client_storage[1] = { 0 };

	ck_assert_int_eq(bitmap4_init(&supported, FATTR4_ATTRIBUTE_MAX), 0);
	bitmap4_attribute_set(&supported, FATTR4_TYPE);
	bitmap4_attribute_set(&supported, FATTR4_FILEID);
	bitmap4_attribute_set(&supported,
			      FATTR4_MOUNTED_ON_FILEID); /* word 1 */

	/* Client only sends word 0. */
	bitmap4_wrap(&client_req, client_storage, 1);
	bitmap4_attribute_set(&client_req, FATTR4_TYPE);
	bitmap4_attribute_set(&client_req, FATTR4_FILEID);

	/* Both in word 0 and supported — must be true. */
	ck_assert(bitmap4_attribute_is_supported_and_set(
		&supported, &client_req, FATTR4_TYPE));
	ck_assert(bitmap4_attribute_is_supported_and_set(
		&supported, &client_req, FATTR4_FILEID));

	/*
	 * FATTR4_MOUNTED_ON_FILEID (55, word 1) is supported but the client
	 * did not send word 1 — must be false.
	 */
	ck_assert(!bitmap4_attribute_is_supported_and_set(
		&supported, &client_req, FATTR4_MOUNTED_ON_FILEID));

	bitmap4_destroy(&supported);
}
END_TEST

/*
 * Client sends one word MORE than FATTR4_ATTRIBUTE_MAX requires.
 * The extra word may contain any bits; the server must not crash and
 * must correctly evaluate attributes it knows about.
 */
START_TEST(test_bitmap_client_oversized)
{
	bitmap4 supported, client_req;
	u_int server_words = BITMAP4_WORDS_FOR_MAX(FATTR4_ATTRIBUTE_MAX);
	u_int client_words = server_words + 1;
	uint32_t *client_storage;

	ck_assert_int_eq(bitmap4_init(&supported, FATTR4_ATTRIBUTE_MAX), 0);
	bitmap4_attribute_set(&supported, FATTR4_TYPE);
	bitmap4_attribute_set(&supported, FATTR4_UNCACHEABLE);

	client_storage = calloc(client_words, sizeof(*client_storage));
	ck_assert_ptr_nonnull(client_storage);
	bitmap4_wrap(&client_req, client_storage, client_words);

	bitmap4_attribute_set(&client_req, FATTR4_TYPE);
	bitmap4_attribute_set(&client_req, FATTR4_UNCACHEABLE);
	/* Simulate a future attribute in the extra word. */
	client_storage[server_words] = 0xFFFFFFFFU;

	ck_assert(bitmap4_attribute_is_supported_and_set(
		&supported, &client_req, FATTR4_TYPE));
	ck_assert(bitmap4_attribute_is_supported_and_set(
		&supported, &client_req, FATTR4_UNCACHEABLE));

	/*
	 * Bits in the extra word are beyond the server's supported range;
	 * bitmap4_attr_fits() on the supported bitmap returns false for them.
	 */
	ck_assert(!bitmap4_attribute_is_supported_and_set(
		&supported, &client_req, FATTR4_ATTRIBUTE_MAX + 1));

	free(client_storage);
	bitmap4_destroy(&supported);
}
END_TEST

/*
 * Attribute present in client request but absent from server supported map.
 */
START_TEST(test_bitmap_unsupported_attr_not_set)
{
	bitmap4 supported, client_req;

	ck_assert_int_eq(bitmap4_init(&supported, FATTR4_ATTRIBUTE_MAX), 0);
	ck_assert_int_eq(bitmap4_init(&client_req, FATTR4_ATTRIBUTE_MAX), 0);

	/* Server does NOT support ACL. */
	bitmap4_attribute_clear(&supported, FATTR4_ACL);
	/* Client requests it anyway. */
	bitmap4_attribute_set(&client_req, FATTR4_ACL);

	ck_assert(!bitmap4_attribute_is_supported_and_set(
		&supported, &client_req, FATTR4_ACL));

	bitmap4_destroy(&supported);
	bitmap4_destroy(&client_req);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Tests: bitmap4_has_unsupported_bits                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_bitmap_has_unsupported_bits_clean)
{
	bitmap4 supported, wire;

	ck_assert_int_eq(bitmap4_init(&supported, FATTR4_ATTRIBUTE_MAX), 0);
	ck_assert_int_eq(bitmap4_init(&wire, FATTR4_ATTRIBUTE_MAX), 0);

	bitmap4_attribute_set(&supported, FATTR4_TYPE);
	bitmap4_attribute_set(&supported, FATTR4_MODE);

	/* Wire only requests what is supported. */
	bitmap4_attribute_set(&wire, FATTR4_TYPE);
	ck_assert(!bitmap4_has_unsupported_bits(&supported, &wire));

	bitmap4_destroy(&supported);
	bitmap4_destroy(&wire);
}
END_TEST

START_TEST(test_bitmap_has_unsupported_bits_dirty)
{
	bitmap4 supported, wire;

	ck_assert_int_eq(bitmap4_init(&supported, FATTR4_ATTRIBUTE_MAX), 0);
	ck_assert_int_eq(bitmap4_init(&wire, FATTR4_ATTRIBUTE_MAX), 0);

	bitmap4_attribute_set(&supported, FATTR4_TYPE);
	/* ACL is not supported. */

	bitmap4_attribute_set(&wire, FATTR4_ACL);
	ck_assert(bitmap4_has_unsupported_bits(&supported, &wire));

	bitmap4_destroy(&supported);
	bitmap4_destroy(&wire);
}
END_TEST

/*
 * Client sends an oversized wire bitmap with a bit set in the extra word.
 * That extra word is entirely beyond the server's supported range.
 */
START_TEST(test_bitmap_has_unsupported_bits_extra_word)
{
	bitmap4 supported, wire;
	u_int server_words = BITMAP4_WORDS_FOR_MAX(FATTR4_ATTRIBUTE_MAX);
	uint32_t *wire_storage;

	ck_assert_int_eq(bitmap4_init(&supported, FATTR4_ATTRIBUTE_MAX), 0);
	bitmap4_attribute_set(&supported, FATTR4_TYPE);

	wire_storage = calloc(server_words + 1, sizeof(*wire_storage));
	ck_assert_ptr_nonnull(wire_storage);
	bitmap4_wrap(&wire, wire_storage, server_words + 1);

	bitmap4_attribute_set(&wire, FATTR4_TYPE); /* supported */
	wire_storage[server_words] = 1U; /* extra, unsupported */

	ck_assert(bitmap4_has_unsupported_bits(&supported, &wire));

	free(wire_storage);
	bitmap4_destroy(&supported);
}
END_TEST

/*
 * Wire bitmap is shorter than the supported bitmap.
 * If every bit the client sent is supported, there are no unsupported bits.
 */
START_TEST(test_bitmap_has_unsupported_bits_short_wire)
{
	bitmap4 supported, wire;
	uint32_t wire_storage[1] = { 0 };

	ck_assert_int_eq(bitmap4_init(&supported, FATTR4_ATTRIBUTE_MAX), 0);
	bitmap4_attribute_set(&supported, FATTR4_TYPE);
	bitmap4_attribute_set(&supported, FATTR4_FILEID);

	/* Wire sends only word 0, requests only TYPE (which is supported). */
	bitmap4_wrap(&wire, wire_storage, 1);
	bitmap4_attribute_set(&wire, FATTR4_TYPE);

	ck_assert(!bitmap4_has_unsupported_bits(&supported, &wire));

	bitmap4_destroy(&supported);
}
END_TEST

/*
 * NULL-pointer inputs must not crash.
 */
START_TEST(test_bitmap_has_unsupported_bits_null)
{
	bitmap4 bm;

	ck_assert_int_eq(bitmap4_init(&bm, FATTR4_ATTRIBUTE_MAX), 0);

	ck_assert(!bitmap4_has_unsupported_bits(NULL, &bm));
	ck_assert(!bitmap4_has_unsupported_bits(&bm, NULL));
	ck_assert(!bitmap4_has_unsupported_bits(NULL, NULL));

	bitmap4_destroy(&bm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Tests: nfs4_attribute_init() populates expected attributes          */
/* ------------------------------------------------------------------ */

START_TEST(test_attr_init_supported_set)
{
	nfs4_attribute_init();

	/* Required/mandatory attributes. */
	ck_assert(bitmap4_attribute_is_set(supported_attributes,
					   FATTR4_SUPPORTED_ATTRS));
	ck_assert(bitmap4_attribute_is_set(supported_attributes, FATTR4_TYPE));
	ck_assert(bitmap4_attribute_is_set(supported_attributes,
					   FATTR4_FH_EXPIRE_TYPE));
	ck_assert(
		bitmap4_attribute_is_set(supported_attributes, FATTR4_CHANGE));
	ck_assert(bitmap4_attribute_is_set(supported_attributes, FATTR4_SIZE));
	ck_assert(
		bitmap4_attribute_is_set(supported_attributes, FATTR4_FILEID));
	ck_assert(bitmap4_attribute_is_set(supported_attributes, FATTR4_MODE));
	/* Crosses into word 1. */
	ck_assert(bitmap4_attribute_is_set(supported_attributes,
					   FATTR4_MOUNTED_ON_FILEID));
	ck_assert(bitmap4_attribute_is_set(supported_attributes,
					   FATTR4_DIR_NOTIF_DELAY));
	ck_assert(bitmap4_attribute_is_set(supported_attributes,
					   FATTR4_DIRENT_NOTIF_DELAY));
	/* Word 2 attributes. */
	ck_assert(bitmap4_attribute_is_set(supported_attributes,
					   FATTR4_RETENTION_GET));
	ck_assert(
		bitmap4_attribute_is_set(supported_attributes, FATTR4_OFFLINE));
	ck_assert(bitmap4_attribute_is_set(supported_attributes,
					   FATTR4_OPEN_ARGUMENTS));
	ck_assert(bitmap4_attribute_is_set(supported_attributes,
					   FATTR4_UNCACHEABLE));

	nfs4_attribute_fini();
}
END_TEST

START_TEST(test_attr_init_unsupported_clear)
{
	nfs4_attribute_init();

	/* Attributes explicitly cleared in nfs4_attribute_init(). */
	ck_assert(!bitmap4_attribute_is_set(supported_attributes, FATTR4_ACL));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_ACLSUPPORT));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_FS_LOCATIONS));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_QUOTA_AVAIL_HARD));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_QUOTA_AVAIL_SOFT));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_QUOTA_USED));
	/* Word 1: layout attributes are disabled (not a pNFS MDS). */
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_FS_LAYOUT_TYPES));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_LAYOUT_HINT));
	/* Word 2. */
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_LAYOUT_TYPES));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_LAYOUT_BLKSIZE));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_LAYOUT_ALIGNMENT));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_FS_LOCATIONS_INFO));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_MDSTHRESHOLD));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_SEC_LABEL));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_MODE_UMASK));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_XATTR_SUPPORT));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_TIME_DELEG_ACCESS));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes,
					    FATTR4_TIME_DELEG_MODIFY));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes, FATTR4_DACL));
	ck_assert(!bitmap4_attribute_is_set(supported_attributes, FATTR4_SACL));

	nfs4_attribute_fini();
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                      */
/* ------------------------------------------------------------------ */

Suite *bitmap_suite(void)
{
	Suite *s = suite_create("attr: bitmap4");

	TCase *tc_alloc = tcase_create("Alloc");
	tcase_add_test(tc_alloc, test_bitmap_init_destroy);
	tcase_add_test(tc_alloc, test_bitmap_init_null);
	tcase_add_test(tc_alloc, test_bitmap_init_zeroed);
	tcase_add_test(tc_alloc, test_bitmap_zero);
	tcase_add_test(tc_alloc, test_bitmap_wrap);
	suite_add_tcase(s, tc_alloc);

	TCase *tc_bits = tcase_create("Bits");
	tcase_add_test(tc_bits, test_bitmap_set_clear_isset);
	tcase_add_test(tc_bits, test_bitmap_set_isolation);
	tcase_add_test(tc_bits, test_bitmap_word0_last_bit);
	tcase_add_test(tc_bits, test_bitmap_word1_first_bit);
	tcase_add_test(tc_bits, test_bitmap_word1_last_bit);
	tcase_add_test(tc_bits, test_bitmap_word2_first_bit);
	tcase_add_test(tc_bits, test_bitmap_straddle_word1_word2);
	tcase_add_test(tc_bits, test_bitmap_max_attr);
	tcase_add_test(tc_bits, test_bitmap_out_of_range_read);
	tcase_add_test(tc_bits, test_bitmap_out_of_range_write);
	tcase_add_test(tc_bits, test_bitmap_out_of_range_clear);
	suite_add_tcase(s, tc_bits);

	TCase *tc_wire = tcase_create("Wire bitmap simulation");
	tcase_add_test(tc_wire, test_bitmap_client_one_word);
	tcase_add_test(tc_wire, test_bitmap_client_oversized);
	tcase_add_test(tc_wire, test_bitmap_unsupported_attr_not_set);
	suite_add_tcase(s, tc_wire);

	TCase *tc_unsup = tcase_create("Unsupported bits");
	tcase_add_test(tc_unsup, test_bitmap_has_unsupported_bits_clean);
	tcase_add_test(tc_unsup, test_bitmap_has_unsupported_bits_dirty);
	tcase_add_test(tc_unsup, test_bitmap_has_unsupported_bits_extra_word);
	tcase_add_test(tc_unsup, test_bitmap_has_unsupported_bits_short_wire);
	tcase_add_test(tc_unsup, test_bitmap_has_unsupported_bits_null);
	suite_add_tcase(s, tc_unsup);

	TCase *tc_init = tcase_create("nfs4_attribute_init");
	tcase_add_test(tc_init, test_attr_init_supported_set);
	tcase_add_test(tc_init, test_attr_init_unsupported_clear);
	suite_add_tcase(s, tc_init);

	return s;
}

int main(void)
{
	return nfs4_test_run(bitmap_suite());
}
