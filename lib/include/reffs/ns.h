/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_NS_H
#define _REFFS_NS_H

void release_all_fs_dirents(void);
int reffs_ns_fini(void);
int reffs_ns_init(void);

#endif /* _REFFS_NS_H */
