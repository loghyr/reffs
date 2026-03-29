/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * RocksDB metadata backend — stub.
 *
 * This file provides the rocksdb_storage_ops md template.
 * Implementation of individual ops is in Steps 3-9 of the
 * RocksDB backend plan (.claude/design/rocksdb-backend.md).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>

#include "reffs/backend.h"
#include "reffs/super_block.h"
#include "reffs/log.h"

static int rocksdb_sb_alloc(struct super_block *sb __attribute__((unused)),
			    const char *backend_path __attribute__((unused)))
{
	LOG("RocksDB backend not yet implemented");
	return -ENOSYS;
}

/*
 * rocksdb_storage_ops — md template for RocksDB metadata backend.
 *
 * Only sb_alloc is wired (returns ENOSYS).  All other md ops are
 * NULL until Steps 3-9 implement them.  Data ops are populated by
 * the composer from the POSIX data template.
 */
const struct reffs_storage_ops rocksdb_storage_ops = {
	.type = REFFS_STORAGE_ROCKSDB,
	.name = "rocksdb",
	.sb_alloc = rocksdb_sb_alloc,
};
