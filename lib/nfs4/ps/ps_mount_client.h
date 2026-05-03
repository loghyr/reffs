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
 * Two transport modes selected by `port`:
 *
 *   port == 0 -- portmap-driven.  Uses clnt_create with proto=tcp,
 *                which queries the host's rpcbind for the MOUNT_V3
 *                program port.  Works against any reffs upstream
 *                that pmap-registers MOUNT3 at startup, AND against
 *                a host whose rpcbind knows about MOUNT3.  Fails
 *                on container topologies where the upstream MDS
 *                runs in docker but the host's rpcbind has no
 *                NFS/MOUNT services registered (the bench case).
 *
 *   port  > 0 -- explicit-port direct connect.  Builds a sockaddr_in
 *                via getaddrinfo and uses clnttcp_create on the
 *                supplied port, bypassing portmap entirely.
 *                Required for the bench/container topology and any
 *                deployment where rpcbind is unavailable or
 *                stale.  Mirrors the bdde4f6539db pattern used in
 *                ds_io.c and the mds_session_clnt_open
 *                explicit-port path.  IPv4 only (AF_INET hint to
 *                getaddrinfo); IPv6 upstreams are not supported on
 *                this path.
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

int ps_mount_fetch_exports(const char *host, uint16_t port,
			   struct ps_export_entry **out, size_t *nout);

/*
 * Release an array returned by ps_mount_fetch_exports().  NULL-safe.
 */
void ps_mount_free_exports(struct ps_export_entry *arr);

#endif /* _REFFS_PS_MOUNT_CLIENT_H */
