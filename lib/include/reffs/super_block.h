/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_SB_H
#define _REFFS_SB_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <urcu.h>
#include <uuid/uuid.h>
#include <urcu/rculist.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>

#include "reffs/dirent.h"
struct inode; /* forward decl for sb_root_inode */
#include "reffs/nfs4_stats.h"
#include "reffs/settings.h"
#include "reffs/types.h"
#include "reffs/backend.h"

/*
 * Per-client export policy rule.  A superblock holds an ordered list
 * of these; client_rule_match() returns the first matching rule.
 *
 * scr_match format (subset of exports(5)):
 *   single IPv4 host  192.168.1.5
 *   single IPv6 host  2001:db8::1
 *   IPv4 CIDR         192.168.0.0/24
 *   IPv6 CIDR         2001:db8::/48
 *   hostname wildcard *.lab.example.com
 *   anonymous         *
 *
 * SB_MAX_CLIENT_RULES and SB_CLIENT_MATCH_MAX are defined in settings.h
 * (included above).
 */

struct sb_client_rule {
	char scr_match[SB_CLIENT_MATCH_MAX];
	bool scr_rw; /* false = read-only */
	bool scr_root_squash;
	bool scr_all_squash;
	enum reffs_auth_flavor scr_flavors[REFFS_CONFIG_MAX_FLAVORS];
	unsigned int scr_nflavors;
};

#define SUPER_BLOCK_ROOT_ID (1)
#define SUPER_BLOCK_DS_ID (2)

/*
 * LRU limits -- tunable, but these are reasonable defaults.
 * Eviction kicks in when the respective count exceeds the max.
 */
#define SB_INODE_LRU_MAX_DEFAULT (1024 * 64)
#define SB_DIRENT_LRU_MAX_DEFAULT (1024 * 256)

struct super_block {
	struct rcu_head sb_rcu;
	struct urcu_ref sb_ref;
	struct cds_list_head sb_link; /* List of sbs */
	struct cds_lfht *sb_inodes;

	struct reffs_dirent *sb_dirent;
	struct inode
		*sb_root_inode; /* permanent i_active pin; dropped at unmount */
	uint64_t sb_id;

	/*
	 * Listener scope.  0 = native namespace (served on :2049, the
	 * default for every SB created before the proxy-server feature
	 * landed).  1+ = a proxy namespace owned by a [[proxy_mds]]
	 * config entry.  sb_id values are scoped per listener --
	 * (sb_id, sb_listener_id) is the identity pair.  NFS compound
	 * dispatch uses super_block_find_for_listener() to enforce this
	 * scope; probe/admin paths continue to use the unscoped
	 * super_block_find().
	 */
	uint32_t sb_listener_id;

	uint64_t sb_next_ino;

	char *sb_path;

	enum reffs_storage_type sb_storage_type;
	char *sb_backend_path;

	const struct reffs_storage_ops *sb_ops;
	void *sb_storage_private;

	uuid_t sb_uuid;

	size_t sb_bytes_max;
	_Atomic size_t sb_bytes_used;

	size_t sb_inodes_max;
	_Atomic size_t sb_inodes_used;

	uint64_t sb_delayed_count;

	size_t sb_block_size;

	/* Inode LRU -- inodes with i_active == 0 live here */
	struct cds_list_head sb_inode_lru;
	pthread_mutex_t sb_inode_lru_lock;
	size_t sb_inode_lru_count;
	size_t sb_inode_lru_max;

	/* Dirent LRU -- dirents with rd_active == 0 and no children live here */
	struct cds_list_head sb_dirent_lru;
	pthread_mutex_t sb_dirent_lru_lock;
	size_t sb_dirent_lru_count;
	size_t sb_dirent_lru_max;

	/* Lifecycle state machine. */
	enum sb_lifecycle {
		SB_CREATED = 0,
		SB_MOUNTED = 1,
		SB_UNMOUNTED = 2,
		SB_DESTROYED = 3,
	} sb_lifecycle;

	/*
	 * Mount-point tracking.  When this sb is mounted:
	 * - sb_mount_dirent: the dirent in the parent sb that has
	 *   RD_MOUNTED_ON set (the "covered" directory)
	 * - sb_parent_sb: the parent superblock (non-owning ref)
	 *
	 * Used by LOOKUPP to cross back to the parent sb, and by
	 * READDIR to substitute attributes at mount points.
	 * Set by super_block_mount(), cleared by super_block_unmount().
	 */
	struct reffs_dirent *sb_mount_dirent;
	struct super_block *sb_parent_sb;

	/*
	 * Per-client export policy.  Ordered list of rules; the first
	 * matching rule wins.  sb_all_flavors is the union of all
	 * scr_flavors across all rules, used for SECINFO responses.
	 * Updated by super_block_set_client_rules() whenever rules change.
	 *
	 * If sb_nclient_rules == 0 the export denies all connections.
	 */
	struct sb_client_rule sb_client_rules[SB_MAX_CLIENT_RULES];
	unsigned int sb_nclient_rules;
	enum reffs_auth_flavor sb_all_flavors[REFFS_CONFIG_MAX_FLAVORS];
	unsigned int sb_nall_flavors;

#define SB_IN_LIST (1ULL << 0)
#define SB_IS_READ_ONLY (1ULL << 1)
	uint64_t sb_state;

	/*
	 * Per-export pNFS layout policy.  Bitmask of layout types
	 * this export serves.  0 = no layouts (standalone).
	 * LAYOUTGET returns NFS4ERR_LAYOUTUNAVAILABLE for exports
	 * without the requested layout type.
	 */
#define SB_LAYOUT_FILE (1U << 0) /* LAYOUT4_NFSV4_1_FILES */
#define SB_LAYOUT_FLEX_FILES (1U << 1) /* LAYOUT4_FLEX_FILES */
#define SB_LAYOUT_FLEX_FILES_V2 (1U << 2) /* LAYOUT4_FLEX_FILES_V2 */
	uint32_t sb_layout_types;

	/*
	 * Per-export dstore binding.  LAYOUTGET uses these dstore IDs
	 * instead of the global pool when sb_ndstores > 0.
	 * Set via probe sb-set-dstores, persisted in the registry.
	 */
#define SB_MAX_DSTORES 16
	uint32_t sb_dstore_ids[SB_MAX_DSTORES];
	uint32_t sb_ndstores;
	/*
	 * FFv1 stripe unit in bytes.  0 = whole-file CSM (all mirrors
	 * hold the same data).  Non-zero = RAID-0: client writes stripe i
	 * to mirror i % ffl_mirrors_len.  Set via probe sb-set-stripe-unit.
	 */
	uint32_t sb_stripe_unit;

	/* Per-op NFS4 statistics -- superblock scope. */
	struct reffs_op_stats sb_nfs4_op_stats[REFFS_NFS4_OP_MAX];

	/* Backend I/O statistics for this superblock. */
	struct reffs_backend_stats sb_backend_stats;

	/* Per-sb layout error statistics. */
	struct reffs_layout_error_stats sb_layout_errors;
};

struct super_block *super_block_alloc(uint64_t id, char *path,
				      enum reffs_storage_type type,
				      const char *backend_path);
struct super_block *super_block_find(uint64_t id);

/*
 * Scope-respecting lookup used by the NFS compound dispatch path.
 * Returns an SB only when both sb_id and sb_listener_id match.  An
 * FH minted on one listener and presented on another will therefore
 * miss (the caller returns NFS4ERR_STALE).  The unscoped
 * super_block_find() is retained for admin/probe paths that
 * legitimately need to enumerate SBs across listeners.
 */
struct super_block *super_block_find_for_listener(uint64_t id,
						  uint32_t listener_id);

struct super_block *super_block_get(struct super_block *sb);
void super_block_put(struct super_block *sb);

int super_block_dirent_create(struct super_block *sb, struct reffs_dirent *de,
			      enum reffs_life_action rla);
void super_block_dirent_release(struct super_block *sb,
				enum reffs_life_action rla);

struct cds_list_head *super_block_list_head(void);

void super_block_release_dirents(struct super_block *sb);

/* Evict up to 'count' idle inodes / dirents from the LRU. */
void super_block_evict_inodes(struct super_block *sb, size_t count);
void super_block_evict_dirents(struct super_block *sb, size_t count);
void super_block_drain(struct super_block *sb);

/*
 * Superblock lifecycle state machine.
 * See .claude/design/multi-superblock.md for full state diagram.
 *
 * Returns 0 on success, -errno on invalid transition.
 */
int super_block_mount(struct super_block *sb, const char *path);
int super_block_unmount(struct super_block *sb);
int super_block_destroy(struct super_block *sb);

/* Query lifecycle state. */
enum sb_lifecycle super_block_lifecycle(const struct super_block *sb);
const char *super_block_lifecycle_name(enum sb_lifecycle state);

/*
 * Find the child sb mounted on the given dirent.
 * Returns a ref-held sb, or NULL if no sb is mounted there.
 * Caller must super_block_put() when done.
 */
struct super_block *super_block_find_mounted_on(struct reffs_dirent *de);

/*
 * Set per-client export policy rules.  Copies the rule array into
 * sb->sb_client_rules and recomputes sb->sb_all_flavors.
 * Replaces any existing rules.
 */
void super_block_set_client_rules(struct super_block *sb,
				  const struct sb_client_rule *rules,
				  unsigned int nrules);

/*
 * Compatibility shim: synthesize a single "*" catch-all rule with the
 * given flavor list, root_squash=true, rw=true.  No new call sites --
 * kept only for the SB_SET_FLAVORS probe op.
 * NOT_NOW_BROWN_COW: remove after probe op SB_SET_CLIENT_RULES is wired in.
 */
void super_block_set_flavors(struct super_block *sb,
			     const enum reffs_auth_flavor *flavors,
			     unsigned int nflavors);

/*
 * Lint flavor consistency across the sb tree.
 * Warns when a child sb requires a flavor that no ancestor supports.
 * Returns the number of warnings (>= 0).
 */
int super_block_lint_flavors(void);

/*
 * Check for path conflicts with existing mounted superblocks.
 * Returns 0 if no conflict, -EEXIST if the path is already mounted,
 * -EBUSY if the path is a parent of an existing mount.
 */
int super_block_check_path_conflict(const char *path);

#endif /* _REFFS_SB_H */
