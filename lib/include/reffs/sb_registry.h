/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_SB_REGISTRY_H
#define _REFFS_SB_REGISTRY_H

#include <stdint.h>
#include <uuid/uuid.h>

/*
 * Superblock registry — persists the set of superblocks and their
 * lifecycle state across server restarts.
 *
 * On-disk format:
 *   sb_registry_header + N * sb_registry_entry
 *
 * Written via write-temp/fdatasync/rename (crash-safe).
 * All fields are host byte order (not portable across machines).
 *
 * The registry file lives at <state_dir>/superblocks.registry.
 * Each sb additionally has a per-sb directory <state_dir>/sb_<id>/
 * containing its config and data.
 */

#define SB_REGISTRY_MAGIC 0x53425247U /* "SBRG" */
#define SB_REGISTRY_VERSION 1
#define SB_REGISTRY_FILE "superblocks.registry"
#define SB_REGISTRY_MAX_PATH 256

/*
 * Persistent sb_id counter.  IDs are assigned monotonically and
 * never reused — a deleted export's id is gone forever.  This
 * ensures NFS clients can distinguish a new export at the same
 * path from the old one (different fsid → different filesystem).
 *
 * IDs 1 and 2 are reserved (SUPER_BLOCK_ROOT_ID, SUPER_BLOCK_DS_ID).
 * The counter starts at 3.
 */
#define SB_REGISTRY_FIRST_ID 3

struct sb_registry_header {
	uint32_t srh_magic;
	uint32_t srh_version;
	uint32_t srh_count;
	uint32_t srh_next_id; /* next sb_id to assign (monotonic) */
};

struct sb_registry_entry {
	uint64_t sre_id;
	uint32_t sre_state; /* enum sb_lifecycle */
	uint32_t sre_storage_type; /* enum reffs_storage_type */
	uuid_t sre_uuid; /* stable across restarts */
	char sre_path[SB_REGISTRY_MAX_PATH];
};

/*
 * sb_registry_save — persist all mounted/unmounted superblocks.
 * Scans the sb list and writes a snapshot of each non-root sb.
 * Returns 0 on success, -errno on failure.
 */
int sb_registry_save(const char *state_dir);

/*
 * sb_registry_load — load the registry and recreate superblocks.
 * For each entry, allocates a super_block via super_block_alloc(),
 * creates its root dirent, and transitions to the saved state.
 * Returns 0 on success (or -ENOENT if no registry file exists),
 * -errno on failure.
 */
int sb_registry_load(const char *state_dir);

/*
 * sb_registry_detect_orphans — scan state_dir for sb_<id>/
 * directories that are not in the registry.  Logs a warning
 * for each orphan.  Does not delete them (may be referral source).
 * Returns the number of orphans found (>= 0), or -errno on error.
 */
int sb_registry_detect_orphans(const char *state_dir);

/*
 * sb_registry_alloc_id — return the next sb_id and persist the
 * incremented counter.  Thread-safe (single writer assumed —
 * probe ops are serialized).  Returns 0 on failure.
 */
uint64_t sb_registry_alloc_id(const char *state_dir);

#endif /* _REFFS_SB_REGISTRY_H */
