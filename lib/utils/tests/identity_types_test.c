/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Unit tests for reffs_id type, macros, and enum.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <stdlib.h>
#include <string.h>

#include <check.h>

#include "reffs/identity_types.h"
#include "reffs/inode.h"

/* Round-trip: MAKE then extract components. */
START_TEST(test_make_extract)
{
	reffs_id id = REFFS_ID_MAKE(REFFS_ID_KRB5, 42, 1000);

	ck_assert_uint_eq(REFFS_ID_TYPE(id), REFFS_ID_KRB5);
	ck_assert_uint_eq(REFFS_ID_DOMAIN(id), 42);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 1000);
}
END_TEST

/* Backward compat: UNIX type with domain 0, low 32 bits == uid. */
START_TEST(test_unix_backward_compat)
{
	reffs_id id = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 1000);

	ck_assert_uint_eq(id, 1000);
	ck_assert_uint_eq((uint32_t)id, 1000);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 1000);
	ck_assert(REFFS_ID_IS_UNIX(id));
}
END_TEST

/* Root is UNIX type 0, domain 0, local 0. */
START_TEST(test_root)
{
	reffs_id id = REFFS_ID_ROOT_VAL;

	ck_assert_uint_eq(REFFS_ID_TYPE(id), REFFS_ID_UNIX);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 0);
	ck_assert(REFFS_ID_IS_UNIX(id));
}
END_TEST

/* Nobody is type 15, local 65534. */
START_TEST(test_nobody)
{
	reffs_id id = REFFS_ID_NOBODY_VAL;

	ck_assert_uint_eq(REFFS_ID_TYPE(id), REFFS_ID_NOBODY);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 65534);
	ck_assert(REFFS_ID_IS_NOBODY(id));
	ck_assert(!REFFS_ID_IS_UNIX(id));
}
END_TEST

/* Synthetic type for fencing. */
START_TEST(test_synth)
{
	reffs_id id = REFFS_ID_MAKE(REFFS_ID_SYNTH, 0, 1024);

	ck_assert_uint_eq(REFFS_ID_TYPE(id), REFFS_ID_SYNTH);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 1024);
	ck_assert(!REFFS_ID_IS_UNIX(id));
}
END_TEST

/* Max domain index (28 bits). */
START_TEST(test_max_domain)
{
	reffs_id id = REFFS_ID_MAKE(REFFS_ID_SID, 0x0FFFFFFF, 500);

	ck_assert_uint_eq(REFFS_ID_DOMAIN(id), 0x0FFFFFFF);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 500);
}
END_TEST

/* Domain overflow masked to 28 bits. */
START_TEST(test_domain_overflow)
{
	reffs_id id = REFFS_ID_MAKE(REFFS_ID_SID, 0x1FFFFFFF, 500);

	ck_assert_uint_eq(REFFS_ID_DOMAIN(id), 0x0FFFFFFF);
}
END_TEST

/*
 * Non-UNIX types: REFFS_ID_LOCAL extracts the local_id correctly
 * for use in comparisons and wire encoding.
 */
START_TEST(test_nonunix_local_extraction)
{
	reffs_id krb5 = REFFS_ID_MAKE(REFFS_ID_KRB5, 1, 42);
	reffs_id sid = REFFS_ID_MAKE(REFFS_ID_SID, 2, 501);
	reffs_id synth = REFFS_ID_MAKE(REFFS_ID_SYNTH, 0, 1024);

	/* REFFS_ID_LOCAL must return the local_id regardless of type. */
	ck_assert_uint_eq(REFFS_ID_LOCAL(krb5), 42);
	ck_assert_uint_eq(REFFS_ID_LOCAL(sid), 501);
	ck_assert_uint_eq(REFFS_ID_LOCAL(synth), 1024);

	/* These are NOT UNIX identities. */
	ck_assert(!REFFS_ID_IS_UNIX(krb5));
	ck_assert(!REFFS_ID_IS_UNIX(sid));
	ck_assert(!REFFS_ID_IS_UNIX(synth));
}
END_TEST

/*
 * inode_disk round-trip: verify that storing a full 64-bit reffs_id
 * in the on-disk format and reading it back preserves all bits.
 * This catches regressions where the on-disk field is accidentally
 * truncated to 32 bits.
 */
START_TEST(test_inode_disk_roundtrip)
{
	struct inode_disk id;

	/* KRB5 type with domain 1, local 42 — high bits are non-zero. */
	reffs_id uid = REFFS_ID_MAKE(REFFS_ID_KRB5, 1, 42);
	reffs_id gid = REFFS_ID_MAKE(REFFS_ID_SID, 2, 501);

	memset(&id, 0, sizeof(id));
	id.id_uid = uid;
	id.id_gid = gid;

	/* Read back and verify all 64 bits survived. */
	ck_assert_uint_eq(id.id_uid, uid);
	ck_assert_uint_eq(id.id_gid, gid);
	ck_assert_uint_eq(REFFS_ID_TYPE(id.id_uid), REFFS_ID_KRB5);
	ck_assert_uint_eq(REFFS_ID_DOMAIN(id.id_uid), 1);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id.id_uid), 42);
	ck_assert_uint_eq(REFFS_ID_TYPE(id.id_gid), REFFS_ID_SID);
	ck_assert_uint_eq(REFFS_ID_DOMAIN(id.id_gid), 2);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id.id_gid), 501);
}
END_TEST

/*
 * UNIX backward compat with inode_disk: a plain UNIX uid stored as
 * reffs_id must have the low 32 bits equal to the original uid,
 * ensuring that legacy code truncating to uint32_t still works.
 */
START_TEST(test_inode_disk_unix_compat)
{
	struct inode_disk id;

	id.id_uid = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 1000);
	id.id_gid = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 2000);

	/* Truncating to uint32_t must yield the UNIX uid/gid. */
	ck_assert_uint_eq((uint32_t)id.id_uid, 1000);
	ck_assert_uint_eq((uint32_t)id.id_gid, 2000);
}
END_TEST

static Suite *identity_types_suite(void)
{
	Suite *s = suite_create("identity_types");
	TCase *tc = tcase_create("reffs_id");

	tcase_add_test(tc, test_make_extract);
	tcase_add_test(tc, test_unix_backward_compat);
	tcase_add_test(tc, test_root);
	tcase_add_test(tc, test_nobody);
	tcase_add_test(tc, test_synth);
	tcase_add_test(tc, test_max_domain);
	tcase_add_test(tc, test_domain_overflow);
	tcase_add_test(tc, test_nonunix_local_extraction);
	tcase_add_test(tc, test_inode_disk_roundtrip);
	tcase_add_test(tc, test_inode_disk_unix_compat);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = identity_types_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
