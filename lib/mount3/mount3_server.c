/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
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
#include "reffs/super_block.h"

static int mount3_null(struct rpc_trans *rt)
{
	TRACE(REFFS_TRACE_LEVEL_WARNING, "NULL: xid=0x%08x",
	      rt->rt_info.ri_xid);
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

	TRACE(REFFS_TRACE_LEVEL_WARNING, "MNT: xid=0x%08x", rt->rt_info.ri_xid);

	mr->fhs_status = find_matching_directory_entry(&nm, *dp,
						       LAST_COMPONENT_IS_MATCH);
	if (mr->fhs_status)
		goto out;

	inode = nm->nm_dirent->d_inode;
	ino = inode->i_ino;
	sb_id = inode->i_sb->sb_id;

	nfh = calloc(1, sizeof(*nfh));
	if (!nfh) {
		mr->fhs_status = MNT3ERR_SERVERFAULT;
		goto out;
	}

	flavors = calloc(2, sizeof(*flavors));
	if (!flavors) {
		mr->fhs_status = MNT3ERR_SERVERFAULT;
		goto out;
	}

	mr->mountres3_u.mountinfo.fhandle.fhandle3_val = (char *)nfh;
	mr->mountres3_u.mountinfo.fhandle.fhandle3_len = sizeof(*nfh);
	nfh->nfh_vers = FILEHANDLE_VERSION_CURR;
	nfh->nfh_sb = sb_id; // FIXME: If mounted on, change the sb
	nfh->nfh_ino = ino;

	mr->mountres3_u.mountinfo.auth_flavors.auth_flavors_len = 2;
	mr->mountres3_u.mountinfo.auth_flavors.auth_flavors_val = flavors;

	flavors[0] = AUTH_NONE;
	flavors[1] = AUTH_UNIX;

	nfh = NULL;
	flavors = NULL;

	mr->fhs_status = MNT3_OK;

out:
	free(flavors);
	free(nfh);
	if (nm) {
		dirent_put(nm->nm_dirent);
		free(nm);
	}
	return mr->fhs_status;
}

static int mount3_exports(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	exportnode *en;
	exportnode *en_head = NULL;
	exportnode *next;

	struct cds_list_head *sb_list = super_block_list_head();

	struct super_block *sb = NULL;

	TRACE(REFFS_TRACE_LEVEL_WARNING, "EXPORTS: xid=0x%08x",
	      rt->rt_info.ri_xid);

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, sb_list, sb_link) {
		en = calloc(1, sizeof(*en));
		if (!en)
			goto out_unwind;

		en->ex_dir = strdup(sb->sb_path);
		if (!en->ex_dir)
			goto out_unwind;

		en->ex_next = en_head;
		en_head = en;
	}
	rcu_read_unlock();

	ph->ph_res = en_head;

	return 0;
out_unwind:

	for (en = en_head; en != NULL; en = next) {
		next = en->ex_next;
		free(en->ex_dir);
		free(en);
	}

	return 0;
}

const struct rpc_operations_handler mount3_operations_handler[] = {
	RPC_OPERATION_INIT(MOUNTPROC3_NULL, NULL, NULL, NULL, NULL,
			   mount3_null),
	RPC_OPERATION_INIT(MOUNTPROC3_MNT, xdr_dirpath, dirpath *,
			   xdr_mountres3, mountres3, mount3_mnt),
	RPC_OPERATION_INIT(MOUNTPROC3_DUMP, NULL, NULL, xdr_mountlist,
			   mountlist, NULL),
	RPC_OPERATION_INIT(MOUNTPROC3_UMNT, xdr_dirpath, dirpath *, NULL, NULL,
			   NULL),
	RPC_OPERATION_INIT(MOUNTPROC3_UMNTALL, NULL, NULL, NULL, NULL, NULL),
	RPC_OPERATION_INIT(MOUNTPROC3_EXPORT, NULL, NULL, xdr_exports, exports,
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
