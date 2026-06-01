/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Per-op NFS4 statistics, backend I/O statistics, and callback statistics.
 *
 * Three scopes are tracked for NFS4 ops:
 *   - Global  (server_state.ss_nfs4_op_stats[])  -- probe1 source
 *   - Per-superblock (super_block.sb_nfs4_op_stats[])
 *   - Per-client (nfs4_client.n4c_op_stats[])    -- ephemeral, lost on disconnect
 *
 * No per-scope HDR histograms -- too costly at per-client/per-sb granularity.
 * The existing COMPOUND-level rpc_stats histogram covers latency distribution.
 */

#ifndef _REFFS_NFS4_STATS_H
#define _REFFS_NFS4_STATS_H

#include <stdatomic.h>
#include <stdint.h>

/*
 * REFFS_NFS4_OP_MAX -- upper bound on NFS4 op codes, kept as a plain
 * constant so this header is usable by files that cannot include the
 * generated nfsv42_xdr.h (e.g. backends, utils).
 *
 * OP_MAX is defined in nfsv42_names.h as (highest op code + 1).
 * Highest assigned op is OP_PROXY_CANCEL = 95, so OP_MAX is
 * currently 96.  We use 104 for a small margin.  A _Static_assert
 * in ops.c enforces that OP_MAX never exceeds this value.
 */
#define REFFS_NFS4_OP_MAX 104

/* ------------------------------------------------------------------ */
/* NFS4 per-op stats                                                   */
/* ------------------------------------------------------------------ */

struct reffs_op_stats {
	_Atomic uint64_t os_calls;
	_Atomic uint64_t os_errors; /* results != NFS4_OK */
	_Atomic uint64_t os_bytes_in; /* payload in  (WRITE, SETXATTR, ...) */
	_Atomic uint64_t os_bytes_out; /* payload out (READ, GETATTR, ...) */
	_Atomic uint64_t os_duration_total; /* nanoseconds, cumulative */
	_Atomic uint64_t os_duration_max; /* nanoseconds, high-water */
};

/*
 * nfs4_op_stats_record - update all three scopes atomically.
 *
 * global/sb/client may each be NULL (e.g. before SEQUENCE sets c_nfs4_client).
 * op must be < OP_MAX.  elapsed_ns is the wall-clock duration of the op.
 * bytes_in / bytes_out are 0 for non-data ops.
 */
void nfs4_op_stats_record(struct reffs_op_stats global[REFFS_NFS4_OP_MAX],
			  struct reffs_op_stats sb[REFFS_NFS4_OP_MAX],
			  struct reffs_op_stats client[REFFS_NFS4_OP_MAX],
			  uint32_t op, uint32_t nfs4_status,
			  uint64_t elapsed_ns, uint64_t bytes_in,
			  uint64_t bytes_out);

/* ------------------------------------------------------------------ */
/* Backend I/O stats (per super_block + aggregate in server_state)     */
/* ------------------------------------------------------------------ */

struct reffs_backend_stats {
	_Atomic uint64_t bs_read_ops;
	_Atomic uint64_t bs_write_ops;
	_Atomic uint64_t bs_bytes_read;
	_Atomic uint64_t bs_bytes_written;
	_Atomic uint64_t bs_errors;
	_Atomic uint64_t bs_duration_total; /* nanoseconds, cumulative */
	_Atomic uint64_t bs_duration_max; /* nanoseconds, high-water */
};

/* ------------------------------------------------------------------ */
/* Callback / remote DS stats (per nfs4_client, future ds_peer)        */
/* ------------------------------------------------------------------ */

/*
 * CB op codes top out around 15 today; 32 slots gives comfortable headroom
 * for future CB ops without a large fixed cost.
 */
#define REFFS_CB_OP_MAX 32

struct reffs_cb_stats {
	_Atomic uint64_t cbs_calls;
	_Atomic uint64_t cbs_errors;
	_Atomic uint64_t cbs_duration_total; /* nanoseconds, cumulative */
	_Atomic uint64_t cbs_duration_max; /* nanoseconds, high-water */
};

/* ------------------------------------------------------------------ */
/* Layout error stats (global, per-dstore, per-client)                 */
/* ------------------------------------------------------------------ */

struct reffs_layout_error_stats {
	_Atomic uint64_t les_total; /* all LAYOUTERROR reports */
	_Atomic uint64_t les_access; /* NFS4ERR_ACCESS / NFS4ERR_PERM */
	_Atomic uint64_t les_io; /* NFS4ERR_IO */
	_Atomic uint64_t les_other; /* everything else */
};

/* ------------------------------------------------------------------ */
/* Chunk activity stats (per-sb)                                       */
/*                                                                     */
/* Diagnostic counters for chunk-collision validation                  */
/* (.claude/design/chunk-collision-validation.md, BLOCKER 2).  The     */
/* harness reads these before/after a sweep and asserts on the deltas. */
/* ------------------------------------------------------------------ */

struct reffs_chunk_stats {
	_Atomic uint64_t cs_writes; /* total CHUNK_WRITE accepted */
	_Atomic uint64_t
		cs_pending_displaced; /* new PENDING overrode prior PENDING from a different owner */
	_Atomic uint64_t cs_finalize_crc_fail; /* FINALIZE saw CRC mismatch */
	_Atomic uint64_t
		cs_commit_crc_recompute; /* COMMIT recomputed CRC vs persisted */
	_Atomic uint64_t cs_rollback_invoked; /* CHUNK_ROLLBACK fired */
	_Atomic uint64_t cs_repair_initiated; /* repair entered (today: 0) */
	_Atomic uint64_t cs_fences_rotated; /* synthetic uid/gid bumps */

	/*
	 * INV-1 instrumentation -- partial-stripe write pattern on
	 * the DS.  Answers Hellwig msg 5 (in-place update semantics)
	 * + msg 9 (NFS block size) by quantifying what the DS
	 * actually sees during T1b (sub-chunk interleave) and T2
	 * (IOR shared-file through PSes) workloads.  See
	 * .claude/design/inv1-ds-instrumentation.md.  Relaxed
	 * memory order -- diagnostic counters, not synchronization.
	 */
	_Atomic uint64_t cs_blocks_full; /* len == chunk_size */
	_Atomic uint64_t cs_blocks_partial; /* 0 < len < chunk_size */
	_Atomic uint64_t cs_blocks_first_write; /* prev cb_state == EMPTY */
	_Atomic uint64_t cs_blocks_overwrite; /* prev cb_state != EMPTY */
	_Atomic uint64_t cs_writes_1block; /* cwa_chunks_len == 1 */
	_Atomic uint64_t cs_writes_2to7; /* 2..7 */
	_Atomic uint64_t cs_writes_8to31; /* 8..31 */
	_Atomic uint64_t cs_writes_32plus; /* >= 32 */
	/*
	 * Track 1b chunk-collision gate (Option C, design/chunk-collision-
	 * validation.md "Triage: chunk-store sub-stripe atomicity").  A
	 * CHUNK_WRITE arrived at a block that already holds a PENDING entry
	 * from a different writer; the server returned NFS4ERR_DELAY rather
	 * than silently last-write-wins.  Pairs with cs_pending_displaced
	 * (the same shape of contention BEFORE the gate landed -- the gate
	 * trades silent displacement for a visible retry signal).
	 */
	_Atomic uint64_t cs_chunk_busy_delay;
};

#endif /* _REFFS_NFS4_STATS_H */
