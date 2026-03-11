/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
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

int rpc_cred_to_authunix_parms(struct rpc_cred *cred,
			       struct authunix_parms *ap);

bool is_user_in_group(uid_t uid, gid_t group_to_check,
		      struct authunix_parms *ap);

#define REFFS_ACCESS_OWNER_OVERRIDE 0x1

int inode_access_check(struct inode *inode, struct authunix_parms *ap,
		       int mode);

int inode_access_check_flags(struct inode *inode, struct authunix_parms *ap,
			     int mode, int flags);

enum privilege_op {
	PRIV_CHANGE_OWNER,
	PRIV_CHANGE_GROUP,
	PRIV_SET_SPECIAL_BITS,
	PRIV_TIME_CHANGE
};

int inode_privilege_check(struct inode *inode, struct authunix_parms *ap,
			  enum privilege_op op, void *arg);

#endif /* _REFFS_INDENTITY_H */
