/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "reffs/log.h"
#include "reffs/root_identity.h"

#define ROOT_DIR "sb_1"
#define BACKEND_ID "root-backend-id"
#define ROOT_ID "root-identity"
#define BACKEND_MAGIC 0x52424944U /* RBID */
#define ROOT_MAGIC 0x52524944U /* RRID */
#define ID_VERSION 1U
#define ID_LEN 44U

static uint32_t record_crc(const unsigned char *data, size_t len)
{
	uint32_t crc = ~0U;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (unsigned int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1U));
	}
	return ~crc;
}

static void put_u32(unsigned char *dst, uint32_t value)
{
	value = htonl(value);
	memcpy(dst, &value, sizeof(value));
}

static uint32_t get_u32(const unsigned char *src)
{
	uint32_t value;

	memcpy(&value, src, sizeof(value));
	return ntohl(value);
}

static void encode(unsigned char *data, size_t len, uint32_t magic,
		   const uuid_t backend_uuid, const uuid_t root_uuid)
{
	put_u32(data, magic);
	put_u32(data + 4, ID_VERSION);
	memcpy(data + 8, backend_uuid, sizeof(uuid_t));
	memcpy(data + 24, root_uuid, sizeof(uuid_t));
	put_u32(data + len - 4, record_crc(data, len - 4));
}

static int read_record(int dirfd, const char *name, unsigned char *data,
		       size_t len, uint32_t magic)
{
	struct stat st;
	unsigned char extra;
	ssize_t n;
	int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
	    st.st_size != (off_t)len) {
		close(fd);
		return -EBADMSG;
	}
	n = read(fd, data, len);
	if (n != (ssize_t)len || read(fd, &extra, 1) != 0) {
		close(fd);
		return -EBADMSG;
	}
	close(fd);
	if (get_u32(data) != magic || get_u32(data + 4) != ID_VERSION ||
	    get_u32(data + len - 4) != record_crc(data, len - 4))
		return -EBADMSG;
	if (uuid_is_null(data + 8) || uuid_is_null(data + 24))
		return -EBADMSG;
	return 0;
}

/* Publish a fully synchronized immutable file. A hard link supplies
 * no-replace atomicity on systems without renameat2(RENAME_NOREPLACE). */
static int publish_record(int dirfd, const char *name,
			  const unsigned char *data, size_t len)
{
	char temporary[80];
	size_t offset = 0;
	int fd, ret = 0;

	if (snprintf(temporary, sizeof(temporary), ".%s.tmp.%ld", name,
		     (long)getpid()) >= (int)sizeof(temporary))
		return -ENAMETOOLONG;
	fd = openat(dirfd, temporary,
		    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -errno;
	while (offset < len) {
		ssize_t n = write(fd, data + offset, len - offset);

		if (n <= 0) {
			ret = n < 0 ? -errno : -EIO;
			goto out;
		}
		offset += (size_t)n;
	}
	if (fsync(fd)) {
		ret = -errno;
		goto out;
	}
	if (linkat(dirfd, temporary, dirfd, name, 0)) {
		ret = -errno;
		goto out;
	}
out:
	if (close(fd) && !ret)
		ret = -errno;
	if (unlinkat(dirfd, temporary, 0) && !ret)
		ret = -errno;
	if (!ret && fsync(dirfd))
		ret = -errno;
	return ret;
}

static int directory_nonempty(int dirfd, bool *nonempty)
{
	DIR *dir;
	struct dirent *entry;
	int copy = dup(dirfd);

	if (copy < 0)
		return -errno;
	dir = fdopendir(copy);
	if (!dir) {
		close(copy);
		return -errno;
	}
	*nonempty = false;
	errno = 0;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
			*nonempty = true;
			break;
		}
	}
	int ret = !entry && errno ? -errno : 0;

	closedir(dir);
	return ret;
}

static int inventory_exists(const char *state_dir, bool *exists)
{
	DIR *dir;
	struct dirent *entry;

	*exists = false;
	if (!state_dir || !*state_dir)
		return 0;
	dir = opendir(state_dir);
	if (!dir)
		return errno == ENOENT ? 0 : -errno;
	errno = 0;
	while ((entry = readdir(dir)) != NULL) {
		if (!strncmp(entry->d_name, "ffv2-fixed-inventory-", 21) ||
		    !strncmp(entry->d_name, "ffv2-fixed-status-", 18) ||
		    !strcmp(entry->d_name, "superblocks.registry")) {
			*exists = true;
			break;
		}
	}
	int ret = !entry && errno ? -errno : 0;

	closedir(dir);
	return ret;
}

int reffs_root_identity_load_or_create(const char *backend_path,
				       const char *state_dir, uuid_t root_uuid)
{
	unsigned char backend[ID_LEN], root[ID_LEN];
	uuid_t backend_uuid, new_root;
	bool occupied, inventory;
	int parent = -1, dirfd = -1, ret;

	if (!backend_path || !*backend_path)
		return -EINVAL;
	parent = open(backend_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent < 0)
		return -errno;
	ret = inventory_exists(state_dir, &inventory);
	if (ret)
		goto out;
	if (mkdirat(parent, ROOT_DIR, 0755) && errno != EEXIST) {
		ret = -errno;
		goto out;
	}
	/* Persist the directory itself before storing identities inside it. */
	if (fsync(parent)) {
		ret = -errno;
		goto out;
	}
	dirfd = openat(parent, ROOT_DIR,
		       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (dirfd < 0) {
		ret = -errno;
		goto out;
	}
	if (flock(dirfd, LOCK_EX)) {
		ret = -errno;
		goto out;
	}
	ret = read_record(dirfd, ROOT_ID, root, sizeof(root), ROOT_MAGIC);
	if (ret == 0) {
		ret = read_record(dirfd, BACKEND_ID, backend, sizeof(backend),
				  BACKEND_MAGIC);
		if (ret == -ENOENT)
			ret = -EBADMSG;
		if (!ret && memcmp(root + 8, backend + 8, 2 * sizeof(uuid_t)))
			ret = -EXDEV;
		if (!ret)
			memcpy(root_uuid, root + 24, sizeof(uuid_t));
		goto out;
	}
	if (ret != -ENOENT)
		goto out;
	/* An interrupted two-record creation needs operator inspection; it
	 * cannot be mistaken for a pre-identity legacy backend. */
	struct stat marker;

	if (fstatat(dirfd, BACKEND_ID, &marker, AT_SYMLINK_NOFOLLOW) == 0) {
		ret = -EBADMSG;
		goto out;
	}
	if (errno != ENOENT) {
		ret = -errno;
		goto out;
	}
	ret = directory_nonempty(dirfd, &occupied);
	if (ret)
		goto out;
	if (occupied || inventory) {
		LOG("POSIX root identity missing on nonempty backend or fixed inventory: operator migration required");
		ret = -ESTALE;
		goto out;
	}
	uuid_generate(backend_uuid);
	uuid_generate(new_root);
	encode(backend, sizeof(backend), BACKEND_MAGIC, backend_uuid, new_root);
	ret = publish_record(dirfd, BACKEND_ID, backend, sizeof(backend));
	if (ret)
		goto out;
	encode(root, sizeof(root), ROOT_MAGIC, backend_uuid, new_root);
	ret = publish_record(dirfd, ROOT_ID, root, sizeof(root));
	if (!ret)
		uuid_copy(root_uuid, new_root);
out:
	if (ret && ret != -ESTALE)
		LOG("POSIX root identity refusal: %d", ret);
	if (!ret) {
		char uuid_text[37];

		uuid_unparse(root_uuid, uuid_text);
		LOG("Native POSIX root identity: %s", uuid_text);
	}
	if (dirfd >= 0)
		close(dirfd);
	if (parent >= 0)
		close(parent);
	return ret;
}
