/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * NFSv4 owner/owner_group string <--> uid/gid conversion.
 *
 * Wraps the idmap cache with fallback to numeric format.
 * Used by attr.c for GETATTR and SETATTR.
 */

#ifndef _NFS4_OWNER_H
#define _NFS4_OWNER_H

#include <sys/types.h>
#include "reffs/utf8string.h"

/*
 * reffs_owner_from_uid - produce "user@domain" or numeric "1000".
 *
 * Tries the idmap cache first.  Falls back to decimal format
 * if no name mapping exists.  Caller must utf8string_free(dst).
 */
int reffs_owner_from_uid(utf8string *dst, uid_t uid);
int reffs_owner_group_from_gid(utf8string *dst, gid_t gid);

/*
 * reffs_owner_to_uid - parse "user@domain" or numeric "1000".
 *
 * Accepts both formats.  Returns 0 on success, negative errno
 * on failure (caller converts to NFS4ERR_BADOWNER).
 */
int reffs_owner_to_uid(const utf8string *owner, uid_t *uid);
int reffs_owner_group_to_gid(const utf8string *owner_group, gid_t *gid);

#endif /* _NFS4_OWNER_H */
