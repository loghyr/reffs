/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <urcu.h>
#include <urcu/rculfhash.h>
#include <xxhash.h>

#include "reffs/backend.h"
#include "reffs/super_block.h"

/*
 * RAM dstore reverse index (mirror-lifecycle Slice B'').
 *
 * Per-SB hash table keyed by (ds_id, inum) pairs, lock-free reads via
 * liburcu lfht.  Per-(sb, ds_id) lfht as the design suggests would give
 * O(matching) iter/count, but the cache `ds_instance_count` already
 * makes count O(1) on the probe path; iter is only called by the slice
 * E autopilot which is a cold scan.  One per-SB lfht with composite
 * keys is operationally identical and trivial to manage.
 *
 * The entries are leaf nodes (no embedded refs to outside objects), so
 * lifecycle is creation-ref-only per .claude/patterns/ref-counting.md
 * Rule 6.  Iterators advance before put per
 * .claude/patterns/rcu-violations.md Pattern 7.  The dstore_index_iter
 * callback runs UNDER rcu_read_lock and therefore must be non-blocking.
 */

struct ram_sb_private {
	struct cds_lfht *rsp_idx; /* dstore reverse index */
};

struct ram_idx_entry {
	struct cds_lfht_node rie_node;
	struct rcu_head rie_rcu;
	uint32_t rie_ds_id;
	uint64_t rie_inum;
};

#define RAM_IDX_BUCKETS 256

static unsigned long ram_idx_hash(uint32_t ds_id, uint64_t inum)
{
	uint8_t buf[12];

	memcpy(buf, &ds_id, sizeof(ds_id));
	memcpy(buf + sizeof(ds_id), &inum, sizeof(inum));
	return (unsigned long)XXH3_64bits(buf, sizeof(buf));
}

static int ram_idx_match(struct cds_lfht_node *node, const void *key)
{
	const struct ram_idx_entry *e =
		caa_container_of(node, struct ram_idx_entry, rie_node);
	const struct ram_idx_entry *k = key;

	return e->rie_ds_id == k->rie_ds_id && e->rie_inum == k->rie_inum;
}

static void ram_idx_free_rcu(struct rcu_head *head)
{
	struct ram_idx_entry *e =
		caa_container_of(head, struct ram_idx_entry, rie_rcu);
	free(e);
}

static int ram_sb_alloc(struct super_block *sb,
			const char *backend_path __attribute__((unused)))
{
	struct ram_sb_private *priv;

	priv = calloc(1, sizeof(*priv));
	if (!priv)
		return -ENOMEM;
	priv->rsp_idx = cds_lfht_new(RAM_IDX_BUCKETS, RAM_IDX_BUCKETS, 0,
				     CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING,
				     NULL);
	if (!priv->rsp_idx) {
		free(priv);
		return -ENOMEM;
	}
	sb->sb_storage_private = priv;
	sb->sb_block_size = 4096;
	sb->sb_bytes_max = SIZE_MAX;
	sb->sb_inodes_max = SIZE_MAX;
	return 0;
}

static void ram_sb_free(struct super_block *sb)
{
	struct ram_sb_private *priv = sb->sb_storage_private;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	if (!priv)
		return;

	if (priv->rsp_idx) {
		rcu_read_lock();
		cds_lfht_first(priv->rsp_idx, &iter);
		while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
			struct ram_idx_entry *e = caa_container_of(
				node, struct ram_idx_entry, rie_node);

			cds_lfht_next(priv->rsp_idx, &iter);
			cds_lfht_del(priv->rsp_idx, node);
			call_rcu(&e->rie_rcu, ram_idx_free_rcu);
		}
		rcu_read_unlock();
		synchronize_rcu();
		cds_lfht_destroy(priv->rsp_idx, NULL);
		priv->rsp_idx = NULL;
	}
	free(priv);
	sb->sb_storage_private = NULL;
}

static int ram_dstore_index_add(struct super_block *sb, uint32_t ds_id,
				uint64_t inum)
{
	struct ram_sb_private *priv = sb->sb_storage_private;
	struct ram_idx_entry *e;
	struct ram_idx_entry key = { .rie_ds_id = ds_id, .rie_inum = inum };
	struct cds_lfht_node *existing;

	if (!priv || !priv->rsp_idx)
		return -EINVAL;

	e = calloc(1, sizeof(*e));
	if (!e)
		return -ENOMEM;
	e->rie_ds_id = ds_id;
	e->rie_inum = inum;
	cds_lfht_node_init(&e->rie_node);

	rcu_read_lock();
	existing = cds_lfht_add_unique(priv->rsp_idx, ram_idx_hash(ds_id, inum),
				       ram_idx_match, &key, &e->rie_node);
	rcu_read_unlock();

	if (existing != &e->rie_node) {
		/* Already present -- caller's add is idempotent. */
		free(e);
		return -EEXIST;
	}
	/*
	 * Cache `dstore->ds_instance_count` bumps deferred to slice C/D
	 * caller -- the higher-level helper that owns the
	 * placement-vs-index transaction will own the cache too.
	 * See .claude/design/mirror-lifecycle.md "Hot-path cache".
	 */
	return 0;
}

static int ram_dstore_index_remove(struct super_block *sb, uint32_t ds_id,
				   uint64_t inum)
{
	struct ram_sb_private *priv = sb->sb_storage_private;
	struct ram_idx_entry key = { .rie_ds_id = ds_id, .rie_inum = inum };
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct ram_idx_entry *e;

	if (!priv || !priv->rsp_idx)
		return -EINVAL;

	rcu_read_lock();
	cds_lfht_lookup(priv->rsp_idx, ram_idx_hash(ds_id, inum), ram_idx_match,
			&key, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (!node) {
		rcu_read_unlock();
		return -ENOENT;
	}
	e = caa_container_of(node, struct ram_idx_entry, rie_node);
	cds_lfht_del(priv->rsp_idx, node);
	rcu_read_unlock();
	call_rcu(&e->rie_rcu, ram_idx_free_rcu);
	/* Cache decrement deferred -- matches ram_dstore_index_add. */
	return 0;
}

static int ram_dstore_index_iter(struct super_block *sb, uint32_t ds_id,
				 int (*cb)(uint32_t, uint64_t, void *),
				 void *arg)
{
	struct ram_sb_private *priv = sb->sb_storage_private;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	int ret = 0;

	if (!priv || !priv->rsp_idx)
		return -EINVAL;
	if (!cb)
		return -EINVAL;

	rcu_read_lock();
	cds_lfht_first(priv->rsp_idx, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct ram_idx_entry *e =
			caa_container_of(node, struct ram_idx_entry, rie_node);

		cds_lfht_next(priv->rsp_idx, &iter);
		if (e->rie_ds_id == ds_id) {
			ret = cb(e->rie_ds_id, e->rie_inum, arg);
			if (ret)
				break;
		}
	}
	rcu_read_unlock();
	return ret;
}

static int ram_dstore_index_count(struct super_block *sb, uint32_t ds_id,
				  uint64_t *count_out)
{
	struct ram_sb_private *priv = sb->sb_storage_private;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	uint64_t n = 0;

	if (!priv || !priv->rsp_idx || !count_out)
		return -EINVAL;

	rcu_read_lock();
	cds_lfht_first(priv->rsp_idx, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct ram_idx_entry *e =
			caa_container_of(node, struct ram_idx_entry, rie_node);

		if (e->rie_ds_id == ds_id)
			n++;
		cds_lfht_next(priv->rsp_idx, &iter);
	}
	rcu_read_unlock();
	*count_out = n;
	return 0;
}

/*
 * ram_storage_ops -- md template for RAM metadata backend.
 *
 * Data function pointers (db_*) are intentionally NULL here.
 * They are populated by the composer from the data backend template.
 */
const struct reffs_storage_ops ram_storage_ops = {
	.type = REFFS_STORAGE_RAM,
	.name = "ram",
	.sb_alloc = ram_sb_alloc,
	.sb_free = ram_sb_free,
	.dstore_index_add = ram_dstore_index_add,
	.dstore_index_remove = ram_dstore_index_remove,
	.dstore_index_iter = ram_dstore_index_iter,
	.dstore_index_count = ram_dstore_index_count,
};
