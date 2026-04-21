/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Unit tests for the identity domain table and mapping table.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <urcu.h>
#include <check.h>
#include <xxhash.h>

#include "reffs/identity_map.h"

static char test_dir[] = "/tmp/reffs_idmap_test_XXXXXX";
static bool dir_created;

static void setup(void)
{
	rcu_register_thread();
	identity_domain_init();
	identity_map_init();

	if (mkdtemp(test_dir))
		dir_created = true;
}

static void teardown(void)
{
	identity_map_fini();
	identity_domain_fini();
	rcu_unregister_thread();

	if (dir_created) {
		/* Clean up temp files. */
		char cmd[600];

		snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
		system(cmd);
		dir_created = false;
		/* Reset template for next test. */
		strcpy(test_dir, "/tmp/reffs_idmap_test_XXXXXX");
	}
}

/* ------------------------------------------------------------------ */
/* Domain table tests                                                  */
/* ------------------------------------------------------------------ */

/* Domain 0 is always local UNIX. */
START_TEST(test_domain_zero_exists)
{
	const struct identity_domain *d = identity_domain_get(0);

	ck_assert_ptr_nonnull(d);
	ck_assert_uint_eq(d->id_index, 0);
	ck_assert_uint_eq(d->id_type, REFFS_ID_UNIX);
	ck_assert_str_eq(d->id_name, "local");
}
END_TEST

/* Create a new domain and find it by name. */
START_TEST(test_domain_create_find)
{
	int idx = identity_domain_find_or_create("EXAMPLE.COM", REFFS_ID_KRB5);

	ck_assert_int_gt(idx, 0);

	const struct identity_domain *d = identity_domain_get((uint32_t)idx);

	ck_assert_ptr_nonnull(d);
	ck_assert_str_eq(d->id_name, "EXAMPLE.COM");
	ck_assert_uint_eq(d->id_type, REFFS_ID_KRB5);

	/* Finding again returns the same index. */
	int idx2 = identity_domain_find_or_create("EXAMPLE.COM", REFFS_ID_KRB5);
	ck_assert_int_eq(idx, idx2);
}
END_TEST

/* Domain persistence round-trip. */
START_TEST(test_domain_persist_load)
{
	ck_assert(dir_created);

	int idx = identity_domain_find_or_create("TEST.REALM", REFFS_ID_KRB5);

	ck_assert_int_gt(idx, 0);

	ck_assert_int_eq(identity_domain_persist(test_dir), 0);

	/* Reinitialize and load. */
	identity_domain_fini();
	identity_domain_init();

	ck_assert_int_eq(identity_domain_load(test_dir), 0);

	const struct identity_domain *d = identity_domain_get((uint32_t)idx);

	ck_assert_ptr_nonnull(d);
	ck_assert_str_eq(d->id_name, "TEST.REALM");
}
END_TEST

/* ------------------------------------------------------------------ */
/* Mapping table tests                                                 */
/* ------------------------------------------------------------------ */

/* Bidirectional mapping: A-->B and B-->A. */
START_TEST(test_map_bidirectional)
{
	reffs_id krb5 = REFFS_ID_MAKE(REFFS_ID_KRB5, 1, 42);
	reffs_id unix_id = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 1000);

	ck_assert_int_eq(identity_map_add(krb5, unix_id), 0);

	/* Forward: KRB5 --> UNIX. */
	reffs_id result = identity_map_lookup(krb5);

	ck_assert_uint_eq(result, unix_id);

	/* Reverse: UNIX --> KRB5. */
	result = identity_map_lookup(unix_id);
	ck_assert_uint_eq(result, krb5);
}
END_TEST

/* identity_map_unix_for finds the UNIX alias. */
START_TEST(test_map_unix_for)
{
	reffs_id sid = REFFS_ID_MAKE(REFFS_ID_SID, 2, 501);
	reffs_id unix_id = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 2000);

	ck_assert_int_eq(identity_map_add(sid, unix_id), 0);

	/* SID --> UNIX alias. */
	reffs_id result = identity_map_unix_for(sid);

	ck_assert_uint_eq(result, unix_id);

	/* Already UNIX --> returns directly. */
	result = identity_map_unix_for(unix_id);
	ck_assert_uint_eq(result, unix_id);
}
END_TEST

/* Unmapped non-UNIX id returns NOBODY. */
START_TEST(test_map_unix_for_nobody)
{
	reffs_id unknown = REFFS_ID_MAKE(REFFS_ID_KRB5, 5, 999);
	reffs_id result = identity_map_unix_for(unknown);

	ck_assert_uint_eq(result, REFFS_ID_NOBODY_VAL);
}
END_TEST

/* Mapping persistence round-trip. */
START_TEST(test_map_persist_load)
{
	ck_assert(dir_created);

	reffs_id krb5 = REFFS_ID_MAKE(REFFS_ID_KRB5, 1, 42);
	reffs_id unix_id = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 1000);

	ck_assert_int_eq(identity_map_add(krb5, unix_id), 0);
	ck_assert_int_eq(identity_map_persist(test_dir), 0);

	/* Reinitialize and load. */
	identity_map_fini();
	identity_map_init();

	ck_assert_int_eq(identity_map_load(test_dir), 0);

	/* Verify mappings survived. */
	reffs_id result = identity_map_lookup(krb5);

	ck_assert_uint_eq(result, unix_id);
	result = identity_map_lookup(unix_id);
	ck_assert_uint_eq(result, krb5);
}
END_TEST

/* Mapping survives fini+init+load cycle (full restart simulation). */
START_TEST(test_mapping_survives_restart)
{
	ck_assert(dir_created);

	reffs_id krb5 = REFFS_ID_MAKE(REFFS_ID_KRB5, 1, 99);
	reffs_id unix_id = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 4242);

	ck_assert_int_eq(identity_map_add(krb5, unix_id), 0);
	ck_assert_int_eq(identity_domain_persist(test_dir), 0);
	ck_assert_int_eq(identity_map_persist(test_dir), 0);

	/* Simulate restart: tear down both tables, reload both. */
	identity_map_fini();
	identity_domain_fini();
	identity_domain_init();
	identity_map_init();
	ck_assert_int_eq(identity_domain_load(test_dir), 0);
	ck_assert_int_eq(identity_map_load(test_dir), 0);

	/* Both directions intact after reload. */
	ck_assert_uint_eq(identity_map_lookup(krb5), unix_id);
	ck_assert_uint_eq(identity_map_lookup(unix_id), krb5);
}
END_TEST

/*
 * XXH32 principal hashing is stable: the same principal string must
 * always produce the same local_id, both in this process and across
 * restarts.  These vectors are canonical -- never change them.
 */
START_TEST(test_principal_local_id_stable)
{
	ck_assert_uint_eq(XXH32("alice", 5, 0), (uint32_t)0x753a727d);
	ck_assert_uint_eq(XXH32("bob", 3, 0), (uint32_t)0x02bbe0e7);
	ck_assert_uint_eq(XXH32("a", 1, 0), (uint32_t)0x550d7456);
	ck_assert_uint_eq(XXH32("user123", 7, 0), (uint32_t)0x1ba25d57);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *identity_map_suite(void)
{
	Suite *s = suite_create("identity_map");
	TCase *tc_domain = tcase_create("domain");

	tcase_add_checked_fixture(tc_domain, setup, teardown);
	tcase_add_test(tc_domain, test_domain_zero_exists);
	tcase_add_test(tc_domain, test_domain_create_find);
	tcase_add_test(tc_domain, test_domain_persist_load);
	suite_add_tcase(s, tc_domain);

	TCase *tc_map = tcase_create("map");

	tcase_add_checked_fixture(tc_map, setup, teardown);
	tcase_add_test(tc_map, test_map_bidirectional);
	tcase_add_test(tc_map, test_map_unix_for);
	tcase_add_test(tc_map, test_map_unix_for_nobody);
	tcase_add_test(tc_map, test_map_persist_load);
	tcase_add_test(tc_map, test_mapping_survives_restart);
	tcase_add_test(tc_map, test_principal_local_id_stable);
	suite_add_tcase(s, tc_map);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = identity_map_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
