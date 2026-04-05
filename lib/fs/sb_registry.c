/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "reffs/rcu.h"
#include "reffs/log.h"
#include "reffs/sb_registry.h"
#include "reffs/super_block.h"
#include "reffs/dirent.h"

/* In-memory next-id counter, loaded from registry at startup. */
static uint32_t sb_next_id = SB_REGISTRY_FIRST_ID;

/* ------------------------------------------------------------------ */
/* Save                                                                */
/* ------------------------------------------------------------------ */

int sb_registry_save(const char *state_dir)
{
	struct cds_list_head *sb_list = super_block_list_head();
	struct super_block *sb;
	char path[PATH_MAX];
	char tmp[PATH_MAX];
	int fd = -1;
	int ret = 0;
	ssize_t n;

	if (!state_dir)
		return -EINVAL;

	if (snprintf(path, sizeof(path), "%s/%s", state_dir,
		     SB_REGISTRY_FILE) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;

	/*
	 * Two-pass: count under rcu_read_lock (no blocking), allocate
	 * outside, then populate under a second rcu_read_lock.
	 * No realloc/malloc inside the critical section.
	 */
	uint32_t count = 0;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, sb_list, sb_link) {
		if (sb->sb_id != SUPER_BLOCK_ROOT_ID &&
		    sb->sb_lifecycle != SB_DESTROYED)
			count++;
	}
	rcu_read_unlock();

	struct sb_registry_entry *entries = NULL;

	if (count > 0) {
		entries = calloc(count, sizeof(*entries));
		if (!entries)
			return -ENOMEM;

		uint32_t i = 0;

		rcu_read_lock();
		cds_list_for_each_entry_rcu(sb, sb_list, sb_link) {
			if (sb->sb_id == SUPER_BLOCK_ROOT_ID ||
			    sb->sb_lifecycle == SB_DESTROYED)
				continue;
			if (i >= count)
				break;
			entries[i].sre_id = sb->sb_id;
			entries[i].sre_state = (uint32_t)sb->sb_lifecycle;
			entries[i].sre_storage_type =
				(uint32_t)sb->sb_storage_type;
			uuid_copy(entries[i].sre_uuid, sb->sb_uuid);
			if (sb->sb_path)
				strncpy(entries[i].sre_path, sb->sb_path,
					SB_REGISTRY_MAX_PATH - 1);
			if (sb->sb_backend_path)
				strncpy(entries[i].sre_backend_path,
					sb->sb_backend_path,
					SB_REGISTRY_MAX_PATH - 1);
			entries[i].sre_layout_types = sb->sb_layout_types;
			entries[i].sre_nflavors = sb->sb_nflavors;
			for (unsigned int f = 0;
			     f < sb->sb_nflavors && f < SB_REGISTRY_MAX_FLAVORS;
			     f++)
				entries[i].sre_flavors[f] =
					(uint32_t)sb->sb_flavors[f];
			i++;
		}
		rcu_read_unlock();
		count = i; /* actual count (may be less if sb removed) */
	}

	struct sb_registry_header hdr = {
		.srh_magic = SB_REGISTRY_MAGIC,
		.srh_version = SB_REGISTRY_VERSION,
		.srh_count = count,
		.srh_next_id = sb_next_id,
	};
	size_t entries_sz = count * sizeof(struct sb_registry_entry);

	/* Write temp, fdatasync, rename. */
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}

	n = write(fd, &hdr, sizeof(hdr));
	if (n != (ssize_t)sizeof(hdr)) {
		ret = (n < 0) ? -errno : -EIO;
		goto err_close;
	}

	if (entries_sz > 0) {
		n = write(fd, entries, entries_sz);
		if (n != (ssize_t)entries_sz) {
			ret = (n < 0) ? -errno : -EIO;
			goto err_close;
		}
	}

	if (fdatasync(fd)) {
		ret = -errno;
		goto err_close;
	}
	close(fd);
	fd = -1;

	if (rename(tmp, path)) {
		ret = -errno;
		unlink(tmp);
	}
	goto out;

err_close:
	close(fd);
	unlink(tmp);
out:
	free(entries);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Load                                                                */
/* ------------------------------------------------------------------ */

int sb_registry_load(const char *state_dir)
{
	char path[PATH_MAX];
	int fd;
	ssize_t n;
	int ret = 0;

	if (!state_dir)
		return -EINVAL;

	if (snprintf(path, sizeof(path), "%s/%s", state_dir,
		     SB_REGISTRY_FILE) >= (int)sizeof(path))
		return -ENAMETOOLONG;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT)
			return -ENOENT;
		return -errno;
	}

	struct sb_registry_header hdr;

	n = read(fd, &hdr, sizeof(hdr));
	if (n != (ssize_t)sizeof(hdr)) {
		close(fd);
		return (n < 0) ? -errno : -EINVAL;
	}

	if (hdr.srh_magic != SB_REGISTRY_MAGIC) {
		LOG("sb_registry_load: bad magic 0x%08x", hdr.srh_magic);
		close(fd);
		return -EINVAL;
	}
	if (hdr.srh_version != SB_REGISTRY_VERSION) {
		LOG("sb_registry_load: unknown version %u", hdr.srh_version);
		close(fd);
		return -EINVAL;
	}

	/* Restore the persistent id counter. */
	if (hdr.srh_next_id >= SB_REGISTRY_FIRST_ID)
		sb_next_id = hdr.srh_next_id;

	if (hdr.srh_count == 0) {
		close(fd);
		return 0;
	}

	/* Sanity cap — no deployment has thousands of exports. */
	if (hdr.srh_count > 4096) {
		LOG("sb_registry_load: unreasonable count %u", hdr.srh_count);
		close(fd);
		return -EINVAL;
	}

	size_t entries_sz = hdr.srh_count * sizeof(struct sb_registry_entry);
	struct sb_registry_entry *entries =
		calloc(hdr.srh_count, sizeof(*entries));

	if (!entries) {
		close(fd);
		return -ENOMEM;
	}

	n = read(fd, entries, entries_sz);
	int saved_errno = errno;

	close(fd);

	if (n != (ssize_t)entries_sz) {
		free(entries);
		return (n < 0) ? -saved_errno : -EINVAL;
	}

	/* Recreate each superblock. */
	for (uint32_t i = 0; i < hdr.srh_count; i++) {
		struct sb_registry_entry *e = &entries[i];

		/* Skip if this sb already exists (e.g., root). */
		struct super_block *existing = super_block_find(e->sre_id);

		if (existing) {
			super_block_put(existing);
			continue;
		}

		const char *backend =
			e->sre_backend_path[0] ? e->sre_backend_path : NULL;
		struct super_block *sb = super_block_alloc(
			e->sre_id, e->sre_path,
			(enum reffs_storage_type)e->sre_storage_type, backend);
		if (!sb) {
			LOG("sb_registry_load: failed to alloc sb %lu",
			    (unsigned long)e->sre_id);
			continue;
		}

		/* Restore persisted UUID, layout types, and flavors. */
		uuid_copy(sb->sb_uuid, e->sre_uuid);
		sb->sb_layout_types = e->sre_layout_types;

		if (e->sre_nflavors > 0 &&
		    e->sre_nflavors <= SB_REGISTRY_MAX_FLAVORS) {
			enum reffs_auth_flavor flavors[SB_REGISTRY_MAX_FLAVORS];

			for (uint32_t f = 0; f < e->sre_nflavors; f++)
				flavors[f] = (enum reffs_auth_flavor)
						     e->sre_flavors[f];
			super_block_set_flavors(sb, flavors, e->sre_nflavors);
		}

		ret = super_block_dirent_create(sb, NULL,
						reffs_life_action_birth);
		if (ret) {
			LOG("sb_registry_load: dirent_create failed for sb %lu",
			    (unsigned long)e->sre_id);
			super_block_put(sb);
			continue;
		}

		/* Restore lifecycle state. */
		if (e->sre_state == SB_MOUNTED) {
			ret = super_block_mount(sb, e->sre_path);
			if (ret)
				LOG("sb_registry_load: mount failed for sb %lu: %d",
				    (unsigned long)e->sre_id, ret);
		}
	}

	free(entries);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Orphan detection                                                    */
/* ------------------------------------------------------------------ */

int sb_registry_detect_orphans(const char *state_dir)
{
	char path[PATH_MAX];
	int ret;
	int orphan_count = 0;

	if (!state_dir)
		return -EINVAL;

	/* Load the registry to know what's expected. */
	if (snprintf(path, sizeof(path), "%s/%s", state_dir,
		     SB_REGISTRY_FILE) >= (int)sizeof(path))
		return -ENAMETOOLONG;

	/* Read the registry header + entries. */
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return (errno == ENOENT) ? 0 : -errno;

	struct sb_registry_header hdr;
	ssize_t n = read(fd, &hdr, sizeof(hdr));

	if (n != (ssize_t)sizeof(hdr) || hdr.srh_magic != SB_REGISTRY_MAGIC) {
		close(fd);
		return -EINVAL;
	}

	uint64_t *known_ids = NULL;
	uint32_t known_count = 0;

	if (hdr.srh_count > 0) {
		struct sb_registry_entry *entries =
			calloc(hdr.srh_count, sizeof(*entries));
		if (!entries) {
			close(fd);
			return -ENOMEM;
		}
		size_t esz = hdr.srh_count * sizeof(*entries);

		n = read(fd, entries, esz);
		close(fd);
		fd = -1;

		if (n != (ssize_t)esz) {
			free(entries);
			return -EINVAL;
		}

		known_ids = calloc(hdr.srh_count, sizeof(*known_ids));
		if (!known_ids) {
			free(entries);
			return -ENOMEM;
		}
		for (uint32_t i = 0; i < hdr.srh_count; i++)
			known_ids[i] = entries[i].sre_id;
		known_count = hdr.srh_count;
		free(entries);
	} else {
		close(fd);
	}

	/* Scan state_dir for sb_<id>/ directories. */
	DIR *dir = opendir(state_dir);

	if (!dir) {
		ret = -errno;
		free(known_ids);
		return ret;
	}

	struct dirent *de;

	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "sb_", 3) != 0)
			continue;

		char *endp;
		unsigned long id = strtoul(de->d_name + 3, &endp, 10);

		if (*endp != '\0')
			continue; /* not sb_<number> */

		/* Check if this id is in the registry. */
		int found = 0;

		for (uint32_t i = 0; i < known_count; i++) {
			if (known_ids[i] == id) {
				found = 1;
				break;
			}
		}

		if (!found) {
			LOG("sb_registry: orphan directory %s/%s "
			    "(not in registry — may be stale or "
			    "referral source, not deleting)",
			    state_dir, de->d_name);
			orphan_count++;
		}
	}

	closedir(dir);
	free(known_ids);
	return orphan_count;
}

uint64_t sb_registry_alloc_id(const char *state_dir)
{
	uint64_t id = sb_next_id++;

	/* Persist the incremented counter immediately. */
	if (state_dir)
		sb_registry_save(state_dir);

	return id;
}
