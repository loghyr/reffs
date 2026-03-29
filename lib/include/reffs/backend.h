/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_BACKEND_H
#define _REFFS_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "reffs/types.h"

struct super_block;
struct inode;
struct data_block;
struct chunk_block;

/*
 * Backend composition: metadata and data are independent axes.
 * reffs_backend_compose() builds a single reffs_storage_ops from
 * one md template and one data template.
 */
enum reffs_md_type {
	REFFS_MD_RAM = 0,
	REFFS_MD_POSIX = 1,
	REFFS_MD_ROCKSDB = 2,
};

enum reffs_data_type {
	REFFS_DATA_RAM = 0,
	REFFS_DATA_POSIX = 1,
};

#define REFFS_DISK_MAGIC_META 0x5245464d /* 'REFM' */
#define REFFS_DISK_MAGIC_DIR 0x52454644 /* 'REFD' */
#define REFFS_DISK_MAGIC_DAT 0x52454641 /* 'REFA' (data) */
#define REFFS_DISK_MAGIC_LNK 0x5245464c /* 'REFL' */
#define REFFS_DISK_MAGIC_LAY 0x52454659 /* 'REFY' (layouts) */

#define REFFS_DISK_VERSION_1 1

struct reffs_disk_header {
	uint32_t rdh_magic;
	uint32_t rdh_version;
};

struct reffs_storage_ops {
	enum reffs_storage_type type;
	const char *name;

	/* Superblock operations */
	int (*sb_alloc)(struct super_block *sb, const char *backend_path);
	void (*sb_free)(struct super_block *sb);

	/* Inode operations */
	int (*inode_alloc)(struct inode *inode);
	void (*inode_free)(struct inode *inode);
	void (*inode_sync)(struct inode *inode);

	/* Data block operations */
	int (*db_alloc)(struct data_block *db, struct inode *inode,
			const char *buffer, size_t size, off_t offset);
	void (*db_free)(struct data_block *db);
	void (*db_release_resources)(struct data_block *db);
	ssize_t (*db_read)(struct data_block *db, char *buffer, size_t size,
			   off_t offset);
	ssize_t (*db_write)(struct data_block *db, const char *buffer,
			    size_t size, off_t offset);
	ssize_t (*db_resize)(struct data_block *db, size_t size);
	size_t (*db_get_size)(struct data_block *db);
	int (*db_get_fd)(struct data_block *db); /* -1 if not fd-backed */

	/* Directory operations */
	void (*dir_sync)(struct inode *inode);

	/*
	 * Scan the on-disk directory for dir_ino looking for an entry whose
	 * child ino matches child_ino.  On success fills name_out (NUL-
	 * terminated, truncated to name_max-1) and *cookie_out, returns 0.
	 * Returns ENOENT if not found, or an errno on I/O error.
	 *
	 * This is a cold-path helper used by inode_reconstruct_path_to_root()
	 * to recover the name of an inode whose dirent was evicted.
	 */
	int (*dir_find_entry_by_ino)(struct super_block *sb, uint64_t dir_ino,
				     uint64_t child_ino, char *name_out,
				     size_t name_max, uint64_t *cookie_out);

	/*
	 * Scan the on-disk directory for dir_ino looking for an entry whose
	 * name matches name.  On success fills *child_ino_out and
	 * *cookie_out, returns 0.
	 * Returns ENOENT if not found, or an errno on I/O error.
	 *
	 * Used by dirent_load_child_by_name() for LOOKUP misses when the
	 * directory is not fully resident in memory.
	 */
	int (*dir_find_entry_by_name)(struct super_block *sb, uint64_t dir_ino,
				      const char *name, uint64_t *child_ino_out,
				      uint64_t *cookie_out);

	/*
	 * Recover the in-memory inode/dirent tree from persistent storage.
	 * Called after super_block_dirent_create() has created the root
	 * inode/dirent.  The md backend walks its storage to rebuild the
	 * directory tree (e.g., POSIX reads .dir files, RocksDB iterates
	 * the dirs CF).  If NULL, no recovery is performed (RAM backend).
	 *
	 * The root inode's fields are loaded by calling inode_alloc on
	 * sb->sb_root_inode before walking directories.
	 */
	void (*recover)(struct super_block *sb);

	/*
	 * Persist chunk block metadata for an inode.  If NULL, chunk_store
	 * falls back to its existing flat-file persistence.
	 */
	int (*chunk_persist)(struct super_block *sb, uint64_t ino,
			     const struct chunk_block *blocks, uint32_t nblocks,
			     uint32_t chunk_size);

	/*
	 * Load chunk block metadata for an inode.  Returns 0 and sets
	 * *blocks_out and *nblocks_out on success, -ENOENT if no chunks
	 * stored, other -errno on error.  If NULL, chunk_store falls
	 * back to flat-file loading.
	 */
	int (*chunk_load)(struct super_block *sb, uint64_t ino,
			  struct chunk_block **blocks_out,
			  uint32_t *nblocks_out, uint32_t *chunk_size_out);
};

/*
 * Composed ops — heap-allocated by reffs_backend_compose().
 * Contains the public vtable plus internal function pointers
 * for the inode_free composition (md cleanup + data cleanup).
 */
struct composed_ops {
	struct reffs_storage_ops co_ops;
	void (*co_md_inode_free)(struct inode *);
	void (*co_data_inode_cleanup)(struct inode *);
};

void reffs_backend_init(void);

/*
 * Compose a reffs_storage_ops from independent md + data backends.
 * Returns a heap-allocated ops struct (caller frees with
 * reffs_backend_free_ops).  Returns NULL on invalid combination.
 *
 * Constraints:
 *   md=RAM   -> data=RAM    (all-volatile)
 *   md!=RAM  -> data!=RAM   (no partial persistence)
 */
struct reffs_storage_ops *reffs_backend_compose(enum reffs_md_type md,
						enum reffs_data_type data);

void reffs_backend_free_ops(const struct reffs_storage_ops *ops);

/* ------------------------------------------------------------------ */
/* POSIX data backend — declarations for composition                  */
/* ------------------------------------------------------------------ */

int posix_data_db_alloc(struct data_block *db, struct inode *inode,
			const char *buffer, size_t size, off_t offset);
void posix_data_db_free(struct data_block *db);
void posix_data_db_release_resources(struct data_block *db);
ssize_t posix_data_db_read(struct data_block *db, char *buffer, size_t size,
			   off_t offset);
ssize_t posix_data_db_write(struct data_block *db, const char *buffer,
			    size_t size, off_t offset);
ssize_t posix_data_db_resize(struct data_block *db, size_t size);
size_t posix_data_db_get_size(struct data_block *db);
int posix_data_db_get_fd(struct data_block *db);
void posix_data_inode_cleanup(struct inode *inode);

/* ------------------------------------------------------------------ */
/* RAM data backend — declarations for composition                    */
/* ------------------------------------------------------------------ */

int ram_data_db_alloc(struct data_block *db, struct inode *inode,
		      const char *buffer, size_t size, off_t offset);
void ram_data_db_free(struct data_block *db);
ssize_t ram_data_db_read(struct data_block *db, char *buffer, size_t size,
			 off_t offset);
ssize_t ram_data_db_write(struct data_block *db, const char *buffer,
			  size_t size, off_t offset);
ssize_t ram_data_db_resize(struct data_block *db, size_t size);
size_t ram_data_db_get_size(struct data_block *db);
int ram_data_db_get_fd(struct data_block *db);
void ram_data_inode_cleanup(struct inode *inode);

#endif /* _REFFS_BACKEND_H */
