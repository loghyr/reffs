<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# EC Wire-Level Repair on reffs -- Implementation Plan

*Plan-agent output from 2026-06-09 session.  Drafted to support
the IETF 126 candidate-deck answer to `question-ais.txt` item 4
("cost of collisions and repair").  Scope is Tier 2 (wire-level
repair ops) per user directive after rejecting Tier 1
(client-side write-back via plain CHUNK_WRITE).*

*Status: PENDING -- seven Open Questions in section 12 need
author answers before Slice 1 starts.*

---

## 1. Scope summary

Three vertical slices implement wire-level erasure-coding
repair:

- **Slice 1**: DS-side `OP_CHUNK_WRITE_REPAIR` handler --
  currently a `NFS4ERR_NOTSUPP` stub at
  `lib/nfs4/server/chunk.c:1293` -- promoted to a real handler
  that validates the caller, writes shard bytes into the chunk
  store, and stamps repair-provenance bits on each touched
  block.
- **Slice 2**: MDS-side `OP_CHUNK_REPAIRED` handler -- currently
  the matching stub at `lib/nfs4/server/chunk.c:1021` -- promoted
  to a real handler that finds the affected per-mirror
  `layout_data_file`, clears `FFV2_DS_FLAGS_REPAIR` on it,
  persists the layout, and bumps a new
  `sb_chunk_stats.cs_repair_completed` counter.
- **Slice 3**: `ec_demo repair` subcommand which drives the new
  wire ops end-to-end.  Reads layout, reads all (k+m) shards,
  detects loss, decodes peer shards in memory to reconstruct,
  issues `OP_CHUNK_WRITE_REPAIR` to each affected DS, then
  `OP_CHUNK_REPAIRED` to the MDS.

**Persistence is in scope** (user directive 2026-06-09): the
`FFV2_DS_FLAGS_REPAIR` bit must survive an MDS restart, which
requires adding `ldf_flags` to `struct layout_data_file` +
`struct layout_data_file_disk` (neither currently has a flags
field; the layout encoder in `layout.c:815` synthesises the
wire flag positionally and would not survive an op that mutates
which mirrors are in repair).

**Out of scope** (deferred, flagged in section 12):

- `CB_PROXY_REPAIR` callback / PS-mediated repair
  (proxy-server draft Phase 8)
- MDS-autopilot detection of repair (MDS notices CRC-fail and
  issues repair on its own)
- Lost-DS file rebuild (R3 in `ec-repair-bench.md`)
- Bit-flip injection specifically (only missing-shard injection
  in scope)

## 2. State machine

The DS chunk-block state machine in `lib/nfs4/server/chunk.c`
+ `chunk_store.c` today is:

```
            CHUNK_WRITE                    CHUNK_FINALIZE                CHUNK_COMMIT
   EMPTY -----------------> PENDING --------------------> FINALIZED -------------------> COMMITTED
     ^                         |                            |                            |
     |                         |  CHUNK_ROLLBACK            |  CHUNK_ROLLBACK             |
     |                         v                            v                            |
     +---------------------  EMPTY                        EMPTY        (-ENOTSUP today) -+
```

Verified against code: `enum chunk_state` in
`chunk_store.h:31`; transitions in `chunk_store_transition`
(`chunk_store.c:204`); rollback degeneration in
`chunk_store_rollback` (`chunk_store.c:255-297`, where
`COMMITTED -> EMPTY` returns `-ENOTSUP` and the
COMMITTED-rollback NOT_NOW_BROWN_COW lives).

### Repair state machine: option chosen -- flag bit, not parallel state

Three options were considered:

| Option | What | Pros | Cons |
|--------|------|------|------|
| A | Parallel `REPAIR_PENDING/FINALIZED/COMMITTED` states | Clean isolation | Doubles every state check in chunk.c hot path; breaks every read-side `cb_state >= FINALIZED` test |
| B | Reuse existing states with a `CB_REPAIR_IN_PROGRESS` flag on `cb_flags` | Read path unchanged; FINALIZE/COMMIT walk works as-is | Race window where two writers (real client + repair client) both think they own the PENDING |
| **C** | **Reuse existing states; tag with `CHUNK_BLOCK_REPAIR_PROVENANCE` flag at WRITE_REPAIR time AND require trust-table stateid validation to gate concurrency** | **Same as B but the trust-table stateid + the FFV2_DS_FLAGS_REPAIR gate on the layout means a non-repair client cannot have a trusted stateid for the slot at the same time as the repair client; race is closed at the layout layer, not the chunk layer** | **Repair errors require the chunk to enter the new errored sub-state -- but that is `CHUNK_ERROR`, which is its own deferred op (`chunk.c:990` returns NOTSUPP). We do not need errored-state machinery in this slice because the repair path does not transition into errored -- it transitions OUT of errored. The pre-condition "block must be in errored state" the draft requires (line 8102) degenerates to "block must be in FFV2_DS_FLAGS_REPAIR-marked layout mirror" since we have no errored bit yet.** |

We pick **Option C**.  The chunk-block state machine is
unchanged.  Add `CHUNK_BLOCK_REPAIR_PROVENANCE = 0x2` to
`chunk_store.h:40` (sibling of the existing
`CHUNK_BLOCK_LOCKED`).  The repair-write path sets that flag
on each touched block; the bit is purely informational
(operator audit) and does not gate state transitions.

The flow for a single block on a DS is:

```
                                              +- flag CHUNK_BLOCK_REPAIR_PROVENANCE
                                              |  is set on the block (persists)
                                              v
   any state -- CHUNK_WRITE_REPAIR --> PENDING -- CHUNK_FINALIZE --> FINALIZED -- CHUNK_COMMIT --> COMMITTED
                       |
                       | Preconditions (all must hold):
                       |   - PUTFH set on current FH
                       |   - inode is regular file
                       |   - cwra_owner.co_guard.cg_client_id not in reserved set
                       |   - cwra_chunk_size > 0, cwra_chunks_len > 0
                       |   - per-chunk CRC validates (same as CHUNK_WRITE)
                       |   - stateid is non-special AND in DS trust table
                       |     (NEW: REPAIR-flag check via trust_entry; see Sec 4)
                       |   - per-file checksum_algorithm matches
                       +- Differences from CHUNK_WRITE:
                            - no cwa_guard (no chunk_guard4 CAS); bypasses the
                              Track 1b chunk-collision gate explicitly
                            - sets CHUNK_BLOCK_REPAIR_PROVENANCE on every block touched
                            - bumps sb_chunk_stats.cs_repair_initiated (already declared)
                            - if prior cb_state == COMMITTED with checksums that match
                              the new payload's checksums, return NFS4_OK per-chunk
                              (write was a no-op; matches draft "MAY reject" with the
                              looser "SHOULD accept idempotent")
```

The CHUNK_REPAIRED op on the MDS is **not** a chunk-state
transition on the MDS at all -- the MDS has no chunk store.
CHUNK_REPAIRED is a per-layout-mirror flag transition:
`FFV2_DS_FLAGS_REPAIR -> 0` for the affected
`layout_data_file`.  The MDS's existing layout state machine
(per-inode segments via `inode->i_layout_segments`) is
unchanged; only the new `ldf_flags` field flips.

## 3. On-disk format

Two on-disk formats are touched: the DS-side chunk store
(per-block provenance bit) and the MDS-side layout segments
file (per-mirror repair flag).

### Chunk store (DS-side, per-inode `<state_dir>/chunks/<ino>.meta`)

The persistent format `struct chunk_block_disk` already carries
`cbd_flags` (`chunk_store.h:108`).
`CHUNK_BLOCK_REPAIR_PROVENANCE = 0x2` is a new bit value in
the same flags word -- **no on-disk format change**.  The
header version `CHUNK_STORE_VERSION` stays at `1`.  The
`block_to_disk` / `disk_to_block` converters
(`chunk_store.c:73-108`) already copy `cb_flags` <-> `cbd_flags`
verbatim, so the bit round-trips through persist/load without
any code changes beyond setting it in the WRITE_REPAIR handler.

### Layout segments (MDS-side, per-inode `sb_<id>/ino_<ino>.layouts`)

This is the format change.  `struct layout_data_file` and
`struct layout_data_file_disk` in
`lib/include/reffs/layout_segment.h` have **no `ldf_flags`
field today** -- the layout encoder at
`lib/nfs4/server/layout.c:815` synthesises `ffv2ds_flags`
positionally:

```
ffds->ffv2ds_flags = (i < seg->ls_k) ? FFV2_DS_FLAGS_ACTIVE : FFV2_DS_FLAGS_PARITY;
```

That works for ACTIVE/PARITY (which are derived from the
mirror's position in the layout, fixed at LAYOUTGET time) but
cannot represent REPAIR or SPARE (which are mutable per-mirror
state).  The plan adds:

- `uint32_t ldf_flags` to `struct layout_data_file`
  (`layout_segment.h:91`)
- `uint32_t ldf_flags` (and adjust `ldf_pad` to keep
  alignment) to `struct layout_data_file_disk`
  (`layout_segment.h:49`)

Bits stored in `ldf_flags`:

- `FFV2_DS_FLAGS_REPAIR` (0x8) -- set by an out-of-band
  repair-request path; cleared by CHUNK_REPAIRED
- `FFV2_DS_FLAGS_SPARE` (0x2) -- reserved (mirror-lifecycle
  deferred)

ACTIVE/PARITY remain positional (the existing encoder ORs them
in at LAYOUTGET time alongside `ldf_flags & (REPAIR|SPARE)`).

**Format version**: `REFFS_DISK_VERSION_1` stays at `1` per the
CLAUDE.md "Deployment Status: No persistent storage has been
deployed" rule.  The struct change is in-place; persisted files
written before this change do not exist in any deployment.  No
migration code, per `.claude/roles.md` Reviewer Rule 8.

**Crash recovery**:

| Failure point | Result | Recovery |
|---------------|--------|----------|
| DS power-fail between CHUNK_WRITE_REPAIR persist and CHUNK_COMMIT | Block is PENDING with REPAIR_PROVENANCE flag set; CHUNK_REPAIRED never sent | Layout still has FFV2_DS_FLAGS_REPAIR on MDS; repair client re-runs (idempotent) |
| MDS power-fail after CHUNK_REPAIRED returned NFS4_OK but before `inode_sync_to_disk` | Client thinks it's repaired; MDS thinks it's still in repair on restart | Repair client re-runs CHUNK_REPAIRED on next contact; the DS already cleared its provenance flag at the chunk level so the re-issued op is a no-op on the DS side.  On the MDS side, the cleared flag is restored from disk; the redundant clear is idempotent (writing 0 to a 0 bit) |
| MDS power-fail after `inode_sync_to_disk` but before CHUNK_REPAIRED returned to client | Client retries CHUNK_REPAIRED; MDS sees `FFV2_DS_FLAGS_REPAIR` already cleared, returns NFS4_OK idempotently | MUST treat "REPAIR already cleared for this mirror" as idempotent success in the handler, NOT as a failure |

The persistence rule: CHUNK_REPAIRED's handler MUST call
`inode_sync_to_disk` under `i_attr_mutex` before returning
NFS4_OK to the client.  This mirrors the existing pattern in
`nfs4_op_chunk_commit` (`chunk.c:977`).

### RocksDB backend interaction

The RocksDB md backend (`lib/backends/rocksdb.c:349`) writes
`struct layout_segment_disk` to the `layouts` CF using the
same record shape.  Once `ldf_flags` lands in the struct,
RocksDB persists it for free (the field is in the verbatim
memcpy region around line 349).  No RocksDB-specific code
touches `layout_data_file_disk` fields by name; adding
`ldf_flags` is mechanical.

## 4. Security model

The threat: a malicious or buggy client issues
`OP_CHUNK_WRITE_REPAIR` to overwrite shards on a DS for a file
it should not be repairing, or `OP_CHUNK_REPAIRED` to a file
whose mirrors are not actually under repair.

### Validation rules -- OP_CHUNK_WRITE_REPAIR (DS-side)

The handler MUST validate, in this order, and abort on first
failure:

| # | Check | Failure code | Notes |
|---|-------|--------------|-------|
| 1 | current FH set | NFS4ERR_NOFILEHANDLE | mirrors CHUNK_WRITE |
| 2 | current FH is a regular file | NFS4ERR_INVAL (or NFS4ERR_WRONG_TYPE per draft line 9030) | mirrors CHUNK_WRITE |
| 3 | `cwra_chunk_size > 0` and `cwra_chunks_len > 0` | NFS4ERR_INVAL | mirrors CHUNK_WRITE |
| 4 | `cwra_owner.co_guard.cg_client_id` not in {NONE, MDS} | NFS4ERR_INVAL | mirrors CHUNK_WRITE; the draft (line 8973) repeats this prohibition |
| 5 | per-chunk CRC validates against payload | NFS4ERR_INVAL | mirrors CHUNK_WRITE |
| 6 | `cwra_stateid` is NOT a special stateid (anonymous, current, etc.) | NFS4ERR_BAD_STATEID | the draft (line 8956) requires this; repair MUST use the real layout stateid |
| 7 | trust table contains `cwra_stateid` (active, not expired, principal matches if Kerberos) | NFS4ERR_BAD_STATEID or NFS4ERR_DELAY | reuses the existing trust-table check at `chunk.c:218-249` |
| 8 | trust entry's `te_iomode` is `LAYOUTIOMODE4_RW` | NFS4ERR_ACCESS | repair-write requires RW; a READ layout cannot drive a repair |
| 9 | per-file `cs_checksum_algorithm` matches wire algorithm | NFS4ERR_INVAL | mirrors CHUNK_WRITE |

The "caller must hold a layout with FFV2_DS_FLAGS_REPAIR" check
is **implicit** in rule 7 + 8: the MDS only issues a
repair-flagged layout (and only registers its stateid in the DS
trust table) to a client that requested repair.  The DS does
not need to know which flag is set on the layout; it only needs
to know the caller's stateid is trusted.  (A defence-in-depth
alternative would have the MDS pass a "repair-flagged" bit in
the TRUST_STATEID args; that requires an XDR change to
`TRUST_STATEID4args` and is **out of scope** for this slice --
see Open Question 1.)

The chunk_guard4 CAS check is explicitly bypassed (the draft,
line 8900, makes this explicit).  The Track 1b PENDING-collision
gate (`chunk.c:351-382`) is also bypassed -- the repair client
is the sole writer because the layout-layer gates concurrency.

### Validation rules -- OP_CHUNK_REPAIRED (MDS-side)

| # | Check | Failure code | Notes |
|---|-------|--------------|-------|
| 1 | current FH set | NFS4ERR_NOFILEHANDLE | |
| 2 | current FH is a regular file | NFS4ERR_INVAL | |
| 3 | `cpa_owner.co_guard.cg_client_id` not in {NONE, MDS} | NFS4ERR_INVAL | draft line 8090 |
| 4 | `cpa_stateid` is not special; resolves to a valid OPEN or layout stateid on the inode | NFS4ERR_BAD_STATEID | use existing `stateid_inode_find_*` helpers |
| 5 | inode has `i_layout_segments` with at least one segment covering `cpa_offset` ... `cpa_offset + cpa_count` | NFS4ERR_INVAL | range mismatch check (draft line 8082) |
| 6 | At least one `layout_data_file` in the matched segment has `ldf_flags & FFV2_DS_FLAGS_REPAIR` | NFS4_OK (idempotent -- see crash-recovery table above) | if none are flagged, this is either a duplicate/redundant call or a malicious caller; treat as benign success |
| 7 | The clientid that holds the layout matches the calling client | NFS4ERR_ACCESS | prevent client A from clearing client B's repair flag |

On success: clear `ldf_flags & ~FFV2_DS_FLAGS_REPAIR` on every
flagged file in the matched segment; call `inode_sync_to_disk`
under `i_attr_mutex`; bump `sb->sb_chunk_stats.cs_repair_completed`
(new counter); return NFS4_OK.

NFS4ERR_NOFILEHANDLE / NFS4ERR_BAD_STATEID / NFS4ERR_INVAL /
NFS4ERR_ACCESS are the same set the draft (line 6157) declares
for CHUNK_REPAIRED.  NFS4ERR_PERM is not used (the draft does
not list it; we use ACCESS for credential mismatches per
RFC 8881 convention).

## 5. Admin interface

The IETF demo path is fully driven by `ec_demo repair` (Slice
3) and the bench harness around it.  Admin does not need to
force/cancel/list in-repair files for the demo cells.

**Decision: no probe op in this slice.**  Flagged as deferred:

- `INODE_REPAIR_LIST` probe op -- list files where any mirror
  has `ldf_flags & FFV2_DS_FLAGS_REPAIR` (returns inode, mirror
  index, dstore_id, since-when).  Needed for operational use
  post-IETF.
- `INODE_REPAIR_FORCE` probe op -- admin-driven repair trigger
  (sets the REPAIR flag on a specified mirror so the next
  layout client picks it up).
- `INODE_REPAIR_CANCEL` probe op -- clear stuck REPAIR flag
  without going through CHUNK_REPAIRED.

These follow the existing probe-op shape from
`.claude/design/probe-sb-management.md` and
`.claude/design/mirror-lifecycle.md`.  Since the demo and
bench harness do not need them, deferring them is consistent
with `.claude/roles.md` rule 9.

The existing `sb-list` / `sb-get` probe ops already expose
`sb->sb_chunk_stats` (`probe1_server.c:915-944`), so the new
`cs_repair_completed` counter shows up automatically in
`reffs-probe.py sb-list` once it lands as a sibling field.
The XDR addition (`pcs_repair_completed` in
`probe_chunk_stats1`) is wire-compat-safe because probe is
internal-only and client+server ship together.

## 6. Slice 1: OP_CHUNK_WRITE_REPAIR DS handler

### Tests first

New test file: **`lib/nfs4/tests/chunk_repair_test.c`** (under
the same `make check` target as `chunk_test.c`; uses the same
`cm_alloc` / `cm_free` harness scaffolding adapted from
`chunk_test.c` + `trust_stateid_test.c`).

| Group | Test | Intent |
|-------|------|--------|
| A. Input validation | `test_repair_no_fh` | NFS4ERR_NOFILEHANDLE when current FH unset |
| A | `test_repair_not_regular_file` | NFS4ERR_INVAL on directory FH |
| A | `test_repair_zero_chunk_size` | NFS4ERR_INVAL when `cwra_chunk_size == 0` |
| A | `test_repair_zero_chunks_len` | NFS4ERR_INVAL when `cwra_chunks_len == 0` |
| A | `test_repair_reserved_client_id` | NFS4ERR_INVAL for both CHUNK_GUARD_CLIENT_ID_NONE and ...MDS |
| A | `test_repair_crc_mismatch` | NFS4ERR_INVAL when supplied CRC doesn't match payload |
| A | `test_repair_per_file_algorithm_mismatch` | NFS4ERR_INVAL when wire algo != established file algo |
| B. Stateid auth | `test_repair_special_stateid_rejected` | NFS4ERR_BAD_STATEID for anonymous stateid (repair MUST use real stateid) |
| B | `test_repair_unknown_stateid_rejected` | NFS4ERR_BAD_STATEID when stateid not in trust table |
| B | `test_repair_expired_trust_entry` | NFS4ERR_BAD_STATEID for expired entry |
| B | `test_repair_pending_trust_entry` | NFS4ERR_DELAY for TRUST_PENDING entry (matches CHUNK_WRITE behaviour) |
| B | `test_repair_read_iomode_rejected` | NFS4ERR_ACCESS when trust entry's iomode is READ-only |
| C. Happy path | `test_repair_single_block_empty` | Block transitions EMPTY -> PENDING; REPAIR_PROVENANCE set; cs_repair_initiated bumped |
| C | `test_repair_single_block_overwrite_committed` | Block transitions COMMITTED -> PENDING; REPAIR_PROVENANCE set; chunk_size respected |
| C | `test_repair_multi_block` | 4 blocks written; each gets REPAIR_PROVENANCE; cwrr_status[0..3] all NFS4_OK |
| C | `test_repair_bypasses_guard_check` | A stale cwa_guard would reject a CHUNK_WRITE; CHUNK_WRITE_REPAIR has no guard field at all -- verify the handler does not look it up |
| C | `test_repair_bypasses_pending_collision_gate` | Pre-populate a PENDING block from a different writer; repair-write must succeed (not NFS4ERR_DELAY) |
| D. Persistence | `test_repair_provenance_flag_persisted` | After WRITE_REPAIR + FINALIZE + COMMIT, persist, reload, verify cb_flags & CHUNK_BLOCK_REPAIR_PROVENANCE is set |
| E. Idempotence | `test_repair_committed_no_op_when_checksums_match` | A repair-write of identical bytes over already-COMMITTED block: returns NFS4_OK per-chunk; cs_repair_initiated still bumps (we count the attempt, not the effect) |

### Existing tests affected

| File | Impact |
|------|--------|
| `lib/nfs4/tests/chunk_test.c` | **PASS** -- no production code in CHUNK_WRITE / FINALIZE / COMMIT / READ paths changes; the new handler is in its own function.  The Track 1b collision gate, trust-table check, and INV-1 instrumentation in CHUNK_WRITE are untouched. |
| `lib/nfs4/tests/trust_stateid_test.c` | **PASS** -- the new handler reuses `trust_stateid_find` exactly as CHUNK_WRITE does. |
| `lib/nfs4/tests/proxy_*_test.c` | **PASS** -- no proxy interactions. |
| `tests/fs_test_*.c` | **PASS** -- no FS layer changes. |

### File-level breakdown

| File | Change | New / Modified |
|------|--------|----------------|
| `lib/nfs4/include/nfs4/chunk_store.h` | Add `#define CHUNK_BLOCK_REPAIR_PROVENANCE 0x2` next to `CHUNK_BLOCK_LOCKED` | Modified (1 line) |
| `lib/include/reffs/nfs4_stats.h` | Add `_Atomic uint64_t cs_repair_completed` to `struct reffs_chunk_stats` (sibling of `cs_repair_initiated`) | Modified (1 line; appended) |
| `lib/nfs4/server/chunk.c` | Replace stub `nfs4_op_chunk_write_repair` (line 1293-1307) with full handler.  Factor the shared CRC-pre-validate + trust-table check + per-file algorithm consistency check out of `nfs4_op_chunk_write` into a local `chunk_write_validate_common` helper, then call it from both ops.  Handler body: validate, lock `i_attr_mutex`, run `chunk_store_get`, write payload into data_block (reusing the existing `data_block_write` path), record per-block metadata with `cb_flags |= CHUNK_BLOCK_REPAIR_PROVENANCE`, bump `cs_repair_initiated`, persist on FINALIZE/COMMIT later (no persist at WRITE -- same as CHUNK_WRITE). | Modified (~150 LOC: ~80 new for repair handler, ~70 refactored into shared helper) |
| `lib/nfs4/tests/chunk_repair_test.c` | NEW -- see test table above | NEW (~600 LOC tests + harness) |
| `lib/nfs4/tests/Makefile.am` | Wire `chunk_repair_test` into `check_PROGRAMS` | Modified (4 lines) |
| `lib/probe1/probe1_server.c` | Add `psi->psi_chunk_stats.pcs_repair_completed = atomic_load_explicit(&cs->cs_repair_completed, ...)` next to the `pcs_repair_initiated` line at 927 | Modified (3 lines) |
| `lib/xdr/probe1_xdr.x` | Append `unsigned hyper pcs_repair_completed;` to `struct probe_chunk_stats1` (after `pcs_fragmentation_runs`) | Modified (1 line; `probe1_xdr.x` is the internal-only XDR, freely modifiable per reviewer rule 9) |
| `scripts/reffs/probe_client.py.in` | No code change -- auto-regenerated XDR exposes the new field | Auto-generated |
| `scripts/reffs-probe.py.in` | Add `pcs_repair_completed` to the chunk-stats formatter in `sb-list` / `sb-get` output | Modified (1 line) |

Implementation order within Slice 1: (1) tests-only commit
that adds `chunk_repair_test.c` with all tests but
`nfs4_op_chunk_write_repair` still stub-returning
NFS4ERR_NOTSUPP -- tests will all fail, which is the TDD goal;
(2) refactor `chunk_write_validate_common` out of
`nfs4_op_chunk_write` -- existing chunk_test.c tests must
continue to pass; (3) implement `nfs4_op_chunk_write_repair`;
(4) wire `cs_repair_completed` + probe field + Python
formatter.

### Days estimate: 4 days

(1.0 day tests; 1.5 days handler + refactor; 0.5 day
persistence verification; 0.5 day probe wire-up + Python; 0.5
day reviewer-agent pass + style fixes.)

## 7. Slice 2: OP_CHUNK_REPAIRED MDS handler

### Tests first

Extend `chunk_repair_test.c` with a Group F (MDS-side):

| Test | Intent |
|------|--------|
| `test_repaired_no_fh` | NFS4ERR_NOFILEHANDLE |
| `test_repaired_not_regular_file` | NFS4ERR_INVAL on directory FH |
| `test_repaired_reserved_client_id` | NFS4ERR_INVAL for cg_client_id == NONE/MDS |
| `test_repaired_no_layout_segments` | NFS4ERR_INVAL when inode has no `i_layout_segments` |
| `test_repaired_range_outside_segment` | NFS4ERR_INVAL when `cpa_offset + cpa_count` exceeds any segment |
| `test_repaired_no_mirror_in_repair` | NFS4_OK (idempotent) when no mirror has FFV2_DS_FLAGS_REPAIR set |
| `test_repaired_clears_single_mirror` | One mirror flagged; CHUNK_REPAIRED clears it; `cs_repair_completed` bumps by 1; inode is dirty |
| `test_repaired_clears_multiple_mirrors` | 2 mirrors flagged in same segment; both cleared in one op; `cs_repair_completed` bumps by 2 |
| `test_repaired_persists_across_restart` | Set flag in memory, call CHUNK_REPAIRED, simulate restart by writing inode then reloading via posix backend `inode_alloc`, verify `ldf_flags` is 0 on disk |
| `test_repaired_wrong_client_rejected` | Layout held by client A; client B issues CHUNK_REPAIRED -- NFS4ERR_ACCESS |
| `test_repaired_idempotent_second_call` | Issue CHUNK_REPAIRED twice on the same range; second returns NFS4_OK; counter bumps only once (per Open Question 4) |

A separate test file for the on-disk format change:
**`lib/fs/tests/layout_data_file_flags_test.c`** (NEW):

| Test | Intent |
|------|--------|
| `test_ldf_flags_default_zero` | A freshly allocated layout_data_file has `ldf_flags == 0` |
| `test_ldf_flags_persist_roundtrip` | Set `ldf_flags = FFV2_DS_FLAGS_REPAIR`, sync to disk via posix backend, reload, verify flag preserved |
| `test_ldf_flags_persist_zero` | Set `ldf_flags = 0`, sync, reload, verify still 0 |
| `test_ldf_flags_persist_combined` | Set `ldf_flags = FFV2_DS_FLAGS_REPAIR | FFV2_DS_FLAGS_SPARE`, sync, reload, verify both preserved |

### Existing tests affected

| File | Impact |
|------|--------|
| `lib/nfs4/tests/reflected_getattr_test.c` | **PASS** -- `layout_data_file` calloc-defaults `ldf_flags=0` |
| `lib/nfs4/tests/migration_record_test.c` | **PASS** -- separate struct |
| `lib/nfs4/tests/layout_conflict_scan_test.c` | **PASS** -- doesn't read `ldf_flags` |
| `lib/fs/tests/layout_segment_test.c` | **Verify** -- round-trips may have hardcoded sizes; likely safe |
| `lib/backends/tests/rocksdb_test.c` | **PASS** -- memcpy persistence |

### File-level breakdown

| File | Change | New / Modified |
|------|--------|----------------|
| `lib/include/reffs/layout_segment.h` | Add `uint32_t ldf_flags` to both `struct layout_data_file` (in-memory) and `struct layout_data_file_disk` (on-disk; adjust `ldf_pad` to maintain 8-byte alignment) | Modified (4 lines) |
| `lib/backends/posix.c` | Add `.ldf_flags = ldf->ldf_flags` to the on-disk struct populator (around line 264); add `files[f].ldf_flags = ldfd.ldf_flags` in the loader (around line 622) | Modified (2 lines) |
| `lib/backends/rocksdb.c` | Confirm the memcpy-shaped persistence at lines 349 and 552 picks up the new field without code change; if not, add the two lines | Modified (likely 0 lines, possibly 2) |
| `lib/nfs4/server/layout.c` | At line 815, change `ffds->ffv2ds_flags = ...` to `ffds->ffv2ds_flags = ((i < seg->ls_k) ? FFV2_DS_FLAGS_ACTIVE : FFV2_DS_FLAGS_PARITY) | ldf->ldf_flags;` | Modified (1 line) |
| `lib/nfs4/server/chunk.c` | Replace stub `nfs4_op_chunk_repaired` (line 1021-1029) with full handler.  Body: validate per Sec 4 rules; under `i_attr_mutex`, walk `i_layout_segments`, find segments covering `[cpa_offset, cpa_offset + cpa_count)`, for each `ls_files[f]` with `ldf_flags & FFV2_DS_FLAGS_REPAIR`, clear that bit and increment a local counter; if counter > 0, call `inode_sync_to_disk(compound->c_inode)`; bump `sb_chunk_stats.cs_repair_completed` by counter; return NFS4_OK. | Modified (~120 LOC for handler) |
| `lib/fs/tests/layout_data_file_flags_test.c` | NEW -- see Group F above | NEW (~200 LOC) |
| `lib/fs/tests/Makefile.am` | Wire test into `check_PROGRAMS` | Modified (4 lines) |

A subtle point: the demo uses single-segment whole-file
layouts, but the handler should iterate all segments and match
on byte-range overlap to be future-proof for striped layouts.

Note: matching `cpa_offset` and `cpa_count` units -- the draft
(lines 8076-8081) says cpa_offset is "starting chunk index"
(not bytes).  For the demo, single-segment whole-file means
`cpa_offset == 0` and `cpa_count` covers all blocks; the unit
conversion is moot.  A precise check is needed when striping
arrives.  See Open Question 3.

### Days estimate: 5 days

(1.0 day tests + ldf_flags persistence test; 1.0 day struct +
posix backend wire-up; 0.5 day rocksdb verify + layout.c
encoder wire-up; 2.0 days handler + persistence + segment
iteration; 0.5 day reviewer-agent pass.)

## 8. Slice 3: ec_demo repair subcommand

The subcommand drives the full repair: LAYOUTGET -> CHUNK_READ
all (k+m) shards -> detect loss (NFS4ERR_PAYLOAD_LOST or CRC
mismatch on the client side) -> decode peers via existing
`ec_pipeline` codec to reconstruct the missing shards ->
CHUNK_WRITE_REPAIR + CHUNK_FINALIZE + CHUNK_COMMIT to each
affected DS -> CHUNK_REPAIRED to the MDS -> LAYOUTRETURN.

The existing `cmd_read` path in `tools/ec_demo.c` (line 721)
and the `--skip-ds <mask>` flag already drive degraded-read
reconstruction in `ec_pipeline.c` (line 2046, `ec_decode`).
The repair subcommand reuses that decode logic and then issues
the wire ops to write the reconstructed shards back.

### Tests first

The `tools/` directory does not have unit tests today (it's a
CLI tool).  Tests for the new subcommand land in
**`scripts/test_ec_repair.sh`** (CI integration test).
Functional-level testing only.

| Test (shell-driven against a running combined-role reffsd) | Intent |
|------------------------------------------------------------|--------|
| `test_repair_no_loss_noop` | Run `ec_demo write` then `ec_demo repair` with no `--shard-loss`; verify it's a no-op |
| `test_repair_one_shard_rs_42` | Write 1 MB file via RS 4+2; `ec_demo repair --shard-loss 0x1`; verify `ec_demo verify` succeeds; verify `reffs-probe.py sb-get` shows `cs_repair_completed` bumped |
| `test_repair_two_shards_rs_42` | Same but `--shard-loss 0x3`; verify reconstruction recovers both |
| `test_repair_mojette_systematic_42` | Same as one-shard test but `--codec mojette-sys` |
| `test_repair_idempotent` | Run `ec_demo repair` twice in a row; second run is no-op |
| `test_repair_3_shards_unrecoverable` | `--shard-loss 0x7` on RS 4+2 (only m=2 parity); verify exits non-zero with "unrecoverable" message |
| `test_repair_clears_repair_flag_in_layout` | After repair, run `ec_demo read` and verify LAYOUTGET ffv2ds_flags have no REPAIR set |
| `test_repair_persistence_survives_restart` | Stamp a mirror as REPAIR via debug helper, restart reffsd, verify flag preserved, run repair, verify cleared |

Loss injection: `--shard-loss <mask>` flag in `ec_demo repair`
simulates loss at the **client** by skipping CHUNK_READ to
selected mirrors (returns NFS4ERR_PAYLOAD_LOST locally) -- same
trick `cmd_read --skip-ds` already uses.  For the persistence
test, see Open Question 5.

### File-level breakdown

| File | Change | New / Modified |
|------|--------|----------------|
| `lib/nfs4/ps/ec_pipeline.c` | Add public function `ec_repair_codec(struct mds_session *ms, const char *path, int k, int m, enum ec_codec_type, layouttype4, uint64_t shard_loss_mask, size_t shard_size, struct ec_repair_stats *out_stats)` -- LAYOUTGET, ec_resolve_mirrors, CHUNK_READ (or skip per `shard_loss_mask`), ec_decode to reconstruct missing shards, for each mirror with a "lost" shard call `ec_chunk_write_repair`, CHUNK_FINALIZE, CHUNK_COMMIT, then call `mds_chunk_repaired(ms, mf, cpa_offset, cpa_count, owner)`, then mds_layout_return.  Populate `out_stats` for the bench. | Modified (~250 LOC: ~50 for ec_chunk_write_repair, ~100 for mds_chunk_repaired builder, ~100 for the orchestration) |
| `lib/nfs4/ps/ec_pipeline_internal.h` | Declare `ec_chunk_write_repair` (sibling of `ec_chunk_write` at line 100) | Modified (5 lines) |
| `lib/nfs4/client/ec_client.h` | Declare `ec_repair_codec` and `struct ec_repair_stats` | Modified (15 lines) |
| `lib/nfs4/client/mds_layout.c` (or new `mds_chunk_repaired.c`) | Build the COMPOUND for CHUNK_REPAIRED: `SEQ + PUTFH(mds_fh) + CHUNK_REPAIRED(cpa_*)` and parse result | Modified or NEW (~80 LOC) |
| `tools/ec_demo.c` | Add `cmd_repair` static function (around line 720 alongside `cmd_read`); add dispatch in main (after `cmd_verify` at line 1956); add `--shard-loss <mask>` flag as alias for existing `--skip-ds`; print summary stats from `ec_repair_stats`.  Update `usage()`. | Modified (~150 LOC) |
| `scripts/test_ec_repair.sh` | NEW -- see test table above | NEW (~300 LOC shell) |
| `scripts/ec_repair_bench.sh` | NEW -- drives the 4 sizes x 2 codecs x 2 loss patterns x 5 iters = 80 cells, emits CSV | NEW (~200 LOC) |

Implementation order: (1) `ec_chunk_write_repair` helper in
ec_pipeline.c (mechanical clone of `ec_chunk_write`, swap the
op number) -- gated by Slice 1 being merged; (2)
`mds_chunk_repaired` compound builder -- gated by Slice 2; (3)
`ec_repair_codec` orchestration; (4) `cmd_repair` subcommand;
(5) shell tests.

### Days estimate: 5 days

(1.0 day `ec_chunk_write_repair` + `mds_chunk_repaired`; 1.5
days `ec_repair_codec` orchestration; 1.0 day `cmd_repair` +
flag wiring; 1.0 day shell tests + cross-codec verification;
0.5 day reviewer-agent pass + style.)

## 9. Test impact on existing tests

Aggregated cross-slice.  Pass = no expected behavioural change;
Verify = need to run locally to confirm.

| Test file | Slice that touches it | Impact | Why |
|-----------|----------------------|--------|-----|
| `lib/nfs4/tests/chunk_test.c` | 1 (refactor only) | PASS -- but **Verify** after `chunk_write_validate_common` extract | Refactor moves shared validation |
| `lib/nfs4/tests/trust_stateid_test.c` | 1 | PASS | Trust-table API unchanged |
| `lib/nfs4/tests/reflected_getattr_test.c` | 2 | PASS | `layout_data_file` calloc-defaults `ldf_flags=0` |
| `lib/nfs4/tests/migration_record_test.c` | 2 | PASS | Separate struct |
| `lib/nfs4/tests/layout_conflict_scan_test.c` | 2 | PASS | Doesn't read `ldf_flags` |
| `lib/fs/tests/layout_segment_test.c` | 2 | **Verify** | Round-trips may have hardcoded sizes; likely safe |
| `lib/backends/tests/rocksdb_test.c` | 2 | PASS | memcpy persistence |
| `lib/nfs4/tests/nfs4_session.c` | 1, 2 | PASS | Unaffected |
| `lib/nfs4/tests/nfs4_client_persist.c` | none | PASS | Unaffected |
| `lib/nfs4/tests/proxy_*_test.c` | none | PASS | Repair is fore-channel only; no CB |
| `lib/probe1/tests/*` | 1 (probe XDR append) | PASS | Wire-compat append + Python autogen |
| `lib/nfs3/tests/*` | none | PASS | NFSv3 unaffected |
| `tests/fs_test_*.c` | 2 | PASS | FS layer unaffected by per-mirror flag |
| `scripts/ci_integration_test.sh` | none | PASS | Doesn't exercise EC |
| `scripts/test_sb_probe.py` | 1 (cs_repair_completed appended) | PASS | Field appended; reader autogen |

No existing test is expected to BREAK.

## 10. Bench + slide integration

Carry-over from `ec-repair-bench.md` Sec "Measurement plan".
Tier 2 replaces Tier 1's "fake repair via plain CHUNK_WRITE"
with the real wire ops.  The bench cells are identical: 4
sizes (4 KB, 64 KB, 1 MB, 16 MB) x 2 codecs (RS 4+2, Mojette
sys 4+2) x 2 loss patterns (1 shard, 2 shards) x 5 iters = 80
cells.

`scripts/ec_repair_bench.sh` emits a CSV with columns:
`size,codec,loss_mask,iter,layoutget_ms,read_ms,decode_ms,
write_repair_ms,finalize_ms,commit_ms,chunk_repaired_ms,
layoutreturn_ms,total_ms,bytes_repaired`.  The slide table
reports median per cell of `total_ms` and the derived
`MiB/s = bytes_repaired / total_ms`.

Sanity check: `total_ms` should be roughly `read_ms +
decode_ms + (write_repair_ms + finalize_ms + commit_ms) *
num_lost_shards / (k+m)`.  If actual exceeds the model by >2x,
the bench is hitting an unexpected serialisation point (likely
the single-slot MDS-to-DS session bug noted in
`dstore-vtable-v2.md`).

## 11. Risks

1. **`ldf_flags` field addition is on-disk format change.**  Per
   `.claude/standards.md` reviewer rule 8 + CLAUDE.md
   "Deployment Status: No persistent storage has been
   deployed", no version bump or migration code needed.  But
   the **change must be reviewer-agent-checked** -- adding a
   field to a persisted struct is exactly the bait for the
   BLOCKER-bait flagging.  Mitigation: explicit comment in the
   struct citing the CLAUDE.md deployment status; reviewer will
   accept.

2. **`chunk_write_validate_common` refactor risks regression.**
   Pulling shared validation out of `nfs4_op_chunk_write` is
   the cheap path but it changes the hot-path function.
   Mitigation: do the refactor in its own commit (one-concern-
   per-commit per `.claude/standards.md`); the existing
   `chunk_test.c` suite is the safety net.  If the refactor is
   risky, fall back to copy-paste validation into the repair
   handler (verbose but isolated; ~50 LOC duplication is
   acceptable).

3. **Track 1b chunk-collision gate behaviour with repair-write
   is underspecified by the draft.**  The draft (line 8900)
   says the guard CAS is bypassed but doesn't address the
   PENDING-from-different-writer gate that reffs added on top
   (Option C).  Our design intent is to bypass it too (because
   the layout-level concurrency control + trust-table iomode=RW
   gating means the repair client is the only writer); but a
   future scenario where the MDS issues a repair while a normal
   writer is mid-PENDING would race.  For the demo this can't
   happen because the repair-flagged layout is granted only
   when the mirror has lost data and is not being normally
   written.  **Document this assumption explicitly** and add a
   NOT_NOW_BROWN_COW in the repair handler for the "tighter
   concurrency control needed for production" case.

4. **`cs_repair_completed` counting semantics.**  Two
   reasonable choices: (a) count one per CHUNK_REPAIRED call
   that successfully cleared at least one flag, or (b) count
   one per mirror-flag cleared.  Open Question 4 -- current
   plan picks (b) (per-mirror).

5. **MDS-side handler runs without a session in some test
   paths.**  The `cm_alloc` harness in `chunk_test.c` builds a
   fake compound without a real MDS session.  CHUNK_REPAIRED on
   the MDS uses `compound->c_nfs4_client` to check "wrong
   client rejected" (rule 7).  The test harness needs to attach
   a fake `nfs4_client` -- the `trust_stateid_test.c` shows
   how.  Mitigation: copy the harness extension from
   `trust_stateid_test.c`.

6. **Layout-segment iteration for cpa_offset/cpa_count units
   mismatch.**  As noted in Sec 7, the draft says cpa_offset is
   "chunk index" but the MDS thinks in segment-byte-ranges.
   For the single-segment whole-file demo cells, this doesn't
   bite.  For striped layouts it does.  **Risk: late discovery
   during the bench harness's 80-cell run that some cell
   triggers the unhandled units path.**  Mitigation: explicit
   single-segment assertion in the MDS handler with
   NOT_NOW_BROWN_COW for striped; the 80 demo cells are all
   single-segment, so the bench is safe.

7. **The 3-week budget is tight if reviewer-agent passes are
   slow.**  Each slice triggers reviewer-agent gating (XDR
   change in Slice 1's `probe1_xdr.x`; on-disk format change in
   Slice 2's layout segment file).  Mitigation: batch the
   `probe1_xdr.x` + `layout_segment.h` changes early so the
   reviewer agent runs once per artifact, not per commit.

8. **`ldf_flags` interaction with the proxy-server delegation
   path.**  `ps_proxy_ops.c` consumes layouts via
   `ec_read_codec_with_file` etc. -- the proxy server reads
   `ffv2ds_flags` and could mis-route a read to a
   REPAIR-flagged mirror.  The current PS code does not
   interpret REPAIR specifically.  Out of scope for this slice
   but **flag for a follow-up**: the PS should skip
   REPAIR-flagged mirrors on READ to avoid serving stale data,
   and use REPAIR-flagged mirrors preferentially as the WRITE
   target on a repair.

## 12. Open questions for the author

1. **Does TRUST_STATEID need a "repair-flagged" hint?**  Current
   plan validates the repair-write at the DS via "trust-table
   contains stateid with iomode=RW", which is necessary but not
   sufficient to prevent a malicious-but-trusted client from
   issuing CHUNK_WRITE_REPAIR on a non-flagged file.  The
   defence-in-depth fix is to add a `tsa_flags` field to
   `TRUST_STATEID4args` with a `TS_FLAGS_REPAIR_ALLOWED` bit;
   the MDS sets the bit when it issues a repair-flagged layout.
   This is an XDR change.  **Decision needed: in or out of this
   3-week slice?**  Recommendation: out -- the layout-flag +
   iomode-RW gate is sufficient for the IETF demo's
   cooperative-client model.

2. **Field-prefix discrepancy on CHUNK_REPAIRED4args.**  The
   draft (line 8025) uses `cra_*`.  The implementation XDR
   (`lib/xdr/nfsv42_xdr.x:3528`) uses `cpa_*`.  Both can't be
   right.  Which is canonical?  Looking at the draft's other
   places -- line 8068 has `cra_stateid`, line 8076 `cra_offset`,
   line 8086 `cra_owner`.  The implementation prefix `cpa_*`
   was probably picked to disambiguate from the existing
   `cra_*` of CHUNK_READ.  **Decision: confirm the
   implementation `cpa_*` is canonical and update the draft, OR
   change the implementation XDR to `cra_*` and update the
   draft author's expectation.**  The plan above assumes
   `cpa_*` (matches code today).

3. **cpa_offset / cpa_count units: chunk index or byte range?**
   Draft says chunk index (line 8076).  For striped layouts
   the MDS needs the layout's chunk size to convert; for the
   demo's single-segment whole-file case, it's `cpa_offset ==
   0` and `cpa_count == total_blocks` and the math doesn't
   bite.  **Decision: confirm the chunk-index semantics for the
   MDS-side handler; document that striped repair is out of
   scope until the chunk_size-per-segment plumbing is added.**

4. **`cs_repair_completed` counter semantics: per-call or
   per-mirror-cleared?**  Plan currently picks per-mirror-
   cleared for bench accuracy.  Confirm or specify both
   counters.

5. **Loss injection for the persistence test.**  The
   `test_repair_persistence_survives_restart` test needs to set
   `FFV2_DS_FLAGS_REPAIR` on a mirror without an
   `INODE_REPAIR_FORCE` probe op (which is deferred).  Options:
   (a) build a debug-only `INODE_DEBUG_SET_REPAIR_FLAG` probe
   op gated on `--enable-debug`; (b) skip the persistence test
   in unit tests and verify via the BAT soak harness only; (c)
   write a one-shot C test that directly manipulates `ldf_flags`
   via the FS layer (bypassing the probe protocol).
   Recommendation: (c) for unit-test, (a) deferred to
   follow-up.  **Decision needed.**

6. **CHUNK_REPAIRED idempotence vs error code on already-clear
   mirror.**  Spec'd as NFS4_OK (rule 6 of Sec 4 above) per the
   crash-recovery story.  The draft (line 8110) says
   "NFS4ERR_INVAL if precondition fails".  This is a deliberate
   divergence from the draft for crash recovery -- **ANSWERED
   2026-06-10: diverge to NFS4_OK on the "no mirror flagged"
   path.**  The crash-recovery retry where the client re-issues
   after the MDS already persisted the clear is the load-bearing
   case; NFS4ERR_INVAL would force every client to track its own
   in-flight idempotence ledger.  Shipped in commit `606c7fb79b3c`;
   the draft prose needs the same correction in -07.

7. **Is the OUT-OF-SCOPE `MDS detection logic` deferred-OK for
   the IETF deck?**  The deck answers "cost of collisions and
   repair" but the deferred autopilot means the demo answers it
   only with a client-driven repair.  The "MDS notices CRC-fail
   and issues a repair on its own" is a separate slice.  **ANSWERED
   2026-06-10: yes, deferred-OK for the IETF deck.**  The bench
   harness drives repair via `ec_demo repair --skip-ds`; the deck
   row in `ietf126.md` Bucket 4 names this as "client-driven
   repair on demand" so the audience sees the scope explicitly.

## 12a. Deferred / NOT_NOW_BROWN_COW (post-IETF)

All four NOT_NOW_BROWN_COW markers in the shipped handler
headers are gated on the demo's cooperative-client model.
Production deployments need each addressed before scaling
beyond a single trusted writer per file.

- **CHUNK_WRITE_REPAIR mid-PENDING collision** (`chunk.c`
  `nfs4_op_chunk_write_repair` header).  If the MDS ever issues
  a repair-flagged layout while a normal writer is mid-PENDING
  on the same range, the repair-write races against the normal
  writer because the chunk-layer gates are bypassed.  Today's
  cooperative model prevents this -- the MDS only flags REPAIR
  when the mirror has lost data and is not being normally
  written.  Production fix: a per-block "repair-in-progress"
  sentinel that the normal-writer path checks before stamping
  PENDING (or, alternatively, gate at the layout-server level
  with a recall before granting RW layouts on a REPAIR-flagged
  mirror).

- **CHUNK_REPAIRED rigorous cpa_stateid validation** (`chunk.c`
  `nfs4_op_chunk_repaired` header).  The current handler accepts
  any non-zero cpa_stateid without checking that it resolves to
  a valid OPEN or layout stateid on the inode.  The demo
  cooperative-client model treats stateid auth as a layout-layer
  concern enforced at LAYOUTGET time; the chunk-state-clearing
  surface here is controlled by the layout's own clientid match.
  Production fix: call `stateid_inode_find_*` and reject on miss.

- **CHUNK_REPAIRED cpa_offset / cpa_count range matching**
  (`chunk.c` `nfs4_op_chunk_repaired` header).  The handler
  clears EVERY flagged mirror in EVERY segment regardless of the
  requested range.  Demo cell shape is single-segment whole-file
  so range trivially matches.  Production fix: walk `lss_segs`
  and only clear flags on segments overlapping `[cpa_offset,
  cpa_offset + cpa_count)` in chunk-index units; needs the
  per-segment chunk_size plumbing flagged in Open Question 3.

- **CHUNK_REPAIRED clientid match between layout-holder and
  calling client** (`chunk.c` `nfs4_op_chunk_repaired` header).
  Rule 7 in Section 4 above describes the check; the shipped
  handler skips it.  Production fix: compare
  `seg->ls_holder_clientid` (or equivalent layout-tracking
  field) against `compound->c_nfs4_client->nc_clientid` and
  return `NFS4ERR_ACCESS` on mismatch.

These four markers also have NOT_NOW_BROWN_COW comments in
their respective source locations so a future code-walker
sees them; this section is the planner-side anchor that
followup-trackers see.

## 13. Effort summary

| Slice | Description | Days |
|-------|-------------|------|
| 1 | OP_CHUNK_WRITE_REPAIR DS handler + tests + probe wire-up | 4 |
| 2 | OP_CHUNK_REPAIRED MDS handler + ldf_flags persistence + tests | 5 |
| 3 | ec_demo repair subcommand + ec_pipeline glue + shell tests | 5 |
| Cross-cutting reviewer-agent passes (XDR, on-disk format triggers) | | 1 |
| Bench harness (`ec_repair_bench.sh`) + cell sweep + median extraction | | 2 |
| Slide draft + CSV-to-table integration into deck | | 1 |
| Integration / surprise buffer | | 2 |
| **Total** | | **20 days = 4 weeks** |

**Realism call**: 3 weeks is **tight**; 4 weeks is comfortable.
If the budget is hard 3 weeks (15 working days), the cut path:

- Drop Slice 3's `ec_repair_bench.sh` automation and the slide
  draft (5 days).  Run the 80-cell sweep manually and hand-roll
  the slide table; the wire ops are still real, the bench
  numbers are still real, only the harness automation is
  deferred.  Saves 3 days.
- Defer the persistence test for the layout flag round-trip
  (Slice 2 Group F + `layout_data_file_flags_test.c`) -- verify
  by inspection + one shell test only.  Saves 1 day.
- Combine the `probe1_xdr.x` `pcs_repair_completed` wire-up
  into a single appended-field commit that piggybacks the
  existing review-agent pass for Slice 1.  Saves the
  cross-cutting reviewer-agent pass day.

Total cut path: 5 days.  Lands at 15 days = 3 weeks **with no
buffer**, which is fragile.  Recommendation: budget 4 weeks and
ship clean.

### Critical files for implementation

- `/Volumes/Sensitive/reffs/lib/nfs4/server/chunk.c`
- `/Volumes/Sensitive/reffs/lib/nfs4/include/nfs4/chunk_store.h`
- `/Volumes/Sensitive/reffs/lib/include/reffs/layout_segment.h`
- `/Volumes/Sensitive/reffs/lib/backends/posix.c`
- `/Volumes/Sensitive/reffs/lib/nfs4/ps/ec_pipeline.c`
- `/Volumes/Sensitive/reffs/tools/ec_demo.c`
