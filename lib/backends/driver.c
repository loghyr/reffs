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
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "reffs/backend.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/log.h"

extern const struct reffs_storage_ops ram_storage_ops;
extern const struct reffs_storage_ops posix_storage_ops;

/* ------------------------------------------------------------------ */
/* Metadata templates (md axis)                                       */
/* ------------------------------------------------------------------ */

/*
 * Each md template provides: sb_alloc, sb_free, inode_alloc, inode_free,
 * inode_sync, dir_sync, dir_find_entry_by_ino, dir_find_entry_by_name.
 *
 * inode_free here is the md-side only cleanup (e.g., unlink .meta/.dir/.lnk
 * for POSIX, delete RocksDB keys for RocksDB).  The composer wraps it
 * with data-side cleanup.
 */
#ifdef HAVE_ROCKSDB
extern const struct reffs_storage_ops rocksdb_storage_ops;
#endif

static const struct reffs_storage_ops *md_templates[] = {
	[REFFS_MD_RAM] = &ram_storage_ops,
	[REFFS_MD_POSIX] = &posix_storage_ops,
#ifdef HAVE_ROCKSDB
	[REFFS_MD_ROCKSDB] = &rocksdb_storage_ops,
#endif
};

/* ------------------------------------------------------------------ */
/* Composed inode_free trampoline                                     */
/* ------------------------------------------------------------------ */

/*
 * The composed inode_free calls the md-side cleanup, then the
 * data-side cleanup.  It recovers the raw function pointers from
 * the composed_ops struct that contains the public vtable.
 */
static void composed_inode_free(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	const struct composed_ops *co = (const struct composed_ops *)sb->sb_ops;

	if (co->co_md_inode_free)
		co->co_md_inode_free(inode);
	if (co->co_data_inode_cleanup)
		co->co_data_inode_cleanup(inode);
}

/* ------------------------------------------------------------------ */
/* Composer                                                           */
/* ------------------------------------------------------------------ */

struct reffs_storage_ops *reffs_backend_compose(enum reffs_md_type md,
						enum reffs_data_type data)
{
	/* Constraint: md=RAM <-> data=RAM */
	if (md == REFFS_MD_RAM && data != REFFS_DATA_RAM) {
		LOG("Invalid backend composition: RAM md requires RAM data");
		return NULL;
	}
	if (md != REFFS_MD_RAM && data == REFFS_DATA_RAM) {
		LOG("Invalid backend composition: non-RAM md requires non-RAM data");
		return NULL;
	}

	if (md >= (sizeof(md_templates) / sizeof(md_templates[0])) ||
	    !md_templates[md]) {
		LOG("Unknown md backend type: %d", md);
		return NULL;
	}

	struct composed_ops *co = calloc(1, sizeof(*co));
	if (!co)
		return NULL;

	/* Copy md function pointers from the md template */
	const struct reffs_storage_ops *md_ops = md_templates[md];
	co->co_ops.sb_alloc = md_ops->sb_alloc;
	co->co_ops.sb_free = md_ops->sb_free;
	co->co_ops.inode_alloc = md_ops->inode_alloc;
	co->co_ops.inode_sync = md_ops->inode_sync;
	co->co_ops.dir_sync = md_ops->dir_sync;
	co->co_ops.dir_find_entry_by_ino = md_ops->dir_find_entry_by_ino;
	co->co_ops.dir_find_entry_by_name = md_ops->dir_find_entry_by_name;
	co->co_ops.recover = md_ops->recover;
	co->co_ops.chunk_persist = md_ops->chunk_persist;
	co->co_ops.chunk_load = md_ops->chunk_load;

	/* Copy data function pointers from the data template */
	switch (data) {
	case REFFS_DATA_RAM:
		co->co_ops.db_alloc = ram_data_db_alloc;
		co->co_ops.db_free = ram_data_db_free;
		co->co_ops.db_release_resources = NULL;
		co->co_ops.db_read = ram_data_db_read;
		co->co_ops.db_write = ram_data_db_write;
		co->co_ops.db_resize = ram_data_db_resize;
		co->co_ops.db_get_size = ram_data_db_get_size;
		co->co_ops.db_get_fd = ram_data_db_get_fd;
		co->co_data_inode_cleanup = ram_data_inode_cleanup;
		break;
	case REFFS_DATA_POSIX:
		co->co_ops.db_alloc = posix_data_db_alloc;
		co->co_ops.db_free = posix_data_db_free;
		co->co_ops.db_release_resources =
			posix_data_db_release_resources;
		co->co_ops.db_read = posix_data_db_read;
		co->co_ops.db_write = posix_data_db_write;
		co->co_ops.db_resize = posix_data_db_resize;
		co->co_ops.db_get_size = posix_data_db_get_size;
		co->co_ops.db_get_fd = posix_data_db_get_fd;
		co->co_data_inode_cleanup = posix_data_inode_cleanup;
		break;
	default:
		LOG("Unknown data backend type: %d", data);
		free(co);
		return NULL;
	}

	/* Compose inode_free: md cleanup + data cleanup */
	co->co_md_inode_free = md_ops->inode_free;
	co->co_ops.inode_free = composed_inode_free;

	/*
	 * Set type to the storage type that matches this composition.
	 * Used as discriminant in recovery dispatch and wire/config.
	 */
	switch (md) {
	case REFFS_MD_RAM:
		co->co_ops.type = REFFS_STORAGE_RAM;
		co->co_ops.name = "ram+ram";
		break;
	case REFFS_MD_POSIX:
		co->co_ops.type = REFFS_STORAGE_POSIX;
		co->co_ops.name = "posix+posix";
		break;
	case REFFS_MD_ROCKSDB:
		co->co_ops.type = REFFS_STORAGE_ROCKSDB;
		co->co_ops.name = "rocksdb+posix";
		break;
	}

	return &co->co_ops;
}

void reffs_backend_free_ops(const struct reffs_storage_ops *ops)
{
	if (!ops)
		return;

	/*
	 * Recover the composed_ops container.  The co_ops field is at
	 * offset 0, so the pointer is the same.
	 */
	free((void *)ops);
}

void reffs_backend_init(void)
{
	/* Any global initialization for backends can go here */
}
