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
#define EJUKEBOX EAGAIN

#endif /* _REFFS_ERRNO_H */
