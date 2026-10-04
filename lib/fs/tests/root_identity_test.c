/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <check.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "reffs/dirent.h"
#include "reffs/context.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/ns.h"
#include "reffs/root_identity.h"
#include "reffs/super_block.h"

struct fixture {
	char backend[64];
	char state[64];
};

static void fixture_init(struct fixture *f)
{
	strcpy(f->backend, "/tmp/reffs-root-id-backend-XXXXXX");
	strcpy(f->state, "/tmp/reffs-root-id-state-XXXXXX");
	ck_assert_ptr_nonnull(mkdtemp(f->backend));
	ck_assert_ptr_nonnull(mkdtemp(f->state));
}

static void fixture_fini(const struct fixture *f)
{
	char command[180];

	snprintf(command, sizeof(command), "rm -rf -- %s %s", f->backend,
		 f->state);
	ck_assert_int_eq(system(command), 0);
}

static void path(char *out, size_t capacity, const struct fixture *f,
		 const char *name)
{
	ck_assert_int_lt(snprintf(out, capacity, "%s/sb_1/%s", f->backend,
				  name),
			 (int)capacity);
}

static void replace_byte(const char *name, off_t offset, unsigned char value)
{
	int fd = open(name, O_WRONLY);

	ck_assert_int_ge(fd, 0);
	ck_assert_int_eq(pwrite(fd, &value, 1, offset), 1);
	ck_assert_int_eq(close(fd), 0);
}

START_TEST(root_identity_two_starts)
{
	struct fixture f;
	uuid_t first, second, other;
	struct fixture f2;
	struct super_block *sb;
	struct reffs_context ctx = { .uid = getuid(), .gid = getgid() };
	struct inode *owner;

	fixture_init(&f);
	reffs_set_context(&ctx);
	reffs_fs_set_storage(REFFS_STORAGE_POSIX, f.backend);
	ck_assert_int_eq(reffs_ns_init_with_state(f.state), 0);
	sb = super_block_find(1);
	ck_assert_ptr_nonnull(sb);
	uuid_copy(first, sb->sb_uuid);
	ck_assert(!uuid_is_null(first));
	ck_assert_int_eq(reffs_fs_create("/owned", S_IFREG | 0644), 0);
	super_block_put(sb);
	ck_assert_int_eq(reffs_ns_fini(), 0);

	/* This creates a genuinely new superblock rather than re-reading the
	 * record while the original superblock remains live. */
	ck_assert_int_eq(reffs_ns_init_with_state(f.state), 0);
	sb = super_block_find(1);
	ck_assert_ptr_nonnull(sb);
	uuid_copy(second, sb->sb_uuid);
	ck_assert_int_eq(uuid_compare(first, second), 0);
	reffs_fs_recover(sb);
	owner = inode_find(sb, 2);
	ck_assert_ptr_nonnull(owner);
	ck_assert(S_ISREG(owner->i_mode));
	inode_put(owner);
	super_block_put(sb);
	ck_assert_int_eq(reffs_ns_fini(), 0);

	fixture_init(&f2);
	ck_assert_int_eq(reffs_root_identity_load_or_create(f2.backend,
							    f2.state, other),
			 0);
	ck_assert_int_ne(uuid_compare(first, other), 0);
	fixture_fini(&f2);
	fixture_fini(&f);
	reffs_set_context(NULL);
}
END_TEST

START_TEST(root_identity_rejects_damage)
{
	struct fixture f;
	uuid_t identity, ignored;
	char name[128];
	int fd;

	fixture_init(&f);
	ck_assert_int_eq(reffs_root_identity_load_or_create(f.backend, f.state,
							    identity),
			 0);
	path(name, sizeof(name), &f, "root-identity");
	replace_byte(name, 24, 0);
	ck_assert_int_eq(reffs_root_identity_load_or_create(f.backend, f.state,
							    ignored),
			 -EBADMSG);
	fd = open(name, O_WRONLY);
	ck_assert_int_ge(fd, 0);
	ck_assert_int_eq(ftruncate(fd, 8), 0);
	ck_assert_int_eq(close(fd), 0);
	ck_assert_int_eq(reffs_root_identity_load_or_create(f.backend, f.state,
							    ignored),
			 -EBADMSG);
	fixture_fini(&f);
}
END_TEST

START_TEST(root_identity_rejects_partial_creation)
{
	struct fixture f;
	uuid_t identity;
	char name[128];

	fixture_init(&f);
	ck_assert_int_eq(reffs_root_identity_load_or_create(f.backend, f.state,
							    identity),
			 0);
	path(name, sizeof(name), &f, "root-identity");
	ck_assert_int_eq(unlink(name), 0);
	ck_assert_int_eq(reffs_root_identity_load_or_create(f.backend, f.state,
							    identity),
			 -EBADMSG);
	fixture_fini(&f);
}
END_TEST

START_TEST(root_identity_rejects_unsupported_and_conflict)
{
	struct fixture a, b;
	uuid_t first, second;
	char aroot[128], broot[128];
	unsigned char record[44];
	int fd;

	fixture_init(&a);
	fixture_init(&b);
	ck_assert_int_eq(reffs_root_identity_load_or_create(a.backend, a.state,
							    first),
			 0);
	ck_assert_int_eq(reffs_root_identity_load_or_create(b.backend, b.state,
							    second),
			 0);
	path(aroot, sizeof(aroot), &a, "root-identity");
	path(broot, sizeof(broot), &b, "root-identity");
	fd = open(aroot, O_RDONLY);
	ck_assert_int_ge(fd, 0);
	ck_assert_int_eq(read(fd, record, sizeof(record)), sizeof(record));
	close(fd);
	/* A version mutation is rejected before any new identity is minted. */
	replace_byte(aroot, 7, 2);
	ck_assert_int_eq(reffs_root_identity_load_or_create(a.backend, a.state,
							    first),
			 -EBADMSG);
	fd = open(broot, O_WRONLY | O_TRUNC);
	ck_assert_int_ge(fd, 0);
	ck_assert_int_eq(write(fd, record, sizeof(record)), sizeof(record));
	close(fd);
	ck_assert_int_eq(reffs_root_identity_load_or_create(b.backend, b.state,
							    second),
			 -EXDEV);
	fixture_fini(&a);
	fixture_fini(&b);
}
END_TEST

START_TEST(root_identity_rejects_legacy_and_inventory)
{
	struct fixture f;
	uuid_t ignored;
	char name[128];
	int fd;

	fixture_init(&f);
	ck_assert_int_eq(snprintf(name, sizeof(name), "%s/sb_1", f.backend) > 0,
			 1);
	ck_assert_int_eq(mkdir(name, 0700), 0);
	path(name, sizeof(name), &f, "ino_2.meta");
	fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
	ck_assert_int_ge(fd, 0);
	close(fd);
	ck_assert_int_eq(reffs_root_identity_load_or_create(f.backend, f.state,
							    ignored),
			 -ESTALE);
	fixture_fini(&f);

	fixture_init(&f);
	ck_assert_int_lt(snprintf(name, sizeof(name),
				  "%s/ffv2-fixed-inventory-1", f.state),
			 (int)sizeof(name));
	fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
	ck_assert_int_ge(fd, 0);
	ck_assert_int_eq(write(fd, "legacy", 6), 6);
	close(fd);
	ck_assert_int_eq(reffs_root_identity_load_or_create(f.backend, f.state,
							    ignored),
			 -ESTALE);
	fd = open(name, O_RDONLY);
	ck_assert_int_ge(fd, 0);
	char bytes[6];
	ck_assert_int_eq(read(fd, bytes, sizeof(bytes)), sizeof(bytes));
	ck_assert_int_eq(memcmp(bytes, "legacy", 6), 0);
	close(fd);
	fixture_fini(&f);
}
END_TEST

START_TEST(root_identity_creation_failure_is_closed)
{
	struct fixture f;
	uuid_t ignored;
	char name[128];

	fixture_init(&f);
	ck_assert_int_lt(snprintf(name, sizeof(name), "%s/sb_1", f.backend),
			 (int)sizeof(name));
	ck_assert_int_eq(mkdir(name, 0700), 0);
	ck_assert_int_eq(chmod(name, 0500), 0);
	ck_assert_int_eq(reffs_root_identity_load_or_create(f.backend, f.state,
							    ignored),
			 -EACCES);
	ck_assert_int_eq(chmod(name, 0700), 0);
	fixture_fini(&f);
}
END_TEST

START_TEST(root_identity_creation_race)
{
	struct fixture f;
	uuid_t parent_id, child_id;
	int pipes[2], status;
	pid_t child;

	fixture_init(&f);
	ck_assert_int_eq(pipe(pipes), 0);
	child = fork();
	ck_assert_int_ge(child, 0);
	if (child == 0) {
		close(pipes[0]);
		int ret = reffs_root_identity_load_or_create(f.backend, f.state,
							     child_id);
		if (ret || write(pipes[1], child_id, sizeof(child_id)) !=
				   sizeof(child_id))
			_exit(1);
		_exit(0);
	}
	close(pipes[1]);
	ck_assert_int_eq(reffs_root_identity_load_or_create(f.backend, f.state,
							    parent_id),
			 0);
	ck_assert_int_eq(read(pipes[0], child_id, sizeof(child_id)),
			 sizeof(child_id));
	close(pipes[0]);
	ck_assert_int_eq(waitpid(child, &status, 0), child);
	ck_assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	ck_assert_int_eq(uuid_compare(parent_id, child_id), 0);
	fixture_fini(&f);
}
END_TEST

static Suite *root_identity_suite(void)
{
	Suite *suite = suite_create("POSIX root identity");
	TCase *core = tcase_create("record and restart");

	tcase_add_test(core, root_identity_two_starts);
	tcase_add_test(core, root_identity_rejects_damage);
	tcase_add_test(core, root_identity_rejects_partial_creation);
	tcase_add_test(core, root_identity_rejects_unsupported_and_conflict);
	tcase_add_test(core, root_identity_rejects_legacy_and_inventory);
	tcase_add_test(core, root_identity_creation_failure_is_closed);
	tcase_add_test(core, root_identity_creation_race);
	suite_add_tcase(suite, core);
	return suite;
}

int main(void)
{
	SRunner *runner = srunner_create(root_identity_suite());
	int failed;

	srunner_run_all(runner, CK_NORMAL);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
