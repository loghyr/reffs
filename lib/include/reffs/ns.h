/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NS_H
#define _REFFS_NS_H

#include <stdint.h>

void release_all_fs_dirents(void);
int reffs_ns_fini(void);
int reffs_ns_init(void);

/*
 * Create a listener-scoped root superblock (sb_id=1, listener_id=N).
 * Used by reffsd at startup to give each `[[proxy_mds]]` listener its
 * own empty root namespace so PUTROOTFH on the proxy port has
 * something to return.
 *
 * Always RAM-backed: the proxy namespace is volatile and the real
 * persistence lives on the upstream MDS that the proxy forwards to
 * (when forwarding is implemented).  Callable only after
 * reffs_ns_init() has set up the global backend + evictor.
 *
 * Returns 0 on success, -errno on failure (logs via LOG()).
 * Passing listener_id == 0 is rejected (that would collide with the
 * native root sb that reffs_ns_init() already created).
 */
int reffs_ns_init_proxy_listener(uint32_t listener_id);

#endif /* _REFFS_NS_H */
