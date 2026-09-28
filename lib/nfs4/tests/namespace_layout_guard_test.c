/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdatomic.h>
#include <string.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "reffs/filehandle.h"
#include "reffs/fixed_layout.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/vfs.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"

#include "nfs4_test_harness.h"

static struct super_block *test_sb;
static struct inode *test_root;
static struct authunix_parms test_ap;

static struct inode *create_file(const char *name)
{
	struct inode *inode = NULL;

	ck_assert_int_eq(vfs_create(test_root, name, 0600, &test_ap, &inode,
				    NULL, NULL),
			 0);
	ck_assert_ptr_nonnull(inode);
	return inode;
}

static struct inode *lookup_file(const char *name)
{
	struct inode *inode = inode_name_get_inode(test_root, (char *)name);

	ck_assert_ptr_nonnull(inode);
	return inode;
}

static void setup(void)
{
	nfs4_test_setup();
	test_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(test_sb);
	test_root = inode_find(test_sb, INODE_ROOT_ID);
	ck_assert_ptr_nonnull(test_root);
	memset(&test_ap, 0, sizeof(test_ap));
}

static void teardown(void)
{
	inode_active_put(test_root);
	super_block_put(test_sb);
	nfs4_test_teardown();
}

START_TEST(test_remove_rejects_rebound_name)
{
	struct inode *selected = create_file("remove-selected");
	struct inode *replacement = create_file("remove-replacement");
	struct inode *found;

	ck_assert(nfs4_layout_remove_lock(selected));
	ck_assert_int_eq(vfs_rename(test_root, "remove-selected", test_root,
				    "remove-parked", &test_ap, NULL, NULL, NULL,
				    NULL),
			 0);
	ck_assert_int_eq(vfs_rename(test_root, "remove-replacement", test_root,
				    "remove-selected", &test_ap, NULL, NULL,
				    NULL, NULL),
			 0);
	replacement->i_layout_barrier.active = true;

	ck_assert_int_eq(vfs_remove_expected(test_root, "remove-selected",
					     selected, &test_ap, NULL, NULL),
			 -EAGAIN);
	ck_assert_uint_eq(atomic_load_explicit(&replacement->i_nlink,
					       memory_order_acquire),
			  1);
	found = lookup_file("remove-selected");
	ck_assert_ptr_eq(found, replacement);
	inode_active_put(found);

	nfs4_layout_remove_unlock(selected);
	replacement->i_layout_barrier.active = false;
	ck_assert_int_eq(vfs_remove(test_root, "remove-selected", &test_ap,
				    NULL, NULL),
			 0);
	ck_assert_int_eq(vfs_remove(test_root, "remove-parked", &test_ap, NULL,
				    NULL),
			 0);
	inode_active_put(replacement);
	inode_active_put(selected);
}
END_TEST

START_TEST(test_rename_rejects_assigned_destination)
{
	struct inode *source = create_file("rename-source");
	struct inode *destination = create_file("rename-destination");
	struct inode *found;
	nfs_argop4 arg = { .argop = OP_RENAME };
	nfs_resop4 result = { .resop = OP_RENAME };
	COMPOUND4args args = {
		.argarray = { .argarray_len = 1, .argarray_val = &arg },
	};
	COMPOUND4res response = {
		.resarray = { .resarray_len = 1, .resarray_val = &result },
	};
	struct compound compound = {
		.c_curr_op = 0,
		.c_inode = test_root,
		.c_curr_sb = test_sb,
		.c_saved_sb = test_sb,
		.c_args = &args,
		.c_res = &response,
		.c_ap = test_ap,
	};
	uint64_t source_nlink =
		atomic_load_explicit(&source->i_nlink, memory_order_acquire);
	uint64_t destination_nlink = atomic_load_explicit(&destination->i_nlink,
							  memory_order_acquire);
	uint64_t changeid = atomic_load_explicit(&test_root->i_changeid,
						 memory_order_acquire);

	compound.c_curr_nfh.nfh_sb = test_sb->sb_id;
	compound.c_curr_nfh.nfh_ino = test_root->i_ino;
	compound.c_saved_nfh = compound.c_curr_nfh;
	arg.nfs_argop4_u.oprename.oldname.utf8string_val = "rename-source";
	arg.nfs_argop4_u.oprename.oldname.utf8string_len =
		strlen("rename-source");
	arg.nfs_argop4_u.oprename.newname.utf8string_val = "rename-destination";
	arg.nfs_argop4_u.oprename.newname.utf8string_len =
		strlen("rename-destination");
	destination->i_layout_barrier.active = true;

	ck_assert_uint_eq(nfs4_op_rename(&compound), 0);
	ck_assert_int_eq(result.nfs_resop4_u.oprename.status, NFS4ERR_NOTSUPP);
	ck_assert_uint_eq(atomic_load_explicit(&source->i_nlink,
					       memory_order_acquire),
			  source_nlink);
	ck_assert_uint_eq(atomic_load_explicit(&destination->i_nlink,
					       memory_order_acquire),
			  destination_nlink);
	ck_assert_uint_eq(atomic_load_explicit(&test_root->i_changeid,
					       memory_order_acquire),
			  changeid);
	found = lookup_file("rename-source");
	ck_assert_ptr_eq(found, source);
	inode_active_put(found);
	found = lookup_file("rename-destination");
	ck_assert_ptr_eq(found, destination);
	inode_active_put(found);

	destination->i_layout_barrier.active = false;
	ck_assert_int_eq(vfs_remove(test_root, "rename-source", &test_ap, NULL,
				    NULL),
			 0);
	ck_assert_int_eq(vfs_remove(test_root, "rename-destination", &test_ap,
				    NULL, NULL),
			 0);
	inode_active_put(destination);
	inode_active_put(source);
}
END_TEST

static Suite *namespace_layout_guard_suite(void)
{
	Suite *suite = suite_create("namespace layout guard");
	TCase *test = tcase_create("identity");

	tcase_add_checked_fixture(test, setup, teardown);
	tcase_add_test(test, test_remove_rejects_rebound_name);
	tcase_add_test(test, test_rename_rejects_assigned_destination);
	suite_add_tcase(suite, test);
	return suite;
}

int main(void)
{
	return nfs4_test_run(namespace_layout_guard_suite());
}
