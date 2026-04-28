/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Persistent format for proxy-server migration records (slice 6c-zz).
 *
 * This header lives in lib/include/reffs/ (not lib/nfs4/include/) so
 * the backends layer (lib/backends/flatfile_persist.c) can see the
 * struct shape without depending on lib/nfs4 -- backends layer must
 * not pull in nfs4 (one-way dependency rule, .claude/standards.md).
 *
 * The matching in-memory record (struct migration_record) and the
 * runtime delta state enums live in lib/nfs4/include/nfs4/migration_record.h;
 * lib/nfs4/include/nfs4/migration_record.h re-uses these typedefs
 * verbatim.
 */

#ifndef _REFFS_MIGRATION_PERSIST_H
#define _REFFS_MIGRATION_PERSIST_H

#include <stdint.h>

/*
 * Mirrors the constants in lib/nfs4/include/nfs4/migration_record.h.
 * Kept in sync by hand; a future cleanup may merge the two headers.
 */
#define MR_PERSIST_OWNER_REG_MAX 256
#define MR_PERSIST_FH_MAX 128
#define MR_PERSIST_NFS4_OTHER_SIZE 12

#define MR_PERSIST_MAX_DELTAS 16

enum mr_persist_instance_state {
	MR_PERSIST_INSTANCE_STABLE = 0,
	MR_PERSIST_INSTANCE_DRAINING = 1,
	MR_PERSIST_INSTANCE_INCOMING = 2,
	MR_PERSIST_INSTANCE_INTERPOSED = 3,
};

/*
 * Persistent layout_data_file -- mirror of the in-memory shape but
 * shrunk to just the fields that survive restart.  Times / size /
 * stale flag are recomputed from the live DS state on first
 * GETATTR after reload, so they are not persisted.
 */
struct mr_persist_layout_data_file {
	uint32_t pldf_dstore_id;
	uint32_t pldf_fh_len;
	uint8_t pldf_fh[MR_PERSIST_FH_MAX];
};

struct mr_persist_instance_delta {
	uint32_t pmid_seg_index;
	uint32_t pmid_instance_index;
	uint32_t pmid_state; /* enum mr_persist_instance_state */
	uint32_t pmid_replacement_delta_idx;
	struct mr_persist_layout_data_file pmid_replacement_file;
};

/*
 * Persistent migration record.  Fixed size; the flat-file backend
 * appends one of these per save and strides over the file at load.
 *
 * Wire-stable layout: append-only.  No version field per CLAUDE.md
 * "No persistent storage has been deployed" -- pre-deployment so
 * format changes are free until first ship.  When persistence ships
 * for production, add a leading version word and gate compatibility.
 */
struct migration_record_persistent {
	uint8_t mrp_stateid_other[MR_PERSIST_NFS4_OTHER_SIZE];
	uint32_t mrp_seqid;
	uint32_t mrp_pad0; /* explicit padding so mrp_sb_id is 8-byte aligned */
	uint64_t mrp_sb_id;
	uint64_t mrp_ino;
	uint32_t mrp_owner_reg_len;
	uint32_t mrp_pad1;
	uint8_t mrp_owner_reg[MR_PERSIST_OWNER_REG_MAX];
	uint32_t mrp_ndeltas;
	uint32_t mrp_pad2;
	struct mr_persist_instance_delta mrp_deltas[MR_PERSIST_MAX_DELTAS];
};

#endif /* _REFFS_MIGRATION_PERSIST_H */
