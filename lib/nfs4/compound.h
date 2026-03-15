/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_COMPOUND_H
#define _REFFS_NFS4_COMPOUND_H

#include <rpc/auth_unix.h>
#include <sys/types.h>

#include "reffs/filehandle.h"
#include "reffs/rpc.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"

struct compound {
	struct rpc_trans *c_rt;
	u_int c_curr_op;
	struct authunix_parms c_ap;
	struct network_file_handle c_curr_nfh;
	struct network_file_handle c_saved_nfh;
	struct super_block *c_curr_sb;
	struct super_block *c_saved_sb;
	struct inode *c_inode;
};

int nfs4_proc_compound(struct rpc_trans *rt);
void dispatch_compound(struct compound *c);

#endif
