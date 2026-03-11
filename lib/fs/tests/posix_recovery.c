/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include "posix_recovery.h"
#include "reffs/log.h"
#include "reffs/fs.h"

int test_setup(struct test_context *ctx)
{
	char template[] = "/tmp/reffs_test_XXXXXX";
	if (!mkdtemp(template)) {
		return errno;
	}
	strncpy(ctx->backend_path, template, sizeof(ctx->backend_path));

	/* Create superblock directory */
	char sb_path[PATH_MAX];
	snprintf(sb_path, sizeof(sb_path), "%s/sb_1", ctx->backend_path);
	mkdir(sb_path, 0755);

	/* Initialize superblock */
	ctx->sb = super_block_alloc(SUPER_BLOCK_ROOT_ID, "/",
				    REFFS_STORAGE_POSIX, ctx->backend_path);
	if (!ctx->sb)
		return ENOMEM;

	super_block_dirent_create(ctx->sb, NULL, reffs_life_action_birth);
	return 0;
}

void test_teardown(struct test_context *ctx)
{
	if (ctx->sb) {
		super_block_release_dirents(ctx->sb);
		super_block_put(ctx->sb);
		ctx->sb = NULL;
	}

	/* Recursive delete of temp dir */
	char cmd[PATH_MAX];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", ctx->backend_path);
	if (system(cmd) < 0) {
		LOG("Failed to cleanup temp dir %s", ctx->backend_path);
	}
}

int test_write_meta(struct test_context *ctx, uint64_t ino,
		    struct inode_disk *id)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/sb_1/ino_%lu.meta", ctx->backend_path,
		 ino);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return errno;

	struct {
		struct reffs_disk_header hdr;
		struct inode_disk id;
	} meta;

	meta.hdr.rdh_magic = REFFS_DISK_MAGIC_META;
	meta.hdr.rdh_version = REFFS_DISK_VERSION_1;
	memcpy(&meta.id, id, sizeof(*id));

	if (write(fd, &meta, sizeof(meta)) != sizeof(meta)) {
		close(fd);
		return EIO;
	}
	close(fd);
	return 0;
}

int test_write_dat(struct test_context *ctx, uint64_t ino, const void *data,
		   size_t size)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/sb_1/ino_%lu.dat", ctx->backend_path,
		 ino);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return errno;
	if (size > 0 && write(fd, data, size) != (ssize_t)size) {
		close(fd);
		return EIO;
	}
	close(fd);
	return 0;
}

int test_write_lnk(struct test_context *ctx, uint64_t ino, const char *target)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/sb_1/ino_%lu.lnk", ctx->backend_path,
		 ino);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return errno;
	size_t len = strlen(target);
	if (write(fd, target, len) != (ssize_t)len) {
		close(fd);
		return EIO;
	}
	close(fd);
	return 0;
}

int test_write_dir_header(struct test_context *ctx, uint64_t ino,
			  uint64_t cookie_next, int *fd_out)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/sb_1/ino_%lu.dir", ctx->backend_path,
		 ino);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return errno;

	struct reffs_disk_header hdr = {
		.rdh_magic = REFFS_DISK_MAGIC_DIR,
		.rdh_version = REFFS_DISK_VERSION_1,
	};
	if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		close(fd);
		return EIO;
	}

	if (write(fd, &cookie_next, sizeof(cookie_next)) !=
	    sizeof(cookie_next)) {
		close(fd);
		return EIO;
	}
	*fd_out = fd;
	return 0;
}

int test_write_dir_entry(int fd, uint64_t cookie, uint64_t ino,
			 const char *name)
{
	uint16_t name_len = strlen(name);
	if (write(fd, &cookie, sizeof(cookie)) != sizeof(cookie))
		return EIO;
	if (write(fd, &ino, sizeof(ino)) != sizeof(ino))
		return EIO;
	if (write(fd, &name_len, sizeof(name_len)) != sizeof(name_len))
		return EIO;
	if (write(fd, name, name_len) != (ssize_t)name_len)
		return EIO;
	return 0;
}
