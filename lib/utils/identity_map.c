/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Bidirectional identity mapping table.
 *
 * Maps reffs_id values of different types to each other.  For example,
 * a Kerberos principal reffs_id(KRB5, 1, 42) can be mapped to
 * reffs_id(UNIX, 0, 1000).  Lookups work in either direction.
 *
 * Implementation: a lock-free hash table (cds_lfht) keyed by reffs_id.
 * Each entry stores (key, mapped_to).  A bidirectional mapping inserts
 * two entries: A-->B and B-->A.
 *
 * Persisted to <state_dir>/identity_map via write-temp/fsync/rename.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <urcu.h>
#include <urcu/rculfhash.h>

#include <xxhash.h>

#include "reffs/identity_map.h"
#include "reffs/log.h"

/* ------------------------------------------------------------------ */
/* Hash table entry                                                    */
/* ------------------------------------------------------------------ */

struct map_entry {
	struct cds_lfht_node me_node;
	struct rcu_head me_rcu;
	reffs_id me_key;
	reffs_id me_value;
};

static struct cds_lfht *map_ht;
static pthread_mutex_t map_persist_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Hash and match                                                      */
/* ------------------------------------------------------------------ */

static unsigned long map_hash(reffs_id id)
{
	return (unsigned long)XXH64(&id, sizeof(id), 0);
}

static int map_match(struct cds_lfht_node *node, const void *key)
{
	struct map_entry *e = caa_container_of(node, struct map_entry, me_node);
	const reffs_id *k = (const reffs_id *)key;

	return e->me_key == *k;
}

static void map_entry_free_rcu(struct rcu_head *head)
{
	struct map_entry *e = caa_container_of(head, struct map_entry, me_rcu);

	free(e);
}

/* ------------------------------------------------------------------ */
/* Init / fini                                                         */
/* ------------------------------------------------------------------ */

int identity_map_init(void)
{
	map_ht = cds_lfht_new(64, 64, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!map_ht) {
		LOG("identity_map_init: cds_lfht_new failed");
		return -1;
	}
	return 0;
}

void identity_map_fini(void)
{
	if (!map_ht)
		return;

	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	cds_lfht_for_each(map_ht, &iter, node)
	{
		struct map_entry *e =
			caa_container_of(node, struct map_entry, me_node);

		cds_lfht_del(map_ht, node);
		call_rcu(&e->me_rcu, map_entry_free_rcu);
	}
	rcu_read_unlock();

	synchronize_rcu();
	cds_lfht_destroy(map_ht, NULL);
	map_ht = NULL;
}

/* ------------------------------------------------------------------ */
/* Add / lookup                                                        */
/* ------------------------------------------------------------------ */

static int map_insert(reffs_id key, reffs_id value)
{
	struct map_entry *e = calloc(1, sizeof(*e));

	if (!e)
		return -ENOMEM;

	e->me_key = key;
	e->me_value = value;

	unsigned long hash = map_hash(key);

	rcu_read_lock();

	struct cds_lfht_node *existing =
		cds_lfht_add_unique(map_ht, hash, map_match, &key, &e->me_node);

	if (existing != &e->me_node) {
		/* Already exists -- update the value. */
		struct map_entry *old =
			caa_container_of(existing, struct map_entry, me_node);

		old->me_value = value;
		rcu_read_unlock();
		free(e);
		return 0;
	}

	rcu_read_unlock();
	return 0;
}

int identity_map_add(reffs_id a, reffs_id b)
{
	int ret;

	ret = map_insert(a, b);
	if (ret)
		return ret;

	ret = map_insert(b, a);
	return ret;
}

static struct map_entry *map_find(reffs_id key)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned long hash = map_hash(key);

	if (!map_ht)
		return NULL;

	rcu_read_lock();
	cds_lfht_lookup(map_ht, hash, map_match, &key, &iter);
	node = cds_lfht_iter_get_node(&iter);
	rcu_read_unlock();

	if (!node)
		return NULL;

	return caa_container_of(node, struct map_entry, me_node);
}

reffs_id identity_map_unix_for(reffs_id id)
{
	/* Already UNIX -- return directly. */
	if (REFFS_ID_IS_UNIX(id))
		return id;

	struct map_entry *e = map_find(id);

	if (e && REFFS_ID_IS_UNIX(e->me_value))
		return e->me_value;

	return REFFS_ID_NOBODY_VAL;
}

reffs_id identity_map_lookup(reffs_id id)
{
	struct map_entry *e = map_find(id);

	if (e)
		return e->me_value;

	return id;
}

int identity_map_remove(reffs_id key)
{
	struct map_entry *e = map_find(key);

	if (!e)
		return -ENOENT;

	reffs_id reverse_key = e->me_value;

	/* Remove forward mapping. */
	rcu_read_lock();
	cds_lfht_del(map_ht, &e->me_node);
	rcu_read_unlock();
	call_rcu(&e->me_rcu, map_entry_free_rcu);

	/* Remove reverse mapping. */
	struct map_entry *rev = map_find(reverse_key);

	if (rev) {
		rcu_read_lock();
		cds_lfht_del(map_ht, &rev->me_node);
		rcu_read_unlock();
		call_rcu(&rev->me_rcu, map_entry_free_rcu);
	}

	return 0;
}

int identity_map_iterate(int (*cb)(reffs_id key, reffs_id value, void *arg),
			 void *arg)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	int ret = 0;

	if (!map_ht || !cb)
		return 0;

	rcu_read_lock();
	cds_lfht_for_each(map_ht, &iter, node)
	{
		struct map_entry *e =
			caa_container_of(node, struct map_entry, me_node);

		ret = cb(e->me_key, e->me_value, arg);
		if (ret)
			break;
	}
	rcu_read_unlock();
	return ret;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

#define MAP_FILE "identity_map"
#define MAP_MAGIC 0x5246494D /* "RFIM" */
#define MAP_VERSION 1

struct map_disk_header {
	uint32_t mh_magic;
	uint32_t mh_version;
	uint32_t mh_count;
	uint32_t mh_reserved;
};

struct map_disk_entry {
	uint64_t md_key;
	uint64_t md_value;
};

int identity_map_persist(const char *state_dir)
{
	char path[512], tmp_path[520];

	if (snprintf(path, sizeof(path), "%s/%s", state_dir, MAP_FILE) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;

	if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >=
	    (int)sizeof(tmp_path))
		return -ENAMETOOLONG;

	/*
	 * Collect entries into a heap array outside rcu_read_lock.
	 * I/O is not permitted inside rcu_read_lock; restart the
	 * collection if realloc forces an unlock.
	 */
	struct map_disk_entry *entries = NULL;
	uint32_t count = 0, cap = 0;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	int ret = 0;

retry:
	count = 0;
	rcu_read_lock();
	cds_lfht_for_each(map_ht, &iter, node)
	{
		if (count == cap) {
			uint32_t newcap = cap ? cap * 2 : 64;
			rcu_read_unlock();
			struct map_disk_entry *grown =
				realloc(entries, newcap * sizeof(*entries));

			if (!grown) {
				free(entries);
				return -ENOMEM;
			}
			entries = grown;
			cap = newcap;
			goto retry;
		}
		struct map_entry *e =
			caa_container_of(node, struct map_entry, me_node);
		entries[count].md_key = e->me_key;
		entries[count].md_value = e->me_value;
		count++;
	}
	rcu_read_unlock();

	/*
	 * Serialize concurrent persist calls.  Open the tmp file inside
	 * the lock so two racing callers cannot both truncate the same
	 * inode before either has written its data.
	 */
	pthread_mutex_lock(&map_persist_lock);

	int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0) {
		ret = -errno;
		goto out_unlock;
	}

	struct map_disk_header hdr = {
		.mh_magic = MAP_MAGIC,
		.mh_version = MAP_VERSION,
		.mh_count = count,
	};

	ssize_t n = write(fd, &hdr, sizeof(hdr));
	if (n != (ssize_t)sizeof(hdr))
		goto err;

	for (uint32_t i = 0; i < count; i++) {
		n = write(fd, &entries[i], sizeof(entries[i]));
		if (n != (ssize_t)sizeof(entries[i]))
			goto err;
	}

	if (fdatasync(fd))
		goto err;

	close(fd);
	fd = -1;

	if (rename(tmp_path, path)) {
		ret = -errno;
		LOG("identity_map_persist: rename: %m");
		unlink(tmp_path);
	}

out_unlock:
	pthread_mutex_unlock(&map_persist_lock);
	free(entries);
	return ret;

err:
	pthread_mutex_unlock(&map_persist_lock);
	free(entries);
	close(fd);
	unlink(tmp_path);
	return -EIO;
}

int identity_map_load(const char *state_dir)
{
	char path[512];

	if (snprintf(path, sizeof(path), "%s/%s", state_dir, MAP_FILE) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;

	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return (errno == ENOENT) ? 0 : -errno;

	struct map_disk_header hdr;
	ssize_t n = read(fd, &hdr, sizeof(hdr));

	if (n != (ssize_t)sizeof(hdr) || hdr.mh_magic != MAP_MAGIC ||
	    hdr.mh_version != MAP_VERSION) {
		close(fd);
		return -EINVAL;
	}

	for (uint32_t i = 0; i < hdr.mh_count; i++) {
		struct map_disk_entry de;

		n = read(fd, &de, sizeof(de));
		if (n != (ssize_t)sizeof(de)) {
			close(fd);
			return -EIO;
		}

		map_insert(de.md_key, de.md_value);
	}

	close(fd);
	TRACE("identity_map: loaded %u mappings", hdr.mh_count);
	return 0;
}
