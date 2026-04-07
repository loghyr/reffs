/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Dstore control-plane operations -- vtable interface.
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

#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
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

	/*
	 * InBand I/O -- MDS proxies data for non-pNFS clients.
	 * NULL = not supported (returns -ENOSYS from dispatcher).
	 * uid/gid: synthetic fenced credentials for DS auth.
	 */
	ssize_t (*read)(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
			void *buf, size_t len, uint64_t offset, uint32_t uid,
			uint32_t gid);
	ssize_t (*write)(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
			 const void *buf, size_t len, uint64_t offset,
			 uint32_t uid, uint32_t gid);
	int (*commit)(struct dstore *ds, const uint8_t *fh, uint32_t fh_len,
		      uint64_t offset, uint32_t count);

	/*
	 * Tight-coupling control plane (pNFS Flex Files v2).
	 * NULL = not supported (DS does not implement TRUST_STATEID).
	 *
	 * probe_tight_coupling -- send a capability probe (TRUST_STATEID with
	 * anonymous stateid).  NFS4ERR_INVAL from the DS means tight coupling
	 * is supported.  Returns 0 on success (tight coupling available),
	 * -ENOTSUP if not available, or -errno on transport error.
	 *
	 * trust_stateid -- register a layout stateid on the DS.
	 * stid_seqid / stid_other: the layout stateid's seqid and other[12].
	 * iomode: LAYOUTIOMODE4_READ (1) or LAYOUTIOMODE4_RW (2).
	 * expire_sec / expire_nsec: wall-clock expiry (nfstime4).
	 * principal: GSS principal, or "" for AUTH_SYS sessions.
	 *
	 * revoke_stateid -- revoke a previously-registered stateid.
	 *
	 * bulk_revoke_stateid -- revoke all stateids for a client.
	 * clientid 0 means "revoke all" (MDS restart cleanup).
	 */
	int (*probe_tight_coupling)(struct dstore *ds);

	int (*trust_stateid)(struct dstore *ds, const uint8_t *fh,
			     uint32_t fh_len, uint32_t stid_seqid,
			     const uint8_t *stid_other, uint32_t iomode,
			     uint64_t clientid, int64_t expire_sec,
			     uint32_t expire_nsec, const char *principal);

	int (*revoke_stateid)(struct dstore *ds, const uint8_t *fh,
			      uint32_t fh_len, uint32_t stid_seqid,
			      const uint8_t *stid_other);

	int (*bulk_revoke_stateid)(struct dstore *ds, uint64_t clientid);
};

/* Remote (NFSv3 RPC) vtable -- defined in lib/nfs4/dstore/dstore_ops_nfsv3.c */
extern const struct dstore_ops dstore_ops_nfsv3;

/* Remote (NFSv4.2 compound) vtable -- lib/nfs4/dstore/dstore_ops_nfsv4.c */
extern const struct dstore_ops dstore_ops_nfsv4;

/* Local (VFS direct) vtable -- defined in lib/nfs4/dstore/dstore_ops_local.c */
extern const struct dstore_ops dstore_ops_local;

/* ------------------------------------------------------------------ */
/* Dispatch -- callers use these                                        */
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

/* InBand I/O dispatch */

static inline ssize_t dstore_data_file_read(struct dstore *ds,
					    const uint8_t *fh, uint32_t fh_len,
					    void *buf, size_t len,
					    uint64_t offset, uint32_t uid,
					    uint32_t gid)
{
	if (!ds->ds_ops->read)
		return -ENOSYS;
	return ds->ds_ops->read(ds, fh, fh_len, buf, len, offset, uid, gid);
}

static inline ssize_t dstore_data_file_write(struct dstore *ds,
					     const uint8_t *fh, uint32_t fh_len,
					     const void *buf, size_t len,
					     uint64_t offset, uint32_t uid,
					     uint32_t gid)
{
	if (!ds->ds_ops->write)
		return -ENOSYS;
	return ds->ds_ops->write(ds, fh, fh_len, buf, len, offset, uid, gid);
}

static inline int dstore_data_file_commit(struct dstore *ds, const uint8_t *fh,
					  uint32_t fh_len, uint64_t offset,
					  uint32_t count)
{
	if (!ds->ds_ops->commit)
		return -ENOSYS;
	return ds->ds_ops->commit(ds, fh, fh_len, offset, count);
}

/* Tight-coupling dispatch */

static inline int dstore_probe_tight_coupling(struct dstore *ds)
{
	if (!ds->ds_ops->probe_tight_coupling)
		return -ENOTSUP;
	return ds->ds_ops->probe_tight_coupling(ds);
}

static inline int dstore_trust_stateid(struct dstore *ds, const uint8_t *fh,
				       uint32_t fh_len, uint32_t stid_seqid,
				       const uint8_t *stid_other,
				       uint32_t iomode, uint64_t clientid,
				       int64_t expire_sec, uint32_t expire_nsec,
				       const char *principal)
{
	if (!ds->ds_ops->trust_stateid)
		return -ENOTSUP;
	return ds->ds_ops->trust_stateid(ds, fh, fh_len, stid_seqid, stid_other,
					 iomode, clientid, expire_sec,
					 expire_nsec, principal);
}

static inline int dstore_revoke_stateid(struct dstore *ds, const uint8_t *fh,
					uint32_t fh_len, uint32_t stid_seqid,
					const uint8_t *stid_other)
{
	if (!ds->ds_ops->revoke_stateid)
		return -ENOTSUP;
	return ds->ds_ops->revoke_stateid(ds, fh, fh_len, stid_seqid,
					  stid_other);
}

static inline int dstore_bulk_revoke_stateid(struct dstore *ds,
					     uint64_t clientid)
{
	if (!ds->ds_ops->bulk_revoke_stateid)
		return -ENOTSUP;
	return ds->ds_ops->bulk_revoke_stateid(ds, clientid);
}

#endif /* _REFFS_DSTORE_OPS_H */
