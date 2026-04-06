/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Grace lifecycle state machine unit tests (WI-1.1).
 *
 * Tests the server_state grace period transitions:
 *   BOOTING --> GRACE_STARTED --> IN_GRACE --> GRACE_ENDED
 *   BOOTING --> GRACE_ENDED (fresh start, no clients)
 *   SHUTTING_DOWN from any state
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <check.h>

/*
 * Tests that simulate crashes intentionally leak the server_state
 * to avoid calling server_state_fini (which overwrites clean_shutdown).
 * Suppress LSAN for this file.
 */
#ifdef ASAN_ENABLED
const char *__asan_default_options(void)
{
	return "detect_leaks=0";
}

/*
 * Belt-and-suspenders: __lsan_is_turned_off is checked by LSAN at
 * exit time, after __asan_default_options.  Some check(3) fork
 * configurations re-enable leak detection in children.
 */
int __lsan_is_turned_off(void)
{
	return 1;
}
#endif

#include "reffs/client_persist.h"
#include "reffs/server.h"
#include "reffs/log.h"

static char state_dir[] = "/tmp/reffs-grace-test-XXXXXX";

static void grace_setup(void)
{
	ck_assert_ptr_nonnull(mkdtemp(state_dir));
}

static void grace_teardown(void)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", state_dir);
	system(cmd);
	strcpy(state_dir, "/tmp/reffs-grace-test-XXXXXX");
}

/* ------------------------------------------------------------------ */
/* Fresh start: no prior clients --> skip grace                          */
/* ------------------------------------------------------------------ */

START_TEST(test_fresh_start_skips_grace)
{
	/*
	 * A fresh start (no server_state file, no incarnation records)
	 * should go directly to GRACE_ENDED -- no grace period.
	 */
	struct server_state *ss = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss);

	enum server_lifecycle lc =
		atomic_load_explicit(&ss->ss_lifecycle, memory_order_acquire);
	ck_assert_int_eq(lc, SERVER_GRACE_ENDED);

	server_state_fini(ss);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Dirty start: prior clients --> enter grace                            */
/* ------------------------------------------------------------------ */

START_TEST(test_dirty_start_enters_grace)
{
	/*
	 * Create server state, add a fake incarnation, then simulate
	 * a crash by saving dirty state and NOT calling fini (which
	 * would set clean_shutdown=1).
	 */
	struct server_state *ss1 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss1);

	/* Simulate a client by adding an incarnation record */
	struct client_incarnation_record crc = {
		.crc_magic = 0x434C4943U,
		.crc_slot = 1,
		.crc_boot_seq = ss1->ss_persist.sps_boot_seq,
		.crc_incarnation = 1,
	};
	ss1->ss_persist_ops->client_incarnation_add(ss1->ss_persist_ctx, &crc);

	/*
	 * Simulate crash: save with clean_shutdown=0, then tear down
	 * without calling server_state_fini (which would overwrite
	 * clean_shutdown back to 1).
	 */
	ss1->ss_persist.sps_clean_shutdown = 0;
	ss1->ss_persist_ops->server_state_save(ss1->ss_persist_ctx,
					       &ss1->ss_persist);
	/*
	 * Tear down without server_state_fini (simulating crash).
	 * Set SHUTTING_DOWN so the grace timer exits, then join it.
	 */
	atomic_store_explicit(&ss1->ss_lifecycle, SERVER_SHUTTING_DOWN,
			      memory_order_release);
	if (ss1->ss_grace_thread) {
		pthread_join(ss1->ss_grace_thread, NULL);
		ss1->ss_grace_thread = 0;
	}
	/* Leak ss1 -- acceptable in test (tmpdir cleaned in teardown) */

	/* Restart -- should enter grace */
	struct server_state *ss2 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss2);

	enum server_lifecycle lc =
		atomic_load_explicit(&ss2->ss_lifecycle, memory_order_acquire);
	/* Should be GRACE_STARTED (before any protocol layer get) */
	ck_assert(lc == SERVER_GRACE_STARTED || lc == SERVER_IN_GRACE);

	server_state_fini(ss2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* GRACE_STARTED --> IN_GRACE on first server_state_get                  */
/* ------------------------------------------------------------------ */

START_TEST(test_started_to_in_grace_on_get)
{
	/*
	 * After a dirty start, calling server_state_get() should
	 * transition from GRACE_STARTED to IN_GRACE.
	 */
	struct server_state *ss1 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss1);

	struct client_incarnation_record crc = {
		.crc_magic = 0x434C4943U,
		.crc_slot = 1,
		.crc_boot_seq = ss1->ss_persist.sps_boot_seq,
		.crc_incarnation = 1,
	};
	ss1->ss_persist_ops->client_incarnation_add(ss1->ss_persist_ctx, &crc);
	ss1->ss_persist.sps_clean_shutdown = 0;
	ss1->ss_persist_ops->server_state_save(ss1->ss_persist_ctx,
					       &ss1->ss_persist);
	atomic_store_explicit(&ss1->ss_lifecycle, SERVER_SHUTTING_DOWN,
			      memory_order_release);
	if (ss1->ss_grace_thread) {
		pthread_join(ss1->ss_grace_thread, NULL);
		ss1->ss_grace_thread = 0;
	}

	struct server_state *ss2 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss2);

	/* Simulate first protocol layer call */
	struct server_state *got = server_state_get(ss2);
	ck_assert_ptr_nonnull(got);

	enum server_lifecycle lc =
		atomic_load_explicit(&ss2->ss_lifecycle, memory_order_acquire);
	ck_assert_int_eq(lc, SERVER_IN_GRACE);

	server_state_put(got);
	server_state_fini(ss2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Timer ends grace after 2*grace_time                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_timer_ends_grace)
{
	/*
	 * With grace_time = 1s, the timer should end grace after 2s.
	 * Use a short grace period to keep the test fast.
	 */
	struct server_state *ss1 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss1);

	struct client_incarnation_record crc = {
		.crc_magic = 0x434C4943U,
		.crc_slot = 1,
		.crc_boot_seq = ss1->ss_persist.sps_boot_seq,
		.crc_incarnation = 1,
	};
	ss1->ss_persist_ops->client_incarnation_add(ss1->ss_persist_ctx, &crc);
	ss1->ss_persist.sps_clean_shutdown = 0;
	ss1->ss_persist.sps_lease_time = 1; /* 1 second lease */
	ss1->ss_persist_ops->server_state_save(ss1->ss_persist_ctx,
					       &ss1->ss_persist);
	atomic_store_explicit(&ss1->ss_lifecycle, SERVER_SHUTTING_DOWN,
			      memory_order_release);
	if (ss1->ss_grace_thread) {
		pthread_join(ss1->ss_grace_thread, NULL);
		ss1->ss_grace_thread = 0;
	}

	struct server_state *ss2 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss2);
	ss2->ss_grace_time = 1; /* override to 1 second */

	/* Trigger GRACE_STARTED --> IN_GRACE */
	struct server_state *got = server_state_get(ss2);
	ck_assert_ptr_nonnull(got);
	server_state_put(got);

	/* Wait for timer: 2 * grace_time + margin */
	sleep(3);

	enum server_lifecycle lc =
		atomic_load_explicit(&ss2->ss_lifecycle, memory_order_acquire);
	ck_assert_int_eq(lc, SERVER_GRACE_ENDED);

	server_state_fini(ss2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Shutdown during grace                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_shutdown_during_grace)
{
	/*
	 * Server can shut down cleanly while in grace.
	 */
	struct server_state *ss1 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss1);

	struct client_incarnation_record crc = {
		.crc_magic = 0x434C4943U,
		.crc_slot = 1,
		.crc_boot_seq = ss1->ss_persist.sps_boot_seq,
		.crc_incarnation = 1,
	};
	ss1->ss_persist_ops->client_incarnation_add(ss1->ss_persist_ctx, &crc);
	ss1->ss_persist.sps_clean_shutdown = 0;
	ss1->ss_persist_ops->server_state_save(ss1->ss_persist_ctx,
					       &ss1->ss_persist);
	atomic_store_explicit(&ss1->ss_lifecycle, SERVER_SHUTTING_DOWN,
			      memory_order_release);
	if (ss1->ss_grace_thread) {
		pthread_join(ss1->ss_grace_thread, NULL);
		ss1->ss_grace_thread = 0;
	}

	struct server_state *ss2 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss2);

	/* Enter grace */
	struct server_state *got = server_state_get(ss2);
	ck_assert_ptr_nonnull(got);
	server_state_put(got);

	ck_assert(server_in_grace(ss2));

	/* Shut down while in grace -- should not hang */
	server_state_fini(ss2);
	/* If we get here, shutdown didn't hang */
}
END_TEST

/* ------------------------------------------------------------------ */
/* nfs4_check_grace helper                                             */
/* ------------------------------------------------------------------ */

START_TEST(test_nfs4_check_grace_helper)
{
	/*
	 * nfs4_check_grace() is a self-contained helper that acquires
	 * and releases the server_state ref.  Verify it returns the
	 * correct value.
	 */
	struct server_state *ss = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss);

	/* Fresh start --> not in grace */
	ck_assert(!nfs4_check_grace());

	server_state_fini(ss);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *grace_suite(void)
{
	Suite *s = suite_create("grace_lifecycle");
	TCase *tc;

	tc = tcase_create("transitions");
	tcase_add_checked_fixture(tc, grace_setup, grace_teardown);
	tcase_add_test(tc, test_fresh_start_skips_grace);
	tcase_add_test(tc, test_dirty_start_enters_grace);
	tcase_add_test(tc, test_started_to_in_grace_on_get);
	tcase_add_test(tc, test_timer_ends_grace);
	tcase_add_test(tc, test_shutdown_during_grace);
	tcase_add_test(tc, test_nfs4_check_grace_helper);
	tcase_set_timeout(tc, 10); /* timer test needs 3s */
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = grace_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
