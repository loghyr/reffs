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

int inode_permission_check(struct inode *inode, struct rpc_cred *cred,
			   struct authunix_parms *ap, int mode)
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

	/* Superuser mode for now */
	if (ap->aup_uid == 0)
		return 0;

	if (ap->aup_uid == inode->i_uid) {
		if ((mode & W_OK) && !(inode->i_mode & S_IWUSR))
			return EPERM;
		if ((mode & R_OK) && !(inode->i_mode & S_IRUSR))
			return EPERM;
		if ((mode & X_OK) && !(inode->i_mode & S_IXUSR))
			return EPERM;
	} else if (ap->aup_gid == inode->i_gid ||
		   gid_in_gids(inode->i_gid, ap->aup_len, ap->aup_gids)) {
		if ((mode & W_OK) && !(inode->i_mode & S_IWGRP))
			return EPERM;
		if ((mode & R_OK) && !(inode->i_mode & S_IRGRP))
			return EPERM;
		if ((mode & X_OK) && !(inode->i_mode & S_IXGRP))
			return EPERM;
	} else {
		if ((mode & W_OK) && !(inode->i_mode & S_IWOTH))
			return EPERM;
		if ((mode & R_OK) && !(inode->i_mode & S_IROTH))
			return EPERM;
		if ((mode & X_OK) && !(inode->i_mode & S_IXOTH))
			return EPERM;
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
