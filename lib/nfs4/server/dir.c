/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif
#include <time.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/dirent.h"
#include "reffs/identity.h"
#include "reffs/super_block.h"
#include "reffs/vfs.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/client.h"
#include "reffs/server.h"
#include "reffs/time.h"
#include "nfs4/trace/nfs4.h"
#include "nfs4/changeid.h"

#include "ps_inode.h"
#include "ps_proxy_ops.h"
#include "ps_sb.h"
#include "ps_state.h"

uint32_t nfs4_op_lookup(struct compound *compound)
{
	LOOKUP4args *args = NFS4_OP_ARG_SETUP(compound, oplookup);
	LOOKUP4res *res = NFS4_OP_RES_SETUP(compound, oplookup);
	nfsstat4 *status = &res->status;

	struct reffs_dirent *child_de = NULL;
	struct inode *child = NULL;
	char *name = NULL;
	int ret;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	/*
	 * RFC 8881 S2.6.3.1.1.3: the put-FH + LOOKUP rule means
	 * the put-FH before us must NOT check WRONGSEC; LOOKUP
	 * itself checks WRONGSEC only after mount-point crossing
	 * (below).  No pre-lookup check against the parent export.
	 */

	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	if (args->objname.utf8string_len == 0) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	*status = nfs4_validate_component(&args->objname);
	if (*status)
		goto out;

	name = strndup(args->objname.utf8string_val,
		       args->objname.utf8string_len);
	if (!name) {
		*status = NFS4ERR_DELAY;
		goto out;
	}

	ret = inode_access_check(compound->c_inode, &compound->c_ap, X_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_LOOKUP);
		goto out;
	}

	/*
	 * After PUTFH the inode may be loaded without its dirent chain.
	 * Reconstruct before calling dirent_load_child_by_name.
	 */
	if (!compound->c_inode->i_dirent) {
		ret = inode_reconstruct_path_to_root(compound->c_inode);
		if (ret) {
			*status = NFS4ERR_STALE;
			goto out;
		}
	}

	child_de = dirent_load_child_by_name(compound->c_inode->i_dirent, name);
	if (!child_de) {
		/*
		 * Proxy-SB fast path: if the parent lives on a proxy SB, the
		 * child is not yet materialised locally -- forward the LOOKUP
		 * upstream, then allocate a local dirent+inode for the
		 * returned FH so subsequent ops in this compound (GETFH,
		 * GETATTR, OPEN) operate on a real inode without another
		 * round-trip.
		 */
		if (compound->c_inode->i_sb &&
		    compound->c_inode->i_sb->sb_proxy_binding) {
			const struct ps_sb_binding *binding =
				compound->c_inode->i_sb->sb_proxy_binding;
			uint8_t child_fh[PS_MAX_FH_SIZE];
			uint32_t child_fh_len = 0;
			/*
			 * Request FATTR4_TYPE | FATTR4_MODE on the same
			 * compound so ps_lookup_materialize can promote the
			 * new inode off the S_IFREG placeholder and a second
			 * LOOKUP into a proxied directory succeeds without
			 * a round-trip to refresh the type.  Bits per RFC
			 * 8881 S5.8.1: TYPE=1 (word 0, mask 0x2), MODE=33
			 * (word 1, mask 0x2).
			 */
			static const uint32_t attr_req[] = { 0x2u, 0x2u };
			struct ps_proxy_attrs_min attrs = { 0 };

			int pret = ps_proxy_lookup_forward_for_inode(
				compound->c_inode, args->objname.utf8string_val,
				args->objname.utf8string_len, child_fh,
				sizeof(child_fh), &child_fh_len, attr_req, 2,
				&compound->c_ap, &attrs);

			if (pret == -ENOENT) {
				*status = NFS4ERR_NOENT;
				goto out;
			}
			if (pret == -ENOTCONN) {
				/*
				 * PS session is down; the startup-only
				 * session model has no reconnect yet, so
				 * NFS4ERR_DELAY matches the transient-
				 * unavailability convention used on the
				 * GETATTR hook.
				 */
				*status = NFS4ERR_DELAY;
				goto out;
			}
			if (pret == -ENOTSUP) {
				/*
				 * The upstream GETATTR returned an attr
				 * ps_proxy_parse_attrs_min does not know
				 * how to decode.  The compound-level
				 * translation falls through errno_to_nfs4
				 * to NFS4ERR_SERVERFAULT; LOG so operators
				 * can see when the parser drifts from
				 * what the MDS emits (request mask here
				 * is TYPE|MODE, so any extra attr is an
				 * MDS bug or a parser gap -- both worth
				 * surfacing).
				 */
				LOG("proxy lookup: upstream GETATTR reply "
				    "contains attr parser cannot decode; "
				    "listener=%u parent_ino=%" PRIu64
				    " name=%s",
				    binding->psb_listener_id,
				    compound->c_inode->i_ino, name);
				*status = NFS4ERR_SERVERFAULT;
				goto out;
			}
			if (pret < 0) {
				*status = errno_to_nfs4(pret, OP_LOOKUP);
				goto out;
			}

			/*
			 * Audit-log obligation (see proxy-server.md
			 * "Audit logging"): the forwarded compound rides
			 * on the PS session's credentials rather than the
			 * end client's AUTH_SYS creds.  TRACE until slice
			 * 2e-iv-c plumbs real credential forwarding.
			 */
			TRACE("proxy lookup: listener=%u parent_ino=%" PRIu64
			      " name=%s (forwarded with PS creds)",
			      binding->psb_listener_id,
			      compound->c_inode->i_ino, name);

			pret = ps_lookup_materialize(
				compound->c_inode, args->objname.utf8string_val,
				args->objname.utf8string_len, child_fh,
				child_fh_len, &attrs, &child_de, &child);
			if (pret == -EEXIST) {
				/*
				 * A concurrent LOOKUP materialised the same
				 * child between our forwarding RPC and the
				 * alloc.  Re-find the dirent and fall through
				 * to dirent_ensure_inode below (child stays
				 * NULL in this branch).
				 */
				child_de = dirent_load_child_by_name(
					compound->c_inode->i_dirent, name);
				if (!child_de) {
					*status = NFS4ERR_SERVERFAULT;
					goto out;
				}
			} else if (pret < 0) {
				*status = errno_to_nfs4(pret, OP_LOOKUP);
				goto out;
			}
			/*
			 * On success, child_de holds a ref (out parameter)
			 * and child holds an active ref transferred from
			 * inode_alloc.  The dirent_ensure_inode block below
			 * is a no-op when child is already set.
			 */
		} else {
			*status = NFS4ERR_NOENT;
			goto out;
		}
	}

	if (!child) {
		child = dirent_ensure_inode(child_de);
		if (!child) {
			*status = NFS4ERR_SERVERFAULT;
			goto out;
		}
	}

	/*
	 * Mount-point crossing: if the target dirent has a sb mounted
	 * on it, cross into the child sb's root inode instead.
	 * This implements the NFSv4 fsid-change semantics at mount points.
	 */
	if (__atomic_load_n(&child_de->rd_state, __ATOMIC_ACQUIRE) &
	    RD_MOUNTED_ON) {
		struct super_block *child_sb =
			super_block_find_mounted_on(child_de);
		if (child_sb) {
			struct inode *root =
				inode_find(child_sb, INODE_ROOT_ID);
			if (!root) {
				super_block_put(child_sb);
				*status = NFS4ERR_SERVERFAULT;
				goto out;
			}
			inode_active_put(child);
			child = root;
			super_block_put(compound->c_curr_sb);
			compound->c_curr_sb = child_sb; /* transfer ref */
			compound->c_curr_nfh.nfh_sb = child_sb->sb_id;
		}
	}

	inode_active_put(compound->c_inode);
	compound->c_inode = child;
	compound->c_curr_nfh.nfh_ino = child->i_ino;

	/*
	 * RFC 8881 S2.6.3.1: after crossing a mount point, the
	 * current filehandle is in a new export.  Re-check WRONGSEC
	 * against the child export's flavor list.
	 */
	*status = nfs4_check_wrongsec(compound);
	if (*status)
		goto out;

	stateid_put(compound->c_curr_stid);
	compound->c_curr_stid = NULL;

out:
	trace_nfs4_name(compound, name, __func__, __LINE__);
	free(name);
	dirent_put(child_de);

	return 0;
}

uint32_t nfs4_op_lookupp(struct compound *compound)
{
	LOOKUPP4res *res = NFS4_OP_RES_SETUP(compound, oplookupp);
	nfsstat4 *status = &res->status;

	struct reffs_dirent *parent_de = NULL;
	struct inode *parent = NULL;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	/* RFC 8881 S18.14.4: symlink --> NFS4ERR_SYMLINK, else NOTDIR. */
	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = S_ISLNK(compound->c_inode->i_mode) ? NFS4ERR_SYMLINK :
							       NFS4ERR_NOTDIR;
		goto out;
	}

	/*
	 * RFC 8881 S2.6.3.1.1.4: a put-FH before LOOKUPP must NOT
	 * return WRONGSEC.  LOOKUPP itself may return WRONGSEC after
	 * crossing to the parent export (checked below).  No pre-
	 * crossing check against the current (child) export.
	 */

	/*
	 * At the root of a sb: if this is the pseudo-root (root sb),
	 * there is no parent --> NFS4ERR_NOENT.  If this is a child sb,
	 * cross back to the parent sb's mounted-on directory.
	 */
	if (compound->c_curr_nfh.nfh_ino == INODE_ROOT_ID) {
		if (compound->c_curr_nfh.nfh_sb == SUPER_BLOCK_ROOT_ID) {
			*status = NFS4ERR_NOENT;
			goto out;
		}

		/*
		 * Cross back to parent sb at the mount point.
		 * sb_parent_sb / sb_mount_dirent are stable while the
		 * child sb is mounted -- unmount only runs at shutdown
		 * when no compounds are active.
		 */
		struct super_block *parent_sb =
			compound->c_curr_sb->sb_parent_sb;
		struct reffs_dirent *mount_de =
			compound->c_curr_sb->sb_mount_dirent;

		if (!parent_sb || !mount_de) {
			*status = NFS4ERR_SERVERFAULT;
			goto out;
		}

		parent = dirent_ensure_inode(mount_de);
		if (!parent) {
			*status = NFS4ERR_STALE;
			goto out;
		}

		inode_active_put(compound->c_inode);
		compound->c_inode = parent;
		compound->c_curr_nfh.nfh_ino = parent->i_ino;
		compound->c_curr_nfh.nfh_sb = parent_sb->sb_id;

		super_block_put(compound->c_curr_sb);
		compound->c_curr_sb = super_block_get(parent_sb);

		/*
		 * After crossing to the parent export, check WRONGSEC
		 * against the parent's flavor list.
		 */
		*status = nfs4_check_wrongsec(compound);
		if (*status)
			goto out;

		stateid_put(compound->c_curr_stid);
		compound->c_curr_stid = NULL;

		goto out;
	}

	/*
	 * inode_ensure_parent_dirent loads and links the parent dirent so
	 * that a subsequent LOOKUP on the returned directory works without
	 * needing inode_reconstruct_path_to_root.
	 */
	parent_de = inode_ensure_parent_dirent(compound->c_inode);
	if (!parent_de) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	parent = dirent_ensure_inode(parent_de);
	if (!parent) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	inode_active_put(compound->c_inode);
	compound->c_inode = parent;
	compound->c_curr_nfh.nfh_ino = parent->i_ino;

	stateid_put(compound->c_curr_stid);
	compound->c_curr_stid = NULL;

out:
	dirent_put(parent_de);

	return 0;
}

uint32_t nfs4_op_create(struct compound *compound)
{
	CREATE4args *args = NFS4_OP_ARG_SETUP(compound, opcreate);
	CREATE4res *res = NFS4_OP_RES_SETUP(compound, opcreate);
	nfsstat4 *status = &res->status;
	CREATE4resok *resok = NFS4_OP_RESOK_SETUP(res, CREATE4res_u, resok4);

	struct inode *new_inode = NULL;
	struct timespec dir_before, dir_after;
	changeid4 cinfo_before = 0, cinfo_after = 0;
	char *name = NULL;
	char *linkpath = NULL;
	int ret;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (nfs4_check_grace()) {
		*status = NFS4ERR_GRACE;
		goto out;
	}

	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	/* NF4REG is not valid for CREATE -- clients must use OPEN. */
	if (args->objtype.type == NF4REG) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	if (args->objname.utf8string_len == 0) {
		*status = NFS4ERR_INVAL;
		goto out;
	}
	*status = nfs4_validate_component(&args->objname);
	if (*status) {
		goto out;
	}
	name = strndup(args->objname.utf8string_val,
		       args->objname.utf8string_len);
	if (!name) {
		*status = NFS4ERR_DELAY;
		goto out;
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		*status = NFS4ERR_BADNAME;
		goto out;
	}

	ret = inode_access_check(compound->c_inode, &compound->c_ap, W_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_CREATE);
		goto out;
	}

	cinfo_before = inode_changeid(compound->c_inode);
	switch (args->objtype.type) {
	case NF4DIR:
		ret = vfs_mkdir(compound->c_inode, name, 0777, &compound->c_ap,
				&new_inode, &dir_before, &dir_after);
		break;

	case NF4LNK: {
		linktext4 *ld = &args->objtype.createtype4_u.linkdata;
		linkpath = strndup(ld->linktext4_val, ld->linktext4_len);
		if (!linkpath) {
			*status = NFS4ERR_DELAY;
			goto out;
		}
		ret = vfs_symlink(compound->c_inode, name, linkpath,
				  &compound->c_ap, &new_inode, &dir_before,
				  &dir_after);
		break;
	}

	case NF4BLK: {
		specdata4 *sd = &args->objtype.createtype4_u.devdata;
		ret = vfs_mknod(compound->c_inode, name, S_IFBLK | 0666,
				makedev(sd->specdata1, sd->specdata2),
				&compound->c_ap, &new_inode, &dir_before,
				&dir_after);
		break;
	}

	case NF4CHR: {
		specdata4 *sd = &args->objtype.createtype4_u.devdata;
		ret = vfs_mknod(compound->c_inode, name, S_IFCHR | 0666,
				makedev(sd->specdata1, sd->specdata2),
				&compound->c_ap, &new_inode, &dir_before,
				&dir_after);
		break;
	}

	case NF4SOCK:
		ret = vfs_mknod(compound->c_inode, name, S_IFSOCK | 0666, 0,
				&compound->c_ap, &new_inode, &dir_before,
				&dir_after);
		break;

	case NF4FIFO:
		ret = vfs_mknod(compound->c_inode, name, S_IFIFO | 0666, 0,
				&compound->c_ap, &new_inode, &dir_before,
				&dir_after);
		break;

	default:
		*status = NFS4ERR_BADTYPE;
		goto out;
	}

	if (ret) {
		*status = ret == -EEXIST ? NFS4ERR_EXIST :
			  ret == -ENOSPC ? NFS4ERR_NOSPC :
					   errno_to_nfs4(ret, OP_CREATE);
		goto out;
	}

	/* Recall directory delegations held by other clients. */
	nfs4_recall_dir_delegations(
		compound->c_server_state, compound->c_inode,
		compound->c_nfs4_client ?
			nfs4_client_to_client(compound->c_nfs4_client) :
			NULL);

	/* Switch current FH to the newly created object. */
	inode_active_put(compound->c_inode);
	compound->c_inode = new_inode;
	new_inode = NULL;
	compound->c_curr_nfh.nfh_ino = compound->c_inode->i_ino;

	/*
	 * RFC 8881 S18.1: apply createattrs (mode, owner, etc.) to the
	 * newly created object.  The initial mode is a permissive default;
	 * the client's requested mode overrides it here.
	 */
	if (args->createattrs.attrmask.bitmap4_len > 0) {
		*status = nfs4_apply_createattrs(&args->createattrs,
						 compound->c_inode,
						 &resok->attrset,
						 &compound->c_ap);
		if (*status)
			goto out;
	}

	cinfo_after = inode_changeid(compound->c_inode);
	resok->cinfo.atomic = TRUE;
	resok->cinfo.before = cinfo_before;
	resok->cinfo.after = cinfo_after;

out:
	trace_nfs4_name(compound, name, __func__, __LINE__);
	inode_active_put(new_inode);
	free(linkpath);
	free(name);

	return 0;
}

uint32_t nfs4_op_remove(struct compound *compound)
{
	REMOVE4args *args = NFS4_OP_ARG_SETUP(compound, opremove);
	REMOVE4res *res = NFS4_OP_RES_SETUP(compound, opremove);
	nfsstat4 *status = &res->status;
	REMOVE4resok *resok = NFS4_OP_RESOK_SETUP(res, REMOVE4res_u, resok4);

	struct timespec dir_before, dir_after;
	changeid4 cinfo_before = 0, cinfo_after = 0;
	char *name = NULL;
	int ret;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (nfs4_check_grace()) {
		*status = NFS4ERR_GRACE;
		goto out;
	}

	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	if (args->target.utf8string_len == 0) {
		*status = NFS4ERR_INVAL;
		goto out;
	}
	*status = nfs4_validate_component(&args->target);
	if (*status) {
		goto out;
	}
	name = strndup(args->target.utf8string_val,
		       args->target.utf8string_len);
	if (!name) {
		*status = NFS4ERR_DELAY;
		goto out;
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		*status = NFS4ERR_BADNAME;
		goto out;
	}

	/*
	 * Proxy-SB fast path: forward REMOVE to upstream MDS so the
	 * file actually goes away.  The local proxy SB only holds a
	 * cache of the upstream namespace; deleting it locally would
	 * leave the file on the upstream and cause a future LOOKUP to
	 * see it again.
	 */
	if (compound->c_inode->i_sb &&
	    compound->c_inode->i_sb->sb_proxy_binding) {
		const struct ps_sb_binding *binding =
			compound->c_inode->i_sb->sb_proxy_binding;
		uint8_t parent_fh[PS_MAX_FH_SIZE];
		uint32_t parent_fh_len = 0;

		int fret = ps_inode_get_upstream_fh(compound->c_inode,
						    parent_fh,
						    sizeof(parent_fh),
						    &parent_fh_len);
		if (fret < 0) {
			*status = NFS4ERR_STALE;
			goto out;
		}

		const struct ps_listener_state *pls =
			ps_state_find(binding->psb_listener_id);

		if (!pls || !pls->pls_session) {
			*status = NFS4ERR_DELAY;
			goto out;
		}

		struct ps_proxy_remove_reply rreply;

		memset(&rreply, 0, sizeof(rreply));
		fret = ps_proxy_forward_remove(pls->pls_session, parent_fh,
					       parent_fh_len, name,
					       (uint32_t)strlen(name),
					       &compound->c_ap, &rreply);
		if (fret < 0) {
			*status = errno_to_nfs4(fret, OP_REMOVE);
			goto out;
		}

		resok->cinfo.atomic = rreply.atomic;
		resok->cinfo.before = rreply.before;
		resok->cinfo.after = rreply.after;

		/*
		 * NOT_NOW_BROWN_COW: invalidate the local cached dirent
		 * for `name` after the upstream REMOVE succeeds.  Today
		 * a stale local cache means a follow-up LOOKUP through
		 * the PS hits the warm dirent and reports the file
		 * still exists until the next READDIR repopulates from
		 * upstream.  Safe for cold-mount / single-client BAT
		 * demo (the next op is usually GETATTR which forwards
		 * fresh and gets NFS4ERR_NOENT directly), but a
		 * multi-client warm-cache race needs the local prune.
		 * The fix is dirent_load_child_by_name + a lifecycle
		 * helper to detach the dirent without driving
		 * vfs_remove's local-storage tear-down (proxy SB inodes
		 * have no .dat file).
		 */
		goto out;
	}

	ret = inode_access_check(compound->c_inode, &compound->c_ap, W_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_REMOVE);
		goto out;
	}

	/*
	 * Try removing as a non-directory first.  vfs_remove() returns
	 * -EISDIR if the target is a directory; fall back to vfs_rmdir().
	 */
	cinfo_before = inode_changeid(compound->c_inode);
	ret = vfs_remove(compound->c_inode, name, &compound->c_ap, &dir_before,
			 &dir_after);
	if (ret == -EISDIR)
		ret = vfs_rmdir(compound->c_inode, name, &compound->c_ap,
				&dir_before, &dir_after);

	if (ret) {
		*status = errno_to_nfs4(ret, OP_REMOVE);
		goto out;
	}

	/* Recall directory delegations held by other clients. */
	nfs4_recall_dir_delegations(
		compound->c_server_state, compound->c_inode,
		compound->c_nfs4_client ?
			nfs4_client_to_client(compound->c_nfs4_client) :
			NULL);

	cinfo_after = inode_changeid(compound->c_inode);
	resok->cinfo.atomic = TRUE;
	resok->cinfo.before = cinfo_before;
	resok->cinfo.after = cinfo_after;

out:
	trace_nfs4_name(compound, name, __func__, __LINE__);
	free(name);

	return 0;
}

uint32_t nfs4_op_rename(struct compound *compound)
{
	RENAME4args *args = NFS4_OP_ARG_SETUP(compound, oprename);
	RENAME4res *res = NFS4_OP_RES_SETUP(compound, oprename);
	nfsstat4 *status = &res->status;
	RENAME4resok *resok = NFS4_OP_RESOK_SETUP(res, RENAME4res_u, resok4);

	struct inode *old_dir = NULL;
	struct timespec old_before, old_after, new_before, new_after;
	changeid4 src_cinfo_before = 0, src_cinfo_after = 0;
	changeid4 dst_cinfo_before = 0, dst_cinfo_after = 0;
	char *oldname = NULL;
	char *newname = NULL;
	int ret;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (network_file_handle_empty(&compound->c_saved_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (nfs4_check_grace()) {
		*status = NFS4ERR_GRACE;
		goto out;
	}

	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	/* Validate names. */
	if (args->oldname.utf8string_len == 0 ||
	    args->newname.utf8string_len == 0) {
		*status = NFS4ERR_INVAL;
		goto out;
	}
	*status = nfs4_validate_component(&args->oldname);
	if (!*status)
		*status = nfs4_validate_component(&args->newname);
	if (*status) {
		goto out;
	}
	oldname = strndup(args->oldname.utf8string_val,
			  args->oldname.utf8string_len);
	newname = strndup(args->newname.utf8string_val,
			  args->newname.utf8string_len);
	if (!oldname || !newname) {
		*status = NFS4ERR_DELAY;
		goto out;
	}
	if (strcmp(oldname, ".") == 0 || strcmp(oldname, "..") == 0 ||
	    strcmp(newname, ".") == 0 || strcmp(newname, "..") == 0) {
		*status = NFS4ERR_BADNAME;
		goto out;
	}

	/*
	 * RFC 5661 S18.26: source is SAVED_FH, destination is CURRENT_FH.
	 * Load the saved directory inode.
	 */
	old_dir =
		inode_find(compound->c_saved_sb, compound->c_saved_nfh.nfh_ino);
	if (!old_dir) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	if (!S_ISDIR(old_dir->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	ret = inode_access_check(old_dir, &compound->c_ap, W_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_RENAME);
		goto out;
	}

	ret = inode_access_check(compound->c_inode, &compound->c_ap, W_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_RENAME);
		goto out;
	}

	src_cinfo_before = inode_changeid(old_dir);
	dst_cinfo_before = inode_changeid(compound->c_inode);
	ret = vfs_rename(old_dir, oldname, compound->c_inode, newname,
			 &compound->c_ap, &old_before, &old_after, &new_before,
			 &new_after);
	if (ret) {
		/*
		 * RFC 8881 S18.26.3: renaming a non-directory over a
		 * directory returns NFS4ERR_EXIST, not NFS4ERR_ISDIR
		 * (which is not a valid RENAME error).
		 */
		if (ret == -EISDIR)
			*status = NFS4ERR_EXIST;
		else
			*status = errno_to_nfs4(ret, OP_RENAME);
		goto out;
	}

	/* Recall directory delegations on both source and target dirs. */
	{
		struct client *exclude =
			compound->c_nfs4_client ?
				nfs4_client_to_client(compound->c_nfs4_client) :
				NULL;
		nfs4_recall_dir_delegations(compound->c_server_state, old_dir,
					    exclude);
		nfs4_recall_dir_delegations(compound->c_server_state,
					    compound->c_inode, exclude);
	}

	src_cinfo_after = inode_changeid(old_dir);
	dst_cinfo_after = inode_changeid(compound->c_inode);

	resok->source_cinfo.atomic = TRUE;
	resok->source_cinfo.before = src_cinfo_before;
	resok->source_cinfo.after = src_cinfo_after;
	resok->target_cinfo.atomic = TRUE;
	resok->target_cinfo.before = dst_cinfo_before;
	resok->target_cinfo.after = dst_cinfo_after;

out:
	inode_active_put(old_dir);
	free(oldname);
	free(newname);

	return 0;
}

uint32_t nfs4_op_link(struct compound *compound)
{
	LINK4args *args = NFS4_OP_ARG_SETUP(compound, oplink);
	LINK4res *res = NFS4_OP_RES_SETUP(compound, oplink);
	nfsstat4 *status = &res->status;
	LINK4resok *resok = NFS4_OP_RESOK_SETUP(res, LINK4res_u, resok4);

	struct inode *src_inode = NULL;
	changeid4 cinfo_before = 0, cinfo_after = 0;
	char *name = NULL;
	int ret;

	/*
	 * RFC 8881 S18.9.3: CURRENT_FH is the target directory;
	 * SAVED_FH is the file to link.
	 */
	if (network_file_handle_empty(&compound->c_curr_nfh) ||
	    network_file_handle_empty(&compound->c_saved_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (nfs4_check_grace()) {
		*status = NFS4ERR_GRACE;
		goto out;
	}

	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	if (args->newname.utf8string_len == 0) {
		*status = NFS4ERR_INVAL;
		goto out;
	}
	*status = nfs4_validate_component(&args->newname);
	if (*status) {
		goto out;
	}

	name = strndup(args->newname.utf8string_val,
		       args->newname.utf8string_len);
	if (!name) {
		*status = NFS4ERR_DELAY;
		goto out;
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		*status = NFS4ERR_BADNAME;
		goto out;
	}

	src_inode =
		inode_find(compound->c_saved_sb, compound->c_saved_nfh.nfh_ino);
	if (!src_inode) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	if (S_ISDIR(src_inode->i_mode)) {
		*status = NFS4ERR_ISDIR;
		goto out;
	}

	ret = inode_access_check(compound->c_inode, &compound->c_ap, W_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_LINK);
		goto out;
	}

	cinfo_before = inode_changeid(compound->c_inode);
	ret = vfs_link(src_inode, compound->c_inode, name, &compound->c_ap);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_LINK);
		goto out;
	}

	/* Recall directory delegations held by other clients. */
	nfs4_recall_dir_delegations(
		compound->c_server_state, compound->c_inode,
		compound->c_nfs4_client ?
			nfs4_client_to_client(compound->c_nfs4_client) :
			NULL);

	cinfo_after = inode_changeid(compound->c_inode);

	resok->cinfo.atomic = TRUE;
	resok->cinfo.before = cinfo_before;
	resok->cinfo.after = cinfo_after;

out:
	inode_active_put(src_inode);
	trace_nfs4_name(compound, name, __func__, __LINE__);
	free(name);

	return 0;
}

uint32_t nfs4_op_openattr(struct compound *compound)
{
	OPENATTR4res *res = NFS4_OP_RES_SETUP(compound, opopenattr);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_readlink(struct compound *compound)
{
	READLINK4res *res = NFS4_OP_RES_SETUP(compound, opreadlink);
	nfsstat4 *status = &res->status;
	READLINK4resok *resok =
		NFS4_OP_RESOK_SETUP(res, READLINK4res_u, resok4);

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (!S_ISLNK(compound->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	if (!compound->c_inode->i_symlink) {
		*status = NFS4ERR_SERVERFAULT;
		return 0;
	}

	resok->link.linktext4_val = strdup(compound->c_inode->i_symlink);
	if (!resok->link.linktext4_val) {
		*status = NFS4ERR_DELAY;
		return 0;
	}
	resok->link.linktext4_len = strlen(compound->c_inode->i_symlink);

	return 0;
}
