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
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include "reffs/log.h"
#include "reffs/test.h"
#include "reffs/inode.h"
#include "reffs/identity.h"

static bool gid_in_gids(gid_t gid, uint32_t len, gid_t *gids)
{
	for (uint32_t i = 0; i < len; i++)
		if (gid == gids[i]) {
			TRACE(REFFS_TRACE_LEVEL_WARNING,
			      "gids stop, gid=%u len=%u gids=%p", gid, len,
			      (void *)gids);
			return true;
		}

	return false;
}

int rpc_cred_to_authunix_parms(struct rpc_cred *cred, struct authunix_parms *ap)
{
	switch (cred->rc_flavor) {
	case AUTH_SYS:
		ap->aup_uid = cred->rc_unix.aup_uid;
		ap->aup_gid = cred->rc_unix.aup_gid;
		ap->aup_len = cred->rc_unix.aup_len;
		ap->aup_gids = cred->rc_unix.aup_gids;
		break;
	case AUTH_NONE:
		ap->aup_uid = 65534;
		ap->aup_gid = 65534;

		ap->aup_len = 0;
		ap->aup_gids = NULL;

		break;
	default:
		return EPERM;
	}

	return 0;
}

int inode_access_check(struct inode *inode, struct authunix_parms *ap, int mode)
{
	/* Superuser mode for now */
	if (ap->aup_uid == 0)
		return 0;

	if (ap->aup_uid == inode->i_uid) {
		if ((mode & W_OK) && !(inode->i_mode & S_IWUSR))
			return EACCES;
		if ((mode & R_OK) && !(inode->i_mode & S_IRUSR))
			return EACCES;
		if ((mode & X_OK) && !(inode->i_mode & S_IXUSR))
			return EACCES;
	} else if (ap->aup_gid == inode->i_gid ||
		   gid_in_gids(inode->i_gid, ap->aup_len, ap->aup_gids)) {
		if ((mode & W_OK) && !(inode->i_mode & S_IWGRP))
			return EACCES;
		if ((mode & R_OK) && !(inode->i_mode & S_IRGRP))
			return EACCES;
		if ((mode & X_OK) && !(inode->i_mode & S_IXGRP))
			return EACCES;
	} else {
		if ((mode & W_OK) && !(inode->i_mode & S_IWOTH))
			return EACCES;
		if ((mode & R_OK) && !(inode->i_mode & S_IROTH))
			return EACCES;
		if ((mode & X_OK) && !(inode->i_mode & S_IXOTH))
			return EACCES;
	}

	return 0;
}

/*
 * Check if the specified user is allowed to change a file's group to the target group.
 * A user can only change to a group they are a member of.
 *
 * Parameters:
 *   uid - User ID trying to perform the chgrp operation
 *   target_gid - Group ID they want to change the file to
 *   ap - Auth parameters from the RPC call
 *
 * Returns:
 *   true if the user is allowed to change to the target group
 *   false if the user is not allowed or an error occurred
 */
bool can_user_chgrp_to_group(uid_t uid, gid_t target_gid,
			     struct authunix_parms *ap)
{
	if (uid == 0)
		return true;

	if (!ap)
		return false;

	if (ap->aup_gid == target_gid)
		return true;

	for (uint32_t i = 0; i < ap->aup_len; i++)
		if (ap->aup_gids[i] == target_gid)
			return true;

	return false;
}

/*
 * Check if the specified user is in the specified group using the auth parameters
 * from the RPC call.
 *
 * Parameters:
 *   uid - User ID to check
 *   group_to_check - Group ID to check membership for
 *   ap - Auth parameters from the RPC call
 *
 * Returns:
 *   true if the user is in the group
 *   false if the user is not in the group or an error occurred
 */
bool is_user_in_group(uid_t uid, gid_t group_to_check,
		      struct authunix_parms *ap)
{
	if (uid == 0)
		return true;

	if (!ap)
		return false;

	if (ap->aup_gid == group_to_check)
		return true;

	for (uint32_t i = 0; i < ap->aup_len; i++)
		if (ap->aup_gids[i] == group_to_check)
			return true;

	return false;
}

int inode_privilege_check(struct inode *inode, struct authunix_parms *ap,
			  enum privilege_op op, void *arg)
{
	/* Root can do anything */
	if (!ap || ap->aup_uid == 0)
		return 0;

	/* Owner checks */
	switch (op) {
	case PRIV_CHANGE_OWNER:
		/* Only owner can change ownership */
		if (ap->aup_uid != inode->i_uid)
			return EPERM;

		/* Prevent non-root from giving away files */
		if (arg && *(uid_t *)arg != inode->i_uid &&
		    *(uid_t *)arg != (uid_t)-1)
			return EPERM;
		break;

	case PRIV_CHANGE_GROUP:
		/* Only owner can change group */
		if (ap->aup_uid != inode->i_uid)
			return EPERM;

		/* User must be a member of the target group */
		if (arg) {
			gid_t new_gid = *(gid_t *)arg;
			if (new_gid != (gid_t)-1 && new_gid != ap->aup_gid &&
			    !gid_in_gids(new_gid, ap->aup_len, ap->aup_gids))
				return EPERM;
		}
		break;

	case PRIV_SET_SPECIAL_BITS:
		/* Only owner can set special bits */
		if (ap->aup_uid != inode->i_uid)
			return EPERM;
		break;

	case PRIV_TIME_CHANGE:
		/* Only owner or root can change times */
		if (ap->aup_uid != inode->i_uid)
			return EPERM;
		break;
	}

	return 0;
}
