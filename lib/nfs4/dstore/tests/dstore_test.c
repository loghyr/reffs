/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for dstore lifecycle.
 *
 * These tests exercise the hash table, refcount, and state transitions.
 * No real NFS server is contacted -- dstore_alloc with unreachable
 * addresses creates dstores in the unmounted state, which is a valid
 * and supported configuration (the MDS logs the error and continues).
 *
 * Tests:
 *   1. init/fini:    global hash table lifecycle
 *   2. alloc/find:   insert and lookup by ID
 *   3. refcount:     get/put lifecycle
 *   4. duplicate ID: rejected by cds_lfht_add_unique
 *   5. unmounted:    dstore_is_available returns false
 *   6. unload_all:   drains the hash table
 *   7. drain mechanics (mirror-lifecycle Slice B):
 *        - drain excludes from collect_available
 *        - undrain restores it
 *        - drained still in collect_all
 *        - drain idempotent (DRAINING -> DRAINING no-op)
 *        - drain does not affect is_connected
 */

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>

#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/super_block.h"
#include "nfs4_test_harness.h"

#define FAKE_DS_ADDR "192.0.2.1"
#define FAKE_DS_PATH "/nonexistent"

static void setup(void)
{
	nfs4_test_setup();
	ck_assert_int_eq(dstore_init(), 0);
}

static void teardown(void)
{
	dstore_fini();
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_init_fini)
{
	/* setup/teardown exercises the normal init/fini path.
	 * Verify dstore_fini is safe to call on an already-finalized
	 * subsystem (double-fini is a no-op). */
	dstore_fini();
	dstore_fini(); /* second call is a no-op */
	/* Reinit so the remaining tests and teardown work. */
	ck_assert_int_eq(dstore_init(), 0);
}
END_TEST

START_TEST(test_alloc_find)
{
	struct dstore *ds = dstore_alloc(42, FAKE_DS_ADDR, 0, FAKE_DS_PATH,
					 REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert_uint_eq(ds->ds_id, 42);
	ck_assert_str_eq(ds->ds_address, FAKE_DS_ADDR);
	ck_assert_str_eq(ds->ds_path, FAKE_DS_PATH);

	/* Should be findable. */
	struct dstore *found = dstore_find(42);

	ck_assert_ptr_nonnull(found);
	ck_assert_ptr_eq(found, ds);

	/* Not findable with wrong ID. */
	ck_assert_ptr_null(dstore_find(99));

	dstore_put(found); /* find ref */
	dstore_put(ds); /* alloc ref */
}
END_TEST

START_TEST(test_refcount)
{
	struct dstore *ds = dstore_alloc(1, FAKE_DS_ADDR, 0, FAKE_DS_PATH,
					 REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds);

	/* Bump ref. */
	struct dstore *ref2 = dstore_get(ds);

	ck_assert_ptr_eq(ref2, ds);

	/* Drop extra ref -- dstore should still be alive. */
	dstore_put(ref2);

	/* Still findable. */
	struct dstore *found = dstore_find(1);

	ck_assert_ptr_nonnull(found);
	dstore_put(found);

	/* Drop alloc ref -- hash table still holds one. */
	dstore_put(ds);

	/* Still findable via hash table ref. */
	found = dstore_find(1);
	ck_assert_ptr_nonnull(found);
	dstore_put(found);
}
END_TEST

START_TEST(test_duplicate_id)
{
	struct dstore *ds1 = dstore_alloc(7, FAKE_DS_ADDR, 0, FAKE_DS_PATH,
					  REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds1);

	/* Same ID should fail. */
	struct dstore *ds2 = dstore_alloc(7, "192.0.2.2", 0, "/other",
					  REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_null(ds2);

	/* Original still findable. */
	struct dstore *found = dstore_find(7);

	ck_assert_ptr_nonnull(found);
	ck_assert_str_eq(found->ds_address, FAKE_DS_ADDR);
	dstore_put(found);
	dstore_put(ds1);
}
END_TEST

START_TEST(test_unmounted_not_available)
{
	struct dstore *ds = dstore_alloc(3, FAKE_DS_ADDR, 0, FAKE_DS_PATH,
					 REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds);

	/* Mount to a fake address fails -- dstore is not available. */
	ck_assert(!dstore_is_available(ds));

	/* But it IS in the hash table. */
	struct dstore *found = dstore_find(3);

	ck_assert_ptr_nonnull(found);
	dstore_put(found);
	dstore_put(ds);
}
END_TEST

START_TEST(test_unload_all)
{
	struct dstore *ds1 = dstore_alloc(10, FAKE_DS_ADDR, 0, FAKE_DS_PATH,
					  REFFS_DS_PROTO_NFSV3, false);
	struct dstore *ds2 = dstore_alloc(20, "192.0.2.2", 0, FAKE_DS_PATH,
					  REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds1);
	ck_assert_ptr_nonnull(ds2);

	/* Drop caller refs -- hash table still owns them. */
	dstore_put(ds1);
	dstore_put(ds2);

	/* Both findable. */
	struct dstore *f1 = dstore_find(10);
	struct dstore *f2 = dstore_find(20);

	ck_assert_ptr_nonnull(f1);
	ck_assert_ptr_nonnull(f2);
	dstore_put(f1);
	dstore_put(f2);

	/* Drain. */
	dstore_unload_all();

	/* No longer findable. */
	ck_assert_ptr_null(dstore_find(10));
	ck_assert_ptr_null(dstore_find(20));
}
END_TEST

/* ------------------------------------------------------------------ */
/* Vtable selection tests                                              */
/* ------------------------------------------------------------------ */

START_TEST(test_local_vtable_ipv4)
{
	struct dstore *ds = dstore_alloc(50, "127.0.0.1", 0, FAKE_DS_PATH,
					 REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert_ptr_eq(ds->ds_ops, &dstore_ops_local);
	ck_assert(dstore_is_available(ds));
	dstore_put(ds);
}
END_TEST

START_TEST(test_local_vtable_ipv6)
{
	struct dstore *ds = dstore_alloc(51, "::1", 0, FAKE_DS_PATH,
					 REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert_ptr_eq(ds->ds_ops, &dstore_ops_local);
	ck_assert(dstore_is_available(ds));
	dstore_put(ds);
}
END_TEST

START_TEST(test_local_vtable_localhost)
{
	struct dstore *ds = dstore_alloc(52, "localhost", 0, FAKE_DS_PATH,
					 REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert_ptr_eq(ds->ds_ops, &dstore_ops_local);
	ck_assert(dstore_is_available(ds));
	dstore_put(ds);
}
END_TEST

START_TEST(test_remote_vtable)
{
	struct dstore *ds = dstore_alloc(53, "192.168.1.100", 0, FAKE_DS_PATH,
					 REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert_ptr_eq(ds->ds_ops, &dstore_ops_nfsv3);
	/* Remote without mount is not available. */
	ck_assert(!dstore_is_available(ds));
	dstore_put(ds);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Drain bit mechanics (mirror-lifecycle Slice B)                      */
/* ------------------------------------------------------------------ */

/* Helper: count how many of the given dstore IDs appear in `arr`,
 * dropping a ref on every entry as we go. */
static unsigned drain_count_ids(struct dstore **arr, uint32_t n,
				const uint32_t *ids, unsigned nids)
{
	unsigned hits = 0;

	for (uint32_t i = 0; i < n; i++) {
		for (unsigned j = 0; j < nids; j++) {
			if (arr[i]->ds_id == ids[j]) {
				hits++;
				break;
			}
		}
		dstore_put(arr[i]);
	}
	return hits;
}

START_TEST(test_drain_excludes_from_collect_available)
{
	struct dstore *a = dstore_alloc(60, "127.0.0.1", 0, FAKE_DS_PATH,
					REFFS_DS_PROTO_NFSV3, false);
	struct dstore *b = dstore_alloc(61, "127.0.0.1", 0, FAKE_DS_PATH,
					REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(a);
	ck_assert_ptr_nonnull(b);
	/* Local-vtable dstores start mounted+available. */
	ck_assert(dstore_is_available(a));
	ck_assert(dstore_is_available(b));

	atomic_store_explicit(&a->ds_drained, true, memory_order_release);
	ck_assert(!dstore_is_available(a));
	ck_assert(dstore_is_available(b));

	struct dstore *out[8] = { 0 };
	uint32_t n = dstore_collect_available(out, 8);
	uint32_t want_b[1] = { 61 };
	uint32_t want_a[1] = { 60 };

	ck_assert_uint_eq(drain_count_ids(out, n, want_b, 1), 1);
	/* a must NOT appear -- re-collect to recheck (refs already dropped). */
	n = dstore_collect_available(out, 8);
	ck_assert_uint_eq(drain_count_ids(out, n, want_a, 1), 0);

	dstore_put(a);
	dstore_put(b);
}
END_TEST

START_TEST(test_undrain_restores)
{
	struct dstore *ds = dstore_alloc(62, "127.0.0.1", 0, FAKE_DS_PATH,
					 REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert(dstore_is_available(ds));

	atomic_store_explicit(&ds->ds_drained, true, memory_order_release);
	ck_assert(!dstore_is_available(ds));

	atomic_store_explicit(&ds->ds_drained, false, memory_order_release);
	ck_assert(dstore_is_available(ds));

	struct dstore *out[8] = { 0 };
	uint32_t n = dstore_collect_available(out, 8);
	uint32_t want[1] = { 62 };

	ck_assert_uint_eq(drain_count_ids(out, n, want, 1), 1);

	dstore_put(ds);
}
END_TEST

START_TEST(test_drained_still_in_collect_all)
{
	struct dstore *a = dstore_alloc(63, "127.0.0.1", 0, FAKE_DS_PATH,
					REFFS_DS_PROTO_NFSV3, false);
	struct dstore *b = dstore_alloc(64, "127.0.0.1", 0, FAKE_DS_PATH,
					REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(a);
	ck_assert_ptr_nonnull(b);

	/* Drain a; collect_all must still return both. */
	atomic_store_explicit(&a->ds_drained, true, memory_order_release);

	struct dstore *out[8] = { 0 };
	uint32_t n = dstore_collect_all(out, 8);
	uint32_t want[2] = { 63, 64 };

	ck_assert_uint_eq(drain_count_ids(out, n, want, 2), 2);

	dstore_put(a);
	dstore_put(b);
}
END_TEST

START_TEST(test_drain_idempotent)
{
	struct dstore *ds = dstore_alloc(65, "127.0.0.1", 0, FAKE_DS_PATH,
					 REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert(dstore_is_available(ds));

	/* DSTORE_DRAIN on DRAINING -- second store is a no-op. */
	atomic_store_explicit(&ds->ds_drained, true, memory_order_release);
	atomic_store_explicit(&ds->ds_drained, true, memory_order_release);
	ck_assert(!dstore_is_available(ds));
	ck_assert(atomic_load_explicit(&ds->ds_drained, memory_order_acquire));

	dstore_put(ds);
}
END_TEST

START_TEST(test_drain_does_not_affect_is_connected)
{
	struct dstore *ds = dstore_alloc(66, "127.0.0.1", 0, FAKE_DS_PATH,
					 REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert(dstore_is_available(ds));
	ck_assert(dstore_is_connected(ds));

	atomic_store_explicit(&ds->ds_drained, true, memory_order_release);
	/* Drained -- not eligible for placement, but still connected. */
	ck_assert(!dstore_is_available(ds));
	ck_assert(dstore_is_connected(ds));

	dstore_put(ds);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Reverse-index mechanics (mirror-lifecycle Slice B'')                */
/* ------------------------------------------------------------------ */

/*
 * The dstore reverse index lives on a struct super_block, accessed
 * via sb->sb_ops->dstore_index_*.  The test harness's root SB is
 * RAM-backed, so the RAM impl is exercised here.  Slice C/D/E will
 * exercise it via real layout-mutation paths; this test set covers
 * the underlying primitives.
 */

struct count_arg {
	uint32_t ds_id;
	unsigned hits;
	uint64_t inums[16];
	int stop_after;
};

static int count_cb(uint32_t ds_id, uint64_t inum, void *arg)
{
	struct count_arg *ca = arg;

	ck_assert_uint_eq(ds_id, ca->ds_id);
	if (ca->hits < (sizeof(ca->inums) / sizeof(ca->inums[0])))
		ca->inums[ca->hits] = inum;
	ca->hits++;
	if (ca->stop_after > 0 && (int)ca->hits >= ca->stop_after)
		return 1; /* signal iter to stop */
	return 0;
}

START_TEST(test_dstore_index_add_count_remove)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	uint64_t n = 0;

	ck_assert_ptr_nonnull(sb);
	/* Function-pointer non-NULL check; ck_assert_ptr_nonnull casts to
	 * `const void *` which is pedantic-flagged for fn ptrs. */
	ck_assert(sb->sb_ops->dstore_index_add != NULL);
	ck_assert(sb->sb_ops->dstore_index_remove != NULL);
	ck_assert(sb->sb_ops->dstore_index_count != NULL);

	ck_assert_int_eq(sb->sb_ops->dstore_index_count(sb, 100, &n), 0);
	ck_assert_uint_eq(n, 0);

	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 100, 11), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 100, 22), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 100, 33), 0);

	ck_assert_int_eq(sb->sb_ops->dstore_index_count(sb, 100, &n), 0);
	ck_assert_uint_eq(n, 3);

	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 100, 22), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_count(sb, 100, &n), 0);
	ck_assert_uint_eq(n, 2);

	/* Cleanup -- leave the index empty for the next test. */
	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 100, 11), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 100, 33), 0);
	super_block_put(sb);
}
END_TEST

START_TEST(test_dstore_index_add_duplicate_eexist)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 200, 50), 0);
	/* Same (ds_id, inum) again -- idempotent contract: -EEXIST. */
	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 200, 50), -EEXIST);
	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 200, 50), 0);
	super_block_put(sb);
}
END_TEST

START_TEST(test_dstore_index_remove_missing_enoent)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 300, 99), -ENOENT);
	super_block_put(sb);
}
END_TEST

START_TEST(test_dstore_index_isolation_by_dsid)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	uint64_t n = 0;

	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 400, 1), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 400, 2), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 401, 3), 0);

	ck_assert_int_eq(sb->sb_ops->dstore_index_count(sb, 400, &n), 0);
	ck_assert_uint_eq(n, 2);
	ck_assert_int_eq(sb->sb_ops->dstore_index_count(sb, 401, &n), 0);
	ck_assert_uint_eq(n, 1);

	/* Same inum on a different ds_id is a different entry. */
	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 401, 1), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_count(sb, 400, &n), 0);
	ck_assert_uint_eq(n, 2);
	ck_assert_int_eq(sb->sb_ops->dstore_index_count(sb, 401, &n), 0);
	ck_assert_uint_eq(n, 2);

	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 400, 1), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 400, 2), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 401, 3), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 401, 1), 0);
	super_block_put(sb);
}
END_TEST

START_TEST(test_dstore_index_iter_callback_filters_by_dsid)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	struct count_arg ca = { .ds_id = 500 };

	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 500, 11), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 500, 22), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 501, 33), 0);

	ck_assert_int_eq(sb->sb_ops->dstore_index_iter(sb, 500, count_cb, &ca),
			 0);
	ck_assert_uint_eq(ca.hits, 2);

	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 500, 11), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 500, 22), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 501, 33), 0);
	super_block_put(sb);
}
END_TEST

START_TEST(test_dstore_index_iter_callback_break)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	struct count_arg ca = { .ds_id = 600, .stop_after = 2 };

	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 600, 1), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 600, 2), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 600, 3), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_add(sb, 600, 4), 0);

	/* Callback returns 1 after seeing 2 entries; iter must stop. */
	int r = sb->sb_ops->dstore_index_iter(sb, 600, count_cb, &ca);

	ck_assert_int_eq(r, 1);
	ck_assert_uint_eq(ca.hits, 2);

	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 600, 1), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 600, 2), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 600, 3), 0);
	ck_assert_int_eq(sb->sb_ops->dstore_index_remove(sb, 600, 4), 0);
	super_block_put(sb);
}
END_TEST

START_TEST(test_dstore_instance_count_field_present)
{
	struct dstore *ds = dstore_alloc(700, "127.0.0.1", 0, FAKE_DS_PATH,
					 REFFS_DS_PROTO_NFSV3, false);

	ck_assert_ptr_nonnull(ds);
	/*
	 * Cache starts at 0 -- slice B''-stage1 only adds the field;
	 * stage 2 wires the rebuild.  This test pins the initial value
	 * so a future cache-rebuild change can't silently break the
	 * "fresh dstore is empty" invariant.
	 */
	ck_assert_uint_eq(atomic_load_explicit(&ds->ds_instance_count,
					       memory_order_relaxed),
			  0);
	dstore_put(ds);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

Suite *dstore_suite(void)
{
	Suite *s = suite_create("Dstore Lifecycle");

	TCase *tc = tcase_create("lifecycle");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_init_fini);
	tcase_add_test(tc, test_alloc_find);
	tcase_add_test(tc, test_refcount);
	tcase_add_test(tc, test_duplicate_id);
	tcase_add_test(tc, test_unmounted_not_available);
	tcase_add_test(tc, test_unload_all);
	suite_add_tcase(s, tc);

	TCase *tc_vtable = tcase_create("vtable");

	tcase_add_checked_fixture(tc_vtable, setup, teardown);
	tcase_add_test(tc_vtable, test_local_vtable_ipv4);
	tcase_add_test(tc_vtable, test_local_vtable_ipv6);
	tcase_add_test(tc_vtable, test_local_vtable_localhost);
	tcase_add_test(tc_vtable, test_remote_vtable);
	suite_add_tcase(s, tc_vtable);

	TCase *tc_drain = tcase_create("drain");

	tcase_add_checked_fixture(tc_drain, setup, teardown);
	tcase_add_test(tc_drain, test_drain_excludes_from_collect_available);
	tcase_add_test(tc_drain, test_undrain_restores);
	tcase_add_test(tc_drain, test_drained_still_in_collect_all);
	tcase_add_test(tc_drain, test_drain_idempotent);
	tcase_add_test(tc_drain, test_drain_does_not_affect_is_connected);
	suite_add_tcase(s, tc_drain);

	TCase *tc_idx = tcase_create("dstore_index");

	tcase_add_checked_fixture(tc_idx, setup, teardown);
	tcase_add_test(tc_idx, test_dstore_index_add_count_remove);
	tcase_add_test(tc_idx, test_dstore_index_add_duplicate_eexist);
	tcase_add_test(tc_idx, test_dstore_index_remove_missing_enoent);
	tcase_add_test(tc_idx, test_dstore_index_isolation_by_dsid);
	tcase_add_test(tc_idx, test_dstore_index_iter_callback_filters_by_dsid);
	tcase_add_test(tc_idx, test_dstore_index_iter_callback_break);
	tcase_add_test(tc_idx, test_dstore_instance_count_field_present);
	suite_add_tcase(s, tc_idx);

	return s;
}

int main(void)
{
	return nfs4_test_run(dstore_suite());
}
