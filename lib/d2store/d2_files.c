/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include "d1_codec.h"
#include "d2_files.h"

/* Keep building against libc headers that predate these stable UAPI calls. */
#ifndef FS_IOC_GETFSUUID
struct fsuuid2 {
	uint8_t len;
	uint8_t uuid[16];
};
#define FS_IOC_GETFSUUID _IOR(0x15, 0, struct fsuuid2)
#endif

#ifndef FS_IOC_GETFSSYSFSPATH
struct fs_sysfs_path {
	uint8_t len;
	uint8_t name[128];
};
#define FS_IOC_GETFSSYSFSPATH _IOR(0x15, 1, struct fs_sysfs_path)
#endif

struct d2_files {
	int dirfd;
	int super_fd;
	int wal_fd;
	int payload_fd;
	struct d2_superblock super;
	unsigned int newest_slot;
	uint64_t wal_cursor;
	uint64_t payload_cursor;
	uint64_t next_lsn;
	struct d2_scan_result last_scan;
	d2_io_hook_fn hook;
	void *hook_arg;
	bool fail_next_wal_write;
};

static uint32_t d2_errno_status(void)
{
	return errno == ENOSPC || errno == EDQUOT || errno == EFBIG ? 10u : 11u;
}

static void d2_hook(struct d2_files *f, enum d2_io_point point)
{
	if (f->hook)
		f->hook(point, f->hook_arg);
}

static bool d2_pwrite_all(int fd, const void *buf, size_t len, uint64_t off)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t done = pwrite(fd, p, len, (off_t)off);

		if (done < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (done == 0) {
			errno = EIO;
			return false;
		}
		p += done;
		len -= (size_t)done;
		off += (uint64_t)done;
	}
	return true;
}

static bool d2_pread_all(int fd, void *buf, size_t len, uint64_t off)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t done = pread(fd, p, len, (off_t)off);

		if (done < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (done == 0) {
			errno = EIO;
			return false;
		}
		p += done;
		len -= (size_t)done;
		off += (uint64_t)done;
	}
	return true;
}

static bool d2_uuid_generate(uint8_t out[16])
{
	size_t at = 0;

	while (at < 16) {
		ssize_t done = getrandom(out + at, 16 - at, 0);

		if (done < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		at += (size_t)done;
	}
	out[6] = (uint8_t)((out[6] & 0x0f) | 0x40);
	out[8] = (uint8_t)((out[8] & 0x3f) | 0x80);
	return true;
}

static void d2_token_digest(const uint8_t *token, uint32_t len, uint8_t out[32])
{
	struct d1_cursor c;
	uint8_t *bytes;

	bytes = malloc(4u + len);
	if (!bytes) {
		memset(out, 0, 32);
		return;
	}
	d1_enc_init(&c, bytes, 4u + len);
	d1_enc_u32(&c, len);
	d1_enc_raw(&c, token, len);
	d2_hash_domain("FFV2-D2-TOKEN-v1", bytes, c.len, out);
	free(bytes);
}

static bool d2_dir_empty(int dirfd)
{
	DIR *dir;
	struct dirent *de;
	int copy = dup(dirfd);
	bool empty = true;

	if (copy < 0)
		return false;
	dir = fdopendir(copy);
	if (!dir) {
		close(copy);
		return false;
	}
	errno = 0;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
			empty = false;
			break;
		}
	}
	if (errno)
		empty = false;
	closedir(dir);
	return empty;
}

static bool d2_ext4_qualify(int dirfd)
{
	struct fs_sysfs_path sysfs = { 0 };
	char path[256], options[8193];
	const char *name;
	ssize_t done;
	unsigned int i;
	int fd, printed;

	if (ioctl(dirfd, FS_IOC_GETFSSYSFSPATH, &sysfs) < 0 || sysfs.len == 0 ||
	    sysfs.len >= sizeof(sysfs.name))
		return false;
	sysfs.name[sysfs.len] = 0;
	name = strrchr((const char *)sysfs.name, '/');
	name = name ? name + 1 : (const char *)sysfs.name;
	if (!*name)
		return false;
	for (i = 0; name[i]; i++)
		if (!((name[i] >= 'a' && name[i] <= 'z') ||
		      (name[i] >= 'A' && name[i] <= 'Z') ||
		      (name[i] >= '0' && name[i] <= '9') || name[i] == '-' ||
		      name[i] == '_' || name[i] == '.'))
			return false;
	printed =
		snprintf(path, sizeof(path), "/proc/fs/ext4/%s/options", name);
	if (printed < 0 || (size_t)printed >= sizeof(path))
		return false;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	done = read(fd, options, sizeof(options) - 1);
	close(fd);
	if (done <= 0)
		return false;
	options[done] = 0;
	return (strstr(options, "data=ordered") ||
		strstr(options, "data=journal")) &&
	       !strstr(options, "data=writeback") &&
	       !strstr(options, "noload") && !strstr(options, "norecovery");
}

uint32_t d2_root_qualify(int dirfd, uint8_t fs_uuid[16], uint64_t *root_dev,
			 uint64_t *root_ino)
{
	struct fsuuid2 fsid = { 0 };
	struct stat root, parent;
	struct statfs sfs;
	int parentfd;

	if (dirfd < 0 || !fs_uuid || !root_dev || !root_ino)
		return 2;
	if (fstat(dirfd, &root) < 0 || !S_ISDIR(root.st_mode) ||
	    fstatfs(dirfd, &sfs) < 0)
		return 11;
	if (sfs.f_type != EXT4_SUPER_MAGIC && sfs.f_type != XFS_SUPER_MAGIC)
		return 13;
	if (sfs.f_type == EXT4_SUPER_MAGIC && !d2_ext4_qualify(dirfd))
		return 13;
	parentfd = openat(dirfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parentfd < 0)
		return 11;
	if (fstat(parentfd, &parent) < 0) {
		close(parentfd);
		return 11;
	}
	close(parentfd);
	if (root.st_dev == parent.st_dev)
		return 13;
	if (ioctl(dirfd, FS_IOC_GETFSUUID, &fsid) < 0 || fsid.len != 16)
		return 13;
	memcpy(fs_uuid, fsid.uuid, 16);
	*root_dev = (uint64_t)root.st_dev;
	*root_ino = (uint64_t)root.st_ino;
	return 1;
}

static void d2_files_free(struct d2_files *f)
{
	if (!f)
		return;
	if (f->super_fd >= 0)
		close(f->super_fd);
	if (f->wal_fd >= 0)
		close(f->wal_fd);
	if (f->payload_fd >= 0)
		close(f->payload_fd);
	if (f->dirfd >= 0)
		close(f->dirfd);
	free(f);
}

static struct d2_files *d2_files_alloc(int dirfd)
{
	struct d2_files *f = calloc(1, sizeof(*f));

	if (!f)
		return NULL;
	f->dirfd = dup(dirfd);
	f->super_fd = -1;
	f->wal_fd = -1;
	f->payload_fd = -1;
	if (f->dirfd < 0) {
		d2_files_free(f);
		return NULL;
	}
	return f;
}

static uint32_t d2_open_created(struct d2_files *f)
{
	f->wal_fd = openat(f->dirfd, "wal",
			   O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (f->wal_fd < 0)
		return d2_errno_status();
	f->payload_fd = openat(f->dirfd, "payload",
			       O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (f->payload_fd < 0)
		return d2_errno_status();
	f->super_fd = openat(f->dirfd, "super",
			     O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (f->super_fd < 0)
		return d2_errno_status();
	return 1;
}

static bool d2_write_super_slot(struct d2_files *f, unsigned int slot)
{
	uint8_t bytes[D2_SB_SLOT_BYTES];

	if (!d2_super_encode(&f->super, bytes) ||
	    !d2_pwrite_all(f->super_fd, bytes, sizeof(bytes),
			   slot * D2_SB_SLOT_BYTES))
		return false;
	d2_hook(f, D2_IO_SUPER_WRITTEN);
	if (fdatasync(f->super_fd) < 0)
		return false;
	d2_hook(f, D2_IO_SUPER_DURABLE);
	f->newest_slot = slot;
	return true;
}

static void d2_verifier(const uint8_t store_uuid[16], uint64_t epoch,
			uint8_t out[8])
{
	struct d1_cursor c;
	uint8_t input[24], digest[32];

	d1_enc_init(&c, input, sizeof(input));
	d1_enc_raw(&c, store_uuid, 16);
	d1_enc_u64(&c, epoch);
	d2_hash_domain("FFV2-D2-VERIFIER-v1", input, sizeof(input), digest);
	memcpy(out, digest, 8);
}

static uint32_t d2_files_wal_append_internal(struct d2_files *f,
					     const uint8_t *record, size_t len,
					     bool reserved, uint64_t promised,
					     bool group);

static uint32_t d2_initial_start(struct d2_files *f)
{
	struct d2_wal_header h = { 0 };
	struct d2_start s = { 0 };
	uint8_t record[D2_START_RECORD_BYTES];

	h.family = D2_REC_START;
	memcpy(h.store_uuid, f->super.store_uuid, 16);
	memcpy(h.wal_uuid, f->super.wal_uuid, 16);
	h.lsn = 1;
	h.ds_incarnation = 1;
	s.ds_incarnation = 1;
	s.recovery_decision = D2_RD_FIRST_PROVISION;
	s.wal_append_cursor = D2_START_RECORD_BYTES;
	s.payload_append_cursor = D2_PAYLOAD_ALIGN;
	s.capacity_wal_bytes = f->super.capacity_wal_bytes;
	s.capacity_payload_bytes = f->super.capacity_payload_bytes;
	memcpy(s.export_uuid, f->super.export_uuid, 16);
	if (!d2_start_encode(&h, &s, record))
		return 2;
	f->wal_cursor = 0;
	f->payload_cursor = D2_PAYLOAD_ALIGN;
	f->next_lsn = 1;
	return d2_files_wal_append_internal(f, record, sizeof(record), true, 0,
					    false);
}

uint32_t d2_files_provision(int dirfd, const struct d2_provision *p,
			    struct d2_binding *binding, struct d2_files **out)
{
	struct d2_payload_header ph = { 0 };
	struct d2_files *f;
	uint8_t bytes[D2_PAYLOAD_ALIGN];
	uint32_t status;
	uint64_t root_dev, root_ino;

	if (!p || !binding || !out || !p->binding_token ||
	    p->binding_token_len == 0 ||
	    p->capacity_wal_bytes < D2_MIN_WAL_BYTES ||
	    p->capacity_payload_bytes < D2_PAYLOAD_ALIGN * 2u)
		return 2;
	*out = NULL;
	status = d2_root_qualify(dirfd, binding->fs_uuid, &root_dev, &root_ino);
	if (status != 1)
		return status;
	if (!d2_dir_empty(dirfd))
		return 2;
	f = d2_files_alloc(dirfd);
	if (!f)
		return 10;
	status = d2_open_created(f);
	if (status != 1)
		goto fail;
	if (!d2_uuid_generate(f->super.store_uuid) ||
	    !d2_uuid_generate(f->super.wal_uuid) ||
	    !d2_uuid_generate(f->super.payload_uuid)) {
		status = 11;
		goto fail;
	}
	memcpy(f->super.fs_uuid, binding->fs_uuid, 16);
	f->super.fs_dev_hint = root_dev;
	memcpy(f->super.export_uuid, p->export_uuid, 16);
	d2_token_digest(p->binding_token, p->binding_token_len,
			f->super.binding_token_digest);
	f->super.capacity_wal_bytes = p->capacity_wal_bytes;
	f->super.capacity_payload_bytes = p->capacity_payload_bytes;
	f->super.payload_durable_bytes = D2_PAYLOAD_ALIGN;
	f->super.format_floor = D2_FORMAT_VERSION;
	f->super.state = D2_SB_NEEDS_RECOVERY;
	f->super.generation = 1;
	d2_verifier(f->super.store_uuid, 0, f->super.write_verifier);
	memcpy(ph.store_uuid, f->super.store_uuid, 16);
	memcpy(ph.payload_uuid, f->super.payload_uuid, 16);
	memcpy(ph.wal_uuid, f->super.wal_uuid, 16);
	if (!d2_payload_header_encode(&ph, bytes) ||
	    !d2_pwrite_all(f->payload_fd, bytes, sizeof(bytes), 0) ||
	    fdatasync(f->payload_fd) < 0 ||
	    ftruncate(f->super_fd, D2_SUPER_BYTES) < 0 ||
	    fdatasync(f->super_fd) < 0 || fsync(f->dirfd) < 0 ||
	    !d2_write_super_slot(f, 0)) {
		status = d2_errno_status();
		goto fail;
	}
	status = d2_initial_start(f);
	if (status != 1)
		goto fail;
	f->super.generation = 2;
	f->super.ds_incarnation = 1;
	f->super.wal_durable_lsn = 1;
	f->super.wal_durable_bytes = f->wal_cursor;
	f->super.state = D2_SB_CLEAN;
	if (!d2_write_super_slot(f, 1)) {
		status = d2_errno_status();
		goto fail;
	}
	memcpy(binding->store_uuid, f->super.store_uuid, 16);
	memcpy(binding->export_uuid, f->super.export_uuid, 16);
	memcpy(binding->wal_uuid, f->super.wal_uuid, 16);
	memcpy(binding->payload_uuid, f->super.payload_uuid, 16);
	binding->root_dev = root_dev;
	binding->root_ino = root_ino;
	*out = f;
	return 1;

fail:
	d2_files_free(f);
	return status;
}

static uint32_t d2_read_super(struct d2_files *f,
			      const uint8_t expected_store[16])
{
	struct d2_superblock slots[2];
	uint8_t bytes[D2_SB_SLOT_BYTES];
	bool valid[2] = { false, false };
	unsigned int i, selected;

	for (i = 0; i < 2; i++) {
		if (!d2_pread_all(f->super_fd, bytes, sizeof(bytes),
				  i * D2_SB_SLOT_BYTES))
			continue;
		valid[i] = d2_super_decode(bytes, expected_store, &slots[i]);
	}
	if (!valid[0] && !valid[1])
		return 11;
	if (valid[0] && valid[1] && slots[0].generation == slots[1].generation)
		return 11;
	selected = !valid[0] ||
		   (valid[1] && slots[1].generation > slots[0].generation);
	f->super = slots[selected];
	f->newest_slot = selected;
	if (f->super.state == D2_SB_FENCED)
		return 11;
	if (f->super.ds_incarnation == 0 &&
	    f->super.state == D2_SB_NEEDS_RECOVERY)
		return 2;
	return 1;
}

static uint32_t d2_open_existing(struct d2_files *f)
{
	f->super_fd = openat(f->dirfd, "super", O_RDWR | O_CLOEXEC);
	f->wal_fd = openat(f->dirfd, "wal", O_RDWR | O_CLOEXEC);
	f->payload_fd = openat(f->dirfd, "payload", O_RDWR | O_CLOEXEC);
	if (f->super_fd < 0 || f->wal_fd < 0 || f->payload_fd < 0)
		return 11;
	return 1;
}

uint32_t d2_files_rebind(int dirfd, const struct d2_rebind *r,
			 struct d2_binding *binding, struct d2_files **out)
{
	struct d2_payload_header ph;
	struct d2_scan_result scan;
	struct d2_files *f;
	struct stat st;
	uint8_t token[32], bytes[D2_PAYLOAD_ALIGN];
	uint32_t status;
	uint64_t root_dev, root_ino;

	if (!r || !binding || !out || !r->binding_token ||
	    !r->binding_token_len)
		return 2;
	*out = NULL;
	status = d2_root_qualify(dirfd, binding->fs_uuid, &root_dev, &root_ino);
	if (status != 1)
		return status;
	if (root_ino != r->expected_root_ino)
		return 5;
	f = d2_files_alloc(dirfd);
	if (!f)
		return 10;
	status = d2_open_existing(f);
	if (status != 1)
		goto fail;
	status = d2_read_super(f, r->expected_store_uuid);
	if (status != 1)
		goto fail;
	if (f->super.root_handle_type != 0 || f->super.root_handle_len != 0) {
		status = 11;
		goto fail;
	}
	d2_token_digest(r->binding_token, r->binding_token_len, token);
	if (memcmp(f->super.export_uuid, r->expected_export_uuid, 16) ||
	    memcmp(f->super.fs_uuid, binding->fs_uuid, 16) ||
	    memcmp(f->super.binding_token_digest, token, 32)) {
		status = 5;
		goto fail;
	}
	if (fstat(f->wal_fd, &st) < 0 || st.st_size <= 0 ||
	    fstat(f->payload_fd, &st) < 0 || st.st_size < D2_PAYLOAD_ALIGN ||
	    !d2_pread_all(f->payload_fd, bytes, sizeof(bytes), 0) ||
	    !d2_payload_header_decode(bytes, f->super.store_uuid, &ph) ||
	    memcmp(ph.payload_uuid, f->super.payload_uuid, 16) ||
	    memcmp(ph.wal_uuid, f->super.wal_uuid, 16)) {
		status = 11;
		goto fail;
	}
	status = d2_files_scan(f, NULL, NULL, &scan, true);
	if (status != 1) {
		(void)d2_files_super_update(f, D2_SB_FENCED);
		goto fail;
	}
	if (f->super.wal_durable_lsn > scan.last_lsn ||
	    f->super.wal_durable_bytes > scan.valid_bytes) {
		(void)d2_files_super_update(f, D2_SB_FENCED);
		status = 11;
		goto fail;
	}
	f->wal_cursor = scan.valid_bytes;
	f->payload_cursor = scan.payload_cursor;
	f->next_lsn = scan.last_lsn + 1;
	f->last_scan = scan;
	memcpy(binding->store_uuid, f->super.store_uuid, 16);
	memcpy(binding->export_uuid, f->super.export_uuid, 16);
	memcpy(binding->wal_uuid, f->super.wal_uuid, 16);
	memcpy(binding->payload_uuid, f->super.payload_uuid, 16);
	binding->root_dev = root_dev;
	binding->root_ino = root_ino;
	*out = f;
	return 1;

fail:
	d2_files_free(f);
	return status;
}

void d2_files_close(struct d2_files *files)
{
	d2_files_free(files);
}

void d2_files_crash(struct d2_files *files)
{
	d2_files_free(files);
}

void d2_files_set_io_hook(struct d2_files *files, d2_io_hook_fn hook, void *arg)
{
	if (!files)
		return;
	files->hook = hook;
	files->hook_arg = arg;
}

void d2_files_fail_next_wal_write(struct d2_files *files)
{
	if (files)
		files->fail_next_wal_write = true;
}

const struct d2_superblock *d2_files_super(const struct d2_files *files)
{
	return files ? &files->super : NULL;
}

uint64_t d2_files_next_lsn(const struct d2_files *files)
{
	return files ? files->next_lsn : 0;
}

uint64_t d2_files_wal_cursor(const struct d2_files *files)
{
	return files ? files->wal_cursor : 0;
}

uint64_t d2_files_payload_cursor(const struct d2_files *files)
{
	return files ? files->payload_cursor : 0;
}

const struct d2_scan_result *d2_files_last_scan(const struct d2_files *files)
{
	return files ? &files->last_scan : NULL;
}

uint32_t d2_files_payload_append(struct d2_files *f,
				 const struct d2_payload_object *object,
				 uint64_t *offset)
{
	uint8_t *bytes;
	uint64_t extent;
	size_t written;
	uint32_t status;

	if (!f || !object || !offset ||
	    f->payload_cursor > f->super.capacity_payload_bytes)
		return 2;
	extent = d2_payload_object_bytes(object->content_len);
	if (extent > f->super.capacity_payload_bytes - f->payload_cursor)
		return 10;
	bytes = malloc((size_t)extent);
	if (!bytes)
		return 10;
	if (!d2_payload_encode(object, bytes, (size_t)extent, &written) ||
	    !d2_pwrite_all(f->payload_fd, bytes, written, f->payload_cursor)) {
		status = d2_errno_status();
		free(bytes);
		return status;
	}
	free(bytes);
	d2_hook(f, D2_IO_PAYLOAD_WRITTEN);
	if (fdatasync(f->payload_fd) < 0)
		return d2_errno_status();
	d2_hook(f, D2_IO_PAYLOAD_DURABLE);
	*offset = f->payload_cursor;
	f->payload_cursor += extent;
	return 1;
}

static uint32_t d2_files_wal_append_internal(struct d2_files *f,
					     const uint8_t *record, size_t len,
					     bool reserved, uint64_t promised,
					     bool group)
{
	struct d2_wal_header h = { 0 };
	uint64_t available, restarts, required;
	uint64_t at = 0, expected_lsn;
	uint32_t count = 0;

	if (!f || !record || len > UINT32_MAX ||
	    f->wal_cursor > f->super.capacity_wal_bytes ||
	    len > f->super.capacity_wal_bytes - f->wal_cursor)
		return 2;
	expected_lsn = f->next_lsn;
	do {
		if (!d2_wal_header_decode(record + at, len - (size_t)at,
					  f->super.store_uuid,
					  f->super.wal_uuid, &h) ||
		    h.total_bytes > len - at || h.lsn != expected_lsn ||
		    h.ds_incarnation != f->super.ds_incarnation +
						(f->super.ds_incarnation == 0))
			return 2;
		at += h.total_bytes;
		expected_lsn++;
		count++;
		if (!group && at != len)
			return 2;
	} while (at < len);
	if (at != len)
		return 2;
	available = f->super.capacity_wal_bytes - f->wal_cursor;
	restarts = f->super.ds_incarnation > 0 ? f->super.ds_incarnation - 1 :
						 0;
	if (restarts > D2_MIN_RESTARTS)
		restarts = D2_MIN_RESTARTS;
	required =
		(D2_MIN_RESTARTS - restarts) * D2_RESTART_HEADROOM +
		(D2_RECOVERY_HEADROOM - D2_MIN_RESTARTS * D2_RESTART_HEADROOM);
	if (promised > UINT64_MAX - required ||
	    (!reserved && available - len < required + promised))
		return D1_NOSPC;
	if (f->fail_next_wal_write) {
		f->fail_next_wal_write = false;
		return D1_IO;
	}
	if (!d2_pwrite_all(f->wal_fd, record, len, f->wal_cursor))
		return d2_errno_status();
	d2_hook(f, D2_IO_WAL_WRITTEN);
	if (fdatasync(f->wal_fd) < 0)
		return d2_errno_status();
	d2_hook(f, D2_IO_WAL_DURABLE);
	f->wal_cursor += len;
	f->next_lsn += count;
	f->super.wal_durable_lsn = h.lsn;
	f->super.wal_durable_bytes = f->wal_cursor;
	f->super.payload_durable_bytes = f->payload_cursor;
	return 1;
}

uint32_t d2_files_wal_append(struct d2_files *f, const uint8_t *record,
			     size_t len)
{
	return d2_files_wal_append_internal(f, record, len, false, 0, false);
}

uint32_t d2_files_wal_append_floor(struct d2_files *f, const uint8_t *record,
				   size_t len, uint64_t promised)
{
	return d2_files_wal_append_internal(f, record, len, false, promised,
					    false);
}

uint32_t d2_files_wal_append_group_floor(struct d2_files *f,
					 const uint8_t *records, size_t len,
					 uint64_t promised)
{
	return d2_files_wal_append_internal(f, records, len, false, promised,
					    true);
}

uint32_t d2_files_start(struct d2_files *f, uint32_t recovery_decision,
			uint64_t truncated_bytes, uint64_t payload_cursor,
			uint64_t live_wal_reserved,
			uint64_t live_payload_outstanding,
			uint64_t live_payload_staged)
{
	struct d2_wal_header h = { 0 };
	struct d2_start s = { 0 };
	uint8_t record[D2_START_RECORD_BYTES];
	uint64_t prior, next;
	uint32_t status;

	if (!f || f->super.ds_incarnation >= D2_MAX_INCARNATION ||
	    payload_cursor < D2_PAYLOAD_ALIGN ||
	    payload_cursor > f->super.capacity_payload_bytes)
		return 2;
	prior = f->super.ds_incarnation;
	next = prior + 1;
	h.family = D2_REC_START;
	memcpy(h.store_uuid, f->super.store_uuid, 16);
	memcpy(h.wal_uuid, f->super.wal_uuid, 16);
	h.lsn = f->next_lsn;
	h.ds_incarnation = next;
	s.ds_incarnation = next;
	s.prev_incarnation = prior;
	s.recovery_decision = recovery_decision;
	s.truncated_bytes = truncated_bytes;
	s.last_valid_lsn_before = h.lsn - 1;
	s.wal_append_cursor = f->wal_cursor + D2_START_RECORD_BYTES;
	s.payload_append_cursor = payload_cursor;
	s.capacity_wal_bytes = f->super.capacity_wal_bytes;
	s.capacity_payload_bytes = f->super.capacity_payload_bytes;
	memcpy(s.export_uuid, f->super.export_uuid, 16);
	s.live_txn_wal_reserved = live_wal_reserved;
	s.live_txn_payload_outstanding = live_payload_outstanding;
	s.live_txn_payload_staged = live_payload_staged;
	s.verifier_changed =
		!!(recovery_decision &
		   (D2_RD_DISCARDED_UNSTABLE | D2_RD_TRUNCATED_NONZERO_TAIL));
	s.verifier_epoch = s.verifier_changed ? next : f->super.verifier_epoch;
	if (!d2_start_encode(&h, &s, record))
		return 2;
	/* START is the one record allowed to introduce the next incarnation. */
	f->super.ds_incarnation = next;
	status = d2_files_wal_append_internal(f, record, sizeof(record), true,
					      0, false);
	if (status != 1) {
		f->super.ds_incarnation = prior;
		return status;
	}
	f->super.verifier_epoch = s.verifier_epoch;
	if (s.verifier_changed)
		d2_verifier(f->super.store_uuid, s.verifier_epoch,
			    f->super.write_verifier);
	return d2_files_super_update(f, D2_SB_CLEAN);
}

uint32_t d2_files_super_update(struct d2_files *f, uint32_t state)
{
	unsigned int slot;

	if (!f || state < D2_SB_CLEAN || state > D2_SB_RETIRED ||
	    f->super.generation == UINT64_MAX)
		return 2;
	f->super.generation++;
	f->super.state = state;
	slot = f->newest_slot ^ 1u;
	if (!d2_write_super_slot(f, slot)) {
		f->super.generation--;
		return d2_errno_status();
	}
	return 1;
}

static bool d2_all_zero(const uint8_t *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (p[i])
			return false;
	return true;
}

static uint32_t d2_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static bool d2_tail_prologue(const uint8_t *p, size_t remain,
			     const struct d2_files *f, uint32_t *total)
{
	if (remain < D2_WAL_HEADER_BYTES)
		return false;
	*total = d2_be32(p + 12);
	return !memcmp(p, "D2WL", 4) && p[4] == 0 && p[5] == 1 &&
	       d2_be32(p + 8) == D2_WAL_HEADER_BYTES &&
	       (*total ^ d2_be32(p + 16)) == UINT32_MAX &&
	       *total >= D2_WAL_HEADER_BYTES + 4 &&
	       *total <= D2_MAX_WAL_RECORD &&
	       !memcmp(p + 20, f->super.store_uuid, 16) &&
	       !memcmp(p + 36, f->super.wal_uuid, 16);
}

static bool d2_body_decode(const struct d2_wal_header *h, const uint8_t *record)
{
	switch (h->family) {
	case D2_REC_START: {
		struct d2_start s;
		return d2_start_decode(record, h->total_bytes, h, &s);
	}
	case D2_REC_CONTROL: {
		struct d2_control c;
		return d2_control_decode(record, h->total_bytes, h, &c);
	}
	case D2_REC_ENTRY: {
		struct d2_entry e;
		return d2_entry_decode(record, h->total_bytes, h, &e);
	}
	default: {
		struct d2_cohort c;
		return d2_cohort_decode(record, h->total_bytes, h, &c);
	}
	}
}

static uint64_t d2_record_payload_end(const struct d2_wal_header *h,
				      const uint8_t *record)
{
	uint64_t end = 0;
	unsigned int i;

	if (h->family == D2_REC_ENTRY) {
		struct d2_entry e;
		if (d2_entry_decode(record, h->total_bytes, h, &e) &&
		    e.payload_object_id) {
			uint64_t extent =
				d2_payload_object_bytes(e.payload_content_len);

			end = e.payload_object_offset > UINT64_MAX - extent ?
				      UINT64_MAX :
				      e.payload_object_offset + extent;
		}
	} else if (h->family == D2_REC_COHORT) {
		struct d2_cohort c;
		if (d2_cohort_decode(record, h->total_bytes, h, &c))
			for (i = 0; i < c.member_count; i++)
				if (c.members[i].payload_object_id) {
					uint64_t offset =
						c.members[i]
							.payload_object_offset;
					uint64_t extent = d2_payload_object_bytes(
						c.members[i]
							.payload_content_len);
					uint64_t member_end =
						offset > UINT64_MAX - extent ?
							UINT64_MAX :
							offset + extent;

					if (end < member_end)
						end = member_end;
				}
	}
	return end;
}

static bool d2_payload_ref_matches(struct d2_files *f, uint64_t id,
				   uint64_t offset, uint32_t content_len)
{
	struct d2_payload_object object;
	uint8_t *allocation = NULL;
	uint32_t status;
	bool content_ok, matches;

	if (!id)
		return offset == 0 && content_len == 0;
	status = d2_files_payload_read_content(f, offset, &object, &allocation,
					       &content_ok);
	matches = status == D1_OK && object.payload_object_id == id &&
		  object.content_len == content_len;
	free(allocation);
	return matches;
}

static bool d2_record_payloads_match(struct d2_files *f,
				     const struct d2_wal_header *h,
				     const uint8_t *record)
{
	uint32_t i;

	if (h->family == D2_REC_ENTRY) {
		struct d2_entry e;

		return d2_entry_decode(record, h->total_bytes, h, &e) &&
		       d2_payload_ref_matches(f, e.payload_object_id,
					      e.payload_object_offset,
					      e.payload_content_len);
	}
	if (h->family == D2_REC_COHORT) {
		struct d2_cohort c;

		if (!d2_cohort_decode(record, h->total_bytes, h, &c))
			return false;
		for (i = 0; i < c.member_count; i++)
			if (!d2_payload_ref_matches(
				    f, c.members[i].payload_object_id,
				    c.members[i].payload_object_offset,
				    c.members[i].payload_content_len))
				return false;
	}
	return true;
}

uint32_t d2_files_scan(struct d2_files *f, d2_scan_fn fn, void *arg,
		       struct d2_scan_result *result, bool truncate_tail)
{
	struct d2_wal_header h;
	struct stat st;
	uint8_t *bytes;
	uint64_t at = 0, expected_lsn = 1, incarnation = 0, payload_end = 4096;
	size_t size, remain;
	bool saw_start = false;

	if (!f || !result || fstat(f->wal_fd, &st) < 0 || st.st_size < 0)
		return 11;
	size = (size_t)st.st_size;
	if ((off_t)size != st.st_size || size > f->super.capacity_wal_bytes)
		return 11;
	bytes = malloc(size ? size : 1);
	if (!bytes)
		return 10;
	if (size && !d2_pread_all(f->wal_fd, bytes, size, 0)) {
		free(bytes);
		return 11;
	}
	memset(result, 0, sizeof(*result));
	result->payload_cursor = D2_PAYLOAD_ALIGN;
	while (at < size) {
		uint64_t object_end;
		uint32_t total = 0;
		bool prologue;

		remain = size - (size_t)at;
		if (d2_wal_header_decode(bytes + at, remain,
					 f->super.store_uuid, f->super.wal_uuid,
					 &h) &&
		    d2_body_decode(&h, bytes + at))
			goto valid_record;

		/* The remaining bytes are R; apply the normative tests in order. */
		if (remain > D2_MAX_WAL_RECORD) {
			result->tail_class = D2_TAIL_OVERLONG;
			goto fenced;
		}
		if (d2_all_zero(bytes + at, remain)) {
			result->tail_class = D2_TAIL_ZERO;
			break;
		}
		if (remain < D2_WAL_HEADER_BYTES) {
			result->tail_class = D2_TAIL_INCOMPLETE;
			break;
		}
		prologue = d2_tail_prologue(bytes + at, remain, f, &total);
		if (!prologue ||
		    (total <= remain &&
		     (!d2_wal_header_decode(bytes + at, remain,
					    f->super.store_uuid,
					    f->super.wal_uuid, &h) ||
		      !d2_body_decode(&h, bytes + at)))) {
			/* A valid prologue with bad CRC/LSN is classified below. */
			if (!prologue ||
			    (total <= remain &&
			     d2_wal_header_decode(bytes + at, remain,
						  f->super.store_uuid,
						  f->super.wal_uuid, &h))) {
				result->tail_class = D2_TAIL_UNPARSEABLE;
				goto fenced;
			}
		}
		if (total > remain) {
			result->tail_class = D2_TAIL_INCOMPLETE;
			break;
		}
		result->tail_class = D2_TAIL_BAD_COMPLETE;
		goto fenced;

valid_record:
		if (h.lsn != expected_lsn ||
		    (saw_start && h.family != D2_REC_START &&
		     h.ds_incarnation != incarnation) ||
		    (!saw_start && h.family != D2_REC_START)) {
			result->tail_class = D2_TAIL_BAD_COMPLETE;
			goto fenced;
		}
		if (h.family == D2_REC_START) {
			struct d2_start s;

			if (!d2_start_decode(bytes + at, h.total_bytes, &h,
					     &s) ||
			    s.prev_incarnation != incarnation ||
			    s.last_valid_lsn_before != expected_lsn - 1 ||
			    s.wal_append_cursor != at + h.total_bytes ||
			    s.payload_append_cursor != payload_end ||
			    s.capacity_wal_bytes !=
				    f->super.capacity_wal_bytes ||
			    s.capacity_payload_bytes !=
				    f->super.capacity_payload_bytes ||
			    memcmp(s.export_uuid, f->super.export_uuid, 16))
				goto fenced;
			if (saw_start && h.ds_incarnation != incarnation + 1)
				goto fenced;
			incarnation = h.ds_incarnation;
			saw_start = true;
		}
		if (!d2_record_payloads_match(f, &h, bytes + at))
			goto fenced;
		if (fn && !fn(&h, bytes + at, arg))
			goto fenced;
		object_end = d2_record_payload_end(&h, bytes + at);
		if (object_end > f->super.capacity_payload_bytes ||
		    (object_end && object_end < D2_PAYLOAD_ALIGN))
			goto fenced;
		if (payload_end < object_end)
			payload_end = object_end;
		at += h.total_bytes;
		expected_lsn++;
		result->record_count++;
	}
	if (!saw_start)
		goto fenced;
	if (at == size)
		result->tail_class = D2_TAIL_CLEAN;
	result->valid_bytes = at;
	result->truncated_bytes = size - (size_t)at;
	result->last_lsn = expected_lsn - 1;
	result->last_incarnation = incarnation;
	result->payload_cursor = payload_end;
	if (result->tail_class != D2_TAIL_CLEAN && truncate_tail) {
		if (ftruncate(f->wal_fd, (off_t)at) < 0 ||
		    fdatasync(f->wal_fd) < 0) {
			free(bytes);
			return 11;
		}
	}
	free(bytes);
	return 1;

fenced:
	if (!result->tail_class)
		result->tail_class = D2_TAIL_BAD_COMPLETE;
	free(bytes);
	return 11;
}

uint32_t d2_files_payload_read(struct d2_files *f, uint64_t offset,
			       struct d2_payload_object *object,
			       uint8_t **allocation)
{
	bool content_ok;
	uint32_t status;

	status = d2_files_payload_read_content(f, offset, object, allocation,
					       &content_ok);
	if (status == D1_OK && !content_ok) {
		free(*allocation);
		*allocation = NULL;
		return D1_CHECKSUM;
	}
	return status;
}

uint32_t d2_files_payload_read_content(struct d2_files *f, uint64_t offset,
				       struct d2_payload_object *object,
				       uint8_t **allocation, bool *content_ok)
{
	uint8_t header[D2_PAYLOAD_HEADER_BYTES];
	uint8_t *bytes;
	uint64_t extent;
	uint32_t content_len;

	if (!f || !object || !allocation || !content_ok ||
	    offset < D2_PAYLOAD_ALIGN || offset % D2_PAYLOAD_ALIGN)
		return 2;
	*allocation = NULL;
	*content_ok = false;
	if (!d2_pread_all(f->payload_fd, header, sizeof(header), offset))
		return 11;
	content_len = ((uint32_t)header[120] << 24) |
		      ((uint32_t)header[121] << 16) |
		      ((uint32_t)header[122] << 8) | header[123];
	extent = d2_payload_object_bytes(content_len);
	if (extent > SIZE_MAX ||
	    extent > f->super.capacity_payload_bytes - offset)
		return 11;
	bytes = malloc((size_t)extent);
	if (!bytes)
		return 10;
	if (!d2_pread_all(f->payload_fd, bytes, (size_t)extent, offset) ||
	    !d2_payload_decode_content(bytes, (size_t)extent,
				       f->super.store_uuid, object,
				       content_ok)) {
		free(bytes);
		return 9;
	}
	*allocation = bytes;
	return 1;
}
