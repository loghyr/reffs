/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Local dstore operations — VFS direct path.
 *
 * When the dstore address resolves to the same server (127.0.0.1,
 * ::1, or the configured bind address), operations bypass NFSv3 RPC
 * and call the VFS layer directly.  This avoids the overhead of
 * XDR encoding, TCP loopback, and RPC processing.
 *
 * The dstore's ds_sb points to the local super_block for the export.
 * Filehandles are our own network_file_handle format (ino + sb_id).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/filehandle.h"
#include "reffs/inode.h"
#include "reffs/layout_segment.h"
#include "reffs/log.h"
#include "reffs/super_block.h"
#include "reffs/vfs.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Resolve a filehandle to an inode.  For local dstores, the FH
 * contains our own network_file_handle (ino + sb_id).
 * Returns a ref-bumped inode or NULL.
 */
static struct inode *local_fh_to_inode(const uint8_t *fh, uint32_t fh_len)
{
	struct network_file_handle *nfh;
	struct super_block *sb;
	struct inode *inode;

	if (fh_len != sizeof(*nfh))
		return NULL;

	nfh = (struct network_file_handle *)fh;
	sb = super_block_find(nfh->nfh_sb);
	if (!sb)
		return NULL;

	inode = inode_find(sb, nfh->nfh_ino);
	super_block_put(sb);
	return inode;
}

/*
 * Resolve a directory FH + name to a parent inode for create/remove.
 */
static struct inode *local_dir_fh_to_inode(const uint8_t *dir_fh,
					   uint32_t dir_fh_len)
{
	return local_fh_to_inode(dir_fh, dir_fh_len);
}

/* ------------------------------------------------------------------ */
/* CREATE                                                              */
/* ------------------------------------------------------------------ */

static int local_create(struct dstore *ds __attribute__((unused)),
			const uint8_t *dir_fh, uint32_t dir_fh_len,
			const char *name, uint8_t *out_fh, uint32_t *out_fh_len)
{
	struct inode *parent = local_dir_fh_to_inode(dir_fh, dir_fh_len);
	struct inode *child = NULL;
	struct authunix_parms ap = { 0 };
	int ret;

	if (!parent)
		return -ESTALE;

	ret = vfs_create(parent, name, 0640, &ap, &child, NULL, NULL);
	if (ret == -EEXIST) {
		/*
		 * UNCHECKED semantics: if the file already exists
		 * (e.g., runway restart), look it up and return its FH.
		 */
		child = inode_name_get_inode(parent, (char *)name);
		if (!child) {
			inode_active_put(parent);
			return -ENOENT;
		}
	} else if (ret) {
		inode_active_put(parent);
		return ret;
	}

	/* Build the FH from the child inode. */
	struct network_file_handle nfh = {
		.nfh_vers = FILEHANDLE_VERSION_CURR,
		.nfh_ino = child->i_ino,
		.nfh_sb = child->i_sb->sb_id,
	};

	if (sizeof(nfh) > DSTORE_MAX_FH) {
		inode_active_put(child);
		inode_active_put(parent);
		return -EOVERFLOW;
	}

	memcpy(out_fh, &nfh, sizeof(nfh));
	*out_fh_len = sizeof(nfh);

	inode_active_put(child);
	inode_active_put(parent);
	return 0;
}

/* ------------------------------------------------------------------ */
/* REMOVE                                                              */
/* ------------------------------------------------------------------ */

static int local_remove(struct dstore *ds __attribute__((unused)),
			const uint8_t *dir_fh, uint32_t dir_fh_len,
			const char *name)
{
	struct inode *parent = local_dir_fh_to_inode(dir_fh, dir_fh_len);
	struct authunix_parms ap = { 0 };
	int ret;

	if (!parent)
		return -ESTALE;

	ret = vfs_remove(parent, name, &ap, NULL, NULL);
	inode_active_put(parent);
	return ret;
}

/* ------------------------------------------------------------------ */
/* CHMOD                                                               */
/* ------------------------------------------------------------------ */

static int local_chmod(struct dstore *ds __attribute__((unused)),
		       const uint8_t *fh, uint32_t fh_len,
		       struct dstore_wcc *wcc __attribute__((unused)))
{
	struct inode *inode = local_fh_to_inode(fh, fh_len);

	if (!inode)
		return -ESTALE;

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode->i_mode = (inode->i_mode & S_IFMT) | 0640;
	pthread_mutex_unlock(&inode->i_attr_mutex);

	inode_sync_to_disk(inode);
	inode_active_put(inode);
	return 0;
}

/* ------------------------------------------------------------------ */
/* TRUNCATE                                                            */
/* ------------------------------------------------------------------ */

static int local_truncate(struct dstore *ds __attribute__((unused)),
			  const uint8_t *fh, uint32_t fh_len, uint64_t size,
			  struct dstore_wcc *wcc __attribute__((unused)))
{
	struct inode *inode = local_fh_to_inode(fh, fh_len);
	struct authunix_parms ap = { 0 };
	struct reffs_sattr sattr = { 0 };

	if (!inode)
		return -ESTALE;

	sattr.size = size;
	sattr.size_set = true;

	int ret = vfs_setattr(inode, &sattr, &ap);

	inode_active_put(inode);
	return ret;
}

/* ------------------------------------------------------------------ */
/* FENCE                                                               */
/* ------------------------------------------------------------------ */

static int local_fence(struct dstore *ds __attribute__((unused)),
		       const uint8_t *fh, uint32_t fh_len,
		       struct layout_data_file *ldf, uint32_t fence_min,
		       uint32_t fence_max,
		       struct dstore_wcc *wcc __attribute__((unused)))
{
	struct inode *inode;
	uint32_t new_uid, new_gid;

	if (fence_min > fence_max)
		return -EINVAL;

	inode = local_fh_to_inode(fh, fh_len);
	if (!inode)
		return -ESTALE;

	new_uid = ldf->ldf_uid + 1;
	new_gid = ldf->ldf_gid + 1;
	if (new_uid > fence_max || new_uid < fence_min)
		new_uid = fence_min;
	if (new_gid > fence_max || new_gid < fence_min)
		new_gid = fence_min;

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode->i_uid = new_uid;
	inode->i_gid = new_gid;
	pthread_mutex_unlock(&inode->i_attr_mutex);

	inode_sync_to_disk(inode);

	ldf->ldf_uid = new_uid;
	ldf->ldf_gid = new_gid;

	inode_active_put(inode);
	return 0;
}

/* ------------------------------------------------------------------ */
/* GETATTR                                                             */
/* ------------------------------------------------------------------ */

static int local_getattr(struct dstore *ds __attribute__((unused)),
			 const uint8_t *fh, uint32_t fh_len,
			 struct layout_data_file *ldf)
{
	struct inode *inode = local_fh_to_inode(fh, fh_len);

	if (!inode) {
		ldf->ldf_stale = true;
		return -ESTALE;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);
	ldf->ldf_size = inode->i_size;
	ldf->ldf_uid = inode->i_uid;
	ldf->ldf_gid = inode->i_gid;
	ldf->ldf_mode = inode->i_mode;
	ldf->ldf_atime = inode->i_atime;
	ldf->ldf_mtime = inode->i_mtime;
	ldf->ldf_ctime = inode->i_ctime;
	pthread_mutex_unlock(&inode->i_attr_mutex);

	ldf->ldf_stale = false;

	inode_active_put(inode);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Vtable                                                              */
/* ------------------------------------------------------------------ */

const struct dstore_ops dstore_ops_local = {
	.name = "local",
	.create = local_create,
	.remove = local_remove,
	.chmod = local_chmod,
	.truncate = local_truncate,
	.fence = local_fence,
	.getattr = local_getattr,
};
