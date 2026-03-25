/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <zlib.h>
#include "mntv3_xdr.h"
#include "reffs/rpc.h"
#include "reffs/log.h"
#include "reffs/filehandle.h"
#include "reffs/fs.h"
#include "reffs/dirent.h"
#include "reffs/server.h"
#include "reffs/super_block.h"

static int mount3_null(struct rpc_trans *rt)
{
	TRACE("NULL: xid=0x%08x", rt->rt_info.ri_xid);
	return 0;
}

static int mount3_mnt(struct rpc_trans *rt)
{
	struct name_match *nm = NULL;
	struct inode *inode = NULL;

	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	dirpath *dp = ph->ph_args;
	mountres3 *mr = ph->ph_res;

	struct network_file_handle *nfh = NULL;
	int *flavors = NULL;

	uint64_t ino = 1;
	uint64_t sb_id = 1;

	TRACE("MNT: xid=0x%08x", rt->rt_info.ri_xid);

	mr->fhs_status = find_matching_directory_entry(&nm, *dp,
						       LAST_COMPONENT_IS_MATCH);
	if (mr->fhs_status)
		goto out;

	inode = nm->nm_dirent->rd_inode;
	ino = inode->i_ino;
	sb_id = inode->i_sb->sb_id;

	nfh = calloc(1, sizeof(*nfh));
	if (!nfh) {
		mr->fhs_status = MNT3ERR_SERVERFAULT;
		goto out;
	}

	mr->mountres3_u.mountinfo.fhandle.fhandle3_val = (char *)nfh;
	mr->mountres3_u.mountinfo.fhandle.fhandle3_len = sizeof(*nfh);
	nfh->nfh_vers = FILEHANDLE_VERSION_CURR;
	nfh->nfh_sb = sb_id; // FIXME: If mounted on, change the sb
	nfh->nfh_ino = ino;

	/*
	 * Return the export's configured auth flavors.  The MOUNT
	 * protocol uses RPC wire values (AUTH_SYS=1, RPCSEC_GSS=6).
	 * TLS is transport-level; map it to AUTH_SYS.
	 */
	struct server_state *ss = server_state_find();
	unsigned int nf = ss ? ss->ss_nflavors : 1;

	flavors = calloc(nf, sizeof(*flavors));
	if (!flavors) {
		server_state_put(ss);
		mr->fhs_status = MNT3ERR_SERVERFAULT;
		goto out;
	}

	unsigned int out = 0;
	bool have_auth_sys = false;

	if (ss && ss->ss_nflavors > 0) {
		for (unsigned int i = 0; i < ss->ss_nflavors; i++) {
			int wire;

			switch (ss->ss_flavors[i]) {
			case REFFS_AUTH_SYS:
			case REFFS_AUTH_TLS:
				if (have_auth_sys)
					continue;
				have_auth_sys = true;
				wire = AUTH_SYS;
				break;
			case REFFS_AUTH_KRB5:
			case REFFS_AUTH_KRB5I:
			case REFFS_AUTH_KRB5P:
				wire = RPCSEC_GSS;
				break;
			default:
				wire = AUTH_NONE;
				break;
			}
			flavors[out++] = wire;
		}
	} else {
		flavors[out++] = AUTH_SYS;
	}

	server_state_put(ss);

	mr->mountres3_u.mountinfo.auth_flavors.auth_flavors_len = out;
	mr->mountres3_u.mountinfo.auth_flavors.auth_flavors_val = flavors;

	nfh = NULL;
	flavors = NULL;

	mr->fhs_status = MNT3_OK;

out:
	free(flavors);
	free(nfh);
	name_match_free(nm);
	return mr->fhs_status;
}

static int mount3_dump(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	mountlist *ml = ph->ph_res;

	TRACE("DUMP: xid=0x%08x", rt->rt_info.ri_xid);

	*ml = NULL;

	return 0;
}

static int mount3_exports(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	exports *res = ph->ph_res;

	exportnode *en;
	exportnode *en_head = NULL;
	exportnode *next;

	struct cds_list_head *sb_list = super_block_list_head();

	struct super_block *sb = NULL;

	TRACE("EXPORTS: xid=0x%08x", rt->rt_info.ri_xid);

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, sb_list, sb_link) {
		en = calloc(1, sizeof(*en));
		if (!en)
			goto out_unwind;

		en->ex_dir = strdup(sb->sb_path);
		if (!en->ex_dir) {
			free(en);
			goto out_unwind;
		}

		en->ex_next = en_head;
		en_head = en;
	}
	rcu_read_unlock();

	*res = en_head;

	return 0;
out_unwind:

	rcu_read_unlock();
	for (en = en_head; en != NULL; en = next) {
		next = en->ex_next;
		free(en->ex_dir);
		free(en);
	}

	return 0;
}

struct rpc_operations_handler mount3_operations_handler[] = {
	RPC_OPERATION_INIT(MOUNTPROC3, NULL, NULL, NULL, NULL, NULL,
			   mount3_null),
	RPC_OPERATION_INIT(MOUNTPROC3, MNT, xdr_dirpath, dirpath *,
			   xdr_mountres3, mountres3, mount3_mnt),
	RPC_OPERATION_INIT(MOUNTPROC3, DUMP, NULL, NULL, xdr_mountlist,
			   mountlist, mount3_dump),
	RPC_OPERATION_INIT(MOUNTPROC3, UMNT, xdr_dirpath, dirpath *, NULL, NULL,
			   NULL),
	RPC_OPERATION_INIT(MOUNTPROC3, UMNTALL, NULL, NULL, NULL, NULL, NULL),
	RPC_OPERATION_INIT(MOUNTPROC3, EXPORT, NULL, NULL, xdr_exports, exports,
			   mount3_exports),
};

static struct rpc_program_handler *mount3_handler;

volatile sig_atomic_t mountv3_registered = 0;

int mount3_protocol_register(void)
{
	if (mountv3_registered)
		return 0;

	mountv3_registered = 1;

	mount3_handler = rpc_program_handler_alloc(
		MOUNT_PROGRAM, MOUNT_V3, mount3_operations_handler,
		sizeof(mount3_operations_handler) /
			sizeof(*mount3_operations_handler));
	if (!mount3_handler) {
		mountv3_registered = 0;
		return ENOMEM;
	}

	return 0;
}

int mount3_protocol_deregister(void)
{
	if (!mountv3_registered)
		return 0;

	rpc_program_handler_put(mount3_handler);
	mount3_handler = NULL;
	mountv3_registered = 0;

	return 0;
}
