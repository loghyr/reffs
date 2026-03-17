/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include "reffs/log.h"
#include "reffs/server_persist.h"

int server_persist_load(const char *path, struct server_persistent_state *sps)
{
	int fd;
	ssize_t n;

	if (!path)
		return -ENOENT;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT)
			return -ENOENT;
		LOG("server_persist_load: open(%s): %m", path);
		return -errno;
	}

	n = read(fd, sps, sizeof(*sps));
	close(fd);

	if (n < 0) {
		LOG("server_persist_load: read(%s): %m", path);
		return -errno;
	}
	if ((size_t)n != sizeof(*sps)) {
		LOG("server_persist_load: short read (%zd/%zu) from %s", n,
		    sizeof(*sps), path);
		return -EINVAL;
	}

	if (sps->sps_magic != REFFS_SERVER_STATE_MAGIC) {
		LOG("server_persist_load: bad magic 0x%08x in %s",
		    sps->sps_magic, path);
		return -EINVAL;
	}
	if (sps->sps_version != REFFS_SERVER_STATE_VERSION) {
		LOG("server_persist_load: unknown version %u in %s",
		    sps->sps_version, path);
		return -EINVAL;
	}

	return 0;
}

int server_persist_save(const char *path,
			const struct server_persistent_state *sps)
{
	char tmp[PATH_MAX];
	int fd;
	ssize_t n;
	int ret = 0;

	if (!path)
		return -ENOENT;

	/*
         * Write to a temp file alongside the target, then rename into
         * place.  This avoids leaving a half-written record if we crash
         * mid-write — rename is atomic on POSIX for same-filesystem ops.
         */
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
		LOG("server_persist_save: path too long: %s", path);
		return -ENAMETOOLONG;
	}

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		LOG("server_persist_save: open(%s): %m", tmp);
		return -errno;
	}

	n = write(fd, sps, sizeof(*sps));
	if (n < 0) {
		LOG("server_persist_save: write(%s): %m", tmp);
		ret = -errno;
		goto err_close;
	}
	if ((size_t)n != sizeof(*sps)) {
		LOG("server_persist_save: short write (%zd/%zu) to %s", n,
		    sizeof(*sps), tmp);
		ret = -EIO;
		goto err_close;
	}

	if (fdatasync(fd)) {
		LOG("server_persist_save: fdatasync(%s): %m", tmp);
		ret = -errno;
		goto err_close;
	}

	close(fd);
	fd = -1;

	if (rename(tmp, path)) {
		LOG("server_persist_save: rename(%s, %s): %m", tmp, path);
		ret = -errno;
		goto err_unlink;
	}

	return 0;

err_close:
	close(fd);
err_unlink:
	unlink(tmp);
	return ret;
}
