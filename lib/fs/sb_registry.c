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

#include "reffs/client_match.h"
#include "reffs/fs.h"
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
			/* Dedup: skip if a prior entry has the same id. */
			bool dup_id = false;

			for (uint32_t j = 0; j < i; j++) {
				if (entries[j].sre_id == sb->sb_id) {
					dup_id = true;
					break;
				}
			}
			if (dup_id)
				continue;
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
			/* Persist the all-flavors union for human-readable display. */
			entries[i].sre_nflavors = sb->sb_nall_flavors;
			for (unsigned int f = 0; f < sb->sb_nall_flavors &&
						 f < SB_REGISTRY_MAX_FLAVORS;
			     f++)
				entries[i].sre_flavors[f] =
					(uint32_t)sb->sb_all_flavors[f];
			entries[i].sre_ndstores = sb->sb_ndstores;
			for (uint32_t d = 0;
			     d < sb->sb_ndstores && d < SB_REGISTRY_MAX_DSTORES;
			     d++)
				entries[i].sre_dstore_ids[d] =
					sb->sb_dstore_ids[d];
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

	/*
	 * Save per-sb client rules after the main registry file.
	 * Snapshot sb_ids under rcu_read_lock (no blocking inside),
	 * then do file I/O outside the lock via super_block_find().
	 * Failures are logged but do not fail the overall save.
	 */
	if (ret == 0) {
		/* Snapshot ids under rcu_read_lock; no I/O inside the lock. */
		enum { MAX_SNAPSHOT = 256 };
		uint64_t ids[MAX_SNAPSHOT];
		unsigned int nids = 0;

		rcu_read_lock();
		cds_list_for_each_entry_rcu(sb, sb_list, sb_link) {
			if (sb->sb_id == SUPER_BLOCK_ROOT_ID ||
			    sb->sb_lifecycle == SB_DESTROYED)
				continue;
			if (sb->sb_nclient_rules > 0 && nids < MAX_SNAPSHOT)
				ids[nids++] = sb->sb_id;
		}
		rcu_read_unlock();

		for (unsigned int i = 0; i < nids; i++) {
			struct super_block *found = super_block_find(ids[i]);

			if (!found)
				continue;
			int cr = sb_client_rules_save(state_dir, found->sb_id,
						      found);
			if (cr)
				TRACE("sb_registry_save: client rules save failed for sb %lu: %d",
				      (unsigned long)found->sb_id, cr);
			super_block_put(found);
		}
	}

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

	/* Sanity cap -- no deployment has thousands of exports. */
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

		/* Restore persisted UUID, layout types, flavors, dstores. */
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

		if (e->sre_ndstores > 0 &&
		    e->sre_ndstores <= SB_REGISTRY_MAX_DSTORES) {
			sb->sb_ndstores = e->sre_ndstores;
			for (uint32_t d = 0; d < e->sre_ndstores; d++)
				sb->sb_dstore_ids[d] = e->sre_dstore_ids[d];
		}

		ret = super_block_dirent_create(sb, NULL,
						reffs_life_action_birth);
		if (ret) {
			LOG("sb_registry_load: dirent_create failed for sb %lu",
			    (unsigned long)e->sre_id);
			super_block_put(sb);
			continue;
		}

		/* Restore per-sb client rules (absent file = no rules). */
		{
			int cr = sb_client_rules_load(state_dir, e->sre_id, sb);

			if (cr && cr != -ENOENT)
				TRACE("sb_registry_load: client rules load failed for sb %lu: %d",
				      (unsigned long)e->sre_id, cr);
		}

		/* Restore lifecycle state. */
		if (e->sre_state == SB_MOUNTED) {
			/* Ensure mount path exists (may have been lost
			 * if the data directory was wiped). */
			reffs_fs_mkdir_p(e->sre_path, 0755);
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
/* Per-sb client rules persistence                                     */
/* ------------------------------------------------------------------ */

int sb_client_rules_save(const char *state_dir, uint64_t sb_id,
			 const struct super_block *sb)
{
	char path[PATH_MAX];
	char tmp[PATH_MAX];
	int fd = -1;
	int ret = 0;
	ssize_t n;
	unsigned int nrules = sb->sb_nclient_rules;

	if (!state_dir)
		return -EINVAL;

	if (snprintf(path, sizeof(path), "%s/sb_%lu.clients", state_dir,
		     (unsigned long)sb_id) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -errno;

	/* Write rule count. */
	n = write(fd, &nrules, sizeof(nrules));
	if (n != (ssize_t)sizeof(nrules)) {
		ret = (n < 0) ? -errno : -EIO;
		goto err_close;
	}

	/* Write each rule in the on-disk format. */
	for (unsigned int i = 0; i < nrules; i++) {
		const struct sb_client_rule *r = &sb->sb_client_rules[i];
		struct sb_registry_client_rule rec;

		memset(&rec, 0, sizeof(rec));
		strncpy(rec.srcr_match, r->scr_match,
			SB_REGISTRY_CLIENT_MATCH_MAX - 1);
		if (r->scr_rw)
			rec.srcr_flags |= SRCR_RW;
		if (r->scr_root_squash)
			rec.srcr_flags |= SRCR_ROOT_SQUASH;
		if (r->scr_all_squash)
			rec.srcr_flags |= SRCR_ALL_SQUASH;
		rec.srcr_nflavors = r->scr_nflavors;
		for (unsigned int f = 0;
		     f < r->scr_nflavors && f < SB_REGISTRY_MAX_FLAVORS; f++)
			rec.srcr_flavors[f] = (uint32_t)r->scr_flavors[f];

		n = write(fd, &rec, sizeof(rec));
		if (n != (ssize_t)sizeof(rec)) {
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
	return ret;

err_close:
	close(fd);
	unlink(tmp);
	return ret;
}

int sb_client_rules_load(const char *state_dir, uint64_t sb_id,
			 struct super_block *sb)
{
	char path[PATH_MAX];
	int fd;
	ssize_t n;
	uint32_t nrules;

	if (!state_dir)
		return -EINVAL;

	if (snprintf(path, sizeof(path), "%s/sb_%lu.clients", state_dir,
		     (unsigned long)sb_id) >= (int)sizeof(path))
		return -ENAMETOOLONG;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (errno == ENOENT) ? -ENOENT : -errno;

	n = read(fd, &nrules, sizeof(nrules));
	if (n != (ssize_t)sizeof(nrules)) {
		close(fd);
		return (n < 0) ? -errno : -EINVAL;
	}

	if (nrules == 0) {
		close(fd);
		return 0;
	}

	/* Cap to prevent absurd allocations from corrupt files. */
	if (nrules > SB_MAX_CLIENT_RULES) {
		LOG("sb_client_rules_load: sb %lu: rule count %u > max %u",
		    (unsigned long)sb_id, nrules, SB_MAX_CLIENT_RULES);
		close(fd);
		return -EINVAL;
	}

	struct sb_client_rule rules[SB_MAX_CLIENT_RULES];

	memset(rules, 0, sizeof(rules));

	for (uint32_t i = 0; i < nrules; i++) {
		struct sb_registry_client_rule rec;

		n = read(fd, &rec, sizeof(rec));
		if (n != (ssize_t)sizeof(rec)) {
			close(fd);
			return (n < 0) ? -errno : -EINVAL;
		}

		strncpy(rules[i].scr_match, rec.srcr_match,
			SB_CLIENT_MATCH_MAX - 1);
		rules[i].scr_rw = !!(rec.srcr_flags & SRCR_RW);
		rules[i].scr_root_squash =
			!!(rec.srcr_flags & SRCR_ROOT_SQUASH);
		rules[i].scr_all_squash = !!(rec.srcr_flags & SRCR_ALL_SQUASH);

		uint32_t nf = rec.srcr_nflavors;

		if (nf > REFFS_CONFIG_MAX_FLAVORS)
			nf = REFFS_CONFIG_MAX_FLAVORS;
		rules[i].scr_nflavors = nf;
		for (uint32_t f = 0; f < nf; f++)
			rules[i].scr_flavors[f] =
				(enum reffs_auth_flavor)rec.srcr_flavors[f];
	}

	close(fd);

	super_block_set_client_rules(sb, rules, nrules);
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
			    "(not in registry -- may be stale or "
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
