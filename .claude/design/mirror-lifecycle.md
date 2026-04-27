<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Mirror Lifecycle and Dstore Drain

Admin-driven lifecycle for the mirror set on an existing file, plus
the dstore-drain workflow that reuses it.  The user-visible operations:

- list a file's layout (which dstores hold mirrors)
- add a mirror on a chosen dstore
- remove a specific mirror
- drain a dstore (no new placements + migrate existing instances off)
- destroy a drained dstore

All of this leaves the file's name unchanged in the namespace.  Codec
change (e.g. MIRRORED -> RS, or RS k+m -> different k+m) rides on the
same op surface and lands as the last slice once the underlying
ec_pipeline is callable from MDS-internal context.

## Why

Today reffs places mirrors at LAYOUTGET time using runway-popped FHs
on configured dstores; once an inode has a layout segment, its mirror
set is fixed for the lifetime of the file.  Dstores can be added or
removed only by editing the TOML config and bouncing the server.  An
admin who wants to retire a specific dstore has no in-band path:
edit-and-bounce loses the mirror data on the retiring dstore unless
the admin first manually copies every file with an instance there to
a different dstore -- the "manually copy" step has no tool today.

This design provides the tools.

## Note on line-number citations

This doc cites specific line numbers in the codebase (e.g.
`rocksdb.c:267`, `inode.c:239`, `cb.c:358`, `dstore.h:79`,
`layout.c:1307`).  These were verified at write time but drift as the
code evolves.  Implementer should re-grep at touch time -- the
citations are anchors for "this is the code path I mean," not
guarantees that the line is unchanged.  When the line moves, update
this doc in the same commit that touches the file.

## Concepts

### Existing types reused (one new field on `layout_data_file`)

- `struct layout_segment` (lib/include/reffs/layout_segment.h) -- a
  byte-range descriptor with one or more `layout_data_file` entries
  representing the mirrors / shards for that range.  The full layout
  for an inode is `inode->i_layout_segments`, currently a single
  segment for the BAT scope.
- `struct layout_data_file` -- one per mirror instance, with
  `ldf_dstore_id`, `ldf_fh` (DS-side filehandle), and cached attrs.

  **New field**: `uint8_t ldf_slot` -- for RS layouts, the shard
  ordinal (0 .. k+m-1) this `layout_data_file` represents.  For
  MIRRORED layouts, `ldf_slot` is undefined (mirrors are
  positionally fungible).  Required by slice G's RS REPAIR path
  (the reconstructed shard must be written for the SPECIFIC missing
  slot; without an explicit ordinal, REPAIR would have to assume
  array-index-implies-slot, which breaks when DSTORE_LOST drops an
  entry and the array compacts).  Required by slice F's codec-change
  encoding when the new layout is RS.  No SB_REGISTRY version bump
  per CLAUDE.md "Deployment Status: No persistent storage has been
  deployed."  Old reffsd reading a newer file would mis-parse one
  byte; safe today, would matter only after first deployment.

### New on-inode field

- `_Atomic uint64_t lss_gen` on `struct layout_segments` -- monotonic
  per-mutation counter.  LIST returns it, ADD/REMOVE require the
  client-supplied `expected_gen` to match.  Closes the TOCTOU window
  between LIST and the mutation that follows it.

  On-disk placement: appended after `lss_count` in the per-inode
  layout payload (RocksDB `layouts:lay:<inum>` value, POSIX
  `ino_<N>.layouts` file body).  No SB_REGISTRY version bump per
  CLAUDE.md "Deployment Status: No persistent storage has been
  deployed."  Old reffsd reading a newer file would mis-parse the
  appended bytes as the first segment's `ls_offset` -- safe today
  because no old reffsd is deployed; a future format-version field
  on `layout_segments_disk` would make this explicit.

### New on-inode field

- `_Atomic bool i_broken` on `struct inode` (NOT a bit in
  `inode->i_state`).  Set by `DSTORE_LOST` triage when an inode's
  surviving instance set is no longer recoverable (zero mirrors for
  FFv1 CSM, fewer than k shards for RS).

  Atomic API choice: declared as a **separate field**, not a bit in
  the existing `uint64_t i_state` word.  Per `.claude/standards.md`
  "Two atomic APIs in use," `i_state` is on the **grandfathered
  list** and uses GCC `__atomic_*` builtins, with the explicit rule
  "Do not add new GCC-builtin atomic fields."  C11 `_Atomic bool`
  separate field is the right precedent for fresh state.  Read with
  `atomic_load_explicit(&inode->i_broken, memory_order_acquire)`;
  set with `atomic_store_explicit(..., memory_order_release)`.
  Same shape as `ds_drained` on `struct dstore`.

  Effects (the BROKEN-aware op behaviour matrix):

  | Op | Behaviour |
  |---|---|
  | LAYOUTGET | returns `NFS4ERR_LAYOUTUNAVAILABLE` |
  | READ via MDS proxy | returns `NFS4ERR_IO` |
  | WRITE via MDS proxy | returns `NFS4ERR_IO` |
  | CHUNK_READ / CHUNK_WRITE / CHUNK_COMMIT | return `NFS4ERR_IO` |
  | GETATTR | succeeds normally; metadata is intact (admin's discovery path via `INODE_LIST_BROKEN`) |
  | SETATTR(size) | returns `NFS4ERR_IO` (truncate cannot reach DS bytes) |
  | SETATTR(mode/uid/gid/times) | succeeds (MDS-only metadata) |
  | ACCESS | succeeds; returns supported-access bits regardless of brokenness |
  | OPEN | succeeds (admin needs OPEN to drive REMOVE) |
  | CLOSE | succeeds (releases stateids regardless) |
  | REMOVE / RMDIR | succeeds; the BROKEN bit is sticky for the inode's lifetime, so removal IS the clearing path |
  | RENAME | succeeds (namespace operation, no data touch); renamed inode keeps its BROKEN bit |
  | LINK | succeeds; the new dirent points at the same BROKEN inode |
  | LAYOUTRETURN | succeeds (releases layout stateids, doesn't depend on data) |
  | LAYOUTERROR | succeeds (clients can still report errors against BROKEN inodes) |

  Sticky for first impl: cleared only by REMOVE of the inode.  See
  NOT_NOW_BROWN_COW for "BROKEN clearing path" -- a future op could
  unset the bit if data becomes recoverable (e.g., dstore came back).

### New on-dstore field

- `_Atomic uint64_t ds_instance_count` on `struct dstore` -- cached
  total number of (sb, inum) entries indexed under this dstore across
  all SBs.  Authoritative state lives in the persistent reverse index
  (below); the counter is a fast path for `DSTORE_INSTANCE_COUNT` and
  `DSTORE_DESTROY` admission control.  Memory order is `relaxed` for
  the hot path; the persistent index is the source of truth.

- `_Atomic bool ds_drained` -- when set, LAYOUTGET / runway-pop
  excludes this dstore.  Persistence: see "Drain persistence" below.

  Atomic API choice: declared as a **separate field**, not a bit in
  the existing `uint64_t ds_state` word at dstore.h:79.  `ds_state` is
  accessed today via GCC `__atomic_*` builtins and is NOT in the
  grandfathered list in `.claude/standards.md` "Two atomic APIs in
  use" -- adding new bits there would perpetuate a non-conformant
  idiom for new code.  C11 `_Atomic` separate field is the right
  precedent for fresh state.  Read with
  `atomic_load_explicit(&ds->ds_drained, memory_order_acquire)`;
  toggle with `atomic_store_explicit(..., memory_order_release)`.

### Persistent reverse index (per SB, per dstore)

For every (sb, dstore_id) pair the SB maintains the set of inums whose
layout includes a mirror on that dstore.  Maintained inside the SB's
backend store and atomic with the inode metadata write that placed or
removed the mirror.  See "Reverse index" section for the full design.

## Admin probe surface

Every op ships C and Python clients in the same slice (per the rule in
.claude/design/probe-sb-management.md "Rule: All probe functionality
ships in C and Python at the same time").  Authz follows existing
probe pattern -- see "Security model" below.

| Op | Args | Returns | Effect |
|---|---|---|---|
| `INODE_LAYOUT_LIST` | path-or-fh | `lss_gen`, per-mirror (dstore_id, ds_fh, size, mtime) | read-only |
| `INODE_LAYOUT_ADD_MIRROR` | path-or-fh, dstore_id, expected_gen | new instance descriptor + new lss_gen | reads bytes from existing mirror, writes to runway-popped FH on target dstore, registers in layout segment |
| `INODE_LAYOUT_REMOVE_MIRROR` | path-or-fh, dstore_id, expected_gen | new lss_gen | bracketed CB_LAYOUTRECALL on outstanding clients, waits for LAYOUTRETURN (bounded), unlinks DS file, drops layout entry |
| `INODE_LAYOUT_REPAIR_MIRROR` | path-or-fh, target_dstore_id, expected_gen | new lss_gen | (slice G) like ADD_MIRROR but the byte source is "any surviving instance," not a specific one; for RS layouts requires reconstruction from k shards via `ec_pipeline.h` (gated like slice F) |
| `INODE_LIST_BROKEN` | optional sb_id | inum list per SB | enumerate inodes flagged `INODE_BROKEN` so admin tooling / repair scripts can iterate |
| `DSTORE_LIST` | -- | per-dstore { id, address, path, state (ALIVE/DRAINING/DRAINED/LOST/DESTROYED), drained, lost, instance_count, runway_capacity } | operator dashboard; the entry point for observing the drain workflow |
| `DSTORE_DRAIN` | dstore_id | OK | sets `ds_drained` so LAYOUTGET / runway-pop skips this dstore |
| `DSTORE_UNDRAIN` | dstore_id | OK | clears `ds_drained`.  **Already-migrated files stay on their new dstores** -- the original mirror on D was already removed via REMOVE_MIRROR before the UNDRAIN, and the file's layout no longer references D.  In-flight autopilot workers detect the UNDRAIN at the post-ADD checkpoint (slice E worker step 7a) and **abort the REMOVE**; the file ends up with an extra (harmless) mirror on the target while the original mirror on D stays.  The "stop" is graceful -- no rollback of work already done. |
| `DSTORE_INSTANCE_COUNT` | dstore_id | uint64 | reads cached `ds_instance_count`; admin's fast "is the drain complete?" query (DSTORE_LIST is the dashboard form) |
| `DSTORE_LOST` | dstore_id, force | counts: { inodes_lost, inodes_repairable, inodes_already_off } | declares the dstore unrecoverable (data on it is gone -- network failure, disk loss, decommission without drain).  Distinct from DRAIN: DRAIN reads bytes off; LOST has no bytes to read.  Triages every inode in the reverse index: marks those with no surviving recoverable instance as `INODE_BROKEN`, drops the dead instance from layouts (skipping CB_LAYOUTRECALL because the dstore is dead anyway).  Refuses with `PROBE1ERR_BUSY` if any file would become BROKEN unless `force=true` (admin must opt into data loss). |
| `DSTORE_DESTROY` | dstore_id | OK or `PROBE1ERR_BUSY` | rejected if `ds_instance_count > 0` AND state != LOST; otherwise tears down dstore and removes its index across all SBs.  Allowed on LOST dstores even with non-zero count if all remaining inodes are BROKEN (admin already accepted the loss via DSTORE_LOST(force=true)). |
| `INODE_LAYOUT_CHANGE_CODEC` | path-or-fh, new_coding_type, new_k, new_m, expected_gen | new lss_gen | (slice F) re-encodes file bytes, swaps layout segment atomically |

Op number range: probe ops **28-39** (subject to renumber if other
designs land first).  Tentative assignment: 28=INODE_LAYOUT_LIST,
29=INODE_LAYOUT_ADD_MIRROR, 30=INODE_LAYOUT_REMOVE_MIRROR,
31=INODE_LAYOUT_REPAIR_MIRROR, 32=INODE_LIST_BROKEN, 33=DSTORE_LIST,
34=DSTORE_DRAIN, 35=DSTORE_UNDRAIN, 36=DSTORE_INSTANCE_COUNT,
37=DSTORE_LOST, 38=DSTORE_DESTROY, 39=INODE_LAYOUT_CHANGE_CODEC.
XDR commit at the slice-A integration locks the numbers in.  Slice
B ships ops 33-35 (`DSTORE_LIST`, `DSTORE_DRAIN`, `DSTORE_UNDRAIN`);
slice B'' ships op 36 (`DSTORE_INSTANCE_COUNT`).  Existing
assignments: 13-20 (probe-sb-management.md), 25
(per-export-dstore.md), 26 (export-policy.md), 27 (proxy-server.md
SB_GET_CLIENT_RULES).

CLI naming on `reffs-probe.py`:

```
reffs-probe.py inode-layout-list          --path /foo
reffs-probe.py inode-layout-add-mirror    --path /foo --dstore-id 5 --expected-gen 17
reffs-probe.py inode-layout-remove-mirror --path /foo --dstore-id 5 --expected-gen 18
reffs-probe.py inode-layout-repair-mirror --path /foo --target-dstore-id 7 --expected-gen 19
reffs-probe.py inode-list-broken          [--sb-id 2]
reffs-probe.py dstore-list                            # operator dashboard
reffs-probe.py dstore-drain               --id 5
reffs-probe.py dstore-undrain             --id 5
reffs-probe.py dstore-instance-count      --id 5
reffs-probe.py dstore-watch               --id 5      # CLI sugar -- polls instance-count
reffs-probe.py dstore-lost                --id 5 [--force]
reffs-probe.py dstore-destroy             --id 5
reffs-probe.py inode-layout-change-codec  --path /foo --codec rs --k 4 --m 2 --expected-gen 20
```

## Reverse index

This is the load-bearing infrastructure piece.  Without it, every
drain query (autopilot scan or admin progress check) is O(all inodes
across all SBs) -- minutes on a real deployment.  With it, a drain
query is O(matching entries), measured in milliseconds.

### Vtable surface

New ops on `reffs_md_ops` (the metadata-side template, per
.claude/design/rocksdb-composition.md).  The composer in
`lib/backends/driver.c` copies them into the top-level
`reffs_storage_ops` exactly the way `chunk_persist` / `chunk_load` are
copied today.

```c
struct reffs_md_ops {
    /* ... existing ... */
    int (*dstore_index_add)(struct super_block *sb,
                            uint32_t ds_id, uint64_t inum);
    int (*dstore_index_remove)(struct super_block *sb,
                               uint32_t ds_id, uint64_t inum);
    int (*dstore_index_iter)(struct super_block *sb, uint32_t ds_id,
                             int (*cb)(uint32_t ds_id, uint64_t inum,
                                       void *arg),
                             void *arg);
    int (*dstore_index_count)(struct super_block *sb, uint32_t ds_id,
                              uint64_t *count_out);
};
```

The iter callback receives `ds_id` so the autopilot can scan multiple
dstores in one outer loop and disambiguate.

### Per-backend implementation

| Backend | Storage | Atomicity with layout write |
|---|---|---|
| RocksDB | New CF `dstore_inode_idx`, key `<ds_id_BE><inum_BE>` (4 + 8 = 12 bytes), empty value | `WriteBatch` containing both the `layouts:lay:<inum>` put AND the `dstore_inode_idx` put -- one WAL fsync, atomic |
| POSIX | `sb_<id>/dstore_idx_<ds_id>/` directory of empty files named `<inum>` | Strict ordering rule (below) leaves only extra entries on crash, never missing ones |
| RAM | Per-(sb, ds_id) `cds_lfht` keyed by inum (entries are leaf nodes -- no embedded refs to outside objects -- so lifecycle is creation-ref-only per `.claude/patterns/ref-counting.md` Rule 6; iterators advance before put per `.claude/patterns/rcu-violations.md` Pattern 7; the `dstore_index_iter` callback runs UNDER `rcu_read_lock` and therefore must be non-blocking -- no malloc, no mutex_lock, no I/O -- the autopilot worker queue add must use a lock-free enqueue or the iterator must collect inums and dispatch after dropping the read lock) | None (no persistence; resets on restart matches RAM semantics) |

POSIX backend deferred to a slice-internal follow-up commit (matching
the `chunk_persist` precedent: POSIX backend's `chunk_persist` is NULL
today; flat-file fallback covers it).  Slices C/D/E integration tests
run against the RAM backend, which is sufficient for unit coverage.

### Atomicity contract

> **The dstore index ops MUST live in the same WriteBatch as the inode
> metadata write that introduces or removes the corresponding mirror.**

The contract is enforced via a **caller-passes-diff** mechanism, not
by reading the pre-mutation index from inside `inode_sync`.  The
naive "delete-then-put full snapshot" shape is rejected: there is no
efficient way to enumerate "all (ds_id, inum) entries for this inum"
when the index is keyed by `<ds_id_BE><inum_BE>` (an inum-keyed
secondary index would double storage), and reading the index inside
`inode_sync` races with concurrent mutations on the same dstore from
other inodes.

New md vtable entry (extends the inode-sync surface):

```c
struct reffs_md_ops {
    /* ... existing ... */

    /*
     * inode_sync_with_dstore_diff -- inode_sync variant that also
     * applies a delta to the per-(sb, ds_id) reverse index in the
     * SAME on-disk transaction as the inode metadata write.
     *
     * `adds[]` and `removes[]` carry the diff computed by the caller
     * (slice C/D handlers, inode_free hook).  `adds_len` /
     * `removes_len` may be zero for pure-metadata syncs (e.g. mode
     * change with no layout mutation).
     */
    int (*inode_sync_with_dstore_diff)(struct inode *inode,
                                       const uint32_t *adds,
                                       uint32_t adds_len,
                                       const uint32_t *removes,
                                       uint32_t removes_len);
};
```

The plain `inode_sync` stays in the vtable and is implemented as
`inode_sync_with_dstore_diff(inode, NULL, 0, NULL, 0)`.  Existing
callers (LAYOUTGET / LAYOUTRETURN / LAYOUTCOMMIT for layout creates
that DON'T add a mirror, attribute updates, etc.) keep using the
simple form.

Per-backend behavior:

- **RocksDB** (in `rocksdb_inode_sync_with_dstore_diff`, builds on
  the existing WriteBatch in `rocksdb_inode_sync` around line 267 of
  `lib/backends/rocksdb.c` -- re-grep at touch time, line numbers
  drift): after the existing `put_cf` calls for inode metadata,
  symlink, layouts, and `sb_meta_next_ino`, append one
  `rocksdb_writebatch_put_cf` per ds_id in `adds[]` (key
  `<ds_id_BE><inum_BE>`, empty value) and one
  `rocksdb_writebatch_delete_cf` per ds_id in `removes[]`, all into
  the new `dstore_inode_idx` CF.  Single `rocksdb_write` call with
  `set_sync(1)` -- one WAL fsync, atomic.

- **POSIX**: see ordering rule below.

- **RAM**: lfht insert/remove per add/remove.

- **`inode_free` vtable hook** (md_ops.inode_free, runs from
  `inode_release` BEFORE `call_rcu` -- `lib/fs/inode.c` around line
  239): the hook computes its own `removes[]` by walking
  `i_layout_segments` (still valid; freed in inode_free_rcu which
  runs after) and emits the index removes in the same WriteBatch as
  the inode metadata delete.  The hook MUST NOT live in
  `inode_free_rcu` itself -- by the time that runs the SB may be torn
  down.

How callers compute the diff:

- Slice C ADD_MIRROR: `adds = [target_ds_id]`, `removes = []`.
- Slice D REMOVE_MIRROR: `adds = []`, `removes = [doomed_ds_id]`.
- Slice F INODE_LAYOUT_CHANGE_CODEC: snapshot pre-mutation dstore
  set, snapshot post-mutation dstore set, set-difference both ways.
- `inode_free`: snapshot the full pre-free dstore set into
  `removes`.

### POSIX ordering rule

POSIX has no transaction primitive across two file objects.  The rule:

> **In both add and remove, write `.layouts` FIRST, then operate on the
> index entry SECOND.**

Failure modes:

- **Crash after `.layouts` rename, before `creat()` of the new index
  entry**: layout has the new mirror; index doesn't.  Drain query for
  D under-reports by one.  Self-healing: the next mutation to this
  inode will rewrite `.layouts` (full snapshot) and re-emit all index
  entries, restoring consistency.  Even with no future mutation, the
  autopilot's "no instance to migrate" no-op on the missing entry is
  silent and harmless; the only visible artifact is `ds_instance_count`
  showing N-1 when truth is N.

- **Crash after `.layouts` rewrite that drops mirror D, before
  `unlink()` of the index entry**: layout no longer has the mirror;
  index still does.  Drain query for D over-reports by one.
  Self-healing: autopilot tries to migrate the inode-on-D, discovers
  the inode's layout doesn't include D, no-ops, removes the orphan
  entry.

The asymmetric direction -- "remove the index entry first, then
rewrite `.layouts`" -- creates a real silent drain-completion bug:
the inode disappears from the index while still having an instance on
D, and if no future mutation happens, the autopilot will report drain
complete with an instance still on D.  The ordering rule above
forbids this.

RocksDB has no equivalent hazard: WriteBatch is atomic.

### Hot-path cache

`_Atomic uint64_t ds_instance_count` on `struct dstore` -- cached
total across all SBs.  Bumped/decremented from the same code path
that calls `dstore_index_add` / `dstore_index_remove`.  Memory order
`relaxed` for the bumps (no synchronization-with semantics required
since the persistent index is authoritative); read with `relaxed` for
the probe op.  `DSTORE_INSTANCE_COUNT` and `DSTORE_DESTROY` consult
the cache for an O(1) answer.

The cache is rebuilt at server startup by walking
`super_block_list_head()` after `sb_registry_load()` and the per-sb
`recover()` calls have populated the SB list.  For each SB, for each
known dstore, call `dstore_index_count(sb, ds_id, &n)` and
`atomic_fetch_add_explicit(&dstore->ds_instance_count, n,
memory_order_relaxed)`.  Rebuild cost is O(|SBs| x |dstores|), not
O(inodes) -- typically a few thousand operations even on a 10M-inode
deployment.

A new dstore added at runtime via probe (NOT_NOW_BROWN_COW today;
configured at startup) starts with `ds_instance_count == 0` and never
needs a rebuild.

### Drain persistence

`ds_drained` is operational state, not config.  Persisted in the
namespace database (per .claude/design/rocksdb-database.md
"Namespace Database") as a single key in the default CF, or for the
POSIX namespace as a flat file.  Survives reboots until an admin
explicitly `DSTORE_UNDRAIN`s.  TOML config remains the source of
truth for dstore identity (address, path, protocol); drain state is
layered on top.

## State machine

Per-dstore lifecycle states, transitions admin-driven via probe:

```
       (config: [[data_server]])
                 |
                 v
              ALIVE -------- DSTORE_DRAIN -------->  DRAINING
                |  ^                                     |
                |  |  DSTORE_UNDRAIN                     | autopilot
                |  +-------------------------------------+ moves
                |                                        | instances off
   DSTORE_LOST  |                                        |
                |  +----- DSTORE_LOST -------------------+
                |  |                                     v
                |  |                                  DRAINED
                |  |                            (instance_count == 0)
                v  v                                     |
                LOST                                     | DSTORE_DESTROY
                  \                                      v
                   \  DSTORE_DESTROY              DESTROYED
                    \   (only when remaining
                     \   inodes are all BROKEN)
                      ----------------------------> DESTROYED

(DESTROYED = removed from in-memory pool; still in TOML until admin
 edits.)
```

`LOST` is terminal in the sense that you cannot UNDRAIN out of it
back to ALIVE.  To restore the dstore as a placement target the
admin edits TOML to add a fresh `[[data_server]]` entry with a new
ID; old layouts that referenced the lost ID stay marked BROKEN
(or repaired onto the new ID via REPAIR).

State checks:

- ALIVE -> DRAINING: always permitted.
- DRAINING -> ALIVE: see UNDRAIN race rules below.
- DRAINING -> DRAINED: implicit when `ds_instance_count == 0`; not a
  separate admin op.  Visible via `DSTORE_INSTANCE_COUNT` returning 0
  (or `DSTORE_LIST` reporting state=DRAINED).
- DRAINED -> DESTROYED: `DSTORE_DESTROY` checks `ds_instance_count == 0`
  and `ds_drained == true`.  See DESTROY race rules below.
- ALIVE -> LOST or DRAINING -> LOST: `DSTORE_LOST(force_or_no_breakage)`.
  Always permitted; rejects with `PROBE1ERR_BUSY` if `force=false` and
  triage finds at least one inode that would become BROKEN.
- LOST -> DESTROYED: `DSTORE_DESTROY` permits this when `state == LOST`
  and remaining `ds_instance_count` consists solely of inodes flagged
  `INODE_BROKEN` (admin already accepted the data loss via
  `DSTORE_LOST(force=true)`).
- DESTROYED -> ALIVE: requires admin to remove from TOML, then re-add
  with a fresh ID, then bounce the server.  Out of scope for this
  design.

Race transitions (must be specified before slice B lands):

- **DSTORE_DRAIN on DRAINING**: idempotent.  Returns `PROBE1OK`
  without touching the autopilot worker queue.  Test:
  `test_dstore_drain_on_draining_is_noop`.

- **DSTORE_UNDRAIN mid-migration**: the autopilot may have an
  in-flight worker that just called `ADD_MIRROR(target)` and is
  about to call `REMOVE_MIRROR(D)`.  If UNDRAIN flips `ds_drained`
  to false between those two calls, the REMOVE proceeds anyway
  (worker doesn't re-check) -- the file ends up with a mirror on
  `target` the admin didn't want and the original mirror on D is
  gone.  **Required behaviour**: each autopilot worker step
  re-reads `ds_drained` AFTER the ADD and BEFORE the REMOVE; if
  the dstore is no longer drained, the worker skips the REMOVE
  and aborts (the extra mirror on `target` is harmless and the
  next LAYOUTGET treats it as a normal mirror).  Test:
  `test_undrain_mid_migration_aborts_remove`.

- **DSTORE_DESTROY race with autopilot in-flight worker**: the
  `ds_instance_count` decrements with the WriteBatch commit, but
  the dstore vtable's `unlink` on the DS file (slice D step 11)
  happens AFTER the WriteBatch.  So `ds_instance_count == 0` can
  be reached while a worker still holds an open reference to the
  dstore for the trailing unlink.  **Required behaviour**:
  DSTORE_DESTROY first sets a `ds_destroying` flag (separate
  `_Atomic bool`) that blocks new ADD/REMOVE work, then waits on
  a per-dstore condvar that the last in-flight worker signals on
  exit, then proceeds with teardown.  Test:
  `test_destroy_waits_for_inflight_workers`.

- **DRAINED -> DRAINING via new entry on a drained dstore**: by
  construction this cannot happen.  Slice C ADD_MIRROR step 2
  rejects a drained target with `-NFS4ERR_INVAL`; the autopilot
  itself never targets a drained dstore.  No state-machine
  transition needed; instead, slice B'' adds a TSan invariant
  test `test_no_index_entry_added_to_drained_ds` that asserts
  this property under concurrent admin operations.

## Drain access semantics

> **Status note (2026-04-26)**: this section was substantially
> rewritten in conversation with the draft author after the original
> design (which excluded D from RW layouts during drain) was found to
> introduce divergence races and to over-reach by overloading
> `nc_is_registered_ps` as a "trusted writer" capability.  The
> simplified design below was driven by these recognitions:
>
> 1. The PS does not introspect the file's layout state.  It
>    receives work assignments from the MDS (in `PROXY_PROGRESS`
>    poll-replies) and operates against MDS-supplied descriptors --
>    it never issues a "give me a layout for this file" call that
>    has to know about drain.
> 2. Migration consistency is achieved by keeping the draining
>    dstore as a normal CSM target throughout the migration.
>    Existing client writes flow to all mirrors including D; the
>    PS reads from D as a regular client; the swap happens
>    atomically at PROXY_DONE time via a two-layout commit.
> 3. The privilege model in slice 6b stays narrow.
>    `nc_is_registered_ps` is consumed only by namespace-discovery
>    bypass (slice 6b-ii); it is NOT extended to cover RW LAYOUTGET
>    on draining mirrors.

The DRAINING state is a window during which the dstore is still
serving I/O while the autopilot moves instances off.  The semantic
question is: which layout grants and which I/O fan-outs should
target a draining dstore, and which should be diverted?

### Goal

Live drain.  Clients should not observe `NFS4ERR_LAYOUTUNAVAILABLE`
or write failures while a drain runs in the background.  The
operator's intent is "make the dstore unused", not "shut down all
access to the dstore".

### Rules

The DRAINING state filters **only the runway**.  Everything else
operates normally:

| Path | Behavior on a draining dstore |
|------|-------------------------------|
| Runway pop (new-file placement) | **skip D** -- no new instances allocated on a draining dstore |
| OPEN on existing file with mirror on D | normal (D included in mirror set) |
| LAYOUTGET (READ or RW) | normal (D included in returned layout) |
| Client CSM WRITE | hits all mirrors including D (D stays in sync) |
| InBand WRITE fan-out | hits all mirrors including D |
| Per-file migration (ADD/REMOVE swap) | atomic via the two-layout commit (see below) |

D stays writable throughout the drain.  The drain transitions to
DRAINED when `ds_instance_count == 0`, i.e., when every existing
file's mirror on D has been migrated off.

This was a major simplification from an earlier draft of this
section.  The earlier design tried to filter LAYOUTGET (exclude D
from RW grants) and the InBand fan-out (exclude D from writes) to
"protect" D from new writes.  That introduced two problems:

1. **Append divergence** -- a new client write would land on
   {E, G} but skip D, leaving D's instance stale; if the operator
   then issued UNDRAIN, D would be a stale mirror.
2. **PS-write conflict** -- the PS itself does CSM writes during
   migration; excluding D would also exclude the PS, which broke
   the symmetry it relied on.

By keeping D as a normal CSM target throughout, both problems
disappear.  The migration's correctness comes from the two-layout
commit (next subsection), not from filtering writes.

### Two-layout state for in-flight migrations

For each file F whose mirror on D is being migrated, the MDS
persists **three logical layout records** in the inode while
migration is in flight:

- `L1` -- the **active** layout for external clients.  Mirror set
  includes D unchanged: `{D, E, ...}`.  All external traffic
  (existing layouts in client caches, fresh LAYOUTGETs) sees L1.
- `L2` -- the **candidate** post-migration layout.  Mirror set
  has D replaced by the target G: `{E, G, ...}`.  L2 is not
  visible to external clients during the migration window.
- `L3` -- the **composite** layout served only to the registered
  PS that owns the migration.  L3 has TWO mirror entries the PS
  must understand:
  - `M1` (read source): the current L1 mirror set `{D, E, ...}`
  - `M2` (write target): the augmented set `{D, E, G, ...}` --
    every L1 mirror PLUS G

  The PS reads source bytes from any mirror in `M1`; writes via
  CSM to every mirror in `M2`.  Note `M2` includes D and E, NOT
  just G -- this is what keeps D in sync with concurrent external
  client writes (the PS doing CSM to D+E+G, and external clients
  doing CSM to D+E via L1, converge on the same byte image).

When the migration commits (`PROXY_DONE(OK)`), the MDS atomically:

1. Flips the active layout to L2 (D dropped, G promoted)
2. Drops the L1 record
3. Drops the L3 composite (PS no longer needs the migration view)
4. Issues `CB_LAYOUTRECALL` to external clients holding cached L1
5. Internal `REMOVE_MIRROR(D)` is **deferred** until all L1
   layouts have been recalled and returned (or expired)

Step 5's deferral matters: a client holding a cached L1 may have a
WRITE in flight that lands on D *after* the L1->L2 swap.  If
`REMOVE_MIRROR(D)` ran immediately on the swap, the late write
could arrive at D after D had been unlinked, producing a
NFS3ERR_STALE on the client's CSM (visible client failure).  The
mitigation is to gate `REMOVE_MIRROR(D)` on "no outstanding L1
layout for this file":

- After step 4 (CB_LAYOUTRECALL fired), the autopilot worker
  parks the file in a pending-remove queue.
- Each LAYOUTRETURN that drops an L1 layout for this file
  decrements the file's "L1 holders" counter.
- When the counter reaches zero (or one lease period elapses with
  CB_LAYOUTRECALL declined, see slice E for the force-after-2-
  lease-periods rule), the autopilot runs the deferred
  `REMOVE_MIRROR(D)`.

Late writes that arrive at D between step 1 and step 5 are
**harmless**: they land on D's instance which is being unlinked;
the bytes are dropped with the unlink.  The client sees the write
succeed (CSM to D succeeded *before* unlink) and the data is
present on the surviving mirrors {E, G}.  No data loss; no client
visible error.

If a late write arrives **after** D's instance is unlinked (rare:
client held its L1 cache past CB_LAYOUTRECALL), the write to D
returns NFS3ERR_STALE to the client.  The client retries CSM with
its cached layout; on next LAYOUTGET it gets L2 and writes only
to {E, G} thereafter.  This is a recoverable transient error, not
data loss.

When the migration fails (`PROXY_DONE(FAIL)` or `PROXY_CANCEL`),
the MDS atomically:

1. Keeps L1 active unchanged
2. Drops L2 (G is dropped from the candidate; the half-filled G
   instance is unlinked)
3. Drops L3
4. No CB_LAYOUTRECALL needed (external clients never saw L2)

In the steady state during migration:

- **External clients writing to F**: write CSM to L1's mirror set
  `{D, E}`.  D and E stay in sync via this CSM.
- **PS writing to F**: writes CSM to L3.M2 = `{D, E, G}`.  D and
  E receive the same writes external clients would issue (so no
  divergence).  G fills as the PS catches up from `M1` reads.
- **Concurrent client writes during PS catch-up**: land on D and
  E via L1; the PS's parallel CSM to D, E, AND G means G receives
  the live writes too.  G converges without explicit reconciliation.
- **Reads from F (any caller)**: served from any mirror in L1
  `{D, E}`; never from G during migration (G might still be
  catching up).

### Pinned definitions (avoid framing drift)

- **L1.mirrors = the file's pre-migration mirror set** including D
- **L2.mirrors = (L1.mirrors - {D}) + {G}**
- **L3.M1 = L1.mirrors** (PS's read source set)
- **L3.M2 = L1.mirrors + {G}** (PS's write target set; STILL
  includes D because D must remain in sync until commit)

If a future doc rev needs to refer to "the PS's write set" or
"the post-migration set", use the symbols above to avoid the
ambiguity an earlier draft of this section had.

When the PS believes G has the full byte image, it issues
`PROXY_DONE(layout_stid, status=OK)`.  The MDS atomically swaps
the active layout L1 -> L2.  External clients with cached layouts
get LAYOUTRECALL on next access (or naturally re-LAYOUTGET on
expiry), receive L2, write to {E, G} thereafter, never touch D
again.  Internal `REMOVE_MIRROR(D)` runs from the swap moment.

If the swap is interrupted by an MDS reboot, the persisted
L1+L2+migration_owner state lets the MDS recover: the PS reclaims
its layout via standard NFSv4 reclaim, MDS recognises the
in-flight migration, PS continues from where it was.  The DS
holds the bytes the PS already wrote to G; nothing is lost.

### What the PS does NOT need

The two-layout model lets the drain semantics avoid:

- **No CB_PROXY_* receive infrastructure on the PS.**  Work
  assignments arrive in `PROXY_PROGRESS` poll-replies on the PS's
  outbound MDS session.  Pure fore-channel.
- **No new claim type.**  PS uses normal `OPEN(CLAIM_NULL)` /
  `OPEN_RECLAIM(CLAIM_PREVIOUS)`.  The MDS recognises the
  registered-PS session and serves the L3 composite layout instead
  of a normal RW layout.
- **No new stateid type.**  The layout stateid issued by the
  MDS for L3 *is* the per-move identifier.  The MDS keys its
  persisted migration record on the layout stateid.
- **No `nc_is_registered_ps` overload.**  Slice 6b's privilege
  flag stays narrow (namespace-discovery bypass).  The migration
  layout grant is gated by session identity (registered-PS) only.
- **No write barrier per migration window.**  CSM-during-catch-up
  converges; concurrent writes are not lost; no client stall.

### What the runway DOES still skip

`runway_pop` skips a draining dstore unconditionally (slice B).
This applies to ALL callers (including the PS).  The runway is
for fresh-file creation -- a dstore that's being drained should
not receive new files, since they would immediately need
migration off.

### Drain race with `[[allowed_ps]]` allowlist edit

If the operator removes the PS from the allowlist mid-drain,
in-flight `PROXY_PROGRESS` polls and `PROXY_DONE` commits keep
working: the privilege check is `nc_is_registered_ps`, a per-client
flag set at PROXY_REGISTRATION time, not a per-op recheck against
the live allowlist.  Mid-drain allowlist edits are a sharp tool;
the worst case is a slightly-delayed loss of privilege after the
PS's session expires.  Operator wanting an immediate revoke
restarts reffsd, which kills the PS's session and forces
re-registration against the new allowlist.

### Drain races with DSTORE_LOST

A draining dstore that fails mid-drain (network partition, disk
unrecoverable) transitions DRAINING -> LOST via `DSTORE_LOST`.
Failure shapes, all handled by existing infra:

- **Source-read fail** (PS doing CHUNK_READ from D as part of
  catch-up): returns `-EIO` / `-ENXIO`.  PS reports
  `PROXY_DONE(layout_stid, status=FAIL)`.  MDS commits back to L1
  untouched.  DSTORE_LOST pass 2 owns the index cleanup.
- **Target-write fail** (PS doing CHUNK_WRITE to G): returns
  `-EIO` / `-ENXIO`.  Same `PROXY_DONE(FAIL)` + L1 commit.  G is
  abandoned; a future autopilot pass picks a different target.

The MDS's atomic-rollback semantics on `PROXY_DONE(FAIL)` mean
no partial state escapes to clients.

### Reboot recovery

The MDS persists three independent things to make reboot recovery
work:

1. **Client identity** -- normal NFSv4 client-state persistence
   (RFC 8881 S8.4).  The PS's `client_owner4` + assigned
   `clientid4` survive MDS reboot.  This is not new: reffs already
   persists clients in the namespace DB
   (`lib/fs/client.c`) and the PS shows up as a regular client.
2. **The PS-registration attribute** -- on rejoin, the MDS knows
   the PS was previously registered (`nc_is_registered_ps == true`
   on the persisted `nfs4_client` record).  The PS does NOT need
   to re-issue PROXY_REGISTRATION first.
3. **In-flight migration records** -- per file currently being
   migrated:
   `{file_FH, source_dstore, target_dstore, owning_PS_clientid,
   started_at}`.  Lives in the namespace DB alongside (1) and (2).
   Note: keyed on `clientid` (which is stable across PS-process
   restarts as long as the PS reuses its `client_owner4`), NOT on
   the layout stateid (which is volatile per-boot).

Recovery sequence:

1. MDS reloads (1)+(2)+(3) before granting any reclaim ops.
2. PS reconnects.  EXCHANGE_ID with its prior `client_owner4`
   recovers the same `clientid4` (slice 4b client recovery
   already does this for normal NFSv4 clients; PS is no special
   case).  The MDS sees `nc_is_registered_ps == true` from the
   persisted record.
3. PS issues `RECLAIM_COMPLETE` after CREATE_SESSION.
4. **Per-file recovery** for each in-flight migration uses
   standard NFSv4 reclaim semantics, with one PS-side
   persistence requirement:

   - **PS persists each migration's layout stateid locally** when
     the MDS grants L3.  Lives in PS-side state (a small sidecar
     file or DB table keyed by file FH).  This is the only PS-side
     persistence the data mover requires; without it the PS cannot
     reclaim the layout after PS-process restart, and the
     migration is abandoned.
   - PS issues `OPEN_RECLAIM(CLAIM_PREVIOUS, file_FH)` per RFC
     8881 S9.11.1 / S10.2.1.  The MDS validates that the prior
     `clientid4` had an OPEN on this file (which it did -- the
     OPEN was created via the PROXY_PROGRESS-reply assignment
     handler).  MDS re-grants the OPEN.
   - PS issues `LAYOUTGET(reclaim=true)` per RFC 8881 S18.43.3,
     supplying the previously-persisted layout stateid as the
     reclaim key.  The MDS validates that:
     - The session's client has `nc_is_registered_ps == true`
     - A persisted in-flight migration record exists for this
       `(clientid, file_FH, layout_stateid)` triple
     - The reclaim falls within the grace window
     and re-grants the L3 composite with a fresh layout stateid
     (the stateid the PS supplied is consumed; a new one is
     issued for the resumed migration session).
   - This is exactly the standard NFSv4 layout reclaim path; no
     new claim type or wire signal is required.  The PS-side
     persistence of the layout stateid is the only protocol
     surface the PS must implement beyond what a normal
     layout-holding NFSv4 client does.
5. PS continues the migration from wherever it left off.  Bytes
   already on G are preserved (DS holds them); PS reads remaining
   bytes from D, writes to G.
6. Eventually `PROXY_DONE` fires; commit completes.

**Ordering**: standard NFSv4 reclaim (RECLAIM_COMPLETE) MUST
precede any per-file migration reclaim above.  PROXY_REGISTRATION
is NOT re-issued -- the persisted `nc_is_registered_ps` flag
makes it implicit.  If the PS's identity rotated (different
`client_owner4` because PS process state was lost), it must
PROXY_REGISTRATION fresh; but then the in-flight migration
records keyed on the OLD clientid don't match the NEW one,
recovery fails (NFS4ERR_NO_GRACE), and the autopilot retries the
moves as fresh assignments.

If the persisted migration record cannot be matched (e.g., MDS
lost it in a state corruption), the per-file reclaim returns
`NFS4ERR_NO_GRACE` or `NFS4ERR_RECLAIM_BAD`; PS reports the move
as failed via PROXY_DONE(FAIL); autopilot retries on a fresh
`PROXY_PROGRESS` poll with a new target dstore.

### Implementation pointers

The drain semantics themselves are nearly trivial after this
simplification:

1. **Runway pop** (slice B, already done): skip drained dstores.
2. **LAYOUTGET (lib/nfs4/server/layout.c)**: no change required.
   D stays in normal layouts.
3. **InBand WRITE fan-out (lib/nfs4/server/file.c)**: no change
   required.  D stays in normal fan-out.

The complexity moves to the two-layout commit machinery (separate
section, slice E):

4. **Per-inode migration record** persisted in the namespace DB.
5. **Two-layout state** in the inode (L1 active, L2 candidate).
6. **PS-recognising LAYOUTGET path**: when a registered-PS session
   does LAYOUTGET on a file with an in-flight migration record
   owned by that session, return L3 composite instead of L1.
7. **PROXY_DONE handler**: on success, swap active layout L1->L2,
   delete migration record, internal REMOVE_MIRROR(D); on failure,
   keep L1, drop L2/G via internal cleanup.
8. **Reclaim recognition**: on OPEN_RECLAIM/LAYOUTGET-reclaim from
   a registered-PS session, look up persisted migration records
   keyed by `(session_principal, file_FH)`, re-grant if found.

LOC estimate: ~600-1000 LOC across slices E + 6c-iii (PROXY_DONE)
+ 6d (reclaim recognition).  This is its own slice ladder, tracked
in proxy-server-phase6c.md.

### Tests

Add to slice E (drain autopilot) and slice 6c-iii (PROXY_DONE):

| Test | Intent |
|------|--------|
| `test_drain_layoutget_normal_includes_d` | LAYOUTGET on a file with mirror on D returns D in the layout (READ and RW iomode) |
| `test_drain_inband_write_includes_d` | InBand WRITE fan-out hits D |
| `test_drain_csm_keeps_d_in_sync` | A series of client writes during drain leave D, E, G all bit-identical |
| `test_migration_ps_layoutget_returns_l3` | Registered-PS session's LAYOUTGET on a file with in-flight migration returns L3 composite (M1=L1, M2=L2 mirrors) |
| `test_migration_proxy_done_ok_commits_l2` | After PROXY_DONE(layout_stid, OK), the inode's active layout flips to L2 (D removed); subsequent LAYOUTGETs return L2 |
| `test_migration_proxy_done_fail_keeps_l1` | After PROXY_DONE(layout_stid, FAIL), the inode's active layout stays L1 (D retained); G is dropped from inode state |
| `test_migration_proxy_cancel_keeps_l1` | PROXY_CANCEL has the same effect as PROXY_DONE(FAIL) |
| `test_migration_concurrent_csm_during_catchup` | Client writes during PS catch-up land on G via PS's CSM; final state is consistent |
| `test_migration_reboot_recovery` | MDS restart mid-migration; PS reclaims via OPEN_RECLAIM(CLAIM_PREVIOUS); migration completes |
| `test_migration_reboot_lost_record` | MDS lost migration record during reboot; PS reclaim returns NO_GRACE; PS retries via fresh PROXY_PROGRESS poll |
| `test_undrain_layoutget_unchanged` | Drain has no effect on LAYOUTGET; UNDRAIN is observable only via the autopilot stopping new work assignments |
| `test_drain_dstore_lost_midflight` | DSTORE_LOST during PS migration: source-read returns -EIO; PS reports PROXY_DONE(FAIL); L1 commits |

### RFC alignment

- RFC 8435 S5.1 (Flex Files layout iomode semantics)
- RFC 8435 S5.4 (mirror repair / Flex Files mirroring writes)
- RFC 8881 S8.4 (state recovery / RECLAIM_COMPLETE / grace period)
- RFC 8881 S9.11.1 (CLAIM_PREVIOUS reclaim preconditions)
- RFC 8881 S10.2.1 (recovery from server reboot)
- RFC 8881 S13.6 (layout iomode and CSM write semantics; gives
  the framing for L3.M2 carrying both source-readable and write-
  target mirror entries)
- RFC 8881 S18.43 (LAYOUTGET; S18.43.3 specifically for the
  reclaim variant -- and the data-mover-specific PS reclaim
  divergence from it, see "Reboot recovery" subsection above)
- RFC 8881 S18.51 (LAYOUTRETURN; relevant to the L1->L2 swap
  + deferred REMOVE_MIRROR(D) gate on L1 holders)
- RFC 8881 S20.3 (CB_LAYOUTRECALL semantics for the L1 recall
  path during the swap)
- draft-haynes-nfsv4-flexfiles-v2 (FFV2 layout body / mirror
  semantics)
- draft-haynes-nfsv4-flexfiles-v2-data-mover (PS protocol; **needs
  draft updates**, see proxy-server-phase6c.md)


## Slice ladder

Each slice is a single git topic branch with one or more commits,
ff-merged to main.  Sizing is S/M/L per .claude/roles.md planner
conventions.

| Slice | Size | Depends on |
|---|---|---|
| A: `INODE_LAYOUT_LIST` (read-only) | S | -- |
| B': `lss_gen` counter | S | -- |
| B: `DSTORE_DRAIN` / `UNDRAIN` flag + `DSTORE_LIST` (no migration yet) | S | -- |
| B'': persistent reverse index (vtable + RAM + RocksDB + cache + rebuild + `DSTORE_INSTANCE_COUNT` probe) | M | B' |
| C: `INODE_LAYOUT_ADD_MIRROR` (sync, MDS-inline) | M | B', B'' |
| D: `INODE_LAYOUT_REMOVE_MIRROR` + bracketed CB_LAYOUTRECALL wait | L | B', B'' |
| E: PS-driven drain autopilot | M | B, B'', C, D |
| F: `INODE_LAYOUT_CHANGE_CODEC` | M | gated on `lib/nfs4/ps/ec_pipeline.h` extraction (proxy-server.md action item 1) |
| G1: `DSTORE_LOST` + `INODE_BROKEN` + `INODE_LIST_BROKEN` + LOST state + BROKEN-aware op behaviour matrix + `ldf_slot` field | M | B'', C (for the layout-diff machinery), D (for the layout-rewrite-without-recall path) |
| G2: `INODE_LAYOUT_REPAIR_MIRROR` (MIRRORED + RS variants) | M | G1; the RS-reconstruct path is gated on the same `ec_pipeline.h` extraction as F (MIRRORED-only repair can ship without it as a sub-slice G2a) |

Slice D is the load-bearing infra slice: the existing
`nfs4_cb_layoutrecall_send` (lib/nfs4/server/cb.c around line 358) has
zero callers today, so D builds the first caller plus the
wait-for-LAYOUTRETURN condvar / per-inode wait infra.

### Slice A: INODE_LAYOUT_LIST

Tests:
- `test_layout_list_no_segments` -- inode with no layout returns empty
- `test_layout_list_returns_all_files` -- N mirrors, all reported
- `test_layout_list_returns_gen` -- gen field present and stable
  across two reads on an unchanged inode

Probe op: `INODE_LAYOUT_LIST { path-or-fh } -> { lss_gen, mirrors[] }`
where each mirror is `(dstore_id, ds_fh, size, mtime)`.

Persistence: none.

RFC: RFC 8881 S12.2 (layout taxonomy), RFC 8435 S5 (FlexFiles
structure).

### Slice B': lss_gen counter

Tests (in `lib/fs/tests/layout_segment_test.c`):
- `test_layout_segments_gen_starts_at_zero`
- `test_layout_segments_gen_bumps_on_add`
- `test_layout_segments_gen_persists_across_inode_sync`

Persistence: yes -- new `uint64_t lss_gen` in the on-disk
`layout_segments` representation.  No SB_REGISTRY version bump
(CLAUDE.md "Deployment Status: No persistent storage has been
deployed."  Format stays v1).

### Slice B: DSTORE_DRAIN / UNDRAIN + DSTORE_LIST

Tests (in `lib/nfs4/dstore/tests/dstore_test.c` and a new
`lib/nfs4/server/tests/layoutget_drain_test.c` if needed):
- `test_dstore_drain_excludes_from_collect_available`
- `test_dstore_undrain_restores`
- `test_dstore_drain_persists_across_restart`
- `test_layoutget_skips_drained_ds`
- `test_layoutget_layoutunavailable_when_all_drained`
- `test_dstore_drain_on_draining_is_noop` (race transition)
- `test_dstore_list_returns_all_states` (ALIVE / DRAINING / DRAINED;
  LOST / DESTROYED test added in slice G when those states exist)
- `test_dstore_list_includes_runway_capacity`

Probe ops:
- `DSTORE_DRAIN { id }`
- `DSTORE_UNDRAIN { id }` -- with the "already-migrated stays;
  in-flight aborts at post-ADD" semantics surfaced in the op table
- `DSTORE_LIST { } -> per-dstore { id, address, path, state, drained,
  lost, instance_count, runway_capacity }`.  Reads `ds_instance_count`
  with `memory_order_relaxed` (matches the cache's relaxed-bumps
  semantics).  Reads `runway_capacity` via the existing
  per-runway accessor.  The snapshot is per-dstore, not transactional
  across dstores -- concurrent autopilot mutations may show counts
  that are seconds out of date, which is fine for a dashboard view.

Persistence: drain bit in namespace DB.  `DSTORE_LIST` is read-only,
no persistence.

NOT_NOW_BROWN_COW: drain timeout / auto-undrain.

### Slice B'': persistent reverse index

This is the slice the iteration-2 review reshaped.  Lands as one M
slice with the following internal commit ladder (matching the
`chunk_persist` precedent: vtable + RAM + RocksDB; POSIX deferred):

| # | Commit |
|---|---|
| i | `reffs_md_ops` vtable additions (add/remove/iter/count) + composer wiring; stubs return -ENOSYS |
| ii | RAM impl + unit tests (covers test harness needs for slices C/D/E) |
| iii | RocksDB impl + WriteBatch integration in `rocksdb_inode_sync` + tests including a fault-injection test that verifies the index entry is absent when the WriteBatch is constructed but not yet committed |
| iv | `inode_free` md hook to scrub indexes (same WriteBatch as inode-delete) |
| v | Startup `ds_instance_count` rebuild walking `super_block_list_head()` after `sb_registry_load` |
| vi | Probe op `DSTORE_INSTANCE_COUNT(ds_id)` (C + Python clients) |

Tests:
- `test_dstore_index_insert_on_layout_add`
- `test_dstore_index_remove_on_layout_drop`
- `test_dstore_index_scrubbed_on_inode_free`
- `test_dstore_index_count`
- `test_dstore_index_iter_callback_receives_ds_id`
- `test_dstore_index_rebuilt_on_startup` (round-trip: start, populate,
  restart, verify cache matches sum of per-SB iter counts)
- `test_dstore_instance_count_atomic` (TSan: concurrent add/remove)
- `test_dstore_destroy_drains_index` (LSan clean)
- `test_rocksdb_inode_sync_emits_index_diff` -- write known layout +
  diff via `inode_sync_with_dstore_diff(adds, removes)`, read back
  the `dstore_inode_idx` CF, assert exactly the right keys are
  present.  This is the operational atomicity test -- a real
  "WriteBatch failure" test isn't tractable without a fault injector
  at the rocksdb library level (WriteBatch atomicity is a property
  of the underlying engine, not of our code).
- `test_rocksdb_inode_sync_writebatch_failure_leaves_no_index_change`
  -- mock `rocksdb_write` returns error; verify no entries in the
  index CF afterwards (defensive test for the engine behaviour we
  rely on)

POSIX impl deferred (slice-internal follow-up).  Cite chunk_persist
precedent in the deferral comment.

NOT_NOW_BROWN_COW:
- POSIX backend impl
- Orphan index entries when a dstore is removed from TOML config
  (admin would need an fsck-style scan; today's DSTORE_DESTROY only
  works through the drain path)

### Slice C: INODE_LAYOUT_ADD_MIRROR

Tests:
- `test_add_mirror_appends_data_file`
- `test_add_mirror_bumps_gen`
- `test_add_mirror_rejects_stale_gen` (TOCTOU defence)
- `test_add_mirror_runway_empty_returns_busy`
- `test_add_mirror_dstore_drained_returns_inval`
- `test_add_mirror_file_layout_sb_returns_notsupp` (per
  .claude/design/per-export-dstore.md "file layouts = single DS per
  export")
- `test_add_mirror_persists` (round-trip)
- `test_add_mirror_partial_failure_unlinks_target` (orphan cleanup)
- `test_add_mirror_inserts_index_entry` (B'' integration)

Op flow:

1. Validate path-or-fh, resolve inode.
2. Validate target dstore exists and is not drained
   (`-NFS4ERR_INVAL`).
3. Validate this is not a file-layout-only SB
   (`-NFS4ERR_NOTSUPP`).
4. `runway_pop` on target dstore.  If empty, return
   `PROBE1ERR_BUSY`.
5. `dstore_data_file_fence` + chmod on the new file (mds.md fencing
   triggers).
6. Read bytes from one of the existing mirrors via the dstore vtable
   (mds.md / dstore-vtable-v2.md `read` op).
7. Write bytes to the new file on target dstore.
8. Take `i_attr_mutex` on the inode.
9. Verify `lss_gen == expected_gen`; if not, drop mutex, unlink the
   new DS file, return `PROBE1ERR_STALE`.
10. Append a new `layout_data_file` to the layout segment, bump
    `lss_gen`.
11. `inode_sync_to_disk(inode)` -- this is the call that emits both
    the layout write AND the `dstore_index_add` put in one
    RocksDB WriteBatch (per slice B''.iii).
12. Drop `i_attr_mutex`.
13. Reflected GETATTR fan-out to populate `ldf_size/mtime/ctime` on
    the new mirror (mds.md "GETATTR aggregation"); without it the
    new mirror reports zero size and the next aggregation triggers
    WWWL.
14. Return new `lss_gen`.

Failure rollback: if step 7 fails, the new DS file (already runway-
popped and fenced) is unlinked via the dstore vtable.  If step 11
fails, on-DS bytes exist but the layout doesn't reference them --
unlink as orphan.  Document this orphan-cleanup behaviour.

Probe op: `INODE_LAYOUT_ADD_MIRROR { path-or-fh, dstore_id,
expected_gen } -> { new_gen, ldf_dstore_id, ldf_fh }`.

Persistence: layout segments + reverse index, both via existing
`inode_sync_to_disk` path (B'' integration).

RFC: RFC 8435 S5.1 (FlexFiles mirror semantics), RFC 8881 S18.43
(LAYOUTGET).

NOT_NOW_BROWN_COW:
- Async copy with progress reporting (sync inline today; large files
  block the probe op for the duration of the byte copy)
- Cross-codec change in this op (defer to F)

### Slice D: INODE_LAYOUT_REMOVE_MIRROR + bracketed CB_LAYOUTRECALL

Tests:
- `test_remove_mirror_recalls_outstanding_layouts`
- `test_remove_mirror_waits_for_layoutreturn`
- `test_remove_mirror_timeout_fences_and_proceeds`
- `test_remove_mirror_drops_entry_and_unlinks_ds_file`
- `test_remove_mirror_bumps_gen`
- `test_remove_mirror_last_mirror_returns_inval` (don't allow
  zero-mirror layout)
- `test_remove_mirror_removes_index_entry` (B'' integration)

Op flow:

1. Validate path-or-fh, resolve inode.
2. Take `i_attr_mutex`; verify `lss_gen == expected_gen`.
3. **Orphan-cleanup early return**: if `dstore_id` is NOT present
   in `i_layout_segments` (inode's layout doesn't include this
   dstore -- POSIX over-report self-healing case, or autopilot
   targeting a stale index entry), call
   `dstore_index_remove(sb, dstore_id, inum)` directly to scrub
   the orphan, drop `i_attr_mutex`, return `PROBE1OK` WITHOUT
   bumping `lss_gen`.
4. Verify dropping this mirror leaves at least one remaining; if
   not, return `-NFS4ERR_INVAL` (or PROBE1ERR_INVAL).
5. Build the list of layout stateids on this inode (filter
   `i_stateids` lfht for `STATEID_TYPE_LAYOUT`).
6. Drop `i_attr_mutex`.
7. Call `nfs4_inode_recall_all_layouts(inode, timeout_ns)` (new
   helper, see "New infrastructure" below).  Bounded wait (default
   one lease period); if timeout, fence the credentials on the
   doomed mirror per mds.md "fencing triggers" so any in-flight
   client write gets ACCESS error from the DS.
8. Take `i_attr_mutex` again; re-verify `lss_gen` (may have been
   bumped by a concurrent op -- if so, return `PROBE1ERR_STALE`
   and abort).
9. Drop the `layout_data_file` entry from the segment, bump
   `lss_gen`.
10. Call `inode_sync_with_dstore_diff(inode, NULL, 0,
    &dstore_id, 1)` -- single WriteBatch with layout update AND
    index delete.
11. Drop `i_attr_mutex`.
12. Unlink the DS file via the dstore vtable.
13. Return new `lss_gen`.

Probe op: `INODE_LAYOUT_REMOVE_MIRROR { path-or-fh, dstore_id,
expected_gen } -> { new_gen }`.

**New infrastructure** -- this slice introduces three concretely
new artifacts that did not exist before:

a) `stateid_inode_find_layout(inode, stateid_array_out, count_out)`
   helper in `lib/nfs4/server/stateid.c`: walks `inode->i_stateids`
   filtered by `STATEID_TYPE_LAYOUT`.  Mirrors the existing
   `stateid_inode_find_open` / `stateid_inode_find_delegation` shape.

b) `pthread_cond_t i_layoutreturn_cv` field on `struct inode`,
   plus matching `pthread_mutex_t i_layoutreturn_mutex` (cannot
   reuse `i_attr_mutex` since the wait is performed without
   holding it).

c) `nfs4_op_layoutreturn` (lib/nfs4/server/layout.c around line
   1307 -- re-grep at touch time) gains a signal-on-completion
   call: after the layout stateid is removed from `i_stateids`,
   `pthread_cond_broadcast(&inode->i_layoutreturn_cv)` to wake
   every waiter on this inode.  Broadcast (not signal) because
   multiple REMOVE_MIRROR ops can be racing for different mirrors
   on the same inode.

These three pieces -- helper + condvar + signal hook -- are
collectively the "wait for LAYOUTRETURN" infrastructure that
`nfs4_cb_layoutrecall_send` (cb.c:358 -- re-grep at touch time)
has been waiting for since it was written; this slice writes its
first caller AND the wait machinery.

Additional unit tests for the new infra (beyond the listed slice-D
op tests):
- `test_stateid_inode_find_layout_filter` -- inode with mixed
  open / delegation / layout stateids returns only layout
- `test_layoutreturn_signals_waiter` -- wait blocks until
  LAYOUTRETURN fires, then unblocks
- `test_layoutreturn_broadcast_wakes_all_waiters` -- two waiters
  on same inode; one LAYOUTRETURN wakes both

RFC: RFC 8881 S12.5.5 (LAYOUTRECALL semantics), S20.3 (CB_LAYOUTRECALL),
S12.5.5.2 (recall completion).

NOT_NOW_BROWN_COW:
- Revoke-on-timeout for delegations the recall couldn't get back;
  today's flow fences-and-proceeds, accepting the in-flight write
  ACCESS error.

### Slice E: PS-driven drain autopilot

Tests:
- `test_drain_scanner_finds_inodes_using_drained_ds`
- `test_drain_autopilot_picks_undrained_target`
- `test_drain_autopilot_calls_add_then_remove`
- `test_drain_autopilot_skips_inodes_already_off_drained_ds`
- `test_drain_autopilot_atomic_count_drops_to_zero`
- `test_drain_autopilot_idempotent_on_extra_index_entries` (POSIX
  self-healing path)

Op flow:

1. For each dstore D where `ds_drained` is set:
   2. For each SB:
      3. `dstore_index_iter(sb, D, cb)` -- callback enqueues each
         (sb, D, inum) to a worker queue.
4. Each worker takes (sb, D, inum):
   5. Resolve inode via `inode_find(sb, inum)`.
   5a. **Orphan-entry fast path**: take `i_attr_mutex` briefly to
       check if D is actually present in `i_layout_segments`.  If
       not (POSIX over-report self-healing case), call
       REMOVE_MIRROR(D) -- which now takes its own orphan-cleanup
       early-return path (slice D step 3) and scrubs the index
       entry without touching the layout.  Drop mutex, proceed to
       next entry without doing ADD.
   5b. **UNDRAIN cancellation check**: re-read `ds_drained`.  If
       false (admin UNDRAINed since this entry was queued),
       discard this work item without ADD or REMOVE.
   6. Pick a target dstore: any non-drained dstore not already in
      the file's mirror set.  If none, log and skip.
   7. Call ADD_MIRROR(target).  If `PROBE1ERR_BUSY` (runway empty),
      requeue with backoff.  If the file's layout already includes
      target, skip ADD and proceed.  **If the source byte-read from
      D fails with -EIO / -ENXIO (e.g. D became LOST mid-read), do
      NOT call REMOVE_MIRROR(D)**: the autopilot abandons this work
      item and DSTORE_LOST pass 2 owns the cleanup.
   7a. **UNDRAIN cancellation check (post-ADD)**: re-read
       `ds_drained` AFTER the ADD completes and BEFORE the REMOVE.
       If false, abort -- the file ends up with an extra (harmless)
       mirror on `target`; log the abort.  This is the race
       transition specified in the State machine "DSTORE_UNDRAIN
       mid-migration" rule.
   8. Call REMOVE_MIRROR(D).
9. Drain completes when `ds_instance_count == 0`.

Probe op: optional `DSTORE_DRAIN_PROGRESS` -- returns
`{ drained, instance_count, queued, in_flight, succeeded, failed }`
for admin observability.  Or rely on `DSTORE_INSTANCE_COUNT`
returning 0.

Dependency on PS: when a PS is registered, the autopilot can
delegate the byte-shoveling part of ADD_MIRROR via the existing
`CB_PROXY_MOVE` infra (proxy-server.md phase 8).  Without a PS, the
MDS does the work inline.  First implementation: MDS-inline only;
PS delegation is a follow-up.

RFC: RFC 8435 S5.4 (mirror repair).

NOT_NOW_BROWN_COW:
- Rate limiting / parallelism cap
- Multi-PS coordination (which PS handles which inode)
- Rebalancing (vs straight drain): admin says "spread instances more
  evenly across surviving dstores" -- separate op surface

### Slice F: INODE_LAYOUT_CHANGE_CODEC

Tests:
- `test_change_codec_rs_to_mojette_sys`
- `test_change_codec_rejects_in_flight_io` (WWWL guard)
- `test_change_codec_atomic_swap`
- `test_change_codec_rejects_stale_gen`

Op flow: similar to ADD+REMOVE in spirit -- read all bytes via
existing layout, re-encode via ec_pipeline, write new shards to
runway-popped FHs on chosen dstores, swap layout segment in one
`inode_sync` (`ls_layout_type`, `ls_k`, `ls_m` plus full new
`layout_data_file` array), unlink old DS files.

Probe op: `INODE_LAYOUT_CHANGE_CODEC { path-or-fh, new_coding_type,
new_k, new_m, expected_gen } -> { new_gen }`.

**Hard dependency**: `lib/nfs4/ps/ec_pipeline.c` has no public header
today; it is reachable only from PS proxy data path and ec_demo.
Slice F MUST NOT start until proxy-server.md action item 1 (deeper
`lib/nfs4/client` refactor) has extracted `ec_pipeline.h` with a
callable API reachable from MDS-internal context.  Mark this as a
gating prerequisite.

RFC: draft-haynes-nfsv4-flexfiles-v2 S5 (FFV2 encoding types), RFC
8435 S5.1 (mirror vs encoding).

NOT_NOW_BROWN_COW:
- Codec change for files with pending CHUNK_WRITE (return
  `PROBE1ERR_BUSY`)
- Partial-range codec change (whole-file only)

### Slice G: DSTORE_LOST + REPAIR + INODE_BROKEN

Distinct from drain because the dstore is **gone** -- bytes can't be
read off it for migration.  Triage decides per-inode whether the
file is recoverable (surviving instances cover the codec's quorum)
or BROKEN (not recoverable).  Recoverable inodes are repaired by
copying / re-encoding from surviving instances onto a chosen target.

Tests:
- `test_dstore_lost_marks_inodes_broken_when_no_quorum`
- `test_dstore_lost_marks_inodes_repairable_when_quorum_holds`
- `test_dstore_lost_force_false_refuses_when_data_loss_imminent`
- `test_dstore_lost_force_true_proceeds_with_data_loss`
- `test_dstore_lost_drops_dead_instances_from_layouts` (no
  CB_LAYOUTRECALL, since the dstore is dead)
- `test_layoutget_on_broken_inode_returns_layoutunavailable`
- `test_read_through_mds_proxy_on_broken_inode_returns_io`
- `test_getattr_on_broken_inode_succeeds` (admin discovery path)
- `test_inode_list_broken_enumerates_per_sb`
- `test_repair_mirror_csm_uses_surviving_mirror`
- `test_repair_mirror_rs_reconstructs_from_k_shards` (gated on
  `ec_pipeline.h` extraction)
- `test_repair_mirror_rejects_inode_already_recoverable_without_repair`
  (REPAIR is for restoring quorum on inodes that have lost
  instances -- not for adding mirrors to healthy files; that's
  ADD_MIRROR's job)
- `test_destroy_lost_dstore_with_only_broken_inodes_succeeds`
- `test_destroy_lost_dstore_with_repairable_inodes_pending_returns_busy`

Op flow -- `DSTORE_LOST(id, force)`:

1. Take a per-dstore mutex (serialises against any concurrent
   DSTORE_LOST or DSTORE_DRAIN on the same id).
2. First pass (read-only triage): for each (sb, inum) in the
   dstore's reverse index:
   - Resolve inode; take `i_attr_mutex` briefly.
   - Compute "instances surviving without D" by walking
     `i_layout_segments` minus D's entries.
   - Classify:
     - **MIRRORED**: recoverable iff surviving mirror count > 0.
     - **RS**: recoverable iff surviving shard count >= k.
   - Drop `i_attr_mutex`.
   - Increment `inodes_repairable_pass1` or `inodes_lost_pass1`.
3. If `inodes_lost_pass1 > 0` and `force == false`: return
   `PROBE1ERR_BUSY` with the counts surfaced; admin retries with
   `force=true` after deciding to accept the loss.
4. Set `ds_lost = true` (separate `_Atomic bool` from `ds_drained`;
   LOST is sticky).  Implicitly set `ds_drained = true` so any
   in-flight LAYOUTGET / runway-pop also skips this dstore.
5. Second pass (mutating; **re-classifies under `i_attr_mutex` to
   close the pass-1-to-pass-2 race window**): for each (sb, inum):
   - Take `i_attr_mutex`.
   - **Re-compute** "instances surviving without D" -- a concurrent
     REMOVE_MIRROR(other_ds) between pass 1 and pass 2 may have
     dropped this file's last surviving instance, downgrading it
     from `repairable` to `lost`.  Re-classification surfaces the
     drift.
   - If classified `lost`: set `i_broken = true` (atomic store
     release).  Drop the dead instance entries from layout, bump
     `lss_gen`, call `inode_sync_with_dstore_diff(inode, NULL, 0,
     &D, 1)`.  No CB_LAYOUTRECALL (dstore is dead; clients fail
     naturally).  Increment `inodes_lost_pass2`.
   - If classified `repairable`: drop the dead instance entries
     from layout, bump `lss_gen`, sync.  Enqueue the inode for
     autopilot-driven REPAIR (slice E worker queue with a "repair"
     op variant).  Increment `inodes_repairable_pass2`.
   - Drop `i_attr_mutex`.
6. Drop per-dstore mutex.
7. Return counts: `{ inodes_lost_pass1, inodes_lost_pass2,
   inodes_repairable_pass1, inodes_repairable_pass2,
   inodes_already_off }`.  Admin reads `pass2` for what actually
   happened; the `pass2 - pass1` delta on `inodes_lost` surfaces
   any concurrent-mutation downgrades for visibility.

Op flow -- `INODE_LAYOUT_REPAIR_MIRROR(path-or-fh, target_dstore_id,
expected_gen)`:

1. Validate target is not drained, not lost, not the same as any
   existing instance, exists.
2. Take `i_attr_mutex`; verify `lss_gen == expected_gen`.
3. Determine source for the byte copy:
   - **MIRRORED layout**: pick any surviving mirror from
     `i_layout_segments`; bytes come from there via the existing
     dstore vtable `read` op.  Falls into ADD_MIRROR's flow.
   - **RS layout**: gather k surviving shards via the dstore
     vtable; reconstruct the original data via `ec_pipeline.h`
     decode; encode the missing shard for the target slot;
     write to runway-popped FH on target.  The shard slot the
     repair fills is determined by `target_dstore_id`'s position
     in the layout's deterministic mapping (or by the admin
     specifying a slot index in a future op variant -- first
     impl picks the first missing slot).
4. Drop `i_attr_mutex`.
5. Run the byte copy / reconstruct.
6. Take `i_attr_mutex` again; re-verify `lss_gen`.
7. Append the new `layout_data_file`, bump `lss_gen`.
8. `inode_sync_with_dstore_diff(inode, &target_dstore_id, 1,
   NULL, 0)`.
9. Drop `i_attr_mutex`.
10. Reflected GETATTR fan-out (matches ADD_MIRROR step 13).
11. Return new `lss_gen`.

Op flow -- `INODE_LIST_BROKEN(optional sb_id)`:

Read-only.  Walks per-SB inodes (or all SBs if sb_id is unset),
filters by `INODE_BROKEN` bit, returns the inum list.  For
operator scripting (e.g., bulk re-create from external backup,
or bulk REMOVE).

Probe ops:
- `DSTORE_LOST { dstore_id, force } -> { inodes_lost,
  inodes_repairable, inodes_already_off }`
- `INODE_LAYOUT_REPAIR_MIRROR { path-or-fh, target_dstore_id,
  expected_gen } -> { new_gen }`
- `INODE_LIST_BROKEN { sb_id_optional } -> [inum_list per sb]`

Persistence: layout segments + reverse index (existing
`inode_sync_with_dstore_diff` path); `INODE_BROKEN` bit lives in
`i_state` and is persisted as part of inode metadata.

State machine: this slice introduces the `LOST` state and the
`ALIVE -> LOST`, `DRAINING -> LOST`, and `LOST -> DESTROYED`
transitions.

Dependency on PS: REPAIR (especially the RS-reconstruct path) is
a natural fit for PS-driven background work (proxy-server.md phase
8 `CB_PROXY_REPAIR`).  First impl is MDS-inline; PS delegation is
a follow-up matching the slice E pattern.

RFC: RFC 8881 S12.5.5 (LAYOUTRECALL), RFC 8435 S5.4 (mirror
repair), draft-haynes-nfsv4-flexfiles-v2 S5 (FFV2 codec semantics
for RS reconstruction).

NOT_NOW_BROWN_COW:
- `INODE_BROKEN` clearing path (today: sticky until REMOVE)
- Alerting integration (admin must poll `INODE_LIST_BROKEN`; no
  push notification)
- Slot-specific REPAIR (today: fills first missing slot)
- Multi-dstore-loss recovery (loss of two dstores within one RS
  layout's k+m where k is no longer met -- BROKEN; no automatic
  attempt to combine partial recoveries)
- LOST -> DRAINED transition (admin must DESTROY the LOST dstore;
  cannot revive it without a fresh ID)

## Cross-slice dependencies

- B'' depends on B' for sequencing only (so slices C/D consume both
  in the same commit set without rebase churn -- the startup rebuild
  itself does NOT need `lss_gen` because it walks the persistent
  index post-recovery, with no in-flight mutations to race against)
- C requires B' (TOCTOU defence) and B'' (atomic index update)
- D requires B' and B''
- E requires B (drain flag), B'' (per-dstore iteration), C (placement),
  D (removal)
- F is independent of B/C/D mechanics but is gated on the
  ec_pipeline.h extraction (a separate design's action item)
- G1 requires B'' (reverse iteration to enumerate affected inodes),
  C (layout-segment-diff machinery used in pass 2), D (the layout-
  rewrite-without-recall path; G1 skips the recall because the
  dstore is dead).
- G2 requires G1 (BROKEN bit handling, INODE_LIST_BROKEN); the
  byte-copy machinery for MIRRORED repair (slice C); and shares F's
  gate on `ec_pipeline.h` extraction for the RS-reconstruct path.
  G2 can split into G2a (MIRRORED-only REPAIR; ships without
  ec_pipeline.h) and G2b (RS REPAIR; gated on F's prerequisite).

## Test impact analysis (per roles.md item 2)

| File | Impact | Reason |
|---|---|---|
| `lib/nfs4/dstore/tests/dstore_test.c` | minor-update (B) | new tests for `ds_drained` flag; existing tests pass since default = false |
| `lib/nfs4/dstore/tests/dstore_root_probe_test.c` | PASS-no-change | independent of drain/mirror |
| `lib/nfs4/dstore/tests/dstore_wcc_test.c` | PASS-no-change | independent |
| `lib/nfs4/tests/cb_pending.c` | PASS-no-change (D) | recall-wait infra is additive on top |
| `lib/nfs4/tests/reflected_getattr_test.c` | minor-update (C) | new test for reflected GETATTR after ADD_MIRROR |
| `lib/nfs4/tests/delegation_lifecycle.c` | PASS-no-change | delegation orthogonal |
| `lib/nfs4/tests/chunk_test.c` | PASS-no-change (until F) | chunk-store orthogonal |
| `lib/nfs4/tests/file_ops.c` | PASS-no-change | no layout mutation in OPEN/READ/WRITE path |
| `lib/nfs4/tests/trust_stateid_test.c` | PASS-no-change | trust table not affected by mirror count |
| `lib/fs/tests/layout_segment_test.c` | minor-update (B') | tests for `lss_gen` bump on add/remove |
| `lib/fs/tests/sb_persistence_test.c` | minor-update if drain stored per-SB | drain lives in namespace DB so no impact expected |
| `lib/fs/tests/sb_lifecycle_test.c` | PASS-no-change | SB lifecycle separate from dstore lifecycle |
| All other `lib/fs/tests/sb_*` | PASS-no-change | unrelated |
| `lib/nfs4/ps/tests/*` | PASS-no-change (slices A-E); slice F adds PS-side coverage if applicable | PS path unchanged |
| `lib/nfs4/server/tests/*` (op handlers for OPEN, REMOVE, RENAME, SETATTR, ACCESS) | minor-update (G1) | new tests for INODE_BROKEN-aware behaviour per the matrix in "New on-inode field"; existing tests pass since default i_broken=false |
| `lib/nfs4/tests/layoutget_test.c` | minor-update (G1) | new test `test_layoutget_on_broken_inode_returns_layoutunavailable` |
| `lib/fs/tests/inode_test.c` | minor-update (G1) | new tests for `i_broken` atomic field roundtrip and persistence |
| `lib/backends/tests/rocksdb_test.c` (if exists) | major-add (B'') | new tests for `dstore_inode_idx` CF, atomicity, iter, count |
| `lib/backends/tests/posix_dstore_idx_test.c` (NEW, deferred) | new file | when POSIX impl lands |

No existing test forces a redesign.  All adds are additive; the
gen counter and index ops are introduced with default-zero / NULL
behaviour until consumed by later slices.

## Persistence inventory delta

Cross-references for `.claude/design/rocksdb-persistence.md` "Complete
Persistence Inventory":

Per-superblock additions:
- `dstore_inode_idx` CF (RocksDB) / `sb_<id>/dstore_idx_<ds_id>/`
  directory (POSIX, deferred): per-(ds_id, inum) index entry,
  empty value
- `lss_gen` field appended to `layout_segments` on-disk
  representation (existing `layouts` CF / `.layouts` file)

Server-wide additions:
- `dstore_drained:<ds_id>` keys in namespace DB default CF
  (RocksDB) or `dstore_drained` flat file (POSIX namespace)

No version bump (CLAUDE.md "No persistent storage has been
deployed.").

## Security model (per roles.md item 7)

The probe protocol has no per-op authentication today (see
.claude/design/probe-sb-management.md "future consideration").  These
new ops mutate persistent server state on actual dstores -- equivalent
in blast radius to existing destructive ops (`SB_DESTROY`,
`SB_SET_CLIENT_RULES`).

Per .claude/design/proxy-server.md, an `[[allowed_ps]]` allowlist is
in scope for the PS slice.  Generalise the same shape as
`[[allowed_admin]]` (TLS client cert fingerprint or GSS principal) for
this design's destructive ops.  Implementation: the probe transport
authenticates the caller; the per-op handler checks the caller against
the allowlist before mutating; non-allowlisted callers get
`PROBE1ERR_PERM`.

**Cross-machine destruction warning.**  Unlike SB_DESTROY (which
removes server-local state only), DSTORE_DESTROY removes data on
**other machines** -- the dstores can be remote NFSv3/v4 hosts.
DSTORE_DRAIN itself triggers byte-level data movement across the
network.  The `[[allowed_admin]]` allowlist is therefore substantially
more important for these ops than for any prior destructive probe op
in the codebase.  Without it, anyone with network access to the probe
port can issue cross-machine data deletion.

NOT_NOW_BROWN_COW: actual `[[allowed_admin]]` enforcement.  First
slices ship without it (matching today's other destructive probe
ops), but the allowlist **MUST land before any production deployment**
-- not optional like for SB_DESTROY's local-state-only blast radius.
Track this as a hard prerequisite to "production deployment" in any
deployment checklist.

NFSv4-side: these ops never appear on the wire; NFS4ERR_DELAY-vs-
WRONGSEC concerns from .claude/standards.md don't apply.

## Risks

1. **Slice D recall-wait infra is new**.  `nfs4_cb_layoutrecall_send`
   exists with cb_pending support but has zero callers; D builds the
   first caller PLUS the wait-for-LAYOUTRETURN per-inode condvar.
   Misjudging the wait timeout causes either spurious fence-and-proceed
   (timeout too tight) or admin operations stalling on unresponsive
   clients (timeout too loose).  Default: lease period.

2. **POSIX backend impl deferred**.  Acceptable per chunk_persist
   precedent, but means production deployments on POSIX get the slice
   surface without the persistent index -- the iter/count ops return
   -ENOSYS, drain autopilot won't function.  Document the limitation
   clearly; production = RocksDB until POSIX impl lands.

3. **ec_pipeline header extraction is on someone else's slice ladder**.
   Slice F's gating prerequisite is proxy-server.md action item 1.  If
   that work doesn't land, slice F is blocked indefinitely.  No risk
   to slices A-E.

4. **Concurrent autopilot vs admin manual mutation**.  Both paths take
   `i_attr_mutex` and the gen counter; concurrent operations serialise
   and the loser sees `PROBE1ERR_STALE` and retries.  No corruption;
   may produce surprising stalls under heavy contention.

5. **Orphan index entries when a dstore is forcibly removed from TOML
   config without going through DRAIN -> DESTROY**.  Documented as
   NOT_NOW_BROWN_COW; admin must run a cleanup tool that doesn't
   exist yet.  Recommend the design doc references this gap whenever
   the topic of "remove dstore from config" comes up.

6. **`INODE_BROKEN` files persist as silent data-loss markers in the
   namespace**.  Admin discovery requires polling
   `INODE_LIST_BROKEN`; there is no automatic notification, alerting
   webhook, or syslog summary.  A monitoring system can be wired up
   externally by polling the probe op, but the design ships without
   it.  NOT_NOW_BROWN_COW: alerting integration.

7. **`DSTORE_LOST(force=true)` is a one-way operation**.  Once an
   inode is marked BROKEN, the bit is sticky until the inode is
   REMOVEd.  If the dstore later turns out to be recoverable (e.g.,
   network was just partitioned), the BROKEN inodes do not
   spontaneously self-heal.  Admin would need to: (a) edit TOML to
   re-add the dstore as a fresh ID, (b) script REMOVE on every
   BROKEN inode and re-create from external backup.  The data on
   the (recovered) original dstore is unreachable through reffs
   once the LOST state is committed.

## Deferred / NOT_NOW_BROWN_COW summary

- POSIX backend impl of dstore index ops
- Async copy in ADD_MIRROR (sync inline today)
- Cross-codec change in ADD_MIRROR (defer to slice F)
- Drain timeout / auto-undrain
- Revoke-on-CB_LAYOUTRECALL-timeout (today: fence-and-proceed)
- Rate limiting / parallelism cap on autopilot
- Multi-PS coordination
- Rebalancing op surface (vs straight drain)
- Codec change for files with pending CHUNK_WRITE (returns BUSY)
- Partial-range codec change (whole-file only)
- Orphan index GC when dstore removed from TOML without DRAIN
- `[[allowed_admin]]` allowlist for destructive probe ops
- Background runway replenishment (existing TODO)
- DSTORE_DRAIN_PROGRESS detail probe op (rely on
  DSTORE_INSTANCE_COUNT or DSTORE_LIST for now)
- Codec change for files with active CHUNK locks
- `INODE_BROKEN` clearing path (today: sticky until REMOVE)
- Alerting integration for BROKEN inodes (today: poll
  `INODE_LIST_BROKEN`)
- LOST -> DRAINED transition for "dstore came back" recovery
  (today: admin must DESTROY the LOST id and re-add a fresh one)
- Slot-specific REPAIR (today: fills first missing slot)
- Multi-dstore-loss recovery within an RS layout's k+m (today:
  if surviving shards < k, BROKEN; no partial-recovery combine)
- PS-driven REPAIR autopilot (first impl is MDS-inline; mirrors
  the slice E pattern for drain)

## RFC and design references

- RFC 8881 S12.2 (layout taxonomy), S12.5.5 (LAYOUTRECALL),
  S20.3 (CB_LAYOUTRECALL), S18.43 (LAYOUTGET)
- RFC 8435 S5.1 (FlexFiles mirror semantics), S5.4 (mirror repair)
- draft-haynes-nfsv4-flexfiles-v2 S5 (FFV2 encoding types) -- slice F
- .claude/design/mds.md (existing dstore + layout + runway + reflected
  GETATTR + fencing)
- .claude/design/per-export-dstore.md (per-SB dstore binding,
  file-layout single-DS-per-export constraint)
- .claude/design/dstore-vtable-v2.md (dstore_ops vtable: read/write
  used by ADD_MIRROR's byte-copy step)
- .claude/design/probe-sb-management.md (probe op shape, C+Python ship
  rule)
- .claude/design/proxy-server.md (PS phase plan; CB_PROXY_MOVE infra
  used by slice E; action item 1 gates slice F)
- .claude/design/rocksdb-database.md (per-SB DB layout, namespace DB)
- .claude/design/rocksdb-composition.md (md/data axis; vtable
  composition pattern)
- .claude/design/rocksdb-persistence.md (`chunk_persist` precedent
  for vtable-backed per-SB persistence)
- .claude/standards.md "Persistent state write pattern" (write-temp /
  fdatasync / rename), "Atomic Operations" (C11 stdatomic)
- .claude/patterns/ref-counting.md Rule 6 (lfht entry lifecycle for
  RAM index impl)
- .claude/patterns/rcu-violations.md Pattern 1, 7 (lfht iteration
  rules for the autopilot scan)

Cross-reference from CLAUDE.md once this design is approved.
