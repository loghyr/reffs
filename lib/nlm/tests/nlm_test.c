/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <rpc/rpc.h>
#include "../../fs/tests/fs_test_harness.h"
#include "reffs/nlm_lock.h"
#include "nlm4_prot.h"

uid_t fs_test_uid;
gid_t fs_test_gid;

static void setup(void)
{
	fs_test_setup();
}
static void teardown(void)
{
	fs_test_teardown();
}

static void init_lockargs(struct nlm4_lockargs *args, char *oh_bytes,
			  uint32_t oh_len, char *caller, uint64_t offset,
			  uint64_t len, bool exclusive)
{
	memset(args, 0, sizeof(*args));
	args->alock.caller_name = caller;
	args->alock.oh.n_len = oh_len;
	args->alock.oh.n_bytes = oh_bytes;
	args->alock.svid = 1234;
	args->alock.l_offset = offset;
	args->alock.l_len = len;
	args->exclusive = exclusive;
}

/*
 * Test 1: Basic lock and unlock.
 */
START_TEST(test_nlm_basic_lock_unlock)
{
	struct super_block *sb;
	struct inode *inode;
	struct nlm4_lockargs args;
	struct nlm4_unlockargs uargs;
	char *oh = "owner1";

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	inode = inode_find(sb, INODE_ROOT_ID); /* Root inode */

	init_lockargs(&args, oh, 6, "client1", 0, 100, true);
	ck_assert_int_eq(reffs_nlm4_lock(inode, &args), NLM4_GRANTED);

	/* Unlock it */
	memset(&uargs, 0, sizeof(uargs));
	uargs.alock = args.alock;
	ck_assert_int_eq(reffs_nlm4_unlock(inode, &uargs), NLM4_GRANTED);

	inode_put(inode);
	super_block_put(sb);
}
END_TEST

/*
 * Test 2: Conflicting locks.
 */
START_TEST(test_nlm_conflicting_locks)
{
	struct super_block *sb;
	struct inode *inode;
	struct nlm4_lockargs args1, args2;
	struct nlm4_unlockargs uargs;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	inode = inode_find(sb, INODE_ROOT_ID);

	/* Client 1 takes an exclusive lock on 0-100 */
	init_lockargs(&args1, "owner1", 6, "client1", 0, 100, true);
	ck_assert_int_eq(reffs_nlm4_lock(inode, &args1), NLM4_GRANTED);

	/* Client 2 tries to take a shared lock on 50-150 -> should fail */
	init_lockargs(&args2, "owner2", 6, "client2", 50, 100, false);
	ck_assert_int_eq(reffs_nlm4_lock(inode, &args2), NLM4_DENIED);

	/* Client 1 unlocks */
	memset(&uargs, 0, sizeof(uargs));
	uargs.alock = args1.alock;
	ck_assert_int_eq(reffs_nlm4_unlock(inode, &uargs), NLM4_GRANTED);

	/* Now client 2 should succeed */
	ck_assert_int_eq(reffs_nlm4_lock(inode, &args2), NLM4_GRANTED);

	/* Cleanup */
	uargs.alock = args2.alock;
	reffs_nlm4_unlock(inode, &uargs);

	inode_put(inode);
	super_block_put(sb);
}
END_TEST

/*
 * Test 3: Multiple shared locks.
 */
START_TEST(test_nlm_shared_locks)
{
	struct super_block *sb;
	struct inode *inode;
	struct nlm4_lockargs args1, args2;
	struct nlm4_unlockargs uargs;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	inode = inode_find(sb, INODE_ROOT_ID);

	/* Client 1 takes shared lock on 0-100 */
	init_lockargs(&args1, "owner1", 6, "client1", 0, 100, false);
	ck_assert_int_eq(reffs_nlm4_lock(inode, &args1), NLM4_GRANTED);

	/* Client 2 takes shared lock on 50-150 -> should succeed */
	init_lockargs(&args2, "owner2", 6, "client2", 50, 100, false);
	ck_assert_int_eq(reffs_nlm4_lock(inode, &args2), NLM4_GRANTED);

	/* Cleanup */
	uargs.alock = args1.alock;
	reffs_nlm4_unlock(inode, &uargs);
	uargs.alock = args2.alock;
	reffs_nlm4_unlock(inode, &uargs);

	inode_put(inode);
	super_block_put(sb);
}
END_TEST

Suite *nlm_test_suite(void)
{
	Suite *s = suite_create("nlm: core locking");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_nlm_basic_lock_unlock);
	tcase_add_test(tc, test_nlm_conflicting_locks);
	tcase_add_test(tc, test_nlm_shared_locks);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(nlm_test_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
