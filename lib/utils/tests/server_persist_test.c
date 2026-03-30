/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Server state persistence verification tests (WI-1.4 + WI-1.5).
 *
 * Verifies that boot_seq, UUID, slot_next, and export registry
 * all survive server restart correctly.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <check.h>

#include "reffs/server.h"
#include "reffs/log.h"

static char state_dir[] = "/tmp/reffs-srvpersist-XXXXXX";

static void sp_setup(void)
{
	ck_assert_ptr_nonnull(mkdtemp(state_dir));
}

static void sp_teardown(void)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", state_dir);
	system(cmd);
	strcpy(state_dir, "/tmp/reffs-srvpersist-XXXXXX");
}

/* ------------------------------------------------------------------ */
/* boot_seq monotonically increases                                    */
/* ------------------------------------------------------------------ */

START_TEST(test_boot_seq_increases)
{
	struct server_state *ss1 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss1);

	uint16_t seq1 = ss1->ss_persist.sps_boot_seq;
	server_state_fini(ss1);

	struct server_state *ss2 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss2);

	uint16_t seq2 = ss2->ss_persist.sps_boot_seq;
	ck_assert_uint_gt(seq2, seq1);
	server_state_fini(ss2);

	struct server_state *ss3 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss3);

	uint16_t seq3 = ss3->ss_persist.sps_boot_seq;
	ck_assert_uint_gt(seq3, seq2);
	server_state_fini(ss3);
}
END_TEST

/* ------------------------------------------------------------------ */
/* UUID preserved across restart                                       */
/* ------------------------------------------------------------------ */

START_TEST(test_uuid_preserved)
{
	struct server_state *ss1 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss1);

	uuid_t uuid1;
	uuid_copy(uuid1, ss1->ss_persist.sps_uuid);
	ck_assert(!uuid_is_null(uuid1));

	server_state_fini(ss1);

	struct server_state *ss2 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss2);

	ck_assert_int_eq(uuid_compare(uuid1, ss2->ss_persist.sps_uuid), 0);
	server_state_fini(ss2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* slot_next preserved across restart                                  */
/* ------------------------------------------------------------------ */

START_TEST(test_slot_next_preserved)
{
	struct server_state *ss1 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss1);

	/* Simulate allocating some client slots */
	ss1->ss_persist.sps_slot_next = 42;
	ss1->ss_persist_ops->server_state_save(ss1->ss_persist_ctx,
					       &ss1->ss_persist);

	server_state_fini(ss1);

	struct server_state *ss2 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss2);

	ck_assert_uint_eq(ss2->ss_persist.sps_slot_next, 42);
	server_state_fini(ss2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* clean shutdown flag persists                                        */
/* ------------------------------------------------------------------ */

START_TEST(test_clean_shutdown_flag)
{
	struct server_state *ss1 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss1);

	/* fini writes clean_shutdown = 1 */
	server_state_fini(ss1);

	struct server_state *ss2 = server_state_init(
		state_dir, 2049, reffs_text_case_sensitive, REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(ss2);

	/*
	 * After clean restart, server should go straight to GRACE_ENDED
	 * (no prior clients with slot_next == 1 from fresh start).
	 */
	enum server_lifecycle lc =
		atomic_load_explicit(&ss2->ss_lifecycle, memory_order_acquire);
	ck_assert_int_eq(lc, SERVER_GRACE_ENDED);
	server_state_fini(ss2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *server_persist_suite(void)
{
	Suite *s = suite_create("server_persist");
	TCase *tc;

	tc = tcase_create("server_state");
	tcase_add_checked_fixture(tc, sp_setup, sp_teardown);
	tcase_add_test(tc, test_boot_seq_increases);
	tcase_add_test(tc, test_uuid_preserved);
	tcase_add_test(tc, test_slot_next_preserved);
	tcase_add_test(tc, test_clean_shutdown_flag);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = server_persist_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
