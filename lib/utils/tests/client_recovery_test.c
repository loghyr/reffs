/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Client state persistence and recovery unit tests (WI-1.2).
 *
 * Verifies the persist_ops round-trip for client identity and
 * incarnation records, and the restart recovery flow.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <check.h>

#include "reffs/client_persist.h"
#include "reffs/server.h"
#include "reffs/log.h"

/*
 * Crash simulation leaks server_state intentionally.
 * Suppress leak detection unconditionally -- this test is designed
 * to leak as part of crash simulation.
 */
const char *__lsan_default_options(void)
{
	return "exitcode=0";
}
const char *__asan_default_options(void)
{
	return "detect_leaks=0";
}

static char state_dir[] = "/tmp/reffs-client-recov-XXXXXX";

static void recov_setup(void)
{
	ck_assert_ptr_nonnull(mkdtemp(state_dir));
}

static void recov_teardown(void)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", state_dir);
	system(cmd);
	strcpy(state_dir, "/tmp/reffs-client-recov-XXXXXX");
}

/* Helper: simulate crash (save dirty, tear down without fini) */
static void simulate_crash(struct server_state *ss)
{
	ss->ss_persist.sps_clean_shutdown = 0;
	ss->ss_persist_ops->server_state_save(ss->ss_persist_ctx,
					      &ss->ss_persist);
	atomic_store_explicit(&ss->ss_lifecycle, SERVER_SHUTTING_DOWN,
			      memory_order_release);
	if (ss->ss_grace_thread) {
		pthread_join(ss->ss_grace_thread, NULL);
		ss->ss_grace_thread = 0;
	}
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int count_identity_cb(const struct client_identity_record *cir
			     __attribute__((unused)),
			     void *arg)
{
	int *count = arg;

	(*count)++;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Identity record round-trip                                          */
/* ------------------------------------------------------------------ */

START_TEST(test_identity_roundtrip)
{
	struct server_state *ss = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss);

	/* Write two identity records */
	struct client_identity_record cir1 = {
		.cir_magic = CLIENT_IDENTITY_MAGIC,
		.cir_slot = 1,
		.cir_ownerid_len = 7,
	};
	memcpy(cir1.cir_ownerid, "client1", 7);
	strncpy(cir1.cir_domain, "test.com", sizeof(cir1.cir_domain) - 1);

	struct client_identity_record cir2 = {
		.cir_magic = CLIENT_IDENTITY_MAGIC,
		.cir_slot = 2,
		.cir_ownerid_len = 7,
	};
	memcpy(cir2.cir_ownerid, "client2", 7);
	strncpy(cir2.cir_domain, "test.com", sizeof(cir2.cir_domain) - 1);

	int ret = ss->ss_persist_ops->client_identity_append(ss->ss_persist_ctx,
							     &cir1);
	ck_assert_int_eq(ret, 0);
	ret = ss->ss_persist_ops->client_identity_append(ss->ss_persist_ctx,
							 &cir2);
	ck_assert_int_eq(ret, 0);

	/* Load and verify both are present */
	int id_count = 0;
	ret = ss->ss_persist_ops->client_identity_load(
		ss->ss_persist_ctx, count_identity_cb, &id_count);
	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(id_count, 2);

	server_state_fini(ss);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Incarnation round-trip                                              */
/* ------------------------------------------------------------------ */

START_TEST(test_incarnation_roundtrip)
{
	struct server_state *ss = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss);

	/* Add two incarnation records */
	struct client_incarnation_record crc1 = {
		.crc_magic = CLIENT_INCARNATION_MAGIC,
		.crc_slot = 1,
		.crc_boot_seq = ss->ss_persist.sps_boot_seq,
		.crc_incarnation = 1,
	};
	struct client_incarnation_record crc2 = {
		.crc_magic = CLIENT_INCARNATION_MAGIC,
		.crc_slot = 2,
		.crc_boot_seq = ss->ss_persist.sps_boot_seq,
		.crc_incarnation = 1,
	};

	int ret = ss->ss_persist_ops->client_incarnation_add(ss->ss_persist_ctx,
							     &crc1);
	ck_assert_int_eq(ret, 0);
	ret = ss->ss_persist_ops->client_incarnation_add(ss->ss_persist_ctx,
							 &crc2);
	ck_assert_int_eq(ret, 0);

	/* Load and verify */
	struct client_incarnation_record loaded[8];
	size_t count = 0;
	ret = ss->ss_persist_ops->client_incarnation_load(ss->ss_persist_ctx,
							  loaded, 8, &count);
	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(count, 2);

	/* Remove one */
	ret = ss->ss_persist_ops->client_incarnation_remove(ss->ss_persist_ctx,
							    1);
	ck_assert_int_eq(ret, 0);

	/* Verify only one remains */
	count = 0;
	ret = ss->ss_persist_ops->client_incarnation_load(ss->ss_persist_ctx,
							  loaded, 8, &count);
	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(count, 1);
	ck_assert_uint_eq(loaded[0].crc_slot, 2);

	server_state_fini(ss);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Empty incarnations on fresh start                                   */
/* ------------------------------------------------------------------ */

START_TEST(test_empty_incarnations_on_fresh_start)
{
	struct server_state *ss = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss);

	struct client_incarnation_record loaded[8];
	size_t count = 99;
	int ret = ss->ss_persist_ops->client_incarnation_load(
		ss->ss_persist_ctx, loaded, 8, &count);

	/* Fresh start: either -ENOENT (no file) or 0 with count=0 */
	ck_assert(ret == -ENOENT || (ret == 0 && count == 0));

	server_state_fini(ss);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Restart sets unreclaimed count from incarnation records             */
/* ------------------------------------------------------------------ */

START_TEST(test_restart_sets_unreclaimed)
{
	struct server_state *ss1 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss1);

	/* Add 3 incarnation records (3 clients with state) */
	for (int i = 1; i <= 3; i++) {
		struct client_incarnation_record crc = {
			.crc_magic = CLIENT_INCARNATION_MAGIC,
			.crc_slot = i,
			.crc_boot_seq = ss1->ss_persist.sps_boot_seq,
			.crc_incarnation = 1,
		};
		ss1->ss_persist_ops->client_incarnation_add(ss1->ss_persist_ctx,
							    &crc);
	}

	simulate_crash(ss1);

	/* Restart */
	struct server_state *ss2 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss2);

	/* Should be in grace with 3 unreclaimed */
	enum server_lifecycle lc =
		atomic_load_explicit(&ss2->ss_lifecycle, memory_order_acquire);
	ck_assert(lc == SERVER_GRACE_STARTED || lc == SERVER_IN_GRACE);

	uint32_t unreclaimed =
		__atomic_load_n(&ss2->ss_unreclaimed, __ATOMIC_RELAXED);
	ck_assert_uint_eq(unreclaimed, 3);

	server_state_fini(ss2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Reclaim complete decrements unreclaimed                             */
/* ------------------------------------------------------------------ */

START_TEST(test_reclaim_decrements_unreclaimed)
{
	struct server_state *ss1 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss1);

	struct client_incarnation_record crc = {
		.crc_magic = CLIENT_INCARNATION_MAGIC,
		.crc_slot = 1,
		.crc_boot_seq = ss1->ss_persist.sps_boot_seq,
		.crc_incarnation = 1,
	};
	ss1->ss_persist_ops->client_incarnation_add(ss1->ss_persist_ctx, &crc);

	simulate_crash(ss1);

	struct server_state *ss2 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss2);

	/* Enter grace */
	struct server_state *got = server_state_get(ss2);
	ck_assert_ptr_nonnull(got);
	server_state_put(got);

	ck_assert(server_in_grace(ss2));
	ck_assert_uint_eq(
		__atomic_load_n(&ss2->ss_unreclaimed, __ATOMIC_RELAXED), 1);

	/* Simulate reclaim_complete */
	server_reclaim_complete(ss2);

	/* Grace should have ended (last client reclaimed) */
	enum server_lifecycle lc =
		atomic_load_explicit(&ss2->ss_lifecycle, memory_order_acquire);
	ck_assert_int_eq(lc, SERVER_GRACE_ENDED);

	server_state_fini(ss2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *client_recovery_suite(void)
{
	Suite *s = suite_create("client_recovery");
	TCase *tc;

	tc = tcase_create("persistence");
	tcase_add_checked_fixture(tc, recov_setup, recov_teardown);
	tcase_add_test(tc, test_identity_roundtrip);
	tcase_add_test(tc, test_incarnation_roundtrip);
	tcase_add_test(tc, test_empty_incarnations_on_fresh_start);
	suite_add_tcase(s, tc);

	tc = tcase_create("recovery");
	tcase_add_checked_fixture(tc, recov_setup, recov_teardown);
	tcase_add_test(tc, test_restart_sets_unreclaimed);
	tcase_add_test(tc, test_reclaim_decrements_unreclaimed);
	tcase_set_timeout(tc, 10);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = client_recovery_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
