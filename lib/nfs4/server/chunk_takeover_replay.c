/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "nfs4/chunk_takeover_replay.h"
#include "reffs/posix_shims.h"

#define CHUNK_TAKEOVER_REPLAY_MAGIC 0x43545250U /* "CTRP" */
#define CHUNK_TAKEOVER_REPLAY_VERSION 1

struct replay_entry {
	uint32_t profile;
	uint32_t reserved;
	uint64_t expires_at;
	char principal[REFFS_CONFIG_MAX_PRINCIPAL];
	uint8_t token_id[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN];
};

struct replay_disk_header {
	uint32_t magic;
	uint32_t version;
	uint32_t count;
	uint32_t reserved;
};

static int replay_paths(char *path, size_t path_len, char *lock_path,
			size_t lock_len, const char *state_dir)
{
	int n;

	if (!state_dir)
		return -EINVAL;
	n = snprintf(path, path_len, "%s/chunk_takeover_replay", state_dir);
	if (n < 0 || (size_t)n >= path_len)
		return -ENAMETOOLONG;
	n = snprintf(lock_path, lock_len, "%s.lock", path);
	if (n < 0 || (size_t)n >= lock_len)
		return -ENAMETOOLONG;
	return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = read(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int replay_load(const char *path, struct replay_entry **entries,
		       uint32_t *count)
{
	struct replay_disk_header header;
	struct replay_entry *loaded = NULL;
	int fd, ret;

	*entries = NULL;
	*count = 0;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return errno == ENOENT ? 0 : -errno;
	ret = read_full(fd, &header, sizeof(header));
	if (ret || header.magic != CHUNK_TAKEOVER_REPLAY_MAGIC ||
	    header.version != CHUNK_TAKEOVER_REPLAY_VERSION ||
	    header.count > CHUNK_TAKEOVER_REPLAY_MAX) {
		close(fd);
		return ret ? ret : -EPROTO;
	}
	if (header.count) {
		loaded = calloc(header.count, sizeof(*loaded));
		if (!loaded) {
			close(fd);
			return -ENOMEM;
		}
		ret = read_full(fd, loaded,
				(size_t)header.count * sizeof(*loaded));
	}
	if (!ret) {
		uint8_t extra;

		if (read(fd, &extra, sizeof(extra)) != 0)
			ret = -EPROTO;
	}
	if (close(fd) && !ret)
		ret = -errno;
	if (!ret) {
		for (uint32_t i = 0; i < header.count; i++) {
			if (!loaded[i].profile ||
			    !memchr(loaded[i].principal, '\0',
				    sizeof(loaded[i].principal))) {
				ret = -EPROTO;
				break;
			}
		}
	}
	if (ret) {
		free(loaded);
		return ret;
	}
	*entries = loaded;
	*count = header.count;
	return 0;
}

static int replay_save(const char *path, const struct replay_entry *entries,
		       uint32_t count)
{
	char tmp[512];
	struct replay_disk_header header = {
		.magic = CHUNK_TAKEOVER_REPLAY_MAGIC,
		.version = CHUNK_TAKEOVER_REPLAY_VERSION,
		.count = count,
	};
	int fd, ret;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -errno;
	ret = write_full(fd, &header, sizeof(header));
	if (!ret && count)
		ret = write_full(fd, entries, (size_t)count * sizeof(*entries));
	if (!ret)
		ret = reffs_fdatasync(fd);
	if (close(fd) && !ret)
		ret = -errno;
	if (ret) {
		unlink(tmp);
		return ret;
	}
	if (rename(tmp, path)) {
		ret = -errno;
		unlink(tmp);
		return ret;
	}
	return 0;
}

int chunk_takeover_replay_claim(
	const char *state_dir, uint32_t profile, const char *principal,
	const uint8_t token_id[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN],
	uint64_t expires_at, uint64_t now_sec)
{
	char path[512], lock_path[520];
	struct replay_entry *entries = NULL;
	struct replay_entry *grown;
	uint32_t count = 0, out = 0;
	int lock_fd, ret;

	if (replay_paths(path, sizeof(path), lock_path, sizeof(lock_path),
			 state_dir) ||
	    !profile || !principal || !principal[0] ||
	    strlen(principal) >= REFFS_CONFIG_MAX_PRINCIPAL || !token_id ||
	    expires_at <= now_sec)
		return -EINVAL;
	lock_fd = open(lock_path, O_RDWR | O_CREAT, 0600);
	if (lock_fd < 0)
		return -errno;
	if (flock(lock_fd, LOCK_EX)) {
		ret = -errno;
		close(lock_fd);
		return ret;
	}
	ret = replay_load(path, &entries, &count);
	if (ret)
		goto out;
	for (uint32_t i = 0; i < count; i++) {
		if (entries[i].expires_at <= now_sec)
			continue;
		if (entries[i].profile == profile &&
		    strcmp(entries[i].principal, principal) == 0 &&
		    memcmp(entries[i].token_id, token_id,
			   CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN) == 0) {
			ret = -EALREADY;
			goto out;
		}
		entries[out++] = entries[i];
	}
	if (out == CHUNK_TAKEOVER_REPLAY_MAX) {
		ret = -ENOSPC;
		goto out;
	}
	grown = realloc(entries, (size_t)(out + 1) * sizeof(*entries));
	if (!grown) {
		ret = -ENOMEM;
		goto out;
	}
	entries = grown;
	entries[out].profile = profile;
	entries[out].reserved = 0;
	entries[out].expires_at = expires_at;
	strcpy(entries[out].principal, principal);
	memcpy(entries[out].token_id, token_id,
	       CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN);
	ret = replay_save(path, entries, out + 1);
out:
	free(entries);
	if (flock(lock_fd, LOCK_UN) && !ret)
		ret = -errno;
	if (close(lock_fd) && !ret)
		ret = -errno;
	return ret;
}
