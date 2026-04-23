/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_MOUNT_CLIENT_H
#define _REFFS_PS_MOUNT_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Enumerate an upstream's exported paths via MOUNT3 EXPORT (proc 5).
 *
 * Used by the proxy-server at startup and on on-demand discovery
 * retries to discover what paths the upstream MDS exports so the PS
 * can allocate a proxy superblock per discovered path.
 *
 * Uses portmapper to locate the MOUNT_PROGRAM port (reffsd
 * pmap-registers MOUNT3 at startup, so this resolves cleanly
 * against any reffs upstream).  Portmapper-less deployments are
 * NOT_NOW_BROWN_COW -- would need clnt_tli_create with an explicit
 * netbuf and port.
 *
 * On success, `*out` is a heap-allocated array of `struct
 * ps_export_entry` with `*nout` elements; caller must release via
 * `ps_mount_free_exports(*out)`.  Returns:
 *
 *   0        success (*nout may be 0 if upstream has no exports)
 *   -EINVAL  host is NULL or empty, or out / nout is NULL
 *   -errno   RPC connect / call failure
 */
/*
 * MNTPATHLEN (1024) is the wire cap on `dirpath` per RFC 1813; add
 * one byte so a maximum-length wire path still terminates with NUL
 * after strncpy with sizeof-1.  Silent truncation would break the
 * caller's `strcmp(local_path, entry->path)` comparisons later.
 */
#define PS_MOUNT_PATH_MAX 1025

struct ps_export_entry {
	char path[PS_MOUNT_PATH_MAX];
};

int ps_mount_fetch_exports(const char *host, struct ps_export_entry **out,
			   size_t *nout);

/*
 * Release an array returned by ps_mount_fetch_exports().  NULL-safe.
 */
void ps_mount_free_exports(struct ps_export_entry *arr);

#endif /* _REFFS_PS_MOUNT_CLIENT_H */
