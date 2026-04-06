/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * NFSv4 owner string conversion -- wraps idmap with numeric fallback.
 *
 * GETATTR: uid --> "user@domain" (from cache/libnfsidmap/nsswitch),
 *          falling back to decimal "1000" if no mapping exists.
 * SETATTR: "user@domain" or "1000" --> uid.
 */

#include <errno.h>
#include <sys/types.h>

#include "reffs/idmap.h"
#include "reffs/utf8string.h"
#include "nfs4/owner.h"

int reffs_owner_from_uid(utf8string *dst, uid_t uid)
{
	if (idmap_uid_to_name(uid, dst) == 0)
		return 0;

	/* No name mapping -- fall back to decimal. */
	return utf8string_from_uid(dst, uid);
}

int reffs_owner_group_from_gid(utf8string *dst, gid_t gid)
{
	if (idmap_gid_to_name(gid, dst) == 0)
		return 0;

	return utf8string_from_gid(dst, gid);
}

int reffs_owner_to_uid(const utf8string *owner, uid_t *uid)
{
	return idmap_name_to_uid(owner, uid);
}

int reffs_owner_group_to_gid(const utf8string *owner_group, gid_t *gid)
{
	return idmap_name_to_gid(owner_group, gid);
}
