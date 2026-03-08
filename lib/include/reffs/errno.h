/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_ERRNO_H
#define _REFFS_ERRNO_H

#include <errno.h>

/*
 * Project-specific error codes starting at 1024 to avoid collisions
 * with standard system errnos.
 */

#define REFFS_ERR_BASE 1024

#define EBADHANDLE (REFFS_ERR_BASE + 1)
#define ENOTSYNC (REFFS_ERR_BASE + 2)
#define EBADTYPE (REFFS_ERR_BASE + 3)
#define EBADNAME (REFFS_ERR_BASE + 4)
#define EWRONGTYPE (REFFS_ERR_BASE + 5)
#define EMSGSTARTED (REFFS_ERR_BASE + 6)
#define EREADDIRFULL (REFFS_ERR_BASE + 7)
#define ENAMETOOSMALL (REFFS_ERR_BASE + 8)
#define EBADXDR (REFFS_ERR_BASE + 9)
#define EATTRNOTSUP (REFFS_ERR_BASE + 10)
#define EBADOWNER (REFFS_ERR_BASE + 11)
#define EJUKEBOX EAGAIN

#endif /* _REFFS_ERRNO_H */
