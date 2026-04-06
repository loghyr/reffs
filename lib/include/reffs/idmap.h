/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Identity mapping cache -- bidirectional name <--> uid/gid lookup.
 *
 * The cache sits between NFSv4 owner string handling and the inode
 * layer.  It maps between wire-format "user@domain" strings and
 * numeric uid/gid values stored on inodes.
 *
 * Entries are populated lazily from:
 *   - GSS authentication (idmap_cache_uid/gid)
 *   - libnfsidmap lookups (automatic on cache miss)
 *   - nsswitch/getpw fallback
 *
 * The cache uses liburcu lock-free hash tables for thread safety.
 */

#ifndef _REFFS_IDMAP_H
#define _REFFS_IDMAP_H

#include <stdint.h>
#include <sys/types.h>

#include "reffs/utf8string.h"

/*
 * idmap_init - initialize the identity mapping cache.
 *
 * @domain: NFSv4 domain string (e.g., "EXAMPLE.COM").
 *          If NULL or empty, auto-detect from libnfsidmap config
 *          or system hostname.
 *
 * Returns 0 on success, negative errno on failure.
 */
int idmap_init(const char *domain);

/*
 * idmap_fini - drain and destroy the identity mapping cache.
 */
void idmap_fini(void);

/*
 * idmap_uid_to_name - look up the "user@domain" string for a uid.
 *
 * Returns 0 and fills @dst with an allocated utf8string on success.
 * Returns -ENOENT if no mapping is found (caller should fall back
 * to numeric format).  Caller must utf8string_free(dst) on success.
 */
int idmap_uid_to_name(uid_t uid, utf8string *dst);
int idmap_gid_to_name(gid_t gid, utf8string *dst);

/*
 * idmap_name_to_uid - look up the uid for a "user@domain" string.
 *
 * Accepts "user@domain" or bare "user" (domain is assumed).
 * Numeric strings ("1000") are parsed directly without cache lookup.
 * Returns 0 on success, -ENOENT if unknown.
 */
int idmap_name_to_uid(const utf8string *name, uid_t *uid);
int idmap_name_to_gid(const utf8string *name, gid_t *gid);

/*
 * idmap_cache_uid - inject a known uid <--> name mapping.
 *
 * Called from the GSS authentication path after principal-->uid
 * mapping succeeds.  The name should include the domain suffix
 * (e.g., "alice@EXAMPLE.COM").
 */
void idmap_cache_uid(uid_t uid, const char *name);
void idmap_cache_gid(gid_t gid, const char *name);

/*
 * idmap_prewarm - resolve a batch of uncached uids and gids in parallel.
 *
 * Called before encoding a READDIR reply to avoid per-entry blocking
 * on external resolvers.  Spawns threads for uncached IDs and waits
 * with a bounded timeout.  IDs that don't resolve in time are silently
 * skipped (the per-entry encode path will fall back to numeric format).
 *
 * @uids/@nuids: unique uid values to resolve
 * @gids/@ngids: unique gid values to resolve
 * @timeout_ms: maximum total wait time (0 = default 3000ms)
 */
void idmap_prewarm(const uid_t *uids, int nuids, const gid_t *gids, int ngids,
		   int timeout_ms);

#endif /* _REFFS_IDMAP_H */
