/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * In-flight proxy migration record table -- slice 6c-x.2.
 *
 * The MDS records one migration_record per active PROXY_OP_MOVE /
 * PROXY_OP_REPAIR assignment delivered to a registered PS.  The
 * record is the persisted (in-memory only in slice 6c-x; on-disk
 * in slice 6c-zz) state that PROXY_DONE / PROXY_CANCEL act on, and
 * that the LAYOUTGET view-build path consults to compute the
 * during-migration view of the file's layout.
 *
 * Two indices:
 *   1. by proxy_stateid.other[12] -- for PROXY_DONE / PROXY_CANCEL
 *      O(1) lookup from a PS-supplied proxy_stateid (slice 6c-x.3)
 *   2. by inode pointer            -- for the LAYOUTGET view-build
 *      path (slice 6c-x.4) and the constraint that an inode has at
 *      most one in-flight migration at a time
 *
 * Both indices are `cds_lfht`.  Rule 6 (patterns/ref-counting.md)
 * governs entry lifecycle, with the dual-index dance noted in the
 * design doc revision section "RCU + Rule 6 discipline for
 * migration_record table".
 *
 * Slice 6c-x.2 ships only the table primitives + reaper; per-instance
 * delta machinery is captured here as the `mr_deltas` field but
 * actual delta application is wired up in slice 6c-x.4 (LAYOUTGET
 * view-build) and 6c-x.5 (CB_LAYOUTRECALL on commit).
 */

#ifndef _REFFS_NFS4_MIGRATION_RECORD_H
#define _REFFS_NFS4_MIGRATION_RECORD_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <urcu/rculfhash.h>
#include <urcu/ref.h>
#include <urcu/call-rcu.h>

#include "nfsv42_xdr.h"
#include "reffs/layout_segment.h"

/*
 * Mirrors REFFS_CONFIG_MAX_PRINCIPAL / _TLS_FINGERPRINT /
 * PROXY_REGISTRATION_ID_MAX from settings.h + nfsv42_xdr -- the
 * record copies whichever variant the caller's session captured at
 * PROXY_REGISTRATION time.  Sized to the largest of the three so a
 * single buffer accommodates any identity rank.
 */
#define MIGRATION_OWNER_REG_MAX 256

/*
 * Migration phase.  Single-writer transitions during the slice's
 * lifetime: PENDING -> IN_PROGRESS -> { COMMITTED, ABANDONED }.
 *
 * COMMITTED and ABANDONED are sticky terminal phases -- once entered,
 * the record is immediately unhashed and the creation ref dropped.
 * A reader that holds a find ref past the phase transition can still
 * observe the final phase value before the RCU-deferred free.
 *
 * _Atomic because the LAYOUTGET view-build path (slice 6c-x.4) reads
 * mr_phase concurrently with the DONE / CANCEL handlers and the
 * reaper.
 */
enum migration_phase {
	MIGRATION_PHASE_PENDING = 0,
	MIGRATION_PHASE_IN_PROGRESS = 1,
	MIGRATION_PHASE_COMMITTED = 2,
	MIGRATION_PHASE_ABANDONED = 3,
};

/*
 * Per-instance delta describing one transformation on one mirror /
 * shard position within one segment of i_layout_segments.  The
 * delta machinery itself is wired up in slice 6c-x.4 (the
 * LAYOUTGET view-build path applies deltas to the base segments to
 * compute the during-migration view); this slice carries the deltas
 * as opaque payload on the record so 6c-x.4 has the array shape
 * already in hand.
 *
 * `ld_state` semantics:
 *   STABLE     -- unchanged baseline (deltas don't normally carry
 *                 STABLE entries; included for completeness)
 *   DRAINING   -- slot being decommissioned; LAYOUTGET omits this
 *                 slot under omit-and-replace policy and replaces
 *                 it with the matching INCOMING slot
 *   INCOMING   -- new slot the PS is filling; emitted in place of
 *                 the matching DRAINING under omit-and-replace
 *   INTERPOSED -- slot whose visible endpoint is the PS, with the
 *                 PS internally fanning writes to one or more
 *                 target DSes.  Used by keep-and-shadow (deferred
 *                 to a future slice when PS-as-DS plumbing exists);
 *                 not emitted by 6c-x autopilot paths.
 */
enum migration_instance_state {
	MIGRATION_INSTANCE_STABLE = 0,
	MIGRATION_INSTANCE_DRAINING = 1,
	MIGRATION_INSTANCE_INCOMING = 2,
	MIGRATION_INSTANCE_INTERPOSED = 3,
};

struct migration_instance_delta {
	uint32_t mid_seg_index; /* index in inode->i_layout_segments */
	uint32_t mid_instance_index; /* index in segment's ls_files */
	enum migration_instance_state mid_state;
	/*
	 * For DRAINING: cross-reference into the parent record's
	 * deltas[] array of the matching INCOMING delta that shadows
	 * this slot.  UINT32_MAX when unused (record builder never
	 * paired this DRAINING with an INCOMING -- e.g., a pure
	 * reduction).
	 */
	uint32_t mid_replacement_delta_idx;
	/*
	 * For INCOMING: the new layout_data_file the LAYOUTGET
	 * view-build path inserts when computing the during-migration
	 * view (slice 6c-x.4).  Built by the migration record's
	 * creator (slice 6c-y autopilot).  Unused for DRAINING /
	 * STABLE / INTERPOSED -- zero-init is fine.
	 */
	struct layout_data_file mid_replacement_file;
};

/*
 * Migration record.  Fields documented inline; see
 * .claude/design/proxy-server-phase6c-revision.md "Authorization"
 * and "State-machine completeness" for the normative contract.
 */
struct migration_record {
	/*
	 * MUST be first -- container_of relies on this for the
	 * stateid-keyed hash table.  cds_lfht_node has no embedded
	 * type tag, so the placement is the only way the hash callback
	 * recovers the enclosing struct.
	 */
	struct cds_lfht_node mr_stid_node;
	/* Second hash node -- inode-keyed index. */
	struct cds_lfht_node mr_ino_node;

	struct rcu_head mr_rcu;
	struct urcu_ref mr_ref;

	/* Identifying handles. */
	uint8_t mr_stateid_other[NFS4_OTHER_SIZE];
	uint64_t mr_ino;
	struct super_block *mr_sb; /* sb pointer, captured at register time */

	/*
	 * Owner identity (registered-PS canonical principal -- selection
	 * order matches nfs4_client_registered_ps_identity in nfs4/client.h).
	 * Bytes copied at register time; NOT a pointer to the client's
	 * field, because the client may be reaped while the record is
	 * still active (see slice 6c-x.0 review note N2).
	 */
	char mr_owner_reg[MIGRATION_OWNER_REG_MAX];
	uint32_t mr_owner_reg_len; /* registration_id length; or strlen()
				    * for principal/fingerprint */

	/*
	 * Most recently issued seqid for this proxy_stateid.  Bumped
	 * on every renewal (slice 6c-y / 6c-z); compared against the
	 * caller's pd_stateid.seqid in the PROXY_DONE / PROXY_CANCEL
	 * priority-ordered authorization rule (-> NFS4ERR_OLD_STATEID
	 * on mismatch, per RFC 8881 S8.2.4).
	 *
	 * _Atomic uint32_t because the renewal path writes from the
	 * PROXY_PROGRESS handler thread while the DONE / CANCEL
	 * handlers read concurrently.  Initialized to the stateid's
	 * minted seqid (1 from proxy_stateid_alloc).
	 */
	_Atomic uint32_t mr_seqid;

	/* Lifecycle / liveness. */
	_Atomic enum migration_phase mr_phase;
	/*
	 * CLOCK_MONOTONIC ns of last PROXY_PROGRESS heartbeat from the
	 * owning PS; the reaper uses this to detect lease expiry
	 * (1.5x lease period of silence -> ABANDONED).  Two-clock
	 * pattern from .claude/design/trust-stateid.md.  _Atomic so the
	 * renewal path can update without locking.
	 */
	_Atomic uint64_t mr_last_progress_mono_ns;

	/*
	 * Per-instance deltas.  Ownership: the record OWNS the array
	 * (allocated at register, freed at release).  Array length is
	 * mr_ndeltas; entries are immutable after the record is hashed
	 * (per design-doc invariant 3 -- record-replacement, not
	 * in-place delta mutation, encodes any state change).
	 */
	uint32_t mr_ndeltas;
	struct migration_instance_delta *mr_deltas;
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/*
 * migration_record_init -- allocate the global hash tables and
 * start the lease-expiry reaper thread.  Called at server startup.
 *
 * Returns 0 on success, negative errno on failure.
 */
int migration_record_init(void);

/*
 * migration_record_fini -- drain both hash tables, stop the reaper,
 * free all resources.  Called at server shutdown.  Idempotent.
 */
void migration_record_fini(void);

/* ------------------------------------------------------------------ */
/* Mutation                                                            */
/* ------------------------------------------------------------------ */

/*
 * migration_record_create -- allocate, populate, and hash a new
 * migration record.
 *
 * Caller passes:
 *   - The proxy_stateid the MDS just minted (slice 6c-x.1 alloc
 *     primitives produce this; slice 6c-y's PROXY_PROGRESS reply
 *     builder threads it through).
 *   - The inode the migration applies to (single record per inode;
 *     a second create that targets an inode with an active record
 *     returns -EBUSY without replacing the prior record).
 *   - The owning registered-PS identity bytes + length (typically
 *     supplied via nfs4_client_registered_ps_identity()).  The
 *     record copies the bytes; the caller may free its source.
 *   - A pre-built array of per-instance deltas; the record takes
 *     ownership of the array (frees at release).  Pass NULL +
 *     ndeltas=0 for an empty migration (rare; mainly tests).
 *   - The initial heartbeat timestamp (CLOCK_MONOTONIC ns).
 *
 * Returns 0 on success.  Errors:
 *   -EINVAL if owner_len exceeds MIGRATION_OWNER_REG_MAX, or any
 *           required pointer is NULL
 *   -EBUSY  if the inode already has an active record
 *   -ENOMEM on allocation failure
 *
 * On any error path the caller's `deltas` array is NOT freed --
 * the caller retains ownership when create fails.  This matches
 * the convention in lib/fs/super_block.c (caller cleans up its
 * own input on construction failure).
 */
int migration_record_create(const stateid4 *stid, struct super_block *sb,
			    uint64_t ino, const char *owner_reg,
			    uint32_t owner_reg_len,
			    struct migration_instance_delta *deltas,
			    uint32_t ndeltas, uint64_t initial_progress_mono_ns,
			    struct migration_record **out_mr);

/*
 * migration_record_renew -- update an existing record's
 * mr_last_progress_mono_ns to extend its lease.
 *
 * Looked up by proxy_stateid.other; not-found returns -ENOENT
 * (ABANDONED records have already been unhashed by the reaper, so
 * this is the right error code -- the PS will see it surface as
 * NFS4ERR_BAD_STATEID via the standard PROXY_PROGRESS error path).
 *
 * Thread-safe; uses an atomic store on mr_last_progress_mono_ns.
 */
int migration_record_renew(const stateid4 *stid, uint64_t now_mono_ns);

/*
 * migration_record_unhash -- explicit removal from both indices.
 * Idempotent; calling on a record already removed is a no-op.
 *
 * The function only removes from the hash tables; it does NOT drop
 * the creation ref.  Call migration_record_put() afterward to
 * complete the destroy.
 *
 * Used by:
 *   - migration_record_commit  (PROXY_DONE(NFS4_OK) handler)
 *   - migration_record_abandon (PROXY_DONE(FAIL), PROXY_CANCEL,
 *                               reaper expiry)
 */
void migration_record_unhash(struct migration_record *mr);

/*
 * migration_record_commit -- transition phase to COMMITTED, unhash,
 * and signal the LAYOUTGET view-build path to flush the
 * during-migration view.  The caller (PROXY_DONE handler)
 * subsequently applies the deltas to the inode's i_layout_segments
 * (slice 6c-x.4) and issues CB_LAYOUTRECALL for affected clients
 * (slice 6c-x.5).
 *
 * Returns 0 on success, -EALREADY if the record is no longer in
 * a committable phase (already COMMITTED or ABANDONED).
 */
int migration_record_commit(struct migration_record *mr);

/*
 * migration_record_abandon -- transition phase to ABANDONED, unhash,
 * and discard the deltas without applying them.  Used by:
 *   - PROXY_DONE(non-OK) handler (rollback)
 *   - PROXY_CANCEL handler
 *   - the lease-expiry reaper
 *
 * Returns 0 on success, -EALREADY if the record is no longer in
 * an abandonable phase.
 */
int migration_record_abandon(struct migration_record *mr);

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

/*
 * migration_record_find_by_stateid -- O(1) lookup by
 * proxy_stateid.other.  Returns a ref-bumped record, or NULL when
 * no record exists for this stateid.
 *
 * Caller MUST drop the find ref via migration_record_put() when
 * done.  Records in terminal phases (COMMITTED / ABANDONED) are
 * NOT returned -- the unhash step in commit / abandon removes them
 * from this index before transitioning the phase atomic.
 */
struct migration_record *migration_record_find_by_stateid(const stateid4 *stid);

/*
 * migration_record_find_by_inode -- lookup by inode pointer.
 * Returns a ref-bumped record, or NULL when the inode has no
 * active migration.  Caller MUST drop via migration_record_put().
 *
 * Used by:
 *   - LAYOUTGET (slice 6c-x.4) to apply deltas before encoding
 *   - migration_record_create's invariant check (single in-flight
 *     migration per inode)
 */
struct migration_record *migration_record_find_by_inode(uint64_t ino);

/*
 * migration_record_put -- drop a reference.  Drives the record
 * through the Rule 6 release callback (cds_lfht_del on both
 * indices, then call_rcu free) when the refcount reaches zero.
 */
void migration_record_put(struct migration_record *mr);

/* ------------------------------------------------------------------ */
/* Reaper                                                              */
/* ------------------------------------------------------------------ */

/*
 * migration_record_reaper_scan -- one-shot pass over the table,
 * abandoning any record whose mr_last_progress_mono_ns has aged
 * past `max_silence_ns` ago.
 *
 * Public so unit tests can drive expiry deterministically (the
 * production reaper thread calls this on its own cadence).
 */
void migration_record_reaper_scan(uint64_t max_silence_ns,
				  uint64_t now_mono_ns);

/* ------------------------------------------------------------------ */
/* Slice 6c-x.4: layout-build "during-migration view"                  */
/* ------------------------------------------------------------------ */

/*
 * migration_apply_deltas_to_segment -- compute the during-migration
 * view of `base_seg` per the omit-and-replace policy.
 *
 * For each instance position in base_seg->ls_files:
 *   - If a DRAINING delta in `mr` matches (base_seg_index, i),
 *     omit this position from the view.
 *   - Otherwise copy the base entry to the view.
 * Then, for each INCOMING delta in `mr` matching base_seg_index,
 * append `mid_replacement_file` to the view.
 *
 * STABLE deltas are no-ops at this scope (they describe the
 * baseline; LAYOUTGET emits the base entry unchanged whether the
 * record carries a STABLE delta for the slot or not).
 *
 * INTERPOSED deltas are NOT consumed -- they require PS-as-DS
 * plumbing that is out of scope for slice 6c-x.  An INTERPOSED
 * delta in the record is silently passed through as a STABLE-
 * equivalent (the base entry stays); record builders in this
 * slice MUST NOT emit INTERPOSED.
 *
 * Out parameters (caller responsibilities):
 *   - `*out_view` is populated with the view's scalar fields and
 *     a freshly-malloc'd `ls_files` array of length `out_view->ls_nfiles`.
 *   - The caller MUST call migration_release_view(out_view) once
 *     it has finished encoding the layout body.
 *
 * Returns 0 on success, -ENOMEM on allocation failure.
 *
 * Thread-safe: reads only immutable record fields (deltas are
 * frozen after the record is hashed per design-doc invariant 3).
 * No RCU section is taken; the caller's existing record-find ref
 * keeps the record alive for the duration of this call.
 */
int migration_apply_deltas_to_segment(const struct layout_segment *base_seg,
				      uint32_t base_seg_index,
				      const struct migration_record *mr,
				      struct layout_segment *out_view);

/*
 * migration_release_view -- free the view's `ls_files` array.
 * Idempotent on a zero-initialized view.  Does NOT touch
 * `view->ls_files` if it is NULL.
 */
void migration_release_view(struct layout_segment *view);

/* ------------------------------------------------------------------ */
/* Slice 6c-x.5: post-commit recall on DRAINING removal                */
/* ------------------------------------------------------------------ */

/*
 * Forward declarations -- avoids pulling the full inode / client /
 * server_state headers into the migration record header.
 */
struct inode;
struct client;
struct server_state;

/*
 * migration_recall_layouts -- queue CB_LAYOUTRECALL on every
 * external layout outstanding for `inode`, except those held by
 * `exclude_client` (the registered PS that just committed the
 * migration; that client already acknowledged the post-image via
 * its own LAYOUTRETURN inside the same compound).
 *
 * Fire-and-forget: each recall uses nfs4_cb_layoutrecall_fnf and
 * does not wait for the client's ack.  The caller (PROXY_DONE
 * handler) returns NFS4_OK as soon as the queueing is done; the
 * lease reaper handles any client whose CB back-channel is broken.
 *
 * Returns the number of CB_LAYOUTRECALLs queued (0 if no external
 * layouts exist on this inode, or if `inode` is NULL).
 *
 * No-op if the inode has no stateids hashed (i_stateids == NULL)
 * or has only stateids of types other than Layout_Stateid.  Skips
 * any layout stateid whose s_client is NULL or matches
 * `exclude_client`.
 */
unsigned int migration_recall_layouts(struct inode *inode,
				      struct client *exclude_client,
				      struct server_state *ss);

#endif /* _REFFS_NFS4_MIGRATION_RECORD_H */
