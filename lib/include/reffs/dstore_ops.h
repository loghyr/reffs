/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Dstore control-plane operations — vtable interface.
 *
 * Each dstore carries a pointer to a dstore_ops vtable.  Remote
 * dstores use the NFSv3 RPC implementation; local dstores (same
 * server, detected at init) use the VFS layer directly.
 *
 * Callers use the inline dispatch functions below and never check
 * whether the dstore is local or remote.
 */

#ifndef _REFFS_DSTORE_OPS_H
#define _REFFS_DSTORE_OPS_H

#include <stdint.h>
#include <time.h>

struct dstore;
struct layout_data_file;

/*
 * WCC (Weak Cache Consistency) result from SETATTR ops.
 *
 * Populated by the NFSv3 implementation from the post-op attrs in the
 * SETATTR3res wcc_data.  Left zeroed by the local VFS implementation.
 * Callers may pass NULL if they don't need WCC data.
 */
struct dstore_wcc {
	int64_t wcc_size;
	struct timespec wcc_mtime;
	struct timespec wcc_ctime;
	uint32_t wcc_valid; /* 0 = no data, 1 = post-op attrs present */
};

struct dstore_ops {
	const char *name; /* "nfsv3" or "local" */

	int (*create)(struct dstore *ds, const uint8_t *dir_fh,
		      uint32_t dir_fh_len, const char *name, uint8_t *out_fh,
		      uint32_t *out_fh_len);

	int (*remove)(struct dstore *ds, const uint8_t *dir_fh,
		      uint32_t dir_fh_len, const char *name);

	int (*chmod)(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
		     struct dstore_wcc *wcc);

	int (*truncate)(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
			uint64_t size, struct dstore_wcc *wcc);

	int (*fence)(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
		     struct layout_data_file *ldf, uint32_t fence_min,
		     uint32_t fence_max, struct dstore_wcc *wcc);

	int (*getattr)(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
		       struct layout_data_file *ldf);
};

/* Remote (NFSv3 RPC) vtable — defined in lib/nfs4/dstore/dstore_ops_nfsv3.c */
extern const struct dstore_ops dstore_ops_nfsv3;

/* Local (VFS direct) vtable — defined in lib/nfs4/dstore/dstore_ops_local.c */
extern const struct dstore_ops dstore_ops_local;

/* ------------------------------------------------------------------ */
/* Dispatch — callers use these                                        */
/* ------------------------------------------------------------------ */

static inline int dstore_data_file_create(struct dstore *ds,
					  const uint8_t *dir_fh,
					  uint32_t dir_fh_len, const char *name,
					  uint8_t *out_fh, uint32_t *out_fh_len)
{
	return ds->ds_ops->create(ds, dir_fh, dir_fh_len, name, out_fh,
				  out_fh_len);
}

static inline int dstore_data_file_remove(struct dstore *ds,
					  const uint8_t *dir_fh,
					  uint32_t dir_fh_len, const char *name)
{
	return ds->ds_ops->remove(ds, dir_fh, dir_fh_len, name);
}

static inline int dstore_data_file_chmod(struct dstore *ds, const uint8_t *fh,
					 uint32_t fh_len,
					 struct dstore_wcc *wcc)
{
	return ds->ds_ops->chmod(ds, fh, fh_len, wcc);
}

static inline int dstore_data_file_truncate(struct dstore *ds,
					    const uint8_t *fh, uint32_t fh_len,
					    uint64_t size,
					    struct dstore_wcc *wcc)
{
	return ds->ds_ops->truncate(ds, fh, fh_len, size, wcc);
}

static inline int dstore_data_file_fence(struct dstore *ds, const uint8_t *fh,
					 uint32_t fh_len,
					 struct layout_data_file *ldf,
					 uint32_t fence_min, uint32_t fence_max,
					 struct dstore_wcc *wcc)
{
	return ds->ds_ops->fence(ds, fh, fh_len, ldf, fence_min, fence_max,
				 wcc);
}

static inline int dstore_data_file_getattr(struct dstore *ds, const uint8_t *fh,
					   uint32_t fh_len,
					   struct layout_data_file *ldf)
{
	return ds->ds_ops->getattr(ds, fh, fh_len, ldf);
}

#endif /* _REFFS_DSTORE_OPS_H */
