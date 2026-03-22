/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Dstore control-plane operations.
 *
 * The MDS uses these to manage data files on the NFSv3 data servers.
 * All operations are synchronous and use the dstore's CLIENT handle.
 * They are NOT data-path operations and do NOT count as WRITE layouts.
 *
 * Write semantics: create, remove, chmod, truncate, fence must
 * succeed on ALL mirrors in a CSM set or the operation fails.
 *
 * Read semantics: getattr must succeed on ALL mirrors or returns
 * NFS4ERR_DELAY; stale mirrors are tracked per data_file.
 */

#ifndef _REFFS_DSTORE_OPS_H
#define _REFFS_DSTORE_OPS_H

#include <stdint.h>
#include <time.h>

struct dstore;
struct layout_data_file;

/*
 * dstore_data_file_create -- create a data file on the DS.
 *
 * dir_fh/dir_fh_len: parent directory FH (typically the dstore root FH).
 * name: filename to create.
 * out_fh/out_fh_len: on success, receives the new file's FH.
 *
 * Returns 0 on success, negative errno on failure.
 */
int dstore_data_file_create(struct dstore *ds, const uint8_t *dir_fh,
			    uint32_t dir_fh_len, const char *name,
			    uint8_t *out_fh, uint32_t *out_fh_len);

/*
 * dstore_data_file_remove -- delete a data file on the DS.
 *
 * dir_fh/dir_fh_len: parent directory FH.
 * name: filename to remove.
 *
 * Returns 0 on success, negative errno on failure.
 */
int dstore_data_file_remove(struct dstore *ds, const uint8_t *dir_fh,
			    uint32_t dir_fh_len, const char *name);

/*
 * dstore_data_file_chmod -- set permissions on a data file.
 * Sets mode to 0640 (owner rw, group r, no other).
 *
 * Returns 0 on success, negative errno on failure.
 */
int dstore_data_file_chmod(struct dstore *ds, const uint8_t *fh,
			   uint32_t fh_len);

/*
 * dstore_data_file_truncate -- set file size on the DS.
 *
 * This is called when the MDS receives SETATTR(size) from a client.
 * Must succeed on ALL mirrors (write semantics).
 *
 * Returns 0 on success, negative errno on failure.
 */
int dstore_data_file_truncate(struct dstore *ds, const uint8_t *fh,
			      uint32_t fh_len, uint64_t size);

/*
 * dstore_data_file_fence -- rotate synthetic uid/gid to fence a client.
 *
 * Bumps uid and gid atomically within the configured fence range
 * (default 1024-2048), wrapping at the upper bound.  Called in
 * response to client I/O errors.
 *
 * fence_min/fence_max: uid/gid range (inclusive).
 * ldf: the layout_data_file whose uid/gid are updated.
 *
 * Returns 0 on success, negative errno on failure.
 */
int dstore_data_file_fence(struct dstore *ds, const uint8_t *fh,
			   uint32_t fh_len, struct layout_data_file *ldf,
			   uint32_t fence_min, uint32_t fence_max);

/*
 * dstore_data_file_getattr -- fetch attributes from the DS.
 *
 * Updates ldf->ldf_size, ldf_atime, ldf_mtime, ldf_ctime, etc.
 * On failure, sets ldf->ldf_stale = true.
 * On success, clears ldf->ldf_stale.
 *
 * Returns 0 on success, negative errno on failure.
 */
int dstore_data_file_getattr(struct dstore *ds, const uint8_t *fh,
			     uint32_t fh_len, struct layout_data_file *ldf);

#endif /* _REFFS_DSTORE_OPS_H */
