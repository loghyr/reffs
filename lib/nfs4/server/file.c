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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/rpc.h"
#include "reffs/inode.h"
#include "reffs/data_block.h"
#include "reffs/identity.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "reffs/dirent.h"
#include "reffs/lock.h"
#include "reffs/vfs.h"
#include "reffs/utf8string.h"
#include "reffs/time.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/stateid.h"
#include "nfs4/client.h"
#include "nfs4/cb.h"
#include "nfs4/session.h"

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
static nfsstat4 nfs4_stateid_resolve(struct compound *compound,
				     const stateid4 *wire, bool want_write,
				     struct stateid **out_stid)
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
		if (!compound->c_curr_stid)
			return NFS4ERR_BAD_STATEID;
		*out_stid = stateid_get(compound->c_curr_stid);
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

	struct stateid *stid = stateid_find(compound->c_inode, id);
	if (!stid)
		return NFS4ERR_BAD_STATEID;

	/* Verify the type tag and cookie both match. */
	if (stid->s_tag != type || stid->s_cookie != cookie) {
		stateid_put(stid);
		return NFS4ERR_BAD_STATEID;
	}

	/* Verify ownership: stateid must belong to this session's client. */
	if (compound->c_nfs4_client &&
	    stid->s_client != nfs4_client_to_client(compound->c_nfs4_client)) {
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

	/*
	 * A read-delegation stateid cannot authorise a write; a
	 * write-delegation stateid (DELEG_STATEID_ACCESS_WRITE) can.
	 */
	if (type == Delegation_Stateid && want_write) {
		struct delegation_stateid *ds = stid_to_delegation(stid);
		if (!(ds->ds_state & DELEG_STATEID_ACCESS_WRITE)) {
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
static void nfs4_write_verf(struct server_state *ss, verifier4 out_verf)
{
	memcpy(out_verf, ss->ss_uuid, NFS4_VERIFIER_SIZE - 2);
	uint16_t boot_seq = server_boot_seq(ss);
	memcpy(out_verf + NFS4_VERIFIER_SIZE - 2, &boot_seq, 2);
}

/*
 * No-op release for the lock owner embedded in open_stateid.  The owner
 * memory is part of the open_stateid allocation and is freed by the RCU
 * callback; we never need a separate release action.
 */
static void nfs4_open_owner_release(struct urcu_ref __attribute__((unused)) *
				    ref)
{
}

static int
fill_delegation_permissions_from_mode(nfsace4 *ace, struct compound *compound,
				      open_delegation_type4 deleg_type)
{
	static char everyone_who[] = "EVERYONE@";
	mode_t mode;
	acemask4 mask = 0;
	bool all_read;
	bool all_write;
	bool all_exec;

	int ret;

	assert(ace != NULL);
	assert(compound != NULL);
	assert(compound->c_inode != NULL);

	mode = compound->c_inode->i_mode & 07777;

	/*
         * Conservative synthesis from POSIX mode bits:
         * advertise only permissions granted to EVERYONE,
         * i.e. the intersection of owner/group/other bits.
         */
	all_read = (mode & S_IRUSR) && (mode & S_IRGRP) && (mode & S_IROTH);
	all_write = (mode & S_IWUSR) && (mode & S_IWGRP) && (mode & S_IWOTH);
	all_exec = (mode & S_IXUSR) && (mode & S_IXGRP) && (mode & S_IXOTH);

	/*
         * A read delegation should only advertise read-side permissions.
         * A write delegation may advertise both read and write permissions.
         */
	if (all_read) {
		mask |= ACE4_READ_DATA;
		mask |= ACE4_READ_ATTRIBUTES;
		mask |= ACE4_READ_NAMED_ATTRS;
		mask |= ACE4_READ_ACL;
		mask |= ACE4_SYNCHRONIZE;
	}

	if (all_exec)
		mask |= ACE4_EXECUTE;

	if (deleg_type == OPEN_DELEGATE_WRITE && all_write) {
		mask |= ACE4_WRITE_DATA;
		mask |= ACE4_APPEND_DATA;

		/*
                 * Optional. Conservative servers often omit these.
                 * Add them only if you want broader write-side hinting.
                 */
		/* mask |= ACE4_WRITE_ATTRIBUTES; */
		/* mask |= ACE4_WRITE_NAMED_ATTRS; */
	}

	ace->type = ACE4_ACCESS_ALLOWED_ACE_TYPE;
	ace->flag = 0;
	ace->access_mask = mask;

	ret = cstr_to_utf8string(&ace->who, everyone_who);
	return ret;
}

uint32_t nfs4_op_open(struct compound *compound)
{
	OPEN4args *args = NFS4_OP_ARG_SETUP(compound, opopen);
	OPEN4res *res = NFS4_OP_RES_SETUP(compound, opopen);
	nfsstat4 *status = &res->status;
	OPEN4resok *resok = NFS4_OP_RESOK_SETUP(res, OPEN4res_u, resok4);

	struct open_stateid *os = NULL;
	struct reffs_share *share = NULL;
	struct inode *child = NULL; /* active ref; owned for CLAIM_NULL */
	struct reffs_dirent *child_de = NULL;
	char *name = NULL;
	bool new_file = false;
	fattr4 *createattrs = NULL;
	struct timespec dir_before = { 0 };
	struct timespec dir_after = { 0 };
	int ret;

	/*
	 * Strip the delegation-want hint bits (upper 24 bits) from
	 * share_access before any conflict or mode checks.
	 */
	uint32_t share_access = args->share_access & OPEN4_SHARE_ACCESS_BOTH;
	uint32_t share_deny = args->share_deny;
	bool want_xor_deleg = !!(args->share_access &
				 OPEN4_SHARE_ACCESS_WANT_OPEN_XOR_DELEGATION);
	bool want_timestamps = !!(args->share_access &
				  OPEN4_SHARE_ACCESS_WANT_DELEG_TIMESTAMPS);

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	*status = nfs4_check_wrongsec(compound);
	if (*status)
		goto out;

	if (share_access == 0 || share_deny > OPEN4_SHARE_DENY_BOTH) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/*
	 * During grace, only reclaim claims are allowed.
	 * RFC 5661 §8.4.2.1: non-reclaim OPEN returns NFS4ERR_GRACE.
	 */
	if (args->claim.claim != CLAIM_PREVIOUS &&
	    args->claim.claim != CLAIM_DELEGATE_PREV) {
		if (nfs4_check_grace()) {
			*status = NFS4ERR_GRACE;
			goto out;
		}
	}

	/* Resolve target inode based on claim type. */
	switch (args->claim.claim) {
	case CLAIM_PREVIOUS:
		/*
		 * Grace reclaim: client held an open on this file
		 * before the server rebooted.  Stateids are in-memory
		 * only, so we issue a fresh stateid — same as CLAIM_FH.
		 * Delegation is not re-granted (no persisted state).
		 * CREATE is not valid for reclaim.
		 */
		if (args->openhow.opentype == OPEN4_CREATE) {
			*status = NFS4ERR_INVAL;
			goto out;
		}
		break;

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
		if (!S_ISDIR(compound->c_inode->i_mode)) {
			*status = NFS4ERR_NOTDIR;
			goto out;
		}

		component4 *fname = &args->claim.open_claim4_u.file;

		*status = nfs4_validate_component(fname);
		if (*status)
			goto out;
		name = strndup(fname->utf8string_val, fname->utf8string_len);
		if (!name) {
			*status = NFS4ERR_DELAY;
			goto out;
		}
		/* Need W_OK on the directory for CREATE, X_OK for NOCREATE. */
		int dir_amode =
			(args->openhow.opentype == OPEN4_CREATE) ? W_OK : X_OK;
		ret = inode_access_check(compound->c_inode, &compound->c_ap,
					 dir_amode);
		if (ret) {
			*status = errno_to_nfs4(ret, OP_OPEN);
			goto out;
		}

		if (!compound->c_inode->i_dirent) {
			ret = inode_reconstruct_path_to_root(compound->c_inode);
			if (ret) {
				*status = NFS4ERR_STALE;
				goto out;
			}
		}

		if (args->openhow.opentype == OPEN4_CREATE) {
			createhow4 *how = &args->openhow.openflag4_u.how;

			switch (how->mode) {
			case UNCHECKED4:
				ret = vfs_create(compound->c_inode, name, 0666,
						 &compound->c_ap, &child,
						 &dir_before, &dir_after);
				if (ret == 0) {
					new_file = true;
					createattrs =
						&how->createhow4_u.createattrs;
				} else if (ret == -EEXIST) {
					/*
					 * File exists: open it, as if the
					 * create had not been requested.
					 */
					child = inode_name_get_inode(
						compound->c_inode, name);
					ret = child ? 0 : -ENOENT;
				}
				break;

			case GUARDED4:
				ret = vfs_create(compound->c_inode, name, 0666,
						 &compound->c_ap, &child,
						 &dir_before, &dir_after);
				if (ret == 0) {
					new_file = true;
					createattrs =
						&how->createhow4_u.createattrs;
				}
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
				 * the same way, then apply cva_attrs to the
				 * new inode if the create succeeds.
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

				ret = vfs_create(compound->c_inode, name, 0666,
						 &compound->c_ap, &child,
						 &dir_before, &dir_after);
				if (ret == 0) {
					new_file = true;
					if (how->mode == EXCLUSIVE4_1)
						createattrs =
							&how->createhow4_u
								 .ch_createboth
								 .cva_attrs;
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
						compound->c_inode->i_dirent,
						name);
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
			if (new_file && createattrs &&
			    createattrs->attrmask.bitmap4_len > 0) {
				*status = nfs4_apply_createattrs(
					createattrs, child, &resok->attrset,
					&compound->c_ap);
				if (*status)
					goto out;
			}
		} else {
			/* NOCREATE: look up an existing file. */
			child_de = dirent_load_child_by_name(
				compound->c_inode->i_dirent, name);
			if (!child_de) {
				*status = NFS4ERR_NOENT;
				goto out;
			}
			child = dirent_ensure_inode(child_de);
			if (!child) {
				*status = NFS4ERR_SERVERFAULT;
				goto out;
			}

			/*
			 * Mount-point crossing: if this name has a
			 * filesystem mounted on it, cross into the
			 * child sb's root.  RFC 8881 does not restrict
			 * mount points to directories — a file can be
			 * mounted on top of another file.
			 */
			if (__atomic_load_n(&child_de->rd_state,
					    __ATOMIC_ACQUIRE) &
			    RD_MOUNTED_ON) {
				struct super_block *child_sb =
					super_block_find_mounted_on(child_de);
				if (child_sb) {
					struct inode *root = inode_find(
						child_sb, INODE_ROOT_ID);
					if (!root) {
						super_block_put(child_sb);
						*status = NFS4ERR_SERVERFAULT;
						goto out;
					}
					inode_active_put(child);
					child = root;
					super_block_put(compound->c_curr_sb);
					compound->c_curr_sb = child_sb;
					compound->c_curr_nfh.nfh_sb =
						child_sb->sb_id;
				}
			}
		}
		break;
	}

	default:
		*status = NFS4ERR_NOTSUPP;
		goto out;
	}

	/* The target inode is child (CLAIM_NULL) or compound->c_inode (CLAIM_FH). */
	struct inode *target = child ? child : compound->c_inode;

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
	ret = inode_access_check(target, &compound->c_ap, amode);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_OPEN);
		goto out;
	}

	/*
	 * Check for conflicting delegations held by other clients.
	 * If found, fire CB_RECALL (best-effort) and return NFS4ERR_DELAY.
	 * The client will retry OPEN after returning the delegation.
	 */
	struct client *client =
		compound->c_nfs4_client ?
			nfs4_client_to_client(compound->c_nfs4_client) :
			NULL;
	{
		struct stateid *ds =
			stateid_inode_find_delegation(target, client);
		if (ds) {
			struct nfs4_client *ds_nc =
				ds->s_client ? client_to_nfs4(ds->s_client) :
					       NULL;
			struct nfs4_session *ds_session =
				ds_nc ? nfs4_session_find_for_client(
						compound->c_server_state,
						ds_nc) :
					NULL;
			if (ds_session) {
				stateid4 recall_sid;
				struct network_file_handle cb_nfh =
					compound->c_curr_nfh;
				cb_nfh.nfh_ino = target->i_ino;
				nfs_fh4 cb_fh4 = {
					.nfs_fh4_len = sizeof(cb_nfh),
					.nfs_fh4_val = (char *)&cb_nfh,
				};

				pack_stateid4(&recall_sid, ds);
				nfs4_cb_recall(ds_session, &recall_sid, &cb_fh4,
					       false);
				nfs4_session_put(ds_session);
			}
			stateid_put(ds);
			*status = NFS4ERR_DELAY;
			goto out;
		}
	}

	/* Allocate the open stateid. */
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
	lock_owner_get(&os->os_owner);
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
		inode_active_put(compound->c_inode);
		compound->c_inode = child;
		compound->c_curr_nfh.nfh_ino = child->i_ino;
		child = NULL; /* ownership transferred */
	}

	/*
	 * Give the compound a ref on the open stateid.  The initial "state
	 * ref" (refcount=1 from stateid_assign) remains and keeps the
	 * stateid alive after this compound completes.
	 */
	stateid_put(compound->c_curr_stid);
	compound->c_curr_stid = stateid_get(&os->os_stid);

	resok->cinfo.atomic = TRUE;
	resok->cinfo.before = timespec_to_ns(&dir_before);
	resok->cinfo.after = timespec_to_ns(&dir_after);

	/*
	 * OPEN4_RESULT_NO_OPEN_STATEID is set only when the server grants
	 * a delegation instead of an open stateid (RFC 9754).  We always
	 * return an open stateid, so this flag is never set.
	 */
	resok->rflags = OPEN4_RESULT_LOCKTYPE_POSIX;

	resok->attrset.bitmap4_len = 0;
	resok->attrset.bitmap4_val = NULL;

	/*
	 * Attempt to grant a delegation.
	 *
	 * RFC 9754 OPEN XOR: want_xor_deleg means the client wants a
	 * delegation *instead of* a separate open stateid.  We still
	 * allocate an open_stateid internally for share tracking, but
	 * store it in ds_open and return only the delegation stateid
	 * to the client.  DELEGRETURN (not CLOSE) tears down the open.
	 *
	 * RFC 9754 WANT_DELEG_TIMESTAMPS: grant _ATTRS_DELEG variant
	 * and set ds_timestamps so the delegation tracks this state.
	 * CB_GETATTR for authoritative timestamps is not yet
	 * implemented; GETATTR falls back to server-side values.
	 */
	uint32_t want_deleg = args->share_access &
			      OPEN4_SHARE_ACCESS_WANT_DELEG_MASK;
	if (want_deleg == OPEN4_SHARE_ACCESS_WANT_NO_DELEG ||
	    want_deleg == OPEN4_SHARE_ACCESS_WANT_CANCEL) {
		resok->delegation.delegation_type = OPEN_DELEGATE_NONE;
	} else {
		struct delegation_stateid *ds =
			delegation_stateid_alloc(target, client);
		if (ds) {
			ds->ds_timestamps = want_timestamps;

			/*
			 * RFC 9754 XOR: store internal open_stateid and
			 * return the delegation stateid as resok->stateid.
			 * The client skips CLOSE; DELEGRETURN cleans up.
			 */
			if (want_xor_deleg) {
				ds->ds_open = os;
				os = NULL; /* ownership transferred */
				pack_stateid4(&resok->stateid, &ds->ds_stid);
				resok->rflags |= OPEN4_RESULT_NO_OPEN_STATEID;
				stateid_put(compound->c_curr_stid);
				compound->c_curr_stid =
					stateid_get(&ds->ds_stid);
			}

			/*
			 * RFC 5661 §8.1.3: delegation stateid seqid starts
			 * at 0.  stateid_assign() initialises s_seqid to 0.
			 */
			open_delegation_type4 dt_read =
				want_timestamps ?
					OPEN_DELEGATE_READ_ATTRS_DELEG :
					OPEN_DELEGATE_READ;
			open_delegation_type4 dt_write =
				want_timestamps ?
					OPEN_DELEGATE_WRITE_ATTRS_DELEG :
					OPEN_DELEGATE_WRITE;

			if (share_access & OPEN4_SHARE_ACCESS_WRITE) {
				ds->ds_state |= DELEG_STATEID_ACCESS_WRITE;
				resok->delegation.delegation_type = dt_write;
				pack_stateid4(
					&resok->delegation.open_delegation4_u
						 .write.stateid,
					&ds->ds_stid);
				resok->delegation.open_delegation4_u.write
					.recall = FALSE;
				resok->delegation.open_delegation4_u.write
					.space_limit.limitby = NFS_LIMIT_SIZE;
				resok->delegation.open_delegation4_u.write
					.space_limit.nfs_space_limit4_u
					.filesize = UINT64_MAX;

				ret = fill_delegation_permissions_from_mode(
					&resok->delegation.open_delegation4_u
						 .write.permissions,
					compound, OPEN_DELEGATE_WRITE);
			} else {
				resok->delegation.delegation_type = dt_read;
				pack_stateid4(
					&resok->delegation.open_delegation4_u
						 .read.stateid,
					&ds->ds_stid);
				resok->delegation.open_delegation4_u.read
					.recall = FALSE;

				ret = fill_delegation_permissions_from_mode(
					&resok->delegation.open_delegation4_u
						 .read.permissions,
					compound, OPEN_DELEGATE_READ);
			}

			errno_to_nfs4(ret, NFS4_OP_NUM(compound));
		} else {
			if (want_deleg) {
				resok->delegation.delegation_type =
					OPEN_DELEGATE_NONE_EXT;
				resok->delegation.open_delegation4_u.od_whynone
					.ond_why = WND4_RESOURCE;
			} else {
				resok->delegation.delegation_type =
					OPEN_DELEGATE_NONE;
			}
		}
	}

out:
	inode_active_put(child); /* NULL-safe; only set if not transferred */
	dirent_put(child_de);
	free(name);
	TRACE("%s status=%s(%d) claim=%d access=%u deny=%u", __func__,
	      nfs4_err_name(*status), *status, args->claim.claim, share_access,
	      share_deny);

	return 0;
}

uint32_t nfs4_op_open_confirm(struct compound *compound)
{
	OPEN_CONFIRM4res *res = NFS4_OP_RES_SETUP(compound, opopen_confirm);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_open_downgrade(struct compound *compound)
{
	OPEN_DOWNGRADE4args *args =
		NFS4_OP_ARG_SETUP(compound, opopen_downgrade);
	OPEN_DOWNGRADE4res *res = NFS4_OP_RES_SETUP(compound, opopen_downgrade);
	nfsstat4 *status = &res->status;
	OPEN_DOWNGRADE4resok *resok =
		NFS4_OP_RESOK_SETUP(res, OPEN_DOWNGRADE4res_u, resok4);

	uint32_t new_access = 0;
	uint32_t new_deny = 0;
	uint32_t cur_access = 0;
	uint32_t cur_deny = 0;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
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

	struct stateid *stid = stateid_find(compound->c_inode, id);
	if (!stid || stid->s_tag != Open_Stateid || stid->s_cookie != cookie) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	if (compound->c_nfs4_client &&
	    stid->s_client != nfs4_client_to_client(compound->c_nfs4_client)) {
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
	new_access = args->share_access & OPEN4_SHARE_ACCESS_BOTH;
	new_deny = args->share_deny & OPEN4_SHARE_DENY_BOTH;
	cur_access = (uint32_t)(os->os_state & 0x3);
	cur_deny = (uint32_t)((os->os_state >> 2) & 0x3);

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
	lock_owner_get(&os->os_owner);
	share->s_owner = &os->os_owner;
	share->s_inode = inode_active_get(compound->c_inode);
	share->s_access = new_access;
	share->s_mode = new_deny;

	pthread_mutex_lock(&compound->c_inode->i_lock_mutex);
	reffs_share_add(compound->c_inode, share,
			NULL); /* always succeeds: downgrade */
	pthread_mutex_unlock(&compound->c_inode->i_lock_mutex);

	/* Update os_state to reflect the new modes. */
	os->os_state = (uint64_t)new_access | ((uint64_t)new_deny << 2);

	/* Bump seqid (RFC 5661 §8.1.3.1). */
	__atomic_fetch_add(&stid->s_seqid, 1, __ATOMIC_SEQ_CST);
	pack_stateid4(&resok->open_stateid, stid);

	/* Update c_curr_stid to the downgraded stateid. */
	stateid_put(compound->c_curr_stid);
	compound->c_curr_stid = stid; /* transfer the find ref */

out:
	TRACE("%s status=%s(%d) access=%u deny=%u", __func__,
	      nfs4_err_name(*status), *status, new_access, new_deny);

	return 0;
}

uint32_t nfs4_op_close(struct compound *compound)
{
	CLOSE4args *args = NFS4_OP_ARG_SETUP(compound, opclose);
	CLOSE4res *res = NFS4_OP_RES_SETUP(compound, opclose);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (stateid4_is_special(&args->open_stateid)) {
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	uint32_t seqid, id, type, cookie;
	unpack_stateid4(&args->open_stateid, &seqid, &id, &type, &cookie);

	if (type != Open_Stateid) {
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	struct stateid *stid = stateid_find(compound->c_inode, id);
	if (!stid || stid->s_tag != Open_Stateid || stid->s_cookie != cookie) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	if (compound->c_nfs4_client &&
	    stid->s_client != nfs4_client_to_client(compound->c_nfs4_client)) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	uint32_t cur_seqid = __atomic_load_n(&stid->s_seqid, __ATOMIC_RELAXED);
	if (seqid != 0) {
		if (seqid < cur_seqid) {
			stateid_put(stid);
			*status = NFS4ERR_OLD_STATEID;
			return 0;
		}
		if (seqid > cur_seqid) {
			stateid_put(stid);
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}
	}

	struct open_stateid *os = stid_to_open(stid);

	/* Remove the share reservation. */
	pthread_mutex_lock(&compound->c_inode->i_lock_mutex);
	reffs_share_remove(compound->c_inode, &os->os_owner, NULL);
	pthread_mutex_unlock(&compound->c_inode->i_lock_mutex);

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
	if (compound->c_curr_stid == stid) {
		stateid_put(stid); /* put the c_curr_stid ref */
		compound->c_curr_stid = NULL;
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

	return 0;
}

/*
 * nfs4_op_read_resume -- rt_next_action callback after async pread completes.
 *
 * rt->rt_io_result holds the pread return value (bytes read, or -errno).
 * The buffer was allocated and the resok pointer was set before the pause;
 * we just fix up the length, eof flag, and status here.
 */
static uint32_t nfs4_op_read_resume(struct rpc_trans *rt)
{
	struct compound *compound = rt->rt_compound;
	READ4args *args = NFS4_OP_ARG_SETUP(compound, opread);
	READ4res *res = NFS4_OP_RES_SETUP(compound, opread);
	nfsstat4 *status = &res->status;
	READ4resok *resok = NFS4_OP_RESOK_SETUP(res, READ4res_u, resok4);

	rt->rt_next_action = NULL;

	ssize_t nread = rt->rt_io_result;
	if (nread < 0) {
		free(resok->data.data_val);
		resok->data.data_val = NULL;
		resok->data.data_len = 0;
		*status = NFS4ERR_IO;
		return 0;
	}

	resok->data.data_len = (u_int)nread;
	resok->eof = (args->offset + (uint64_t)nread >=
		      (uint64_t)compound->c_inode->i_size);

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	inode_update_times_now(compound->c_inode, REFFS_INODE_UPDATE_ATIME);
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	return 0;
}

uint32_t nfs4_op_read(struct compound *compound)
{
	READ4args *args = NFS4_OP_ARG_SETUP(compound, opread);
	READ4res *res = NFS4_OP_RES_SETUP(compound, opread);
	nfsstat4 *status = &res->status;
	READ4resok *resok = NFS4_OP_RESOK_SETUP(res, READ4res_u, resok4);

	struct stateid *stid = NULL;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISREG(compound->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	*status = nfs4_stateid_resolve(compound, &args->stateid, false, &stid);
	if (*status != NFS4_OK)
		goto out;

	/*
	 * For anonymous and regular stateids, verify POSIX read permission.
	 * Read-bypass skips this check (stid == NULL and seqid == UINT32_MAX).
	 */
	if (!stateid4_is_read_bypass(&args->stateid)) {
		int ret = inode_access_check(compound->c_inode, &compound->c_ap,
					     R_OK);
		if (ret) {
			*status = errno_to_nfs4(ret, OP_READ);
			goto out;
		}
	}

	/* Clamp to a server-side limit. */
	count4 req_count = args->count;
	if (req_count > NFS4_MAX_RW_SIZE)
		req_count = NFS4_MAX_RW_SIZE;

	if (!compound->c_inode->i_db ||
	    args->offset >= (uint64_t)compound->c_inode->i_size) {
		resok->eof = true;
		resok->data.data_len = 0;
		resok->data.data_val = NULL;
		goto out;
	}

	if (req_count == 0) {
		resok->eof =
			(args->offset >= (uint64_t)compound->c_inode->i_size);
		resok->data.data_len = 0;
		resok->data.data_val = NULL;
		goto out;
	}

	resok->data.data_val = calloc(req_count, 1);
	if (!resok->data.data_val) {
		*status = NFS4ERR_DELAY;
		goto out;
	}
	resok->data.data_len = req_count;

	/*
	 * If the backend has a real file descriptor, submit an async pread
	 * via the backend io_uring ring and yield the task.  The resume
	 * callback (nfs4_op_read_resume) picks up rt_io_result and fills
	 * in resok.
	 *
	 * The rwlock is NOT held across the async pause: the stateid held
	 * by the client prevents conflicting truncates for the duration.
	 * For the RAM backend (fd == -1) the synchronous path is used.
	 */
	int db_fd = data_block_get_fd(compound->c_inode->i_db);
	struct ring_context *rc_backend = io_backend_get_global();
	if (db_fd >= 0 && rc_backend && compound->c_rt->rt_task) {
		struct rpc_trans *rt = compound->c_rt;
		rt->rt_next_action = nfs4_op_read_resume;
		task_pause(rt->rt_task);
		if (io_request_backend_pread(db_fd, resok->data.data_val,
					     req_count, args->offset, rt,
					     rc_backend) < 0) {
			/*
			 * Submission failed: undo the pause so dispatch
			 * doesn't stall, then fall through to error.
			 */
			rt->rt_next_action = NULL;
			task_resume(rt->rt_task);
			free(resok->data.data_val);
			resok->data.data_val = NULL;
			resok->data.data_len = 0;
			*status = NFS4ERR_DELAY;
			goto out;
		}
		/* Async I/O submitted — do not touch rt or compound. */
		stateid_put(stid);
		return NFS4_OP_FLAG_ASYNC;
	}

	/* Synchronous path (RAM backend or no backend ring). */
	pthread_rwlock_rdlock(&compound->c_inode->i_db_rwlock);
	ssize_t nread = data_block_read(compound->c_inode->i_db,
					resok->data.data_val, req_count,
					args->offset);
	if (nread < 0) {
		free(resok->data.data_val);
		resok->data.data_val = NULL;
		resok->data.data_len = 0;
		pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
		*status = NFS4ERR_IO;
		goto out;
	}

	resok->data.data_len = (u_int)nread;
	resok->eof = (args->offset + (uint64_t)nread >=
		      (uint64_t)compound->c_inode->i_size);
	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	inode_update_times_now(compound->c_inode, REFFS_INODE_UPDATE_ATIME);
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

out:
	stateid_put(stid);
	TRACE("%s status=%s(%d) offset=%llu count=%u", __func__,
	      nfs4_err_name(*status), *status, (unsigned long long)args->offset,
	      args->count);

	return 0;
}

/*
 * READ_PLUS — RFC 7862 S15.10.
 *
 * Returns read_plus_content with data and/or hole segments.
 * Initial implementation: always returns a single NFS4_CONTENT_DATA
 * segment (no SEEK_HOLE detection).  Valid DS operation per S3.3.1.
 *
 * NOT_NOW_BROWN_COW: hole detection via SEEK_HOLE/SEEK_DATA on
 * POSIX backends, async io_uring path.
 */
uint32_t nfs4_op_read_plus(struct compound *compound)
{
	READ_PLUS4args *args = NFS4_OP_ARG_SETUP(compound, opread_plus);
	READ_PLUS4res *res = NFS4_OP_RES_SETUP(compound, opread_plus);
	nfsstat4 *status = &res->rp_status;
	read_plus_res4 *resok =
		NFS4_OP_RESOK_SETUP(res, READ_PLUS4res_u, rp_resok4);
	struct stateid *stid = NULL;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISREG(compound->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	*status = nfs4_stateid_resolve(compound, &args->rpa_stateid, false,
				       &stid);
	if (*status != NFS4_OK)
		goto out;

	int ret = inode_access_check(compound->c_inode, &compound->c_ap, R_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_READ_PLUS);
		goto out;
	}

	/* Clamp requested count */
	uint32_t req_count = args->rpa_count;
	if (req_count > NFS4_MAX_RW_SIZE)
		req_count = NFS4_MAX_RW_SIZE;

	/* Handle read past EOF or empty file */
	if (!compound->c_inode->i_db ||
	    args->rpa_offset >= (uint64_t)compound->c_inode->i_size) {
		/* Return empty data segment at the requested offset */
		resok->rpr_eof = TRUE;
		resok->rpr_contents.rpr_contents_len = 1;
		resok->rpr_contents.rpr_contents_val =
			calloc(1, sizeof(read_plus_content));
		if (!resok->rpr_contents.rpr_contents_val) {
			*status = NFS4ERR_SERVERFAULT;
			goto out;
		}
		resok->rpr_contents.rpr_contents_val[0].rpc_content =
			NFS4_CONTENT_DATA;
		resok->rpr_contents.rpr_contents_val[0]
			.read_plus_content_u.rpc_data.d_offset =
			args->rpa_offset;
		resok->rpr_contents.rpr_contents_val[0]
			.read_plus_content_u.rpc_data.d_data.d_data_len = 0;
		resok->rpr_contents.rpr_contents_val[0]
			.read_plus_content_u.rpc_data.d_data.d_data_val = NULL;
		goto out;
	}

	/* Allocate data buffer */
	char *buf = malloc(req_count);
	if (!buf) {
		*status = NFS4ERR_SERVERFAULT;
		goto out;
	}

	/* Synchronous read */
	pthread_rwlock_rdlock(&compound->c_inode->i_db_rwlock);
	ssize_t nread = data_block_read(compound->c_inode->i_db, buf, req_count,
					args->rpa_offset);
	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);

	if (nread < 0) {
		free(buf);
		*status = NFS4ERR_IO;
		goto out;
	}

	/* Build single NFS4_CONTENT_DATA segment */
	resok->rpr_contents.rpr_contents_len = 1;
	resok->rpr_contents.rpr_contents_val =
		calloc(1, sizeof(read_plus_content));
	if (!resok->rpr_contents.rpr_contents_val) {
		free(buf);
		*status = NFS4ERR_SERVERFAULT;
		goto out;
	}

	resok->rpr_contents.rpr_contents_val[0].rpc_content = NFS4_CONTENT_DATA;
	resok->rpr_contents.rpr_contents_val[0]
		.read_plus_content_u.rpc_data.d_offset = args->rpa_offset;
	resok->rpr_contents.rpr_contents_val[0]
		.read_plus_content_u.rpc_data.d_data.d_data_val = buf;
	resok->rpr_contents.rpr_contents_val[0]
		.read_plus_content_u.rpc_data.d_data.d_data_len = (u_int)nread;

	resok->rpr_eof = (args->rpa_offset + (uint64_t)nread >=
			  (uint64_t)compound->c_inode->i_size);

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	inode_update_times_now(compound->c_inode, REFFS_INODE_UPDATE_ATIME);
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

out:
	stateid_put(stid);
	TRACE("%s status=%s(%d) offset=%llu count=%u", __func__,
	      nfs4_err_name(*status), *status,
	      (unsigned long long)args->rpa_offset, args->rpa_count);

	return 0;
}

/*
 * nfs4_op_write_resume -- rt_next_action callback after async pwrite completes.
 *
 * The pre-pause code already extended db_size on disk via ftruncate but did
 * NOT update i_size or sb_bytes_used, so compound->c_inode->i_size is still
 * the pre-write value.  db->db_size is already the new (post-extend) value.
 */
static uint32_t nfs4_op_write_resume(struct rpc_trans *rt)
{
	struct compound *compound = rt->rt_compound;
	WRITE4res *res = NFS4_OP_RES_SETUP(compound, opwrite);
	nfsstat4 *status = &res->status;
	WRITE4resok *resok = NFS4_OP_RESOK_SETUP(res, WRITE4res_u, resok4);
	struct super_block *sb = compound->c_curr_sb;

	rt->rt_next_action = NULL;

	ssize_t nwritten = rt->rt_io_result;
	if (nwritten < 0) {
		*status = (nwritten == -ENOSPC) ? NFS4ERR_NOSPC : NFS4ERR_IO;
		return 0;
	}

	resok->count = (count4)nwritten;

	pthread_rwlock_wrlock(&compound->c_inode->i_db_rwlock);

	int64_t old_size = compound->c_inode->i_size;
	size_t new_db_size = compound->c_inode->i_db->db_size;

	compound->c_inode->i_size = (int64_t)new_db_size;
	compound->c_inode->i_used =
		compound->c_inode->i_size / sb->sb_block_size +
		(compound->c_inode->i_size % sb->sb_block_size ? 1 : 0);

	size_t old_used, new_used;
	old_used =
		atomic_load_explicit(&sb->sb_bytes_used, memory_order_relaxed);
	do {
		if (new_db_size > (size_t)old_size)
			new_used = old_used + (new_db_size - (size_t)old_size);
		else if ((size_t)old_size > new_db_size)
			new_used = old_used > (size_t)old_size - new_db_size ?
					   old_used - ((size_t)old_size -
						       new_db_size) :
					   0;
		else
			new_used = old_used;
	} while (!atomic_compare_exchange_strong_explicit(
		&sb->sb_bytes_used, &old_used, new_used, memory_order_seq_cst,
		memory_order_relaxed));

	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	inode_update_times_now(compound->c_inode,
			       REFFS_INODE_UPDATE_MTIME |
				       REFFS_INODE_UPDATE_CTIME);
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	inode_sync_to_disk(compound->c_inode);

	resok->committed = FILE_SYNC4;
	nfs4_write_verf(compound->c_server_state, resok->writeverf);

	return 0;
}

uint32_t nfs4_op_write(struct compound *compound)
{
	WRITE4args *args = NFS4_OP_ARG_SETUP(compound, opwrite);
	WRITE4res *res = NFS4_OP_RES_SETUP(compound, opwrite);
	nfsstat4 *status = &res->status;
	WRITE4resok *resok = NFS4_OP_RESOK_SETUP(res, WRITE4res_u, resok4);

	struct stateid *stid = NULL;
	struct super_block *sb = compound->c_curr_sb;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISREG(compound->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	*status = nfs4_stateid_resolve(compound, &args->stateid, true, &stid);
	if (*status != NFS4_OK)
		goto out;

	int ret = inode_access_check(compound->c_inode, &compound->c_ap, W_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_WRITE);
		goto out;
	}

	/* Zero-length write is a no-op (RFC 5661 §18.32.3). */
	if (args->data.data_len == 0) {
		resok->count = 0;
		resok->committed = FILE_SYNC4;
		nfs4_write_verf(compound->c_server_state, resok->writeverf);
		goto out;
	}

	/* Clamp to server-side limit. */
	u_int write_len = args->data.data_len;
	if (write_len > NFS4_MAX_RW_SIZE)
		write_len = NFS4_MAX_RW_SIZE;

	/* Clear SUID/SGID on write by an unprivileged user. */
	if ((compound->c_inode->i_mode & S_ISUID) &&
	    compound->c_ap.aup_uid != 0 &&
	    compound->c_ap.aup_uid != reffs_id_to_uid(compound->c_inode->i_uid))
		compound->c_inode->i_mode &= ~S_ISUID;
	if ((compound->c_inode->i_mode & S_ISGID) &&
	    compound->c_ap.aup_uid != 0 &&
	    compound->c_ap.aup_uid != reffs_id_to_uid(compound->c_inode->i_uid))
		compound->c_inode->i_mode &= ~S_ISGID;

	int64_t old_size;
	pthread_rwlock_wrlock(&compound->c_inode->i_db_rwlock);

	old_size = compound->c_inode->i_size;

	if (!compound->c_inode->i_db) {
		/*
		 * New file: posix_db_alloc writes the initial data
		 * synchronously.  Keep this path synchronous.
		 */
		compound->c_inode->i_db =
			data_block_alloc(compound->c_inode, args->data.data_val,
					 write_len, args->offset);
		if (!compound->c_inode->i_db) {
			pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
			*status = NFS4ERR_NOSPC;
			goto out;
		}
		resok->count = write_len;
		/* Fall through to size/metadata update below. */
	} else {
		int db_fd = data_block_get_fd(compound->c_inode->i_db);
		struct ring_context *rc_backend = io_backend_get_global();

		if (db_fd >= 0 && rc_backend && compound->c_rt->rt_task) {
			/*
			 * Async POSIX path.
			 *
			 * Pre-extend the file so that the pwrite does not
			 * need to grow the file itself (io_uring pwrite
			 * behaves like pwrite(2) and will extend, but we
			 * update db_size here so the resume callback can
			 * compute the size delta without a second fstat).
			 */
			size_t new_db_size = (size_t)args->offset + write_len;
			if (new_db_size > compound->c_inode->i_db->db_size) {
				if (ftruncate(db_fd, (off_t)new_db_size) < 0) {
					int saved = errno;
					pthread_rwlock_unlock(
						&compound->c_inode->i_db_rwlock);
					*status = (saved == ENOSPC) ?
							  NFS4ERR_NOSPC :
							  NFS4ERR_IO;
					goto out;
				}
				compound->c_inode->i_db->db_size = new_db_size;
			}
			pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);

			struct rpc_trans *rt = compound->c_rt;
			rt->rt_next_action = nfs4_op_write_resume;
			task_pause(rt->rt_task);
			if (io_request_backend_pwrite(
				    db_fd, args->data.data_val, write_len,
				    args->offset, rt, rc_backend) < 0) {
				rt->rt_next_action = NULL;
				task_resume(rt->rt_task);
				*status = NFS4ERR_DELAY;
				goto out;
			}
			/* Async I/O submitted — do not touch rt or compound. */
			stateid_put(stid);
			return NFS4_OP_FLAG_ASYNC;
		}

		/* Synchronous path (RAM backend or no backend ring). */
		ssize_t nwritten = data_block_write(compound->c_inode->i_db,
						    args->data.data_val,
						    write_len, args->offset);
		if (nwritten < 0) {
			pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
			*status = (nwritten == -ENOSPC) ? NFS4ERR_NOSPC :
							  NFS4ERR_IO;
			goto out;
		}
		resok->count = (count4)nwritten;
		/* Fall through to size/metadata update below. */
	}

	/* Size and space accounting for the synchronous paths. */
	compound->c_inode->i_size = (int64_t)compound->c_inode->i_db->db_size;
	compound->c_inode->i_used =
		compound->c_inode->i_size / sb->sb_block_size +
		(compound->c_inode->i_size % sb->sb_block_size ? 1 : 0);

	size_t new_db_size = data_block_get_size(compound->c_inode->i_db);
	size_t old_used, new_used;
	old_used =
		atomic_load_explicit(&sb->sb_bytes_used, memory_order_relaxed);
	do {
		if (new_db_size > (size_t)old_size)
			new_used = old_used + (new_db_size - (size_t)old_size);
		else if ((size_t)old_size > new_db_size)
			new_used = old_used > (size_t)old_size - new_db_size ?
					   old_used - ((size_t)old_size -
						       new_db_size) :
					   0;
		else
			new_used = old_used;
	} while (!atomic_compare_exchange_strong_explicit(
		&sb->sb_bytes_used, &old_used, new_used, memory_order_seq_cst,
		memory_order_relaxed));

	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	inode_update_times_now(compound->c_inode,
			       REFFS_INODE_UPDATE_MTIME |
				       REFFS_INODE_UPDATE_CTIME);
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	inode_sync_to_disk(compound->c_inode);

	resok->committed = FILE_SYNC4;
	nfs4_write_verf(compound->c_server_state, resok->writeverf);

out:
	stateid_put(stid);
	TRACE("%s status=%s(%d) offset=%llu count=%u stable=%d", __func__,
	      nfs4_err_name(*status), *status, (unsigned long long)args->offset,
	      args->data.data_len, args->stable);

	return 0;
}

uint32_t nfs4_op_write_same(struct compound *compound)
{
	WRITE_SAME4res *res = NFS4_OP_RES_SETUP(compound, opwrite_same);
	nfsstat4 *status = &res->wsr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_commit(struct compound *compound)
{
	COMMIT4args *args = NFS4_OP_ARG_SETUP(compound, opcommit);
	COMMIT4res *res = NFS4_OP_RES_SETUP(compound, opcommit);
	nfsstat4 *status = &res->status;
	COMMIT4resok *resok = NFS4_OP_RESOK_SETUP(res, COMMIT4res_u, resok4);

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISREG(compound->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	/*
	 * All writes are already FILE_SYNC4, so there is nothing to flush.
	 * Return the stable write verifier so the client can verify
	 * stability across server restarts.
	 */
	nfs4_write_verf(compound->c_server_state, resok->writeverf);

out:
	TRACE("%s status=%s(%d) offset=%llu count=%u", __func__,
	      nfs4_err_name(*status), *status, (unsigned long long)args->offset,
	      args->count);

	return 0;
}

uint32_t nfs4_op_seek(struct compound *compound)
{
	SEEK4res *res = NFS4_OP_RES_SETUP(compound, opseek);
	nfsstat4 *status = &res->sa_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

/*
 * ALLOCATE — RFC 7862 S15.1.
 *
 * Preallocate space in a file.  Standalone mode only; MDS fan-out
 * deferred (NOT_NOW_BROWN_COW).
 */
uint32_t nfs4_op_allocate(struct compound *compound)
{
	ALLOCATE4args *args = NFS4_OP_ARG_SETUP(compound, opallocate);
	ALLOCATE4res *res = NFS4_OP_RES_SETUP(compound, opallocate);
	nfsstat4 *status = &res->ar_status;
	struct stateid *stid = NULL;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (nfs4_check_grace()) {
		*status = NFS4ERR_GRACE;
		goto out;
	}

	if (!S_ISREG(compound->c_inode->i_mode)) {
		*status = NFS4ERR_WRONG_TYPE;
		goto out;
	}

	*status =
		nfs4_stateid_resolve(compound, &args->aa_stateid, true, &stid);
	if (*status != NFS4_OK)
		goto out;

	int ret = inode_access_check(compound->c_inode, &compound->c_ap, W_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_ALLOCATE);
		goto out;
	}

	/*
	 * Extend the file if the requested range exceeds current size.
	 * ALLOCATE guarantees space at [offset, offset+length).
	 */
	uint64_t end = args->aa_offset + args->aa_length;

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	pthread_rwlock_wrlock(&compound->c_inode->i_db_rwlock);

	if (!compound->c_inode->i_db) {
		/* No data block yet — create one at the required size */
		compound->c_inode->i_db =
			data_block_alloc(compound->c_inode, NULL, 0, 0);
		if (!compound->c_inode->i_db) {
			pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_NOSPC;
			goto out;
		}
	}

	if (end > (uint64_t)compound->c_inode->i_size) {
		ssize_t rret =
			data_block_resize(compound->c_inode->i_db, (size_t)end);
		if (rret < 0) {
			pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_NOSPC;
			goto out;
		}
		compound->c_inode->i_size = end;
		inode_update_times_now(compound->c_inode,
				       REFFS_INODE_UPDATE_CTIME |
					       REFFS_INODE_UPDATE_MTIME);
	}

	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

out:
	stateid_put(stid);
	TRACE("%s status=%s(%d) offset=%llu length=%llu", __func__,
	      nfs4_err_name(*status), *status,
	      (unsigned long long)args->aa_offset,
	      (unsigned long long)args->aa_length);
	return 0;
}

/*
 * DEALLOCATE — RFC 7862 S15.4.
 *
 * Deallocate space in a file.  The file size is not changed
 * (FALLOC_FL_KEEP_SIZE semantics).  Standalone mode only; MDS
 * fan-out deferred (NOT_NOW_BROWN_COW).
 *
 * For the RAM and POSIX backends without FALLOC_FL_PUNCH_HOLE
 * support, we zero-fill the range instead.  This satisfies the
 * RFC requirement that subsequent reads return zeros.
 */
uint32_t nfs4_op_deallocate(struct compound *compound)
{
	DEALLOCATE4args *args = NFS4_OP_ARG_SETUP(compound, opdeallocate);
	DEALLOCATE4res *res = NFS4_OP_RES_SETUP(compound, opdeallocate);
	nfsstat4 *status = &res->dr_status;
	struct stateid *stid = NULL;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (nfs4_check_grace()) {
		*status = NFS4ERR_GRACE;
		goto out;
	}

	if (!S_ISREG(compound->c_inode->i_mode)) {
		*status = NFS4ERR_WRONG_TYPE;
		goto out;
	}

	*status =
		nfs4_stateid_resolve(compound, &args->da_stateid, true, &stid);
	if (*status != NFS4_OK)
		goto out;

	int ret = inode_access_check(compound->c_inode, &compound->c_ap, W_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_DEALLOCATE);
		goto out;
	}

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	pthread_rwlock_wrlock(&compound->c_inode->i_db_rwlock);

	if (!compound->c_inode->i_db) {
		/* No data — nothing to deallocate */
		pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		goto out;
	}

	/*
	 * Zero-fill the deallocated range.  Clamp to file size
	 * (deallocating past EOF is a no-op).
	 */
	uint64_t file_size = compound->c_inode->i_size;
	uint64_t start = args->da_offset;
	uint64_t end = start + args->da_length;

	if (start >= file_size) {
		/* Entirely past EOF — no-op */
		pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		goto out;
	}

	if (end > file_size)
		end = file_size;

	size_t zero_len = (size_t)(end - start);
	char *zeros = calloc(1, zero_len);
	if (!zeros) {
		pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_SERVERFAULT;
		goto out;
	}

	ssize_t nwritten = data_block_write(compound->c_inode->i_db, zeros,
					    zero_len, (off_t)start);
	free(zeros);

	if (nwritten < 0) {
		pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_IO;
		goto out;
	}

	inode_update_times_now(compound->c_inode,
			       REFFS_INODE_UPDATE_CTIME |
				       REFFS_INODE_UPDATE_MTIME);

	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

out:
	stateid_put(stid);
	TRACE("%s status=%s(%d) offset=%llu length=%llu", __func__,
	      nfs4_err_name(*status), *status,
	      (unsigned long long)args->da_offset,
	      (unsigned long long)args->da_length);
	return 0;
}
