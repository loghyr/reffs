/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/inode.h"
#include "reffs/data_block.h"
#include "reffs/identity.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "nfs4/compound.h"
#include "nfs4/errors.h"
#include "nfs4/ops.h"

/*
 * COPY — RFC 7862 S15.2 (synchronous only).
 *
 * Copies data from SAVED_FH (source) to CURRENT_FH (destination).
 * Both must be regular files.  The compound sets up the FHs via
 * PUTFH(src) + SAVEFH + PUTFH(dst) + COPY.
 *
 * Synchronous only: ca_synchronous must be TRUE, the server performs
 * the copy inline and returns the result.  Async COPY (WI-3.6) is
 * deferred (NOT_NOW_BROWN_COW).
 *
 * Cross-sb COPY returns NFS4ERR_XDEV for now.  The compound's PUTFH
 * ops already enforce per-export security (NFS4ERR_WRONGSEC).
 *
 * POSIX backend: copy_file_range() for zero-copy where supported.
 * RAM backend: data_block_read + data_block_write.
 */
uint32_t nfs4_op_copy(struct compound *compound)
{
	COPY4args *args = NFS4_OP_ARG_SETUP(compound, opcopy);
	COPY4res *res = NFS4_OP_RES_SETUP(compound, opcopy);
	nfsstat4 *status = &res->cr_status;
	COPY4resok *resok = NFS4_OP_RESOK_SETUP(res, COPY4res_u, cr_resok4);
	struct stateid *src_stid = NULL;
	struct stateid *dst_stid = NULL;
	struct inode *src_inode = NULL;
	uint64_t copied = 0;

	/* Both current (dst) and saved (src) FH must be set. */
	if (network_file_handle_empty(&compound->c_curr_nfh) ||
	    network_file_handle_empty(&compound->c_saved_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (nfs4_check_grace()) {
		*status = NFS4ERR_GRACE;
		goto out;
	}

	/* Synchronous only for now. */
	if (!args->ca_synchronous) {
		/* Return requirements: we only support synchronous. */
		res->COPY4res_u.cr_requirements.cr_consecutive = TRUE;
		res->COPY4res_u.cr_requirements.cr_synchronous = TRUE;
		*status = NFS4ERR_OFFLOAD_NO_REQS;
		goto out;
	}

	/* Cross-sb COPY deferred (NOT_NOW_BROWN_COW). */
	if (compound->c_curr_sb != compound->c_saved_sb) {
		*status = NFS4ERR_XDEV;
		goto out;
	}

	/* Look up source inode from saved FH. */
	src_inode =
		inode_find(compound->c_saved_sb, compound->c_saved_nfh.nfh_ino);
	if (!src_inode) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	/* Both must be regular files. */
	if (!S_ISREG(src_inode->i_mode) ||
	    !S_ISREG(compound->c_inode->i_mode)) {
		*status = NFS4ERR_WRONG_TYPE;
		goto out;
	}

	/*
	 * Same-file COPY with overlapping ranges can corrupt data
	 * in the RAM backend (read + write on the same data_block).
	 * copy_file_range() handles same-file correctly for POSIX,
	 * but reject same-file here for safety.
	 */
	if (src_inode->i_ino == compound->c_inode->i_ino) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/* Validate stateids: source=read, destination=write. */
	*status = nfs4_stateid_resolve(compound, src_inode,
				       &args->ca_src_stateid, false, &src_stid);
	if (*status)
		goto out;

	*status = nfs4_stateid_resolve(compound, compound->c_inode,
				       &args->ca_dst_stateid, true, &dst_stid);
	if (*status)
		goto out;

	/* Permission checks. */
	int ret = inode_access_check(src_inode, &compound->c_ap, R_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_COPY);
		goto out;
	}

	ret = inode_access_check(compound->c_inode, &compound->c_ap, W_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_COPY);
		goto out;
	}

	/* Determine copy range. ca_count == 0 means copy to EOF. */
	uint64_t src_offset = args->ca_src_offset;
	uint64_t dst_offset = args->ca_dst_offset;
	uint64_t count = args->ca_count;

	if (count == 0) {
		if (src_offset >= (uint64_t)src_inode->i_size) {
			/* Nothing to copy — success with 0 bytes. */
			resok->cr_response.wr_count = 0;
			resok->cr_response.wr_committed = FILE_SYNC4;
			nfs4_write_verf(compound->c_server_state,
					resok->cr_response.wr_writeverf);
			resok->cr_requirements.cr_consecutive = TRUE;
			resok->cr_requirements.cr_synchronous = TRUE;
			goto out;
		}
		count = (uint64_t)src_inode->i_size - src_offset;
	}

	/* Overflow check. */
	if (src_offset > UINT64_MAX - count ||
	    dst_offset > UINT64_MAX - count) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/* Source range must not exceed source file size. */
	if (src_offset + count > (uint64_t)src_inode->i_size) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/* Ensure source has data. */
	if (!src_inode->i_db) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/* Ensure destination has a data block. */
	if (!compound->c_inode->i_db) {
		compound->c_inode->i_db =
			data_block_alloc(compound->c_inode, NULL, 0, 0);
		if (!compound->c_inode->i_db) {
			*status = NFS4ERR_NOSPC;
			goto out;
		}
	}

	/*
	 * Perform the copy.  Try copy_file_range() for POSIX backend
	 * (zero-copy where the kernel supports it).  Fall back to
	 * read + write for RAM backend or if FDs aren't available.
	 *
	 * We hold the destination rwlock for size updates but NOT the
	 * source rwlock.  The source is protected by the client's read
	 * stateid — no truncation can occur while the open is held.
	 */
	int src_fd = data_block_get_fd(src_inode->i_db);
	int dst_fd = data_block_get_fd(compound->c_inode->i_db);
	pthread_rwlock_wrlock(&compound->c_inode->i_db_rwlock);

	if (src_fd >= 0 && dst_fd >= 0) {
		/* POSIX path: copy_file_range for zero-copy. */
		loff_t soff = (loff_t)src_offset;
		loff_t doff = (loff_t)dst_offset;
		uint64_t remaining = count;

		while (remaining > 0) {
			ssize_t n = copy_file_range(src_fd, &soff, dst_fd,
						    &doff, (size_t)remaining,
						    0);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				if (copied > 0)
					break; /* partial copy is success */
				pthread_rwlock_unlock(
					&compound->c_inode->i_db_rwlock);
				*status = (errno == ENOSPC) ? NFS4ERR_NOSPC :
							      NFS4ERR_IO;
				goto out;
			}
			if (n == 0)
				break; /* EOF */
			copied += (uint64_t)n;
			remaining -= (uint64_t)n;
		}
	} else {
		/* RAM backend: read + write through a temp buffer. */
#define COPY_BUFSZ (256 * 1024)
		char *buf = malloc(COPY_BUFSZ);
		if (!buf) {
			pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
			*status = NFS4ERR_DELAY;
			goto out;
		}

		uint64_t remaining = count;
		uint64_t soff = src_offset;
		uint64_t doff = dst_offset;

		while (remaining > 0) {
			size_t chunk = (remaining > COPY_BUFSZ) ?
					       COPY_BUFSZ :
					       (size_t)remaining;
			ssize_t nr = data_block_read(src_inode->i_db, buf,
						     chunk, soff);
			if (nr <= 0)
				break;

			ssize_t nw = data_block_write(compound->c_inode->i_db,
						      buf, (size_t)nr, doff);
			if (nw <= 0) {
				if (copied > 0)
					break;
				free(buf);
				pthread_rwlock_unlock(
					&compound->c_inode->i_db_rwlock);
				*status = NFS4ERR_IO;
				goto out;
			}

			copied += (uint64_t)nw;
			soff += (uint64_t)nw;
			doff += (uint64_t)nw;
			remaining -= (uint64_t)nw;
		}
		free(buf);
#undef COPY_BUFSZ
	}

	/* Update destination size if the copy extended it. */
	struct super_block *sb = compound->c_curr_sb;
	uint64_t copy_end = dst_offset + copied;

	if (copy_end > (uint64_t)compound->c_inode->i_size) {
		int64_t old_size = compound->c_inode->i_size;
		compound->c_inode->i_size = (int64_t)copy_end;
		compound->c_inode->i_used =
			compound->c_inode->i_size / sb->sb_block_size +
			(compound->c_inode->i_size % sb->sb_block_size ? 1 : 0);
		compound->c_inode->i_db->db_size = (size_t)copy_end;

		size_t old_used, new_used;
		old_used = atomic_load_explicit(&sb->sb_bytes_used,
						memory_order_relaxed);
		do {
			new_used = old_used +
				   ((size_t)copy_end - (size_t)old_size);
		} while (!atomic_compare_exchange_strong_explicit(
			&sb->sb_bytes_used, &old_used, new_used,
			memory_order_acq_rel, memory_order_relaxed));
	}
	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);

	/* Update timestamps. */
	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	inode_update_times_now(compound->c_inode,
			       REFFS_INODE_UPDATE_CTIME |
				       REFFS_INODE_UPDATE_MTIME);
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	inode_sync_to_disk(compound->c_inode);

	/* Fill the response. */
	resok->cr_response.wr_callback_id.wr_callback_id_len = 0;
	resok->cr_response.wr_callback_id.wr_callback_id_val = NULL;
	resok->cr_response.wr_count = copied;
	resok->cr_response.wr_committed = FILE_SYNC4;
	nfs4_write_verf(compound->c_server_state,
			resok->cr_response.wr_writeverf);

	resok->cr_requirements.cr_consecutive = TRUE;
	resok->cr_requirements.cr_synchronous = TRUE;

out:
	inode_active_put(src_inode);
	stateid_put(src_stid);
	stateid_put(dst_stid);
	TRACE("%s status=%s(%d) src_off=%llu dst_off=%llu count=%llu copied=%llu",
	      __func__, nfs4_err_name(*status), *status,
	      (unsigned long long)args->ca_src_offset,
	      (unsigned long long)args->ca_dst_offset,
	      (unsigned long long)args->ca_count, (unsigned long long)copied);

	return 0;
}

uint32_t nfs4_op_copy_notify(struct compound *compound)
{
	COPY_NOTIFY4res *res = NFS4_OP_RES_SETUP(compound, opcopy_notify);
	nfsstat4 *status = &res->cnr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

/*
 * CLONE — RFC 7862 S15.13.
 *
 * Reflink (shared blocks, copy-on-write) from SAVED_FH (source) to
 * CURRENT_FH (destination).  Both must be regular files in the same
 * filesystem (superblock).
 *
 * Standalone mode only; MDS fan-out deferred (NOT_NOW_BROWN_COW).
 *
 * The underlying filesystem must support FICLONE_RANGE (XFS, Btrfs).
 * RAM backend and non-reflink POSIX filesystems return NFS4ERR_NOTSUPP.
 */
uint32_t nfs4_op_clone(struct compound *compound)
{
	CLONE4args *args = NFS4_OP_ARG_SETUP(compound, opclone);
	CLONE4res *res = NFS4_OP_RES_SETUP(compound, opclone);
	nfsstat4 *status = &res->cl_status;
	struct stateid *src_stid = NULL;
	struct stateid *dst_stid = NULL;
	struct inode *src_inode = NULL;

	/* Both current and saved FH must be set. */
	if (network_file_handle_empty(&compound->c_curr_nfh) ||
	    network_file_handle_empty(&compound->c_saved_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (nfs4_check_grace()) {
		*status = NFS4ERR_GRACE;
		goto out;
	}

	/* Source and destination must be in the same filesystem. */
	if (compound->c_curr_sb != compound->c_saved_sb) {
		*status = NFS4ERR_XDEV;
		goto out;
	}

	/* Look up the source inode from the saved FH. */
	src_inode =
		inode_find(compound->c_saved_sb, compound->c_saved_nfh.nfh_ino);
	if (!src_inode) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	/* Both must be regular files. */
	if (!S_ISREG(src_inode->i_mode) ||
	    !S_ISREG(compound->c_inode->i_mode)) {
		*status = NFS4ERR_WRONG_TYPE;
		goto out;
	}

	/* Source and destination must be different files. */
	if (src_inode->i_ino == compound->c_inode->i_ino) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/* Validate stateids: source=read, destination=write. */
	*status = nfs4_stateid_resolve(compound, src_inode,
				       &args->cl_src_stateid, false, &src_stid);
	if (*status != NFS4_OK)
		goto out;

	*status = nfs4_stateid_resolve(compound, compound->c_inode,
				       &args->cl_dst_stateid, true, &dst_stid);
	if (*status != NFS4_OK)
		goto out;

	/* Permission checks. */
	int ret = inode_access_check(src_inode, &compound->c_ap, R_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_CLONE);
		goto out;
	}

	ret = inode_access_check(compound->c_inode, &compound->c_ap, W_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_CLONE);
		goto out;
	}

	/* Overflow checks on offset + count. */
	if (args->cl_count != 0) {
		if (args->cl_src_offset > UINT64_MAX - args->cl_count ||
		    args->cl_dst_offset > UINT64_MAX - args->cl_count) {
			*status = NFS4ERR_INVAL;
			goto out;
		}
	}

	/*
	 * Alignment check per RFC 7862 S15.13.3: offsets and count
	 * must be aligned to the clone block size advertised in
	 * FATTR4_CLONE_BLKSIZE.  cl_count == 0 (clone-to-EOF) is
	 * exempt since the kernel handles that alignment internally.
	 */
#define CLONE_BLKSIZE 4096
	if ((args->cl_src_offset % CLONE_BLKSIZE) ||
	    (args->cl_dst_offset % CLONE_BLKSIZE) ||
	    (args->cl_count != 0 && (args->cl_count % CLONE_BLKSIZE))) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/* Source must have data. */
	if (!src_inode->i_db) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/*
	 * CLONE requires FICLONE_RANGE support from the underlying
	 * filesystem.  Both FDs must be valid (POSIX backend).
	 */
	int src_fd = data_block_get_fd(src_inode->i_db);
	if (src_fd < 0) {
		/* RAM backend or released FD — reflink not possible. */
		*status = NFS4ERR_NOTSUPP;
		goto out;
	}

	/* Ensure destination has a data block. */
	if (!compound->c_inode->i_db) {
		compound->c_inode->i_db =
			data_block_alloc(compound->c_inode, NULL, 0, 0);
		if (!compound->c_inode->i_db) {
			*status = NFS4ERR_NOSPC;
			goto out;
		}
	}

	int dst_fd = data_block_get_fd(compound->c_inode->i_db);
	if (dst_fd < 0) {
		*status = NFS4ERR_NOTSUPP;
		goto out;
	}

	/*
	 * Determine the clone range.  cl_count == 0 means "clone to
	 * end of source file" (RFC 7862 S15.13.3).
	 */
	uint64_t src_offset = args->cl_src_offset;
	uint64_t dst_offset = args->cl_dst_offset;
	uint64_t count = args->cl_count;

	if (count == 0) {
		if (src_offset >= (uint64_t)src_inode->i_size) {
			/* Nothing to clone — success. */
			goto out;
		}
		count = (uint64_t)src_inode->i_size - src_offset;
	}

	/* Source range must not exceed source file size. */
	if (src_offset + count > (uint64_t)src_inode->i_size) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	struct file_clone_range fcr = {
		.src_fd = src_fd,
		.src_offset = src_offset,
		.src_length = count,
		.dest_offset = dst_offset,
	};

	ret = ioctl(dst_fd, FICLONERANGE, &fcr);
	if (ret < 0) {
		ret = errno;
		if (ret == EOPNOTSUPP || ret == ENOTSUP || ret == ENOTTY ||
		    ret == EXDEV) {
			*status = NFS4ERR_NOTSUPP;
		} else if (ret == ENOSPC || ret == EDQUOT) {
			*status = NFS4ERR_NOSPC;
		} else if (ret == EINVAL || ret == EBADF) {
			*status = NFS4ERR_INVAL;
		} else {
			*status = NFS4ERR_IO;
		}
		goto out;
	}

	/*
	 * Update destination size if the clone extended it.
	 * Lock ordering: i_db_rwlock first (for size), then i_attr_mutex
	 * (for timestamps), matching ALLOCATE/WRITE patterns.
	 */
	struct super_block *sb = compound->c_curr_sb;
	uint64_t clone_end = dst_offset + count;

	pthread_rwlock_wrlock(&compound->c_inode->i_db_rwlock);
	if (clone_end > (uint64_t)compound->c_inode->i_size) {
		int64_t old_size = compound->c_inode->i_size;
		compound->c_inode->i_size = (int64_t)clone_end;
		compound->c_inode->i_used =
			compound->c_inode->i_size / sb->sb_block_size +
			(compound->c_inode->i_size % sb->sb_block_size ? 1 : 0);

		/* Refresh db_size from the fd after the clone. */
		compound->c_inode->i_db->db_size = (size_t)clone_end;

		/* Update superblock space accounting. */
		size_t old_used, new_used;
		old_used = atomic_load_explicit(&sb->sb_bytes_used,
						memory_order_relaxed);
		do {
			new_used = old_used +
				   ((size_t)clone_end - (size_t)old_size);
		} while (!atomic_compare_exchange_strong_explicit(
			&sb->sb_bytes_used, &old_used, new_used,
			memory_order_acq_rel, memory_order_relaxed));
	}
	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	inode_update_times_now(compound->c_inode,
			       REFFS_INODE_UPDATE_CTIME |
				       REFFS_INODE_UPDATE_MTIME);
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	inode_sync_to_disk(compound->c_inode);

out:
	inode_active_put(src_inode);
	stateid_put(src_stid);
	stateid_put(dst_stid);
	TRACE("%s status=%s(%d) src_offset=%llu dst_offset=%llu count=%llu",
	      __func__, nfs4_err_name(*status), *status,
	      (unsigned long long)args->cl_src_offset,
	      (unsigned long long)args->cl_dst_offset,
	      (unsigned long long)args->cl_count);
	return 0;
}

uint32_t nfs4_op_offload_cancel(struct compound *compound)
{
	OFFLOAD_CANCEL4res *res = NFS4_OP_RES_SETUP(compound, opoffload_cancel);
	nfsstat4 *status = &res->ocr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_offload_status(struct compound *compound)
{
	OFFLOAD_STATUS4res *res = NFS4_OP_RES_SETUP(compound, opoffload_status);
	nfsstat4 *status = &res->osr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
