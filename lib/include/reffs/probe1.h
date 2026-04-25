/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PROBE1_H
#define _REFFS_PROBE1_H

#include "reffs/rpc.h"

/* probe_auth_flavor1 is defined in generated probe1_xdr.h.
 * Use uint32_t here to avoid pulling in the full XDR header. */

int probe1_protocol_deregister(void);
int probe1_protocol_register(void);

struct rpc_trans *probe1_client_op_null(void);
struct rpc_trans *probe1_client_op_context(void);
struct rpc_trans *probe1_client_op_stats_gather(uint32_t program,
						uint32_t vers);
struct rpc_trans *probe1_client_op_fs_usage(bool human, const char *path);
struct rpc_trans *probe1_client_op_nfs4_op_stats(void);

/* Superblock management ops. */
struct rpc_trans *probe1_client_op_sb_list(void);
struct rpc_trans *probe1_client_op_sb_create(const char *path,
					     uint32_t storage_type);
struct rpc_trans *probe1_client_op_sb_mount(uint64_t id, const char *path);
struct rpc_trans *probe1_client_op_sb_unmount(uint64_t id);
struct rpc_trans *probe1_client_op_sb_destroy(uint64_t id);
struct rpc_trans *probe1_client_op_sb_get(uint64_t id);
struct rpc_trans *probe1_client_op_sb_set_flavors(uint64_t id,
						  uint32_t *flavors,
						  uint32_t nflavors);
struct rpc_trans *probe1_client_op_sb_lint_flavors(void);

struct sb_client_rule;
struct rpc_trans *probe1_client_op_sb_set_client_rules(
	uint64_t id, const struct sb_client_rule *rules, unsigned int nrules);

struct rpc_trans *probe1_client_op_sb_set_dstores(uint64_t id,
						  const uint32_t *dstore_ids,
						  uint32_t ndstores);
struct rpc_trans *probe1_client_op_sb_set_stripe_unit(uint64_t id,
						      uint32_t stripe_unit);

struct rpc_trans *probe1_client_op_inode_layout_list(uint64_t sb_id,
						     uint64_t inum);

#endif
