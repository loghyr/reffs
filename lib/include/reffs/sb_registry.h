/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_SB_REGISTRY_H
#define _REFFS_SB_REGISTRY_H

#include <stdint.h>
#include <uuid/uuid.h>

/*
 * Superblock registry -- persists the set of superblocks and their
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
#define SB_REGISTRY_MAX_FLAVORS 8
#define SB_REGISTRY_MAX_DSTORES 16

/*
 * Per-sb client rule file: <state_dir>/sb_<id>.clients
 *
 * Format: 4-byte rule count followed by N fixed-size records.
 * Written with write-temp/fdatasync/rename.  Absent file means
 * no client rules are configured for that export.
 */
#define SB_REGISTRY_CLIENT_MATCH_MAX 128

struct sb_registry_client_rule {
	char srcr_match[SB_REGISTRY_CLIENT_MATCH_MAX];
	uint32_t srcr_flags; /* SRCR_RW, SRCR_ROOT_SQUASH, SRCR_ALL_SQUASH */
	uint32_t srcr_nflavors;
	uint32_t srcr_flavors[SB_REGISTRY_MAX_FLAVORS];
};

#define SRCR_RW (1u << 0)
#define SRCR_ROOT_SQUASH (1u << 1)
#define SRCR_ALL_SQUASH (1u << 2)

/*
 * Persistent sb_id counter.  IDs are assigned monotonically and
 * never reused -- a deleted export's id is gone forever.  This
 * ensures NFS clients can distinguish a new export at the same
 * path from the old one (different fsid --> different filesystem).
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
	uint32_t sre_layout_types; /* SB_LAYOUT_FLEX_FILES etc. */
	uint32_t sre_nflavors;
	uint32_t sre_flavors[SB_REGISTRY_MAX_FLAVORS];
	uint32_t sre_ndstores;
	uint32_t sre_dstore_ids[SB_REGISTRY_MAX_DSTORES];
	char sre_path[SB_REGISTRY_MAX_PATH];
	char sre_backend_path[SB_REGISTRY_MAX_PATH];
};

/*
 * sb_registry_save -- persist all mounted/unmounted superblocks.
 * Scans the sb list and writes a snapshot of each non-root sb.
 * Returns 0 on success, -errno on failure.
 */
int sb_registry_save(const char *state_dir);

/*
 * sb_registry_load -- load the registry and recreate superblocks.
 * For each entry, allocates a super_block via super_block_alloc(),
 * creates its root dirent, and transitions to the saved state.
 * Returns 0 on success (or -ENOENT if no registry file exists),
 * -errno on failure.
 */
int sb_registry_load(const char *state_dir);

/*
 * sb_registry_detect_orphans -- scan state_dir for sb_<id>/
 * directories that are not in the registry.  Logs a warning
 * for each orphan.  Does not delete them (may be referral source).
 * Returns the number of orphans found (>= 0), or -errno on error.
 */
int sb_registry_detect_orphans(const char *state_dir);

/*
 * sb_registry_alloc_id -- return the next sb_id and persist the
 * incremented counter.  Thread-safe (single writer assumed --
 * probe ops are serialized).  Returns 0 on failure.
 */
uint64_t sb_registry_alloc_id(const char *state_dir);

/*
 * sb_client_rules_save -- persist a super_block's client rule list
 * to <state_dir>/sb_<sb_id>.clients.  Writes a 4-byte count followed
 * by nrules fixed-size sb_registry_client_rule records.
 * Returns 0 on success, -errno on failure.
 */
struct super_block;
int sb_client_rules_save(const char *state_dir, uint64_t sb_id,
			 const struct super_block *sb);

/*
 * sb_client_rules_load -- load the per-sb client rules file into sb.
 * Calls super_block_set_client_rules() on success.
 * Returns 0 on success, -ENOENT if no .clients file exists (no rules),
 * -errno on I/O or format error.
 */
int sb_client_rules_load(const char *state_dir, uint64_t sb_id,
			 struct super_block *sb);

#endif /* _REFFS_SB_REGISTRY_H */
