/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <netinet/in.h>

#include "reffs/log.h"
#include "reffs/posix_shims.h"
#include "reffs/client_persist.h"

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */

#define CLIENTS_FILE "clients"
#define INCARNATIONS_LINK "client_incarnations"
#define INCARNATIONS_A "client_incarnations.A"
#define INCARNATIONS_B "client_incarnations.B"

static int make_path(char *buf, size_t bufsz, const char *dir, const char *file)
{
	if (snprintf(buf, bufsz, "%s/%s", dir, file) >= (int)bufsz) {
		LOG("make_path: path too long: %s/%s", dir, file);
		return -ENAMETOOLONG;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* clients file -- append-only identity log                            */

int client_identity_append(const char *state_dir,
			   const struct client_identity_record *cir)
{
	char path[PATH_MAX];
	int fd;
	ssize_t n;
	int ret = 0;

	if (make_path(path, sizeof(path), state_dir, CLIENTS_FILE))
		return -ENAMETOOLONG;

	fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0) {
		LOG("client_identity_append: open(%s): %m", path);
		return -errno;
	}

	n = write(fd, cir, sizeof(*cir));
	if (n < 0) {
		LOG("client_identity_append: write(%s): %m", path);
		ret = -errno;
		goto out;
	}
	if ((size_t)n != sizeof(*cir)) {
		LOG("client_identity_append: short write (%zd/%zu) to %s", n,
		    sizeof(*cir), path);
		ret = -EIO;
		goto out;
	}

	if (reffs_fdatasync(fd)) {
		LOG("client_identity_append: fdatasync(%s): %m", path);
		ret = -errno;
	}

out:
	close(fd);
	return ret;
}

int client_identity_load(const char *state_dir,
			 int (*cb)(const struct client_identity_record *cir,
				   void *arg),
			 void *arg)
{
	char path[PATH_MAX];
	struct client_identity_record cir;
	int fd;
	ssize_t n;
	int ret = 0;

	if (make_path(path, sizeof(path), state_dir, CLIENTS_FILE))
		return -ENAMETOOLONG;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT)
			return -ENOENT;
		LOG("client_identity_load: open(%s): %m", path);
		return -errno;
	}

	for (;;) {
		n = read(fd, &cir, sizeof(cir));
		if (n == 0)
			break; /* EOF */
		if (n < 0) {
			LOG("client_identity_load: read(%s): %m", path);
			ret = -errno;
			break;
		}
		if ((size_t)n != sizeof(cir)) {
			LOG("client_identity_load: short record (%zd/%zu) "
			    "in %s -- truncated?",
			    n, sizeof(cir), path);
			ret = -EINVAL;
			break;
		}
		if (cir.cir_magic != CLIENT_IDENTITY_MAGIC) {
			LOG("client_identity_load: bad magic 0x%08x in %s",
			    cir.cir_magic, path);
			ret = -EINVAL;
			break;
		}

		ret = cb(&cir, arg);
		if (ret)
			break;
	}

	close(fd);
	return ret;
}

/* ------------------------------------------------------------------ */
/* client_incarnations -- symlink-swapped active set                   */

/*
 * Determine which of .A or .B the symlink currently points to, so we
 * know which one to write next.  Returns 'A' or 'B', or 0 on error.
 */
static char incarnations_current_side(const char *state_dir)
{
	char link[PATH_MAX];
	char target[PATH_MAX];
	ssize_t len;

	if (make_path(link, sizeof(link), state_dir, INCARNATIONS_LINK))
		return 0;

	len = readlink(link, target, sizeof(target) - 1);
	if (len < 0) {
		if (errno == ENOENT)
			return 'A'; /* no symlink yet -- start with A */
		LOG("incarnations_current_side: readlink(%s): %m", link);
		return 0;
	}
	target[len] = '\0';

	if (strstr(target, ".A"))
		return 'A';
	if (strstr(target, ".B"))
		return 'B';

	LOG("incarnations_current_side: unrecognised target %s", target);
	return 0;
}

/*
 * Write recs[0..count) to the inactive side and symlink-swap it in.
 * fdatasyncs the new file before touching the symlink.
 */
static int
incarnations_write_and_swap(const char *state_dir,
			    const struct client_incarnation_record *recs,
			    size_t count)
{
	char current_path[PATH_MAX];
	char new_path[PATH_MAX];
	char link_path[PATH_MAX];
	char side;
	int fd;
	ssize_t n;
	size_t i;
	int ret = 0;

	side = incarnations_current_side(state_dir);
	if (!side)
		return -EIO;

	/* Write to the inactive side. */
	if (side == 'A') {
		if (make_path(new_path, sizeof(new_path), state_dir,
			      INCARNATIONS_B))
			return -ENAMETOOLONG;
		if (make_path(current_path, sizeof(current_path), state_dir,
			      INCARNATIONS_A))
			return -ENAMETOOLONG;
	} else {
		if (make_path(new_path, sizeof(new_path), state_dir,
			      INCARNATIONS_A))
			return -ENAMETOOLONG;
		if (make_path(current_path, sizeof(current_path), state_dir,
			      INCARNATIONS_B))
			return -ENAMETOOLONG;
	}

	if (make_path(link_path, sizeof(link_path), state_dir,
		      INCARNATIONS_LINK))
		return -ENAMETOOLONG;

	fd = open(new_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		LOG("incarnations_write_and_swap: open(%s): %m", new_path);
		return -errno;
	}

	for (i = 0; i < count; i++) {
		n = write(fd, &recs[i], sizeof(recs[i]));
		if (n < 0) {
			LOG("incarnations_write_and_swap: write(%s): %m",
			    new_path);
			ret = -errno;
			goto err_close;
		}
		if ((size_t)n != sizeof(recs[i])) {
			LOG("incarnations_write_and_swap: short write "
			    "(%zd/%zu) to %s",
			    n, sizeof(recs[i]), new_path);
			ret = -EIO;
			goto err_close;
		}
	}

	if (reffs_fdatasync(fd)) {
		LOG("incarnations_write_and_swap: fdatasync(%s): %m", new_path);
		ret = -errno;
		goto err_close;
	}

	close(fd);
	fd = -1;

	/*
         * Atomic symlink swap:
         *   symlink new_path -> link_path.tmp
         *   rename link_path.tmp -> link_path
         * rename(2) of a symlink is atomic on POSIX.
         */
	char tmp_link[PATH_MAX];
	if (snprintf(tmp_link, sizeof(tmp_link), "%s.tmp", link_path) >=
	    (int)sizeof(tmp_link)) {
		ret = -ENAMETOOLONG;
		goto err_unlink_new;
	}

	/* new_path is the full path; symlink target should be basename */
	const char *target = (side == 'A') ? INCARNATIONS_B : INCARNATIONS_A;

	if (symlink(target, tmp_link)) {
		LOG("incarnations_write_and_swap: symlink(%s, %s): %m", target,
		    tmp_link);
		ret = -errno;
		goto err_unlink_new;
	}

	if (rename(tmp_link, link_path)) {
		LOG("incarnations_write_and_swap: rename(%s, %s): %m", tmp_link,
		    link_path);
		ret = -errno;
		unlink(tmp_link);
		goto err_unlink_new;
	}

	/* Unlink the old side -- best effort. */
	unlink(current_path);
	return 0;

err_close:
	if (fd >= 0)
		close(fd);
err_unlink_new:
	unlink(new_path);
	return ret;
}

int client_incarnation_load(const char *state_dir,
			    struct client_incarnation_record *recs,
			    size_t max_recs, size_t *count)
{
	char link_path[PATH_MAX];
	char target_path[PATH_MAX];
	char target[PATH_MAX];
	struct client_incarnation_record rec;
	ssize_t len;
	int fd;
	ssize_t n;
	int ret = 0;

	*count = 0;

	if (make_path(link_path, sizeof(link_path), state_dir,
		      INCARNATIONS_LINK))
		return -ENAMETOOLONG;

	/* Resolve the symlink to find the active file. */
	len = readlink(link_path, target, sizeof(target) - 1);
	if (len < 0) {
		if (errno == ENOENT)
			return -ENOENT;
		LOG("client_incarnation_load: readlink(%s): %m", link_path);
		return -errno;
	}
	target[len] = '\0';

	if (make_path(target_path, sizeof(target_path), state_dir, target))
		return -ENAMETOOLONG;

	fd = open(target_path, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT)
			return -ENOENT;
		LOG("client_incarnation_load: open(%s): %m", target_path);
		return -errno;
	}

	for (;;) {
		if (*count >= max_recs) {
			LOG("client_incarnation_load: too many records "
			    "(max %zu)",
			    max_recs);
			ret = -ENOSPC;
			break;
		}

		n = read(fd, &rec, sizeof(rec));
		if (n == 0)
			break;
		if (n < 0) {
			LOG("client_incarnation_load: read(%s): %m",
			    target_path);
			ret = -errno;
			break;
		}
		if ((size_t)n != sizeof(rec)) {
			LOG("client_incarnation_load: short record "
			    "(%zd/%zu) in %s",
			    n, sizeof(rec), target_path);
			ret = -EINVAL;
			break;
		}
		if (rec.crc_magic != CLIENT_INCARNATION_MAGIC) {
			LOG("client_incarnation_load: bad magic 0x%08x",
			    rec.crc_magic);
			ret = -EINVAL;
			break;
		}

		recs[(*count)++] = rec;
	}

	close(fd);
	return ret;
}

int client_incarnation_add(const char *state_dir,
			   const struct client_incarnation_record *crc)
{
	/*
         * NOT_NOW_BROWN_COW: for now load all, append, rewrite.
         * Replace with a smarter in-memory list once the protocol
         * layer is stable.
         */
	struct client_incarnation_record *recs;
	size_t count = 0;
	size_t max_recs = 65536; /* more than enough for a prototype */
	int ret;

	recs = calloc(max_recs, sizeof(*recs));
	if (!recs)
		return -ENOMEM;

	ret = client_incarnation_load(state_dir, recs, max_recs, &count);
	if (ret && ret != -ENOENT)
		goto out;

	recs[count++] = *crc;
	ret = incarnations_write_and_swap(state_dir, recs, count);

out:
	free(recs);
	return ret;
}

int client_incarnation_remove(const char *state_dir, uint32_t slot)
{
	struct client_incarnation_record *recs;
	size_t count = 0;
	size_t max_recs = 65536;
	size_t i, new_count;
	bool found = false;
	int ret;

	recs = calloc(max_recs, sizeof(*recs));
	if (!recs)
		return -ENOMEM;

	ret = client_incarnation_load(state_dir, recs, max_recs, &count);
	if (ret == -ENOENT) {
		ret = -ENOENT; /* slot not present in empty file */
		goto out;
	}
	if (ret)
		goto out;

	/* Compact in-place, removing the matching slot. */
	new_count = 0;
	for (i = 0; i < count; i++) {
		if (recs[i].crc_slot == slot) {
			found = true;
			continue;
		}
		recs[new_count++] = recs[i];
	}

	if (!found) {
		ret = -ENOENT;
		goto out;
	}

	ret = incarnations_write_and_swap(state_dir, recs, new_count);

out:
	free(recs);
	return ret;
}
