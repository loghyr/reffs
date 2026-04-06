/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include "reffs/backend.h"
#include "reffs/super_block.h"

static int ram_sb_alloc(struct super_block *sb,
			const char *backend_path __attribute__((unused)))
{
	sb->sb_block_size = 4096;
	sb->sb_bytes_max = SIZE_MAX;
	sb->sb_inodes_max = SIZE_MAX;
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
};
