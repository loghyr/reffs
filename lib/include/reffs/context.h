/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_CONTEXT_H
#define _REFFS_CONTEXT_H

#include <sys/types.h>

struct reffs_context {
	uid_t uid;
	gid_t gid;
};

/* 
 * Global context management for the current thread. 
 * This mimics how modern FS code handles caller credentials.
 */
void reffs_set_context(struct reffs_context *ctx);
struct reffs_context *reffs_get_context(void);

#endif /* _REFFS_CONTEXT_H */
