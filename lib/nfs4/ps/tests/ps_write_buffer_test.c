/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Buffer-table mechanics for PS Phase 4a (slice 4a.2a).  The flush-
 * on-COMMIT shim is slice 4a.2b; this file covers the table itself:
 * alloc, lookup, drop, listener-id disambiguation, table counter,
 * basic quiesce-enter/leave balance.
 *
 * Tests do NOT need a real MDS session or RPC stack -- the buffer
 * table is a pure in-memory store.  ps_state_init/fini wraps each
 * test so the per-listener slot lifecycle is exercised too.
 */

#include <check.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <urcu.h>

#include "reffs/settings.h"
#include "ps_state.h"
#include "ps_write_buffer.h"
#include "ps_write_buffer_internal.h"

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

static void setup(void)
{
	rcu_register_thread();
	ps_state_init();
}

static void teardown(void)
{
	ps_state_fini();
	rcu_unregister_thread();
}

static struct reffs_proxy_mds_config make_cfg(uint32_t id)
{
	struct reffs_proxy_mds_config cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id = id;
	cfg.port = 4098;
	cfg.mds_port = 2049;
	cfg.mds_probe = 20490;
	strncpy(cfg.address, "10.0.0.1", sizeof(cfg.address) - 1);
	return cfg;
}

static struct ps_listener_state *mut_listener(uint32_t id)
{
	/*
	 * ps_state_find returns const; tests need the writable
	 * pointer to drive enter_quiesce_or_bail.  Cast away const
	 * deliberately -- the test owns the listener's lifetime via
	 * ps_state_init/fini.
	 */
	return (struct ps_listener_state *)ps_state_find(id);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_alloc_inserts_buffer)
{
	struct reffs_proxy_mds_config cfg = make_cfg(1);
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xA1 };
	uint8_t fh[] = { 0xF1, 0xF2, 0xF3 };
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(1);
	ck_assert_ptr_nonnull(pls);

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	buf = ps_write_buffer_lookup_or_alloc(pls, stateid, fh, sizeof(fh));
	ck_assert_ptr_nonnull(buf);
	ck_assert_uint_eq(ps_write_buffer_table_count(pls), 1);

	ps_write_buffer_release_find_ref(buf, pls);
}
END_TEST

START_TEST(test_alloc_finds_existing)
{
	struct reffs_proxy_mds_config cfg = make_cfg(2);
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xB1 };
	uint8_t fh[] = { 0xC1, 0xC2 };
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf1, *buf2;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(2);

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	buf1 = ps_write_buffer_lookup_or_alloc(pls, stateid, fh, sizeof(fh));
	ck_assert_ptr_nonnull(buf1);
	ps_write_buffer_release_find_ref(buf1, pls);

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	buf2 = ps_write_buffer_lookup_or_alloc(pls, stateid, fh, sizeof(fh));
	ck_assert_ptr_eq(buf1, buf2);
	ck_assert_uint_eq(ps_write_buffer_table_count(pls), 1);

	ps_write_buffer_release_find_ref(buf2, pls);
}
END_TEST

START_TEST(test_drop_removes_buffer)
{
	struct reffs_proxy_mds_config cfg = make_cfg(3);
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xD1 };
	uint8_t fh[] = { 0xE1 };
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(3);

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	buf = ps_write_buffer_lookup_or_alloc(pls, stateid, fh, sizeof(fh));
	ck_assert_ptr_nonnull(buf);
	ck_assert_uint_eq(ps_write_buffer_table_count(pls), 1);

	ps_write_buffer_drop(buf, pls);
	ck_assert_uint_eq(ps_write_buffer_table_count(pls), 0);
}
END_TEST

START_TEST(test_alloc_different_stateids_distinct)
{
	struct reffs_proxy_mds_config cfg = make_cfg(4);
	uint8_t s1[PS_STATEID_OTHER_SIZE] = { 0x11 };
	uint8_t s2[PS_STATEID_OTHER_SIZE] = { 0x22 };
	uint8_t fh[] = { 0xAA };
	struct ps_listener_state *pls;
	struct ps_write_buffer *b1, *b2;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(4);

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	b1 = ps_write_buffer_lookup_or_alloc(pls, s1, fh, sizeof(fh));
	ps_write_buffer_release_find_ref(b1, pls);

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	b2 = ps_write_buffer_lookup_or_alloc(pls, s2, fh, sizeof(fh));
	ck_assert_ptr_ne(b1, b2);
	ck_assert_uint_eq(ps_write_buffer_table_count(pls), 2);

	ps_write_buffer_release_find_ref(b2, pls);
}
END_TEST

START_TEST(test_listener_id_collision_distinct_buffers)
{
	/*
	 * Two listeners share the same (stateid, fh) key but each owns
	 * its own pls_write_buffer_ht.  The buffers must be distinct
	 * objects and live in their respective tables; the per-listener
	 * disambiguator is the table pointer, not a listener_id hash
	 * component.  Pins the "tables are per-listener" contract from
	 * the design's State / data structures section.
	 */
	struct reffs_proxy_mds_config a = make_cfg(10);
	struct reffs_proxy_mds_config b = make_cfg(20);
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xCC };
	uint8_t fh[] = { 0xDD };
	struct ps_listener_state *pa, *pb;
	struct ps_write_buffer *ba, *bb;

	ck_assert_int_eq(ps_state_register(&a), 0);
	ck_assert_int_eq(ps_state_register(&b), 0);
	pa = mut_listener(10);
	pb = mut_listener(20);

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pa));
	ba = ps_write_buffer_lookup_or_alloc(pa, stateid, fh, sizeof(fh));

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pb));
	bb = ps_write_buffer_lookup_or_alloc(pb, stateid, fh, sizeof(fh));

	ck_assert_ptr_ne(ba, bb);
	ck_assert_uint_eq(ps_write_buffer_table_count(pa), 1);
	ck_assert_uint_eq(ps_write_buffer_table_count(pb), 1);

	ps_write_buffer_release_find_ref(ba, pa);
	ps_write_buffer_release_find_ref(bb, pb);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *ps_write_buffer_suite(void)
{
	Suite *s = suite_create("ps_write_buffer");
	TCase *tc = tcase_create("buffer_table");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_alloc_inserts_buffer);
	tcase_add_test(tc, test_alloc_finds_existing);
	tcase_add_test(tc, test_drop_removes_buffer);
	tcase_add_test(tc, test_alloc_different_stateids_distinct);
	tcase_add_test(tc, test_listener_id_collision_distinct_buffers);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_write_buffer_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
