/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * RocksDB metadata backend.
 *
 * Stores inode metadata, directory entries, symlink targets, layout
 * segments, and chunk metadata in a per-superblock RocksDB database
 * at <backend_path>/sb_<id>/md.rocksdb/.  Bulk file data stays in
 * POSIX files via the data backend (composed by driver.c).
 *
 * Column families:
 *   default  — sb metadata (next_ino)
 *   inodes   — inode_disk keyed by ino
 *   dirs     — directory entries keyed by parent_ino + cookie
 *   symlinks — symlink targets keyed by ino
 *   layouts  — layout segments keyed by ino
 *   chunks   — chunk block metadata keyed by ino + offset
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <rocksdb/c.h>

#include "reffs/backend.h"
#include "reffs/data_block.h"
#include "reffs/dirent.h"
#include "reffs/inode.h"
#include "reffs/layout_segment.h"
#include "reffs/log.h"
#include "reffs/super_block.h"
#include "reffs/trace/fs.h"

#include "rocksdb_keys.h"

/* ------------------------------------------------------------------ */
/* Error handling                                                      */
/* ------------------------------------------------------------------ */

#define ROCKSDB_CHECK_ERR(err, ret_val, label)   \
	do {                                     \
		if (err) {                       \
			LOG("rocksdb: %s", err); \
			rocksdb_free(err);       \
			err = NULL;              \
			ret = (ret_val);         \
			goto label;              \
		}                                \
	} while (0)

/* ------------------------------------------------------------------ */
/* Column family indices                                               */
/* ------------------------------------------------------------------ */

enum rocksdb_cf {
	ROCKSDB_CF_DEFAULT = 0,
	ROCKSDB_CF_INODES,
	ROCKSDB_CF_DIRS,
	ROCKSDB_CF_SYMLINKS,
	ROCKSDB_CF_LAYOUTS,
	ROCKSDB_CF_CHUNKS,
	ROCKSDB_CF_COUNT,
};

static const char *cf_names[ROCKSDB_CF_COUNT] = {
	[ROCKSDB_CF_DEFAULT] = "default", [ROCKSDB_CF_INODES] = "inodes",
	[ROCKSDB_CF_DIRS] = "dirs",	  [ROCKSDB_CF_SYMLINKS] = "symlinks",
	[ROCKSDB_CF_LAYOUTS] = "layouts", [ROCKSDB_CF_CHUNKS] = "chunks",
};

/* ------------------------------------------------------------------ */
/* Per-sb private data                                                 */
/* ------------------------------------------------------------------ */

struct rocksdb_sb_private {
	rocksdb_t *rsp_db;
	rocksdb_column_family_handle_t *rsp_cf[ROCKSDB_CF_COUNT];
	rocksdb_options_t *rsp_opts;
	rocksdb_writeoptions_t *rsp_wopts; /* sync=true for metadata */
	rocksdb_writeoptions_t *rsp_wopts_nosync; /* for bulk writes */
	rocksdb_readoptions_t *rsp_ropts;
	char *rsp_path;
};

/* sb_meta keys in default CF */
static const char sb_meta_next_ino[] = "sb_next_ino";
#define SB_META_NEXT_INO_LEN (sizeof(sb_meta_next_ino) - 1)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static struct rocksdb_sb_private *get_priv(struct super_block *sb)
{
	return (struct rocksdb_sb_private *)sb->sb_storage_private;
}

/*
 * Ensure the sb directory and md.rocksdb subdirectory exist.
 * The sb directory is shared with the POSIX data backend (.dat files).
 */
static int ensure_dirs(const char *backend_path, uint64_t sb_id, char *md_path,
		       size_t md_path_sz)
{
	char sb_dir[PATH_MAX];
	int n;

	n = snprintf(sb_dir, sizeof(sb_dir), "%s/sb_%lu", backend_path,
		     (unsigned long)sb_id);
	if (n < 0 || (size_t)n >= sizeof(sb_dir))
		return -ENAMETOOLONG;

	if (mkdir(sb_dir, 0755) < 0 && errno != EEXIST)
		return -errno;

	n = snprintf(md_path, md_path_sz, "%s/md.rocksdb", sb_dir);
	if (n < 0 || (size_t)n >= md_path_sz)
		return -ENAMETOOLONG;

	return 0;
}

/* ------------------------------------------------------------------ */
/* sb_alloc / sb_free                                                  */
/* ------------------------------------------------------------------ */

static int rocksdb_sb_alloc(struct super_block *sb, const char *backend_path)
{
	struct rocksdb_sb_private *priv;
	char md_path[PATH_MAX];
	char *err = NULL;
	int ret;

	ret = ensure_dirs(backend_path, sb->sb_id, md_path, sizeof(md_path));
	if (ret)
		return ret;

	priv = calloc(1, sizeof(*priv));
	if (!priv)
		return -ENOMEM;

	priv->rsp_path = strdup(md_path);
	if (!priv->rsp_path) {
		free(priv);
		return -ENOMEM;
	}

	/* Create options */
	priv->rsp_opts = rocksdb_options_create();
	rocksdb_options_set_create_if_missing(priv->rsp_opts, 1);
	rocksdb_options_set_create_missing_column_families(priv->rsp_opts, 1);

	/* Write options: sync for metadata */
	priv->rsp_wopts = rocksdb_writeoptions_create();
	rocksdb_writeoptions_set_sync(priv->rsp_wopts, 1);

	/* Write options: nosync for bulk */
	priv->rsp_wopts_nosync = rocksdb_writeoptions_create();
	rocksdb_writeoptions_set_sync(priv->rsp_wopts_nosync, 0);

	/* Read options */
	priv->rsp_ropts = rocksdb_readoptions_create();

	/* Open with all column families */
	rocksdb_options_t *cf_opts[ROCKSDB_CF_COUNT];
	for (int i = 0; i < ROCKSDB_CF_COUNT; i++)
		cf_opts[i] = priv->rsp_opts;

	priv->rsp_db = rocksdb_open_column_families(
		priv->rsp_opts, md_path, ROCKSDB_CF_COUNT, cf_names,
		(const rocksdb_options_t *const *)cf_opts, priv->rsp_cf, &err);

	ret = -EIO;
	ROCKSDB_CHECK_ERR(err, -EIO, err_opts);

	/*
	 * Do NOT load sb_next_ino here.  super_block_dirent_create runs
	 * after sb_alloc and allocates the root inode from sb_next_ino.
	 * If we restore a high value here, the root inode gets the wrong
	 * ino (e.g., 28259 instead of 1) and PUTROOTFH returns ESTALE.
	 * sb_next_ino is restored in rocksdb_recover() after the root
	 * dirent is created.
	 */

	/* Filesystem stats from statvfs of the backend path */
	struct statvfs sv;
	if (statvfs(backend_path, &sv) == 0) {
		sb->sb_block_size = sv.f_bsize;
		sb->sb_bytes_max = (size_t)sv.f_blocks * sv.f_frsize;
		sb->sb_inodes_max = sv.f_files;
	} else {
		sb->sb_block_size = 4096;
	}

	sb->sb_storage_private = priv;
	return 0;

err_opts:
	rocksdb_readoptions_destroy(priv->rsp_ropts);
	rocksdb_writeoptions_destroy(priv->rsp_wopts_nosync);
	rocksdb_writeoptions_destroy(priv->rsp_wopts);
	rocksdb_options_destroy(priv->rsp_opts);
	free(priv->rsp_path);
	free(priv);
	return ret;
}

static void rocksdb_sb_free(struct super_block *sb)
{
	struct rocksdb_sb_private *priv = get_priv(sb);
	if (!priv)
		return;

	for (int i = 0; i < ROCKSDB_CF_COUNT; i++) {
		if (priv->rsp_cf[i])
			rocksdb_column_family_handle_destroy(priv->rsp_cf[i]);
	}

	if (priv->rsp_db)
		rocksdb_close(priv->rsp_db);

	if (priv->rsp_ropts)
		rocksdb_readoptions_destroy(priv->rsp_ropts);
	if (priv->rsp_wopts_nosync)
		rocksdb_writeoptions_destroy(priv->rsp_wopts_nosync);
	if (priv->rsp_wopts)
		rocksdb_writeoptions_destroy(priv->rsp_wopts);
	if (priv->rsp_opts)
		rocksdb_options_destroy(priv->rsp_opts);

	free(priv->rsp_path);
	free(priv);
	sb->sb_storage_private = NULL;
}

/* ------------------------------------------------------------------ */
/* inode_sync — persist inode metadata to RocksDB                      */
/* ------------------------------------------------------------------ */

static void rocksdb_inode_sync(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct rocksdb_sb_private *priv = get_priv(sb);
	if (!priv)
		return;

	trace_fs_inode(inode, __func__, __LINE__);

	char *err = NULL;
	int ret __attribute__((unused));
	rocksdb_writebatch_t *batch = rocksdb_writebatch_create();

	/* Inode metadata */
	struct inode_disk id = {
		.id_uid = inode->i_uid,
		.id_gid = inode->i_gid,
		.id_nlink = inode->i_nlink,
		.id_mode = inode->i_mode,
		.id_size = inode->i_size,
		.id_atime = inode->i_atime,
		.id_ctime = inode->i_ctime,
		.id_mtime = inode->i_mtime,
		.id_btime = inode->i_btime,
		.id_changeid = atomic_load_explicit(&inode->i_changeid,
						    memory_order_relaxed),
		.id_attr_flags = inode->i_attr_flags,
		.id_parent_ino = inode->i_parent_ino,
		.id_dev_major = inode->i_dev_major,
		.id_dev_minor = inode->i_dev_minor,
		.id_sec_label_lfs = inode->i_sec_label_lfs,
		.id_sec_label_pi = inode->i_sec_label_pi,
		.id_sec_label_len =
			(inode->i_sec_label_len > REFFS_SEC_LABEL_MAX) ?
				REFFS_SEC_LABEL_MAX :
				inode->i_sec_label_len,
	};

	if (id.id_sec_label_len > 0)
		memcpy(id.id_sec_label, inode->i_sec_label,
		       id.id_sec_label_len);

	uint8_t key[ROCKSDB_KEY_MAX_SIZE];
	size_t klen = rocksdb_key_ino(key, inode->i_ino);

	rocksdb_writebatch_put_cf(batch, priv->rsp_cf[ROCKSDB_CF_INODES],
				  (const char *)key, klen, (const char *)&id,
				  sizeof(id));

	/* Symlink target */
	if (S_ISLNK(inode->i_mode) && inode->i_symlink) {
		klen = rocksdb_key_lnk(key, inode->i_ino);
		rocksdb_writebatch_put_cf(batch,
					  priv->rsp_cf[ROCKSDB_CF_SYMLINKS],
					  (const char *)key, klen,
					  inode->i_symlink,
					  strlen(inode->i_symlink));
	}

	/* Layout segments (MDS mode) */
	if (inode->i_layout_segments &&
	    inode->i_layout_segments->lss_count > 0) {
		struct layout_segments *lss = inode->i_layout_segments;

		/*
		 * Serialize layout segments in the same binary format as
		 * the POSIX backend's .layouts file (minus the disk header).
		 */
		size_t buf_sz = sizeof(uint32_t); /* count */
		for (uint32_t s = 0; s < lss->lss_count; s++) {
			buf_sz += sizeof(struct layout_segment_disk);
			buf_sz += lss->lss_segs[s].ls_nfiles *
				  sizeof(struct layout_data_file_disk);
		}

		char *buf = malloc(buf_sz);
		if (buf) {
			char *p = buf;
			memcpy(p, &lss->lss_count, sizeof(uint32_t));
			p += sizeof(uint32_t);

			for (uint32_t s = 0; s < lss->lss_count; s++) {
				struct layout_segment *seg = &lss->lss_segs[s];
				struct layout_segment_disk lsd = {
					.ls_offset = seg->ls_offset,
					.ls_length = seg->ls_length,
					.ls_stripe_unit = seg->ls_stripe_unit,
					.ls_k = seg->ls_k,
					.ls_m = seg->ls_m,
					.ls_nfiles = seg->ls_nfiles,
					.ls_layout_type = seg->ls_layout_type,
				};
				memcpy(p, &lsd, sizeof(lsd));
				p += sizeof(lsd);

				for (uint32_t f = 0; f < seg->ls_nfiles; f++) {
					struct layout_data_file *ldf =
						&seg->ls_files[f];
					struct layout_data_file_disk ldfd = {
						.ldf_dstore_id =
							ldf->ldf_dstore_id,
						.ldf_fh_len = ldf->ldf_fh_len,
						.ldf_size = ldf->ldf_size,
						.ldf_atime = ldf->ldf_atime,
						.ldf_mtime = ldf->ldf_mtime,
						.ldf_ctime = ldf->ldf_ctime,
						.ldf_uid = ldf->ldf_uid,
						.ldf_gid = ldf->ldf_gid,
						.ldf_mode = ldf->ldf_mode,
					};
					memcpy(ldfd.ldf_fh, ldf->ldf_fh,
					       ldf->ldf_fh_len);
					memcpy(p, &ldfd, sizeof(ldfd));
					p += sizeof(ldfd);
				}
			}

			klen = rocksdb_key_lay(key, inode->i_ino);
			rocksdb_writebatch_put_cf(
				batch, priv->rsp_cf[ROCKSDB_CF_LAYOUTS],
				(const char *)key, klen, buf,
				(size_t)(p - buf));
			free(buf);
		}
	}

	/* Update sb_next_ino */
	uint64_t next_ino = sb->sb_next_ino;
	rocksdb_writebatch_put_cf(batch, priv->rsp_cf[ROCKSDB_CF_DEFAULT],
				  sb_meta_next_ino, SB_META_NEXT_INO_LEN,
				  (const char *)&next_ino, sizeof(next_ino));

	/* Atomic write */
	rocksdb_write(priv->rsp_db, priv->rsp_wopts, batch, &err);
	if (err) {
		LOG("rocksdb inode_sync ino %" PRIu64 ": %s", inode->i_ino,
		    err);
		rocksdb_free(err);
	}

	rocksdb_writebatch_destroy(batch);
}

/* ------------------------------------------------------------------ */
/* inode_alloc — load inode metadata from RocksDB                      */
/* ------------------------------------------------------------------ */

static int rocksdb_inode_alloc(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct rocksdb_sb_private *priv = get_priv(sb);
	if (!priv)
		return -EINVAL;

	trace_fs_inode(inode, __func__, __LINE__);

	char *err = NULL;
	int ret = 0;

	/* Load inode metadata */
	uint8_t key[ROCKSDB_KEY_MAX_SIZE];
	size_t klen = rocksdb_key_ino(key, inode->i_ino);
	size_t vlen = 0;
	char *val = rocksdb_get_cf(priv->rsp_db, priv->rsp_ropts,
				   priv->rsp_cf[ROCKSDB_CF_INODES],
				   (const char *)key, klen, &vlen, &err);
	ROCKSDB_CHECK_ERR(err, -EIO, out);

	if (!val)
		return 0; /* New inode, nothing to load */

	if (vlen != sizeof(struct inode_disk)) {
		LOG("rocksdb: bad inode_disk size for ino %" PRIu64
		    ": got %zu, expected %zu",
		    inode->i_ino, vlen, sizeof(struct inode_disk));
		rocksdb_free(val);
		return -EINVAL;
	}

	struct inode_disk id;
	memcpy(&id, val, sizeof(id));
	rocksdb_free(val);

	inode->i_uid = id.id_uid;
	inode->i_gid = id.id_gid;
	inode->i_nlink = id.id_nlink;
	inode->i_mode = id.id_mode;
	inode->i_size = id.id_size;
	inode->i_atime = id.id_atime;
	inode->i_ctime = id.id_ctime;
	inode->i_mtime = id.id_mtime;
	inode->i_btime = id.id_btime;
	atomic_store_explicit(&inode->i_changeid, id.id_changeid,
			      memory_order_relaxed);
	inode->i_attr_flags = id.id_attr_flags;
	inode->i_parent_ino = id.id_parent_ino;
	inode->i_dev_major = id.id_dev_major;
	inode->i_dev_minor = id.id_dev_minor;
	inode->i_sec_label_lfs = id.id_sec_label_lfs;
	inode->i_sec_label_pi = id.id_sec_label_pi;
	inode->i_sec_label_len = id.id_sec_label_len;
	if (id.id_sec_label_len > 0 &&
	    id.id_sec_label_len <= REFFS_SEC_LABEL_MAX)
		memcpy(inode->i_sec_label, id.id_sec_label,
		       id.id_sec_label_len);

	if (inode->i_ino >= sb->sb_next_ino)
		sb->sb_next_ino = inode->i_ino + 1;

	/*
	 * Data file loading (.dat) is handled by the composed data
	 * backend via data_block_alloc() — same as POSIX md backend.
	 */
	if (!inode->i_db && S_ISREG(inode->i_mode)) {
		char dat_path[PATH_MAX];
		int n = snprintf(dat_path, sizeof(dat_path),
				 "%s/sb_%lu/ino_%lu.dat", sb->sb_backend_path,
				 (unsigned long)sb->sb_id,
				 (unsigned long)inode->i_ino);
		if (n > 0 && (size_t)n < sizeof(dat_path) &&
		    access(dat_path, F_OK) == 0) {
			inode->i_db = data_block_alloc(inode, NULL, 0, 0);
			if (inode->i_db) {
				size_t db_size = inode->i_db->db_size;
				atomic_fetch_add_explicit(&sb->sb_bytes_used,
							  db_size,
							  memory_order_relaxed);
				inode->i_used =
					db_size / sb->sb_block_size +
					(db_size % sb->sb_block_size ? 1 : 0);
			}
		}
	}

	/* Load symlink target */
	if (S_ISLNK(inode->i_mode) && !inode->i_symlink) {
		klen = rocksdb_key_lnk(key, inode->i_ino);
		val = rocksdb_get_cf(priv->rsp_db, priv->rsp_ropts,
				     priv->rsp_cf[ROCKSDB_CF_SYMLINKS],
				     (const char *)key, klen, &vlen, &err);
		if (err) {
			rocksdb_free(err);
			err = NULL;
		} else if (val) {
			inode->i_symlink = malloc(vlen + 1);
			if (inode->i_symlink) {
				memcpy(inode->i_symlink, val, vlen);
				inode->i_symlink[vlen] = '\0';
			}
			rocksdb_free(val);
		}
	}

	/* Load layout segments (MDS mode) */
	if (!inode->i_layout_segments) {
		klen = rocksdb_key_lay(key, inode->i_ino);
		val = rocksdb_get_cf(priv->rsp_db, priv->rsp_ropts,
				     priv->rsp_cf[ROCKSDB_CF_LAYOUTS],
				     (const char *)key, klen, &vlen, &err);
		if (err) {
			rocksdb_free(err);
			err = NULL;
		} else if (val && vlen >= sizeof(uint32_t)) {
			const char *p = val;
			uint32_t count;
			memcpy(&count, p, sizeof(count));
			p += sizeof(count);

			if (count > 0) {
				struct layout_segments *lss =
					layout_segments_alloc();
				if (lss) {
					bool ok = true;
					for (uint32_t s = 0; ok && s < count;
					     s++) {
						if ((size_t)(p - val) +
							    sizeof(struct layout_segment_disk) >
						    vlen) {
							ok = false;
							break;
						}
						struct layout_segment_disk lsd;
						memcpy(&lsd, p, sizeof(lsd));
						p += sizeof(lsd);

						struct layout_data_file *files =
							calloc(lsd.ls_nfiles,
							       sizeof(*files));
						if (!files) {
							ok = false;
							break;
						}

						for (uint32_t f = 0;
						     ok && f < lsd.ls_nfiles;
						     f++) {
							if ((size_t)(p - val) +
								    sizeof(struct layout_data_file_disk) >
							    vlen) {
								ok = false;
								break;
							}
							struct layout_data_file_disk
								ldfd;
							memcpy(&ldfd, p,
							       sizeof(ldfd));
							p += sizeof(ldfd);
							files[f].ldf_dstore_id =
								ldfd.ldf_dstore_id;
							files[f].ldf_fh_len =
								ldfd.ldf_fh_len;
							if (ldfd.ldf_fh_len >
							    LAYOUT_SEG_MAX_FH) {
								ok = false;
								break;
							}
							memcpy(files[f].ldf_fh,
							       ldfd.ldf_fh,
							       ldfd.ldf_fh_len);
							files[f].ldf_size =
								ldfd.ldf_size;
							files[f].ldf_atime =
								ldfd.ldf_atime;
							files[f].ldf_mtime =
								ldfd.ldf_mtime;
							files[f].ldf_ctime =
								ldfd.ldf_ctime;
							files[f].ldf_uid =
								ldfd.ldf_uid;
							files[f].ldf_gid =
								ldfd.ldf_gid;
							files[f].ldf_mode =
								ldfd.ldf_mode;
						}

						if (ok) {
							struct layout_segment seg = {
								.ls_offset =
									lsd.ls_offset,
								.ls_length =
									lsd.ls_length,
								.ls_stripe_unit =
									lsd.ls_stripe_unit,
								.ls_k = lsd.ls_k,
								.ls_m = lsd.ls_m,
								.ls_nfiles =
									lsd.ls_nfiles,
								.ls_layout_type =
									lsd.ls_layout_type,
								.ls_files =
									files,
							};
							if (layout_segments_add(
								    lss,
								    &seg) !=
							    0) {
								free(files);
								ok = false;
							}
						} else {
							free(files);
						}
					}

					if (ok)
						inode->i_layout_segments = lss;
					else
						layout_segments_free(lss);
				}
			}
			rocksdb_free(val);
		} else if (val) {
			rocksdb_free(val);
		}
	}

	trace_fs_inode(inode, __func__, __LINE__);
out:
	return ret;
}

/* ------------------------------------------------------------------ */
/* inode_free — delete metadata keys from RocksDB                      */
/* ------------------------------------------------------------------ */

static void rocksdb_inode_free(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct rocksdb_sb_private *priv = get_priv(sb);
	if (!priv)
		return;

	trace_fs_inode(inode, __func__, __LINE__);

	char *err = NULL;
	rocksdb_writebatch_t *batch = rocksdb_writebatch_create();

	uint8_t key[ROCKSDB_KEY_MAX_SIZE];
	size_t klen;

	/* Delete inode metadata */
	klen = rocksdb_key_ino(key, inode->i_ino);
	rocksdb_writebatch_delete_cf(batch, priv->rsp_cf[ROCKSDB_CF_INODES],
				     (const char *)key, klen);

	/* Delete symlink */
	klen = rocksdb_key_lnk(key, inode->i_ino);
	rocksdb_writebatch_delete_cf(batch, priv->rsp_cf[ROCKSDB_CF_SYMLINKS],
				     (const char *)key, klen);

	/* Delete layouts */
	klen = rocksdb_key_lay(key, inode->i_ino);
	rocksdb_writebatch_delete_cf(batch, priv->rsp_cf[ROCKSDB_CF_LAYOUTS],
				     (const char *)key, klen);

	/*
	 * Delete directory entries.  Iterate prefix to find all children,
	 * then delete each.
	 */
	uint8_t prefix[12];
	size_t plen = rocksdb_key_dir_prefix(prefix, inode->i_ino);
	rocksdb_iterator_t *it = rocksdb_create_iterator_cf(
		priv->rsp_db, priv->rsp_ropts, priv->rsp_cf[ROCKSDB_CF_DIRS]);
	rocksdb_iter_seek(it, (const char *)prefix, plen);
	while (rocksdb_iter_valid(it)) {
		size_t iter_klen;
		const char *iter_key = rocksdb_iter_key(it, &iter_klen);
		if (iter_klen < plen || memcmp(iter_key, prefix, plen) != 0)
			break;
		rocksdb_writebatch_delete_cf(batch,
					     priv->rsp_cf[ROCKSDB_CF_DIRS],
					     iter_key, iter_klen);
		rocksdb_iter_next(it);
	}
	rocksdb_iter_destroy(it);

	/*
	 * Delete chunk metadata.  Iterate prefix chk:<ino>: to find
	 * all blocks, then delete each.
	 */
	plen = rocksdb_key_chk_prefix(prefix, inode->i_ino);
	it = rocksdb_create_iterator_cf(priv->rsp_db, priv->rsp_ropts,
					priv->rsp_cf[ROCKSDB_CF_CHUNKS]);
	rocksdb_iter_seek(it, (const char *)prefix, plen);
	while (rocksdb_iter_valid(it)) {
		size_t iter_klen;
		const char *iter_key = rocksdb_iter_key(it, &iter_klen);
		if (iter_klen < plen || memcmp(iter_key, prefix, plen) != 0)
			break;
		rocksdb_writebatch_delete_cf(batch,
					     priv->rsp_cf[ROCKSDB_CF_CHUNKS],
					     iter_key, iter_klen);
		rocksdb_iter_next(it);
	}
	rocksdb_iter_destroy(it);

	rocksdb_write(priv->rsp_db, priv->rsp_wopts, batch, &err);
	if (err) {
		LOG("rocksdb inode_free ino %" PRIu64 ": %s", inode->i_ino,
		    err);
		rocksdb_free(err);
	}

	rocksdb_writebatch_destroy(batch);
}

/* ------------------------------------------------------------------ */
/* dir_sync — persist directory entries to RocksDB                     */
/* ------------------------------------------------------------------ */

static void rocksdb_dir_sync(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct rocksdb_sb_private *priv = get_priv(sb);
	if (!priv || !inode->i_dirent)
		return;

	trace_fs_inode(inode, __func__, __LINE__);

	char *err = NULL;
	rocksdb_writebatch_t *batch = rocksdb_writebatch_create();

	/*
	 * Delete all existing entries for this directory.
	 * RocksDB has no prefix-delete — iterate and collect.
	 */
	uint8_t prefix[12];
	size_t plen = rocksdb_key_dir_prefix(prefix, inode->i_ino);
	rocksdb_iterator_t *it = rocksdb_create_iterator_cf(
		priv->rsp_db, priv->rsp_ropts, priv->rsp_cf[ROCKSDB_CF_DIRS]);
	rocksdb_iter_seek(it, (const char *)prefix, plen);
	while (rocksdb_iter_valid(it)) {
		size_t iter_klen;
		const char *iter_key = rocksdb_iter_key(it, &iter_klen);
		if (iter_klen < plen || memcmp(iter_key, prefix, plen) != 0)
			break;
		rocksdb_writebatch_delete_cf(batch,
					     priv->rsp_cf[ROCKSDB_CF_DIRS],
					     iter_key, iter_klen);
		rocksdb_iter_next(it);
	}
	rocksdb_iter_destroy(it);

	/* Write current children */
	struct reffs_dirent *rd;
	rcu_read_lock();
	cds_list_for_each_entry_rcu(rd, &inode->i_dirent->rd_children,
				    rd_siblings) {
		uint8_t key[ROCKSDB_KEY_DIR_SIZE];
		size_t klen = rocksdb_key_dir(key, inode->i_ino, rd->rd_cookie);

		/*
		 * Value: child_ino(8) + name_len(2) + name(variable)
		 */
		uint16_t name_len = strlen(rd->rd_name);
		size_t vlen = sizeof(uint64_t) + sizeof(uint16_t) + name_len;
		char val[sizeof(uint64_t) + sizeof(uint16_t) + 256];

		if (vlen > sizeof(val)) {
			LOG("dir entry name too long: %u", name_len);
			continue;
		}

		uint64_t child_ino = rd->rd_ino;
		memcpy(val, &child_ino, sizeof(child_ino));
		memcpy(val + sizeof(child_ino), &name_len, sizeof(name_len));
		memcpy(val + sizeof(child_ino) + sizeof(name_len), rd->rd_name,
		       name_len);

		rocksdb_writebatch_put_cf(batch, priv->rsp_cf[ROCKSDB_CF_DIRS],
					  (const char *)key, klen, val, vlen);
	}
	rcu_read_unlock();

	rocksdb_write(priv->rsp_db, priv->rsp_wopts, batch, &err);
	if (err) {
		LOG("rocksdb dir_sync ino %" PRIu64 ": %s", inode->i_ino, err);
		rocksdb_free(err);
	}

	rocksdb_writebatch_destroy(batch);
}

/* ------------------------------------------------------------------ */
/* dir_find_entry_by_ino — scan directory for child inode number       */
/* ------------------------------------------------------------------ */

static int rocksdb_dir_find_entry_by_ino(struct super_block *sb,
					 uint64_t dir_ino, uint64_t child_ino,
					 char *name_out, size_t name_max,
					 uint64_t *cookie_out)
{
	struct rocksdb_sb_private *priv = get_priv(sb);
	if (!priv)
		return -EINVAL;

	uint8_t prefix[12];
	size_t plen = rocksdb_key_dir_prefix(prefix, dir_ino);
	int found = -ENOENT;

	rocksdb_iterator_t *it = rocksdb_create_iterator_cf(
		priv->rsp_db, priv->rsp_ropts, priv->rsp_cf[ROCKSDB_CF_DIRS]);
	rocksdb_iter_seek(it, (const char *)prefix, plen);

	while (rocksdb_iter_valid(it)) {
		size_t iter_klen, iter_vlen;
		const char *iter_key = rocksdb_iter_key(it, &iter_klen);
		if (iter_klen < plen || memcmp(iter_key, prefix, plen) != 0)
			break;

		const char *iter_val = rocksdb_iter_value(it, &iter_vlen);
		if (iter_vlen < sizeof(uint64_t) + sizeof(uint16_t)) {
			rocksdb_iter_next(it);
			continue;
		}

		uint64_t ino;
		uint16_t name_len;
		memcpy(&ino, iter_val, sizeof(ino));
		memcpy(&name_len, iter_val + sizeof(ino), sizeof(name_len));

		if (ino == child_ino && iter_vlen >= sizeof(uint64_t) +
							     sizeof(uint16_t) +
							     name_len) {
			size_t copy = name_len < name_max - 1 ? name_len :
								name_max - 1;
			memcpy(name_out,
			       iter_val + sizeof(ino) + sizeof(name_len), copy);
			name_out[copy] = '\0';
			/* Extract cookie from key (bytes 4-12) */
			*cookie_out = decode_be64((const uint8_t *)iter_key +
						  4 + sizeof(uint64_t));
			found = 0;
			break;
		}

		rocksdb_iter_next(it);
	}

	rocksdb_iter_destroy(it);
	return found;
}

/* ------------------------------------------------------------------ */
/* dir_find_entry_by_name — scan directory for child name              */
/* ------------------------------------------------------------------ */

static int rocksdb_dir_find_entry_by_name(struct super_block *sb,
					  uint64_t dir_ino, const char *name,
					  uint64_t *child_ino_out,
					  uint64_t *cookie_out)
{
	struct rocksdb_sb_private *priv = get_priv(sb);
	if (!priv)
		return -EINVAL;

	size_t target_len = strlen(name);
	uint8_t prefix[12];
	size_t plen = rocksdb_key_dir_prefix(prefix, dir_ino);
	int found = -ENOENT;

	rocksdb_iterator_t *it = rocksdb_create_iterator_cf(
		priv->rsp_db, priv->rsp_ropts, priv->rsp_cf[ROCKSDB_CF_DIRS]);
	rocksdb_iter_seek(it, (const char *)prefix, plen);

	while (rocksdb_iter_valid(it)) {
		size_t iter_klen, iter_vlen;
		const char *iter_key = rocksdb_iter_key(it, &iter_klen);
		if (iter_klen < plen || memcmp(iter_key, prefix, plen) != 0)
			break;

		const char *iter_val = rocksdb_iter_value(it, &iter_vlen);
		if (iter_vlen < sizeof(uint64_t) + sizeof(uint16_t)) {
			rocksdb_iter_next(it);
			continue;
		}

		uint64_t ino;
		uint16_t name_len;
		memcpy(&ino, iter_val, sizeof(ino));
		memcpy(&name_len, iter_val + sizeof(ino), sizeof(name_len));

		if (name_len == target_len &&
		    iter_vlen >=
			    sizeof(uint64_t) + sizeof(uint16_t) + name_len &&
		    memcmp(iter_val + sizeof(ino) + sizeof(name_len), name,
			   target_len) == 0) {
			*child_ino_out = ino;
			*cookie_out = decode_be64((const uint8_t *)iter_key +
						  4 + sizeof(uint64_t));
			found = 0;
			break;
		}

		rocksdb_iter_next(it);
	}

	rocksdb_iter_destroy(it);
	return found;
}

/* ------------------------------------------------------------------ */
/* Recovery                                                            */
/* ------------------------------------------------------------------ */

static void rocksdb_recover_directory_recursive(struct rocksdb_sb_private *priv,
						struct reffs_dirent *parent)
{
	struct inode *inode = parent->rd_inode;
	struct super_block *sb = inode->i_sb;

	/* Iterate dirs CF for children of this directory */
	uint8_t prefix[12];
	size_t plen = rocksdb_key_dir_prefix(prefix, inode->i_ino);
	rocksdb_iterator_t *it = rocksdb_create_iterator_cf(
		priv->rsp_db, priv->rsp_ropts, priv->rsp_cf[ROCKSDB_CF_DIRS]);
	rocksdb_iter_seek(it, (const char *)prefix, plen);

	uint64_t max_cookie = 0;

	while (rocksdb_iter_valid(it)) {
		size_t iter_klen, iter_vlen;
		const char *iter_key = rocksdb_iter_key(it, &iter_klen);
		if (iter_klen < plen || memcmp(iter_key, prefix, plen) != 0)
			break;

		const char *iter_val = rocksdb_iter_value(it, &iter_vlen);
		if (iter_vlen < sizeof(uint64_t) + sizeof(uint16_t)) {
			rocksdb_iter_next(it);
			continue;
		}

		uint64_t child_ino;
		uint16_t name_len;
		memcpy(&child_ino, iter_val, sizeof(child_ino));
		memcpy(&name_len, iter_val + sizeof(child_ino),
		       sizeof(name_len));

		if (iter_vlen <
		    sizeof(uint64_t) + sizeof(uint16_t) + name_len) {
			rocksdb_iter_next(it);
			continue;
		}

		char name[256];
		size_t copy = name_len < sizeof(name) - 1 ? name_len :
							    sizeof(name) - 1;
		memcpy(name, iter_val + sizeof(child_ino) + sizeof(name_len),
		       copy);
		name[copy] = '\0';

		/* Extract cookie from key */
		uint64_t cookie = decode_be64((const uint8_t *)iter_key + 4 +
					      sizeof(uint64_t));

		if (cookie > max_cookie)
			max_cookie = cookie;

		struct reffs_dirent *rd = dirent_alloc(
			parent, name, reffs_life_action_load, false);
		if (rd) {
			rd->rd_cookie = cookie;
			struct inode *recovered = inode_alloc(sb, child_ino);
			if (recovered) {
				dirent_attach_inode(rd, recovered);
				if (S_ISDIR(recovered->i_mode))
					rocksdb_recover_directory_recursive(
						priv, rd);
				inode_active_put(recovered);
			}
			dirent_put(rd);
		}

		rocksdb_iter_next(it);
	}

	rocksdb_iter_destroy(it);

	/* Restore cookie_next so new entries don't collide */
	if (max_cookie >= parent->rd_cookie_next)
		parent->rd_cookie_next = max_cookie + 1;
}

static void rocksdb_recover(struct super_block *sb)
{
	struct rocksdb_sb_private *priv = get_priv(sb);
	if (!priv)
		return;

	/*
	 * Scan inodes CF to find the max inode number.
	 * This replaces the POSIX scandir of .meta files.
	 */
	rocksdb_iterator_t *it = rocksdb_create_iterator_cf(
		priv->rsp_db, priv->rsp_ropts, priv->rsp_cf[ROCKSDB_CF_INODES]);
	rocksdb_iter_seek_to_last(it);
	if (rocksdb_iter_valid(it)) {
		size_t klen;
		const char *key = rocksdb_iter_key(it, &klen);
		if (klen == ROCKSDB_KEY_INO_SIZE &&
		    memcmp(key, "ino:", 4) == 0) {
			uint64_t max_ino =
				decode_be64((const uint8_t *)key + 4);
			if (max_ino >= sb->sb_next_ino)
				sb->sb_next_ino = max_ino + 1;
		}
	}
	rocksdb_iter_destroy(it);

	/*
	 * Also check the persisted sb_next_ino counter.  The inodes CF
	 * scan finds the max allocated ino, but the persisted counter
	 * may be higher if inodes were allocated then freed.
	 */
	{
		char *err2 = NULL;
		size_t vlen2 = 0;
		char *val2 = rocksdb_get_cf(priv->rsp_db, priv->rsp_ropts,
					    priv->rsp_cf[ROCKSDB_CF_DEFAULT],
					    sb_meta_next_ino,
					    SB_META_NEXT_INO_LEN, &vlen2,
					    &err2);
		if (err2) {
			rocksdb_free(err2);
		} else if (val2 && vlen2 == sizeof(uint64_t)) {
			uint64_t persisted_next;
			memcpy(&persisted_next, val2, sizeof(uint64_t));
			if (persisted_next > sb->sb_next_ino)
				sb->sb_next_ino = persisted_next;
			rocksdb_free(val2);
		} else if (val2) {
			rocksdb_free(val2);
		}
	}

	/* Load root inode fields from RocksDB */
	struct inode *root_inode = sb->sb_dirent->rd_inode;
	if (sb->sb_ops->inode_alloc)
		sb->sb_ops->inode_alloc(root_inode);

	/* Walk directory tree recursively from dirs CF */
	rocksdb_recover_directory_recursive(priv, sb->sb_dirent);
}

/* ------------------------------------------------------------------ */
/* Storage ops template                                                */
/* ------------------------------------------------------------------ */

/*
 * rocksdb_storage_ops — md template for RocksDB metadata backend.
 *
 * Data ops (db_*) are intentionally NULL — they are populated by
 * reffs_backend_compose() from the POSIX data template.
 */
const struct reffs_storage_ops rocksdb_storage_ops = {
	.type = REFFS_STORAGE_ROCKSDB,
	.name = "rocksdb",
	.sb_alloc = rocksdb_sb_alloc,
	.sb_free = rocksdb_sb_free,
	.inode_alloc = rocksdb_inode_alloc,
	.inode_free = rocksdb_inode_free,
	.inode_sync = rocksdb_inode_sync,
	.dir_sync = rocksdb_dir_sync,
	.dir_find_entry_by_ino = rocksdb_dir_find_entry_by_ino,
	.dir_find_entry_by_name = rocksdb_dir_find_entry_by_name,
	.recover = rocksdb_recover,
};
