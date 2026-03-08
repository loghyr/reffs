/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_NFS4_INTERNAL_H
#define _REFFS_NFS4_INTERNAL_H

#include <rpc/auth_unix.h>
#include <sys/types.h>

#include "reffs/filehandle.h"

struct rpc_trans;
struct super_block;
struct inode;

struct compound {
	struct rpc_trans *c_rt;
	u_int c_curr_op;
	struct authunix_parms c_ap;
	struct network_file_handle c_curr_fh;
	struct network_file_handle c_saved_fh;
	struct super_block *c_curr_sb;
	struct super_block *c_saved_sb;
	struct inode *c_inode;
};

int nfs4_proc_compound(struct rpc_trans *rt);
void dispatch_compound(struct compound *c);

#endif
