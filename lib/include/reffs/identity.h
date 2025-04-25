/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_INDENTITY_H
#define _REFFS_INDENTITY_H

#include <stdbool.h>
#include <rpc/clnt.h>
#include <rpc/rpc_msg.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>

#include "reffs/inode.h"
#include "reffs/rpc.h"

bool can_user_chgrp_to_group(uid_t uid, gid_t target_gid,
			     struct authunix_parms *ap);

bool is_user_in_group(uid_t uid, gid_t group_to_check,
		      struct authunix_parms *ap);

int inode_permission_check(struct inode *inode, struct rpc_cred *cred,
			   struct authunix_parms *ap, int mode);

#endif /* _REFFS_INDENTITY_H */
