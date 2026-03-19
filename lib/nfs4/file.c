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
#include "reffs/dirent.h"
#include "reffs/lock.h"
#include "reffs/vfs.h"
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

/*
 * No-op release for the lock owner embedded in open_stateid.  The owner
 * memory is part of the open_stateid allocation and is freed by the RCU
 * callback; we never need a separate release action.
 */
static void nfs4_open_owner_release(struct urcu_ref *ref)
{
	(void)ref;
}

void nfs4_op_open(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPEN4args *args = NFS4_OP_ARG_SETUP(c, ph, opopen);
	OPEN4res *res = NFS4_OP_RES_SETUP(c, ph, opopen);
	nfsstat4 *status = &res->status;
	OPEN4resok *resok = NFS4_OP_RESOK_SETUP(res, OPEN4res_u, resok4);

	struct open_stateid *os = NULL;
	struct reffs_share *share = NULL;
	struct inode *child = NULL; /* active ref; owned for CLAIM_NULL */
	struct reffs_dirent *child_de = NULL;
	char *name = NULL;
	int ret;

	/*
	 * Strip the delegation-want hint bits (upper 24 bits) from
	 * share_access before any conflict or mode checks.
	 */
	uint32_t share_access = args->share_access & OPEN4_SHARE_ACCESS_BOTH;
	uint32_t share_deny = args->share_deny;
	bool want_xor_deleg = !!(args->share_access &
				 OPEN4_SHARE_ACCESS_WANT_OPEN_XOR_DELEGATION);

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (share_access == 0 || share_deny > OPEN4_SHARE_DENY_BOTH) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/* Resolve target inode based on claim type. */
	switch (args->claim.claim) {
	case CLAIM_FH:
		/*
		 * Current FH is the target file — no lookup needed.
		 * CREATE is not valid with CLAIM_FH.
		 */
		if (args->openhow.opentype == OPEN4_CREATE) {
			*status = NFS4ERR_INVAL;
			goto out;
		}
		break;

	case CLAIM_NULL: {
		if (!S_ISDIR(c->c_inode->i_mode)) {
			*status = NFS4ERR_NOTDIR;
			goto out;
		}

		component4 *fname = &args->claim.open_claim4_u.file;

		if (fname->utf8string_len == 0) {
			*status = NFS4ERR_INVAL;
			goto out;
		}
		if (fname->utf8string_len > REFFS_MAX_NAME) {
			*status = NFS4ERR_NAMETOOLONG;
			goto out;
		}
		name = strndup(fname->utf8string_val, fname->utf8string_len);
		if (!name) {
			*status = NFS4ERR_DELAY;
			goto out;
		}
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
			*status = NFS4ERR_BADNAME;
			goto out;
		}

		/* Need W_OK on the directory for CREATE, X_OK for NOCREATE. */
		int dir_amode =
			(args->openhow.opentype == OPEN4_CREATE) ? W_OK : X_OK;
		ret = inode_access_check(c->c_inode, &c->c_ap, dir_amode);
		if (ret) {
			*status = errno_to_nfs4(ret, OP_OPEN);
			goto out;
		}

		if (!c->c_inode->i_dirent) {
			ret = inode_reconstruct_path_to_root(c->c_inode);
			if (ret) {
				*status = NFS4ERR_STALE;
				goto out;
			}
		}

		if (args->openhow.opentype == OPEN4_CREATE) {
			createhow4 *how = &args->openhow.openflag4_u.how;

			switch (how->mode) {
			case UNCHECKED4:
				ret = vfs_create(c->c_inode, name, 0666,
						 &c->c_ap, &child);
				if (ret == -EEXIST) {
					/*
					 * File exists: open it, as if the
					 * create had not been requested.
					 */
					child = inode_name_get_inode(c->c_inode,
								     name);
					ret = child ? 0 : -ENOENT;
				}
				break;

			case GUARDED4:
				ret = vfs_create(c->c_inode, name, 0666,
						 &c->c_ap, &child);
				/* -EEXIST → NFS4ERR_EXIST below */
				break;

			case EXCLUSIVE4_1:
				/*
				 * EXCLUSIVE4_1 (RFC 5661 §18.16.3): same
				 * exclusive-create semantics as EXCLUSIVE4
				 * but the verifier lives in
				 * ch_createboth.cva_verf and the client
				 * may supply optional create attributes in
				 * cva_attrs.  We apply the verifier cookie
				 * the same way and leave attrset empty
				 * (cva_attrs ignored for now).
				 */
				/* fall through */
			case EXCLUSIVE4: {
				/*
				 * Use the verifier4 as the ctime cookie.
				 * Map the 8-byte verifier the same way
				 * NFS3 does: first 4 bytes → tv_sec,
				 * last 4 bytes → tv_nsec.
				 */
				struct timespec verf_ts;
				verifier4 *v =
					(how->mode == EXCLUSIVE4_1) ?
						&how->createhow4_u.ch_createboth
							 .cva_verf :
						&how->createhow4_u.createverf;
				memcpy(&verf_ts.tv_sec, v, 4);
				memcpy(&verf_ts.tv_nsec, (uint8_t *)v + 4, 4);

				ret = vfs_create(c->c_inode, name, 0666,
						 &c->c_ap, &child);
				if (ret == 0) {
					/* New file: stamp ctime with verf. */
					pthread_mutex_lock(
						&child->i_attr_mutex);
					child->i_ctime = verf_ts;
					child->i_mtime = verf_ts;
					child->i_atime = verf_ts;
					child->i_btime = verf_ts;
					pthread_mutex_unlock(
						&child->i_attr_mutex);
					inode_sync_to_disk(child);
				} else if (ret == -EEXIST) {
					/*
					 * Possible idempotent retry.  Find
					 * the existing inode and check that
					 * its ctime matches the verifier.
					 */
					child_de = dirent_load_child_by_name(
						c->c_inode->i_dirent, name);
					if (!child_de) {
						*status = NFS4ERR_SERVERFAULT;
						goto out;
					}
					child = dirent_ensure_inode(child_de);
					if (!child) {
						*status = NFS4ERR_SERVERFAULT;
						goto out;
					}
					pthread_mutex_lock(
						&child->i_attr_mutex);
					bool match = child->i_ctime.tv_sec ==
							     verf_ts.tv_sec &&
						     child->i_ctime.tv_nsec ==
							     verf_ts.tv_nsec;
					pthread_mutex_unlock(
						&child->i_attr_mutex);
					if (!match) {
						*status = NFS4ERR_EXIST;
						goto out;
					}
					ret = 0;
				}
				break;
			}

			default:
				/* EXCLUSIVE4_1 and unknown modes */
				*status = NFS4ERR_NOTSUPP;
				goto out;
			}

			if (ret) {
				*status = ret == -EEXIST ?
						  NFS4ERR_EXIST :
					  ret == -ENOSPC ?
						  NFS4ERR_NOSPC :
						  errno_to_nfs4(ret, OP_OPEN);
				goto out;
			}
			if (!child) {
				*status = NFS4ERR_SERVERFAULT;
				goto out;
			}
		} else {
			/* NOCREATE: look up an existing file. */
			child_de = dirent_load_child_by_name(
				c->c_inode->i_dirent, name);
			if (!child_de) {
				*status = NFS4ERR_NOENT;
				goto out;
			}
			child = dirent_ensure_inode(child_de);
			if (!child) {
				*status = NFS4ERR_SERVERFAULT;
				goto out;
			}
		}
		break;
	}

	default:
		*status = NFS4ERR_NOTSUPP;
		goto out;
	}

	/* The target inode is child (CLAIM_NULL) or c->c_inode (CLAIM_FH). */
	struct inode *target = child ? child : c->c_inode;

	if (!S_ISREG(target->i_mode)) {
		*status = S_ISDIR(target->i_mode) ? NFS4ERR_ISDIR :
						    NFS4ERR_SYMLINK;
		goto out;
	}

	/* POSIX access check for the requested modes. */
	int amode = 0;
	if (share_access & OPEN4_SHARE_ACCESS_READ)
		amode |= R_OK;
	if (share_access & OPEN4_SHARE_ACCESS_WRITE)
		amode |= W_OK;
	ret = inode_access_check(target, &c->c_ap, amode);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_OPEN);
		goto out;
	}

	/* Allocate the open stateid. */
	struct client *client =
		c->c_nfs4_client ? nfs4_client_to_client(c->c_nfs4_client) :
				   NULL;
	os = open_stateid_alloc(target, client);
	if (!os) {
		*status = NFS4ERR_DELAY;
		goto out;
	}

	/*
	 * Initialise the embedded lock owner.  The initial urcu_ref count
	 * of 1 is the "state ref" that keeps the stateid alive until CLOSE
	 * explicitly drops it.
	 */
	urcu_ref_init(&os->os_owner.lo_ref);
	os->os_owner.lo_release = nfs4_open_owner_release;
	os->os_owner.lo_match = NULL;
	CDS_INIT_LIST_HEAD(&os->os_owner.lo_list);

	/* Build share reservation.  The share holds a ref on the owner. */
	share = calloc(1, sizeof(*share));
	if (!share) {
		stateid_inode_unhash(&os->os_stid);
		stateid_client_unhash(&os->os_stid);
		stateid_put(&os->os_stid);
		os = NULL;
		*status = NFS4ERR_DELAY;
		goto out;
	}
	urcu_ref_get(&os->os_owner.lo_ref);
	share->s_owner = &os->os_owner;
	share->s_inode = inode_active_get(target);
	share->s_access = share_access;
	share->s_mode = share_deny;

	pthread_mutex_lock(&target->i_lock_mutex);
	ret = reffs_share_add(target, share, NULL);
	pthread_mutex_unlock(&target->i_lock_mutex);
	if (ret) {
		/*
		 * Conflict: reffs_share_add returned -EACCES without
		 * consuming the share.  Free it and abort.
		 */
		reffs_share_free(share);
		share = NULL;
		stateid_inode_unhash(&os->os_stid);
		stateid_client_unhash(&os->os_stid);
		stateid_put(&os->os_stid);
		os = NULL;
		*status = NFS4ERR_SHARE_DENIED;
		goto out;
	}
	share = NULL; /* ownership transferred to inode's share list */

	/*
	 * Encode access and deny flags into os_state:
	 *   bits 0-1: OPEN4_SHARE_ACCESS_* (R/W)
	 *   bits 2-3: OPEN4_SHARE_DENY_* (R/W), shifted left by 2
	 */
	os->os_state = (uint64_t)share_access | ((uint64_t)share_deny << 2);

	/*
	 * RFC 5661 §8.1.3: open stateid seqid starts at 1.
	 * stateid_assign() initialises s_seqid to 0; bump it now.
	 */
	__atomic_fetch_add(&os->os_stid.s_seqid, 1, __ATOMIC_SEQ_CST);
	pack_stateid4(&resok->stateid, &os->os_stid);

	/*
	 * For CLAIM_NULL: switch the current FH from directory to the
	 * opened file, mirroring what LOOKUP does.
	 */
	if (child) {
		inode_active_put(c->c_inode);
		c->c_inode = child;
		c->c_curr_nfh.nfh_ino = child->i_ino;
		child = NULL; /* ownership transferred */
	}

	/*
	 * Give the compound a ref on the open stateid.  The initial "state
	 * ref" (refcount=1 from stateid_assign) remains and keeps the
	 * stateid alive after this compound completes.
	 */
	stateid_put(c->c_curr_stid);
	c->c_curr_stid = stateid_get(&os->os_stid);

	resok->cinfo.atomic = FALSE;
	resok->cinfo.before = 0;
	resok->cinfo.after = 0;

	/*
	 * OPEN4_RESULT_NO_OPEN_STATEID is set only when the server grants
	 * a delegation instead of an open stateid (RFC 9754).  We always
	 * return an open stateid, so this flag is never set.
	 */
	resok->rflags = OPEN4_RESULT_LOCKTYPE_POSIX;

	resok->attrset.bitmap4_len = 0;
	resok->attrset.bitmap4_val = NULL;

	/*
	 * We never grant delegations.  Inform the client with NONE_EXT +
	 * WND4_RESOURCE if it requested one; NONE otherwise.
	 */
	uint32_t want_deleg = args->share_access &
			      OPEN4_SHARE_ACCESS_WANT_DELEG_MASK;
	if (want_deleg && want_deleg != OPEN4_SHARE_ACCESS_WANT_NO_DELEG &&
	    want_deleg != OPEN4_SHARE_ACCESS_WANT_CANCEL && !want_xor_deleg) {
		resok->delegation.delegation_type = OPEN_DELEGATE_NONE_EXT;
		resok->delegation.open_delegation4_u.od_whynone.ond_why =
			WND4_RESOURCE;
	} else {
		resok->delegation.delegation_type = OPEN_DELEGATE_NONE;
	}

	*status = NFS4_OK;

out:
	inode_active_put(child); /* NULL-safe; only set if not transferred */
	dirent_put(child_de);
	free(name);
	LOG("%s status=%s(%d) claim=%d access=%u deny=%u", __func__,
	    nfs4_err_name(*status), *status, args->claim.claim, share_access,
	    share_deny);
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

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (stateid4_is_special(&args->open_stateid)) {
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	uint32_t seqid, id, type, cookie;
	unpack_stateid4(&args->open_stateid, &seqid, &id, &type, &cookie);

	if (type != Open_Stateid) {
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	struct stateid *stid = stateid_find(c->c_inode, id);
	if (!stid || stid->s_tag != Open_Stateid || stid->s_cookie != cookie) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	if (c->c_nfs4_client &&
	    stid->s_client != nfs4_client_to_client(c->c_nfs4_client)) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	uint32_t cur_seqid = __atomic_load_n(&stid->s_seqid, __ATOMIC_RELAXED);
	if (seqid != 0) {
		if (seqid < cur_seqid) {
			stateid_put(stid);
			*status = NFS4ERR_OLD_STATEID;
			goto out;
		}
		if (seqid > cur_seqid) {
			stateid_put(stid);
			*status = NFS4ERR_BAD_STATEID;
			goto out;
		}
	}

	struct open_stateid *os = stid_to_open(stid);

	/*
	 * New access/deny must be a subset of current.
	 * os_state bits 0-1: access (R/W), bits 2-3: deny (R/W).
	 */
	uint32_t new_access = args->share_access & OPEN4_SHARE_ACCESS_BOTH;
	uint32_t new_deny = args->share_deny & OPEN4_SHARE_DENY_BOTH;
	uint32_t cur_access = (uint32_t)(os->os_state & 0x3);
	uint32_t cur_deny = (uint32_t)((os->os_state >> 2) & 0x3);

	if ((new_access & ~cur_access) || (new_deny & ~cur_deny)) {
		stateid_put(stid);
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/*
	 * Build a new share with the downgraded modes.  reffs_share_add()
	 * detects the same owner and updates the existing entry in place,
	 * then frees the new share struct.
	 */
	struct reffs_share *share = calloc(1, sizeof(*share));
	if (!share) {
		stateid_put(stid);
		*status = NFS4ERR_DELAY;
		goto out;
	}
	urcu_ref_get(&os->os_owner.lo_ref);
	share->s_owner = &os->os_owner;
	share->s_inode = inode_active_get(c->c_inode);
	share->s_access = new_access;
	share->s_mode = new_deny;

	pthread_mutex_lock(&c->c_inode->i_lock_mutex);
	reffs_share_add(c->c_inode, share,
			NULL); /* always succeeds: downgrade */
	pthread_mutex_unlock(&c->c_inode->i_lock_mutex);

	/* Update os_state to reflect the new modes. */
	os->os_state = (uint64_t)new_access | ((uint64_t)new_deny << 2);

	/* Bump seqid (RFC 5661 §8.1.3.1). */
	__atomic_fetch_add(&stid->s_seqid, 1, __ATOMIC_SEQ_CST);
	pack_stateid4(&resok->open_stateid, stid);

	/* Update c_curr_stid to the downgraded stateid. */
	stateid_put(c->c_curr_stid);
	c->c_curr_stid = stid; /* transfer the find ref */

	*status = NFS4_OK;
	LOG("%s status=%s(%d) access=%u deny=%u", __func__,
	    nfs4_err_name(*status), *status, new_access, new_deny);
	return;

out:
	LOG("%s status=%s(%d)", __func__, nfs4_err_name(*status), *status);
}

void nfs4_op_close(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	CLOSE4args *args = NFS4_OP_ARG_SETUP(c, ph, opclose);
	CLOSE4res *res = NFS4_OP_RES_SETUP(c, ph, opclose);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (stateid4_is_special(&args->open_stateid)) {
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	uint32_t seqid, id, type, cookie;
	unpack_stateid4(&args->open_stateid, &seqid, &id, &type, &cookie);

	if (type != Open_Stateid) {
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	struct stateid *stid = stateid_find(c->c_inode, id);
	if (!stid || stid->s_tag != Open_Stateid || stid->s_cookie != cookie) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	if (c->c_nfs4_client &&
	    stid->s_client != nfs4_client_to_client(c->c_nfs4_client)) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	uint32_t cur_seqid = __atomic_load_n(&stid->s_seqid, __ATOMIC_RELAXED);
	if (seqid != 0) {
		if (seqid < cur_seqid) {
			stateid_put(stid);
			*status = NFS4ERR_OLD_STATEID;
			goto out;
		}
		if (seqid > cur_seqid) {
			stateid_put(stid);
			*status = NFS4ERR_BAD_STATEID;
			goto out;
		}
	}

	struct open_stateid *os = stid_to_open(stid);

	/* Remove the share reservation. */
	pthread_mutex_lock(&c->c_inode->i_lock_mutex);
	reffs_share_remove(c->c_inode, &os->os_owner, NULL);
	pthread_mutex_unlock(&c->c_inode->i_lock_mutex);

	/*
	 * Unhash so that future stateid_find() calls fail.  This must
	 * happen before we drop the state ref so no new caller can race.
	 */
	stateid_inode_unhash(stid);
	stateid_client_unhash(stid);

	/*
	 * If this compound's c_curr_stid points here, clear it so
	 * compound_free() does not do an extra put.
	 */
	if (c->c_curr_stid == stid) {
		stateid_put(stid); /* put the c_curr_stid ref */
		c->c_curr_stid = NULL;
	}

	/*
	 * Drop the stateid_find() ref and the initial "state ref" from
	 * open_stateid_alloc().  After both puts the stateid is freed
	 * via call_rcu().
	 */
	stateid_put(stid); /* find ref */
	stateid_put(stid); /* state ref → ref=0 → freed */

	/*
	 * RFC 5661 §18.2.4: return a dead stateid (seqid=0, other=zeros).
	 */
	res->CLOSE4res_u.open_stateid = stateid4_anonymous;
	*status = NFS4_OK;

out:
	LOG("%s status=%s(%d)", __func__, nfs4_err_name(*status), *status);
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

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISREG(c->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/*
	 * All writes are already FILE_SYNC4, so there is nothing to flush.
	 * Return the stable write verifier so the client can verify
	 * stability across server restarts.
	 */
	nfs4_write_verf(resok->writeverf);
	*status = NFS4_OK;

out:
	LOG("%s status=%s(%d) offset=%llu count=%u", __func__,
	    nfs4_err_name(*status), *status, (unsigned long long)args->offset,
	    args->count);
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
