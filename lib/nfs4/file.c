/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/stateid.h"
#include "nfs4/client.h"

/* Maximum bytes we'll service in a single READ or WRITE. */
#define NFS4_MAX_RW_SIZE (1u << 20) /* 1 MiB */

/*
 * nfs4_stateid_resolve - validate a wire stateid4 and return the
 * corresponding in-memory struct stateid (ref-bumped), or NULL for the
 * special stateids that bypass stateid-level checks.
 *
 * On success sets *out_stid and returns NFS4_OK.
 * On error returns the appropriate nfsstat4; *out_stid is unmodified.
 *
 * want_write: reject read-only stateids (read-bypass, delegation-read).
 */
static nfsstat4 nfs4_stateid_resolve(struct compound *c, const stateid4 *wire,
				     bool want_write, struct stateid **out_stid)
{
	/* Anonymous stateid — caller falls through to POSIX permission check. */
	if (stateid4_is_anonymous(wire)) {
		*out_stid = NULL;
		return NFS4_OK;
	}

	/* Read-bypass stateid — skip all checks for READ, reject for WRITE. */
	if (stateid4_is_read_bypass(wire)) {
		if (want_write)
			return NFS4ERR_OPENMODE;
		*out_stid = NULL;
		return NFS4_OK;
	}

	/* Current stateid — use whatever the compound already holds. */
	if (stateid4_is_current(wire)) {
		if (!c->c_curr_stid)
			return NFS4ERR_BAD_STATEID;
		*out_stid = stateid_get(c->c_curr_stid);
		return NFS4_OK;
	}

	/* Regular stateid — unpack and validate fully. */
	uint32_t seqid, id, type, cookie;
	unpack_stateid4(wire, &seqid, &id, &type, &cookie);

	if (type >= Max_Stateid)
		return NFS4ERR_BAD_STATEID;

	/* Layout stateids are not used for I/O operations. */
	if (type == Layout_Stateid)
		return NFS4ERR_BAD_STATEID;

	/*
	 * A read-delegation stateid cannot authorise a write.
	 * (Write-delegations are not yet issued, so any delegation stateid
	 * here implies read-only.)
	 */
	if (want_write && type == Delegation_Stateid)
		return NFS4ERR_OPENMODE;

	struct stateid *stid = stateid_find(c->c_inode, id);
	if (!stid)
		return NFS4ERR_BAD_STATEID;

	/* Verify the type tag and cookie both match. */
	if (stid->s_tag != type || stid->s_cookie != cookie) {
		stateid_put(stid);
		return NFS4ERR_BAD_STATEID;
	}

	/* Verify ownership: stateid must belong to this session's client. */
	if (c->c_nfs4_client &&
	    stid->s_client != nfs4_client_to_client(c->c_nfs4_client)) {
		stateid_put(stid);
		return NFS4ERR_BAD_STATEID;
	}

	/*
	 * Verify seqid (RFC 5661 §8.1.3.1):
	 *   seqid == 0 in the request is a wildcard — match any current seqid.
	 *   seqid < current_seqid → NFS4ERR_OLD_STATEID
	 *   seqid > current_seqid → NFS4ERR_BAD_STATEID
	 */
	uint32_t cur_seqid = __atomic_load_n(&stid->s_seqid, __ATOMIC_RELAXED);
	if (seqid != 0) {
		if (seqid < cur_seqid) {
			stateid_put(stid);
			return NFS4ERR_OLD_STATEID;
		}
		if (seqid > cur_seqid) {
			stateid_put(stid);
			return NFS4ERR_BAD_STATEID;
		}
	}

	/* For open stateids, verify the access mode allows this I/O. */
	if (type == Open_Stateid) {
		struct open_stateid *os = stid_to_open(stid);
		uint64_t need = want_write ? OPEN_STATEID_ACCESS_WRITE :
					     OPEN_STATEID_ACCESS_READ;
		if (!(os->os_state & need)) {
			stateid_put(stid);
			return NFS4ERR_OPENMODE;
		}
	}

	*out_stid = stid;
	return NFS4_OK;
}

/*
 * Build the 8-byte write verifier.  The verifier is constant within a
 * server boot but changes across restarts.  We derive it from the first
 * six bytes of the server UUID (stable across reboots) plus the two-byte
 * boot_seq (incremented on every restart).
 */
static void nfs4_write_verf(verifier4 out_verf)
{
	struct server_state *ss = server_state_find();

	if (!ss) {
		memset(out_verf, 0, NFS4_VERIFIER_SIZE);
		return;
	}

	memcpy(out_verf, ss->ss_uuid, NFS4_VERIFIER_SIZE - 2);
	uint16_t boot_seq = server_boot_seq(ss);
	memcpy(out_verf + NFS4_VERIFIER_SIZE - 2, &boot_seq, 2);
	server_state_put(ss);
}

void nfs4_op_open(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPEN4args *args = NFS4_OP_ARG_SETUP(c, ph, opopen);
	OPEN4res *res = NFS4_OP_RES_SETUP(c, ph, opopen);
	nfsstat4 *status = &res->status;
	OPEN4resok *resok = NFS4_OP_RESOK_SETUP(res, OPEN4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_open_confirm(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPEN_CONFIRM4args *args = NFS4_OP_ARG_SETUP(c, ph, opopen_confirm);
	OPEN_CONFIRM4res *res = NFS4_OP_RES_SETUP(c, ph, opopen_confirm);
	nfsstat4 *status = &res->status;
	OPEN_CONFIRM4resok *resok =
		NFS4_OP_RESOK_SETUP(res, OPEN_CONFIRM4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_open_downgrade(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPEN_DOWNGRADE4args *args = NFS4_OP_ARG_SETUP(c, ph, opopen_downgrade);
	OPEN_DOWNGRADE4res *res = NFS4_OP_RES_SETUP(c, ph, opopen_downgrade);
	nfsstat4 *status = &res->status;
	OPEN_DOWNGRADE4resok *resok =
		NFS4_OP_RESOK_SETUP(res, OPEN_DOWNGRADE4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_close(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	CLOSE4args *args = NFS4_OP_ARG_SETUP(c, ph, opclose);
	CLOSE4res *res = NFS4_OP_RES_SETUP(c, ph, opclose);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_read(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READ4args *args = NFS4_OP_ARG_SETUP(c, ph, opread);
	READ4res *res = NFS4_OP_RES_SETUP(c, ph, opread);
	nfsstat4 *status = &res->status;
	READ4resok *resok = NFS4_OP_RESOK_SETUP(res, READ4res_u, resok4);

	struct stateid *stid = NULL;

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISREG(c->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	*status = nfs4_stateid_resolve(c, &args->stateid, false, &stid);
	if (*status != NFS4_OK)
		goto out;

	/*
	 * For anonymous and regular stateids, verify POSIX read permission.
	 * Read-bypass skips this check (stid == NULL and seqid == UINT32_MAX).
	 */
	if (!stateid4_is_read_bypass(&args->stateid)) {
		int ret = inode_access_check(c->c_inode, &c->c_ap, R_OK);
		if (ret) {
			*status = errno_to_nfs4(ret, OP_READ);
			goto out;
		}
	}

	/* Clamp to a server-side limit. */
	count4 req_count = args->count;
	if (req_count > NFS4_MAX_RW_SIZE)
		req_count = NFS4_MAX_RW_SIZE;

	if (!c->c_inode->i_db || args->offset >= (uint64_t)c->c_inode->i_size) {
		resok->eof = true;
		resok->data.data_len = 0;
		resok->data.data_val = NULL;
		*status = NFS4_OK;
		goto out;
	}

	if (req_count == 0) {
		resok->eof = (args->offset >= (uint64_t)c->c_inode->i_size);
		resok->data.data_len = 0;
		resok->data.data_val = NULL;
		*status = NFS4_OK;
		goto out;
	}

	resok->data.data_val = calloc(req_count, 1);
	if (!resok->data.data_val) {
		*status = NFS4ERR_DELAY;
		goto out;
	}
	resok->data.data_len = req_count;

	pthread_rwlock_rdlock(&c->c_inode->i_db_rwlock);
	ssize_t nread = data_block_read(c->c_inode->i_db, resok->data.data_val,
					req_count, args->offset);
	if (nread < 0) {
		free(resok->data.data_val);
		resok->data.data_val = NULL;
		resok->data.data_len = 0;
		pthread_rwlock_unlock(&c->c_inode->i_db_rwlock);
		*status = NFS4ERR_IO;
		goto out;
	}

	resok->data.data_len = (u_int)nread;
	resok->eof = (args->offset + (uint64_t)nread >=
		      (uint64_t)c->c_inode->i_size);
	pthread_rwlock_unlock(&c->c_inode->i_db_rwlock);

	pthread_mutex_lock(&c->c_inode->i_attr_mutex);
	inode_update_times_now(c->c_inode, REFFS_INODE_UPDATE_ATIME);
	pthread_mutex_unlock(&c->c_inode->i_attr_mutex);

	*status = NFS4_OK;

out:
	stateid_put(stid);
	LOG("%s status=%s(%d) offset=%llu count=%u", __func__,
	    nfs4_err_name(*status), *status, (unsigned long long)args->offset,
	    args->count);
}

void nfs4_op_read_plus(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READ_PLUS4args *args = NFS4_OP_ARG_SETUP(c, ph, opread_plus);
	READ_PLUS4res *res = NFS4_OP_RES_SETUP(c, ph, opread_plus);
	nfsstat4 *status = &res->rp_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_write(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	WRITE4args *args = NFS4_OP_ARG_SETUP(c, ph, opwrite);
	WRITE4res *res = NFS4_OP_RES_SETUP(c, ph, opwrite);
	nfsstat4 *status = &res->status;
	WRITE4resok *resok = NFS4_OP_RESOK_SETUP(res, WRITE4res_u, resok4);

	struct stateid *stid = NULL;
	struct super_block *sb = c->c_curr_sb;

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISREG(c->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	*status = nfs4_stateid_resolve(c, &args->stateid, true, &stid);
	if (*status != NFS4_OK)
		goto out;

	int ret = inode_access_check(c->c_inode, &c->c_ap, W_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_WRITE);
		goto out;
	}

	/* Zero-length write is a no-op (RFC 5661 §18.32.3). */
	if (args->data.data_len == 0) {
		resok->count = 0;
		resok->committed = FILE_SYNC4;
		nfs4_write_verf(resok->writeverf);
		*status = NFS4_OK;
		goto out;
	}

	/* Clamp to server-side limit. */
	u_int write_len = args->data.data_len;
	if (write_len > NFS4_MAX_RW_SIZE)
		write_len = NFS4_MAX_RW_SIZE;

	/* Clear SUID/SGID on write by an unprivileged user. */
	if ((c->c_inode->i_mode & S_ISUID) && c->c_ap.aup_uid != 0 &&
	    c->c_ap.aup_uid != c->c_inode->i_uid)
		c->c_inode->i_mode &= ~S_ISUID;
	if ((c->c_inode->i_mode & S_ISGID) && c->c_ap.aup_uid != 0 &&
	    c->c_ap.aup_uid != c->c_inode->i_uid)
		c->c_inode->i_mode &= ~S_ISGID;

	int64_t old_size;
	pthread_rwlock_wrlock(&c->c_inode->i_db_rwlock);

	old_size = c->c_inode->i_size;

	if (!c->c_inode->i_db) {
		c->c_inode->i_db = data_block_alloc(c->c_inode,
						    args->data.data_val,
						    write_len, args->offset);
		if (!c->c_inode->i_db) {
			pthread_rwlock_unlock(&c->c_inode->i_db_rwlock);
			*status = NFS4ERR_NOSPC;
			goto out;
		}
		resok->count = write_len;
	} else {
		ssize_t nwritten = data_block_write(c->c_inode->i_db,
						    args->data.data_val,
						    write_len, args->offset);
		if (nwritten < 0) {
			pthread_rwlock_unlock(&c->c_inode->i_db_rwlock);
			*status = (nwritten == -ENOSPC) ? NFS4ERR_NOSPC :
							  NFS4ERR_IO;
			goto out;
		}
		resok->count = (count4)nwritten;
	}

	c->c_inode->i_size = (int64_t)c->c_inode->i_db->db_size;
	c->c_inode->i_used = c->c_inode->i_size / sb->sb_block_size +
			     (c->c_inode->i_size % sb->sb_block_size ? 1 : 0);

	/* Track superblock space usage. */
	size_t new_db_size = data_block_get_size(c->c_inode->i_db);
	size_t old_used, new_used;
	do {
		__atomic_load(&sb->sb_bytes_used, &old_used, __ATOMIC_RELAXED);
		if (new_db_size > (size_t)old_size)
			new_used = old_used + (new_db_size - (size_t)old_size);
		else if ((size_t)old_size > new_db_size)
			new_used = old_used > (size_t)old_size - new_db_size ?
					   old_used - ((size_t)old_size -
						       new_db_size) :
					   0;
		else
			new_used = old_used;
	} while (!__atomic_compare_exchange(&sb->sb_bytes_used, &old_used,
					    &new_used, false, __ATOMIC_SEQ_CST,
					    __ATOMIC_RELAXED));

	pthread_rwlock_unlock(&c->c_inode->i_db_rwlock);

	pthread_mutex_lock(&c->c_inode->i_attr_mutex);
	inode_update_times_now(c->c_inode, REFFS_INODE_UPDATE_MTIME |
						   REFFS_INODE_UPDATE_CTIME);
	pthread_mutex_unlock(&c->c_inode->i_attr_mutex);

	inode_sync_to_disk(c->c_inode);

	/* Always commit synchronously for now. */
	resok->committed = FILE_SYNC4;
	nfs4_write_verf(resok->writeverf);

	*status = NFS4_OK;

out:
	stateid_put(stid);
	LOG("%s status=%s(%d) offset=%llu count=%u stable=%d", __func__,
	    nfs4_err_name(*status), *status, (unsigned long long)args->offset,
	    args->data.data_len, args->stable);
}

void nfs4_op_write_same(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	WRITE_SAME4args *args = NFS4_OP_ARG_SETUP(c, ph, opwrite_same);
	WRITE_SAME4res *res = NFS4_OP_RES_SETUP(c, ph, opwrite_same);
	nfsstat4 *status = &res->wsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_commit(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	COMMIT4args *args = NFS4_OP_ARG_SETUP(c, ph, opcommit);
	COMMIT4res *res = NFS4_OP_RES_SETUP(c, ph, opcommit);
	nfsstat4 *status = &res->status;
	COMMIT4resok *resok = NFS4_OP_RESOK_SETUP(res, COMMIT4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_seek(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SEEK4args *args = NFS4_OP_ARG_SETUP(c, ph, opseek);
	SEEK4res *res = NFS4_OP_RES_SETUP(c, ph, opseek);
	nfsstat4 *status = &res->sa_status;
	seek_res4 *resok = NFS4_OP_RESOK_SETUP(res, SEEK4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_allocate(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	ALLOCATE4args *args = NFS4_OP_ARG_SETUP(c, ph, opallocate);
	ALLOCATE4res *res = NFS4_OP_RES_SETUP(c, ph, opallocate);
	nfsstat4 *status = &res->ar_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_deallocate(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	DEALLOCATE4args *args = NFS4_OP_ARG_SETUP(c, ph, opdeallocate);
	DEALLOCATE4res *res = NFS4_OP_RES_SETUP(c, ph, opdeallocate);
	nfsstat4 *status = &res->dr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}
