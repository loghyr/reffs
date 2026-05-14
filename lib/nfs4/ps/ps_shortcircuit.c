/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "reffs/data_block.h"
#include "reffs/errno.h"
#include "reffs/filehandle.h"
#include "reffs/identity.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"

#include "nfs4/trust_stateid.h"

#include "ps_shortcircuit.h"
#include "ps_state.h"

/*
 * CLOCK_MONOTONIC nanoseconds, used for trust-entry expiry checks.
 * Inlined here rather than pulling in reffs/time.h because that
 * header transitively includes nfsv3_xdr.h, which is not on this
 * TU's include path (ps_shortcircuit.c lives in lib/nfs4/ps, which
 * does not link the NFSv3 wire stubs).
 */
static uint64_t sc_now_mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
	       (uint64_t)ts.tv_nsec;
}

/*
 * Decode a wire FH into (sb_id, ino).  Returns 0 on success or
 * -EINVAL for any malformed buffer (NULL pointer, undersized, or
 * unrecognised version).  The version check is exact -- a future
 * wire-format bump would land a new FILEHANDLE_VERSION_CURR and
 * the helper would have to learn how to decode it; silently
 * accepting an unknown version risks misinterpreting the rest of
 * the bytes as sb_id/ino.
 */
static int decode_fh(const uint8_t *fh, uint32_t fh_len, uint64_t *sb_id_out,
		     uint64_t *ino_out)
{
	struct network_file_handle nfh;

	if (!fh || fh_len < sizeof(nfh))
		return -EINVAL;

	memcpy(&nfh, fh, sizeof(nfh));
	if (nfh.nfh_vers != FILEHANDLE_VERSION_CURR)
		return -EINVAL;

	*sb_id_out = nfh.nfh_sb;
	*ino_out = nfh.nfh_ino;
	return 0;
}

/*
 * Resolve (sb_id, ino) to a pinned inode + sb pair.  Both are
 * ref-held on success and MUST be dropped via finish_lookup().
 * super_block_find() is intentionally the unscoped variant -- a
 * co-resident DS sb is owned by the native listener but the
 * Phase 5 short-circuit reaches it from the PS listener thread,
 * so the sb_listener_id will not match.  This is the documented
 * cross-listener access pattern for the DS sb.
 */
static int lookup_target(uint64_t sb_id, uint64_t ino,
			 struct super_block **sb_out, struct inode **inode_out)
{
	struct super_block *sb = super_block_find(sb_id);

	if (!sb)
		return -ESTALE;

	struct inode *inode = inode_find(sb, ino);

	if (!inode) {
		super_block_put(sb);
		return -ESTALE;
	}

	*sb_out = sb;
	*inode_out = inode;
	return 0;
}

static void finish_lookup(struct super_block *sb, struct inode *inode)
{
	inode_active_put(inode);
	super_block_put(sb);
}

/*
 * Forwarded-cred check against the local file's synthetic uid/gid.
 * The MDS chmod'd the runway file with the synthetic uid/gid the
 * layout grants to the upstream client, so a layout-honoured I/O
 * always carries matching credentials.  The RPC path enforces this
 * via AUTH_SYS owner / group match before the data block is touched;
 * the short path replicates the rule so a forwarded cred the RPC
 * path would reject (wrong uid, or root squashed to nobody) does
 * not silently succeed here.  Accept-on-either matches the typical
 * UNIX open-access pattern (owner OR group hit grants access).
 */
static bool creds_match(const struct inode *inode, uint32_t fwd_uid,
			uint32_t fwd_gid)
{
	uint32_t stored_uid = reffs_id_to_uid(inode->i_uid);
	uint32_t stored_gid = reffs_id_to_uid(inode->i_gid);

	return fwd_uid == stored_uid || fwd_gid == stored_gid;
}

/*
 * Trust-stateid gate for the short path.  NULL stateid bypasses the
 * check (matches the RPC path's stateid4_is_special() bypass for
 * anonymous I/O).  Non-NULL stateid is required to be present in the
 * global trust table with TRUST_ACTIVE set, not TRUST_PENDING, and
 * not expired -- the same rule lib/nfs4/server/chunk.c's
 * nfs4_op_chunk_write enforces before allowing CHUNK_WRITE.  Without
 * this gate the short path becomes a layout-stateid override, since
 * a stale or revoked stateid the RPC path would reject can still
 * land bytes on the local DS file.
 */
static int check_stateid(const stateid4 *layout_stid)
{
	if (!layout_stid)
		return 0;

	struct trust_entry *te = trust_stateid_find(layout_stid);

	if (!te)
		return -EBADSTATEID;

	uint32_t flags =
		atomic_load_explicit(&te->te_flags, memory_order_acquire);
	int ret = 0;

	if (flags & TRUST_PENDING) {
		ret = -EAGAIN;
	} else if (!(flags & TRUST_ACTIVE)) {
		ret = -EBADSTATEID;
	} else {
		uint64_t now = sc_now_mono_ns();
		uint64_t exp = atomic_load_explicit(&te->te_expire_ns,
						    memory_order_acquire);

		if (exp != 0 && exp <= now)
			ret = -EEXPIREDSTATEID;
	}
	trust_entry_put(te);
	return ret;
}

int ps_shortcircuit_write(const uint8_t *fh, uint32_t fh_len,
			  uint64_t block_offset, const uint8_t *data,
			  size_t data_len, uint32_t forwarded_uid,
			  uint32_t forwarded_gid, const stateid4 *layout_stid)
{
	uint64_t sb_id, ino;
	int ret;

	ret = decode_fh(fh, fh_len, &sb_id, &ino);
	if (ret)
		return ret;
	ret = check_stateid(layout_stid);
	if (ret)
		return ret;

	struct super_block *sb;
	struct inode *inode;

	ret = lookup_target(sb_id, ino, &sb, &inode);
	if (ret)
		return ret;

	if (!creds_match(inode, forwarded_uid, forwarded_gid)) {
		finish_lookup(sb, inode);
		return -EACCES;
	}

	/*
	 * Lock ordering matches lib/nfs4/server/chunk.c's nfs4_op_chunk_write:
	 * i_db_rwlock (write) covers both the lazy data_block_alloc and
	 * the size bookkeeping.  The Phase 5 design mandates "the short
	 * path must produce a state byte-identical to the RPC path", so
	 * we follow the same lock + size-update sequence the DS would
	 * have run inside its CHUNK_WRITE handler.
	 */
	pthread_rwlock_wrlock(&inode->i_db_rwlock);
	if (!inode->i_db) {
		inode->i_db = data_block_alloc(inode, (const char *)data,
					       data_len, (off_t)block_offset);
		ret = inode->i_db ? 0 : -ENOSPC;
	} else {
		ssize_t wret = data_block_write(inode->i_db, (const char *)data,
						data_len, (off_t)block_offset);
		ret = (wret < 0) ? (int)wret : 0;
	}
	if (ret == 0) {
		int64_t new_end = (int64_t)block_offset + (int64_t)data_len;

		if (new_end > inode->i_size)
			inode->i_size = new_end;
	}
	pthread_rwlock_unlock(&inode->i_db_rwlock);

	finish_lookup(sb, inode);
	return ret;
}

void ps_shortcircuit_install(struct ps_listener_state *pls)
{
	if (!pls)
		return;
	/*
	 * Plain assignment is safe: ps_shortcircuit_install runs at
	 * register time, before any worker can drive a pipeline call
	 * that consults pls_sc_write_fn / pls_sc_read_fn.  The publish
	 * edge is the release-store on ps_nlisteners inside
	 * ps_state_register, the same edge that fences pls_local_addrs.
	 */
	pls->pls_sc_write_fn = ps_shortcircuit_write;
	pls->pls_sc_read_fn = ps_shortcircuit_read;
}

int ps_shortcircuit_read(const uint8_t *fh, uint32_t fh_len,
			 uint64_t block_offset, size_t buf_len, uint8_t *buf,
			 uint32_t *nread, uint32_t forwarded_uid,
			 uint32_t forwarded_gid, const stateid4 *layout_stid)
{
	uint64_t sb_id, ino;
	int ret;

	if (nread)
		*nread = 0;
	ret = decode_fh(fh, fh_len, &sb_id, &ino);
	if (ret)
		return ret;
	ret = check_stateid(layout_stid);
	if (ret)
		return ret;

	struct super_block *sb;
	struct inode *inode;

	ret = lookup_target(sb_id, ino, &sb, &inode);
	if (ret)
		return ret;

	if (!creds_match(inode, forwarded_uid, forwarded_gid)) {
		finish_lookup(sb, inode);
		return -EACCES;
	}

	pthread_rwlock_rdlock(&inode->i_db_rwlock);
	if (inode->i_db) {
		ssize_t rret = data_block_read(inode->i_db, (char *)buf,
					       buf_len, (off_t)block_offset);
		if (rret < 0)
			ret = (int)rret;
		else if (nread)
			*nread = (uint32_t)rret;
	}
	/*
	 * Missing i_db is a valid "file written zero bytes" state on the
	 * RPC path (chunk_read returns the eof flag with no bytes); the
	 * short path mirrors that by leaving *nread == 0 and returning 0.
	 */
	pthread_rwlock_unlock(&inode->i_db_rwlock);

	finish_lookup(sb, inode);
	return ret;
}
