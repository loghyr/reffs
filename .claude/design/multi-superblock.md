<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Multi-Superblock (Multi-Export) Design

## Context

reffs currently has a single export (sb_id=1). To support multiple
exports with independent security policies, layout policies, and
backends — and to be referral-ready for future server migration —
we need a multi-superblock architecture.

Last major feature before RocksDB and persistent server.

## Design Decisions

- **Root pseudo-export**: auto-generated at `/` with ALL security
  flavors enabled. Cannot be deleted. Always sb_id=1. Admin can
  restrict flavors via probe.
- **No automatic flavor inheritance**: each export's flavors are
  set explicitly by the admin. No union/push-up. WRONGSEC fires
  at child export boundary per RFC 8881 §2.6.3.
- **Lint tool**: `reffs-probe lint-flavors` warns about unreachable
  flavor paths without constraining the model.
- **Multi-tenancy deferred**: virtual-root-per-identity is orthogonal.
- **Persistence**: registry file + self-contained per-sb directories
  (referral-ready).
- **Test-first (TDD)**: write failing unit tests first.

## Superblock State Machine

```
                    ┌──────────┐
           create() │          │
        ───────────►│ CREATED  │  not visible to clients
                    │          │  admin configures flavors/backend
                    └────┬─────┘
                         │ mount(path)
                         ▼
                    ┌──────────┐
                    │          │  visible in pseudo-fs
                    │ MOUNTED  │  clients access via FH
                    │          │  LOOKUP/READDIR/GETATTR
                    └────┬─────┘
                         │ unmount()
                         ▼
                    ┌──────────┐
                    │          │  NOT in LOOKUP/READDIR
                    │UNMOUNTED │  existing FH access still works
                    │          │  admin can delete mount path dirs
                    └────┬─────┘
                         │ destroy() (no open files)
                         ▼
                    ┌──────────┐
                    │ DESTROYED│  FH → NFS4ERR_STALE
                    └──────────┘
```

### Transition rules

| From | To | Condition | Error |
|------|-----|-----------|-------|
| — | CREATED | admin creates via probe | — |
| CREATED | MOUNTED | mount(path), path exists, is a dir | ENOENT, ENOTDIR |
| MOUNTED | MOUNTED | prohibited | EBUSY (must unmount first) |
| MOUNTED | UNMOUNTED | no child sb mounted on this sb | EBUSY (child mounted) |
| UNMOUNTED | MOUNTED | mount(path), possibly different path | ENOENT |
| UNMOUNTED | DESTROYED | no active open files | EBUSY (open files) |
| MOUNTED | DESTROYED | prohibited | EBUSY (must unmount first) |
| CREATED | DESTROYED | never mounted, always OK | — |

### Known limitation (v1)

No DRAINING state for graceful teardown. Admin must wait for
clients to close files before destroy succeeds. Polling required.
DRAINING state (stop new opens, wait for drain) is a future
enhancement.

### Note on existing SB_IS_MOUNTED flag

`super_block.h` already defines `SB_IS_MOUNTED (1ULL << 2)` in
`sb_state`. The new state machine enum will replace this single
flag with the full lifecycle. The existing flag will be removed.

## Security Flavor Model

- Root starts with ALL flavors.
- Each child export has its own flavor list, set by admin.
- No automatic inheritance up the tree.
- WRONGSEC fires at child export boundary (RFC 8881 §2.6.3):
  client does LOOKUP into child, gets WRONGSEC, uses SECINFO
  to discover required flavors, re-authenticates.
- `reffs-probe lint-flavors` checks for inconsistencies: warns
  if a child requires a flavor no ancestor supports. Advisory only.

### Per-sb security implementation

Both `nfs4_check_wrongsec()` and `nfs4_build_secinfo()` currently
use global `server_state->ss_flavors`. Must change to use
`compound->c_curr_sb->sb_flavors`. Both need a per-sb flavor list.

`SECINFO_NO_NAME` must also return the per-sb flavor list.

### Multi-tenancy (deferred)

Virtual-root-per-identity: map client identity → tenant → virtual
root sb. Orthogonal to the export tree.

## Namespace Traversal

### Mount points in the dirent tree

When sb_2 is mounted at `/alpo`:
- Root sb (sb_id=1) has dirent `alpo` with `RD_MOUNTED_ON` flag
- LOOKUP("alpo") detects the flag → switches to sb_2's root inode
- LOOKUPP from sb_2's root → back to sb_1's `alpo` dirent
- READDIR on `/` shows `alpo` with sb_2's root attributes
- The mounted-on directory's attributes are hidden
- `FATTR4_MOUNTED_ON_FILEID` returns the HIDDEN directory's fileid
  (RFC 8881 §5.8.2.19), not the mounted filesystem's root fileid

### LOOKUPP at child sb root vs pseudo-root

Current LOOKUPP returns NFS4ERR_NOENT when `nfh_ino == INODE_ROOT_ID`.
With multi-sb, must distinguish:
- Root of pseudo-root (sb_id=1, ino=1) → NFS4ERR_NOENT (no parent)
- Root of child sb (sb_id=N, ino=1) → cross back to parent sb

### Cross-export operation guards

RFC 8881 requires NFS4ERR_XDEV for operations that attempt to
cross filesystem (export) boundaries:

| Op | Guard needed | RFC section |
|----|-------------|-------------|
| RENAME | saved_sb != curr_sb → XDEV | §18.26.3 |
| LINK | saved_sb != curr_sb → XDEV | §18.9.3 |
| COPY | deferred (currently NOTSUPP) | §18.32 |

Currently `nfs4_op_rename` and `nfs4_op_link` in `dir.c` do NOT
check `c_saved_sb == c_curr_sb`. Must add guards.

### OPEN at mount point

OPEN with CREATE on a name that is a mount point must traverse
into the child sb. OPEN-by-name does an implicit LOOKUP that
needs the same mount-point detection.

### WRONGSEC at mount point

LOOKUP into a child export with the wrong flavor → NFS4ERR_WRONGSEC.
Client uses SECINFO to discover required flavors.

### Path protection

Directories in mount paths cannot be deleted while mounted:
- `/alpo` cannot be removed while sb_2 is mounted
- After unmount, path entries become deletable

### Unmount with child mounted

Unmount sb_2 while sb_3 is mounted at `/alpo/garbo/mage` → EBUSY.
Must unmount bottom-up.

### Absent filesystem semantics (RFC 8881 §7.4.1)

Intermediate directories in the pseudo-fs path that have no real
export behind them (just connectors):
- GETATTR/READDIR work (return directory attrs)
- CREATE/WRITE/etc. fail (read-only pseudo-fs)
- `fs_locations` attribute: deferred until referral implementation

## Persistence

### Registry: `<state_dir>/superblocks.registry`

```json
{
  "version": 1,
  "superblocks": [
    { "id": 1, "path": "/", "state": "mounted", "dir": "sb_1" },
    { "id": 2, "path": "/alpo", "state": "mounted", "dir": "sb_2" }
  ]
}
```

Write-temp/fsync/rename. Protected by a sb_registry_mutex to
serialize concurrent probe requests.

### Per-sb directory: `<state_dir>/sb_<id>/`

Self-contained — everything to reconstitute on another server:

```
sb_2/
├── sb_config.json      # version, flavors, backend, dstore set
├── inodes/             # inode metadata
├── data/               # file data
├── chunks/             # chunk store
├── identity_domains    # identity domain table
├── identity_map        # identity mapping table
└── layouts/            # MDS layout segments
```

`sb_config.json` includes a version field and checksum for
integrity verification (required for referral).

### Crash recovery

1. Create sb directory first (idempotent)
2. Update registry via write-temp/fsync/rename
3. On startup, scan for orphaned sb directories not in registry
   and log a warning (don't auto-delete — may be referral source)

## Admin Tooling (probe protocol)

| Op | Args | Description |
|----|------|-------------|
| SB_LIST | — | List all sbs with state + path |
| SB_CREATE | config | Create new sb (→ CREATED) |
| SB_MOUNT | id, path | Mount at path (→ MOUNTED) |
| SB_UNMOUNT | id | Unmount (→ UNMOUNTED) |
| SB_DESTROY | id | Destroy (→ DESTROYED) |
| SB_GET | id | Get config + state |
| SB_SET | id, config | Modify config (not path while mounted) |
| SB_LINT_FLAVORS | — | Check flavor consistency |

## Unit Tests (TDD — write first, expect failures)

### Phase 1: State machine

```
test_sb_create                      — verify CREATED state
test_sb_create_duplicate_id         — existing id → error
test_sb_mount                       — mount at path → MOUNTED
test_sb_mount_nonexistent_path      — missing path → ENOENT
test_sb_mount_not_directory         — file → ENOTDIR
test_sb_mount_already_mounted       — MOUNTED→MOUNTED → EBUSY
test_sb_unmount                     — MOUNTED→UNMOUNTED
test_sb_unmount_with_child          — child mounted → EBUSY
test_sb_destroy                     — UNMOUNTED→DESTROYED
test_sb_destroy_mounted             — MOUNTED → EBUSY
test_sb_destroy_with_open_files     — open files → EBUSY
test_sb_destroy_created             — CREATED→DESTROYED (always OK)
test_sb_remount                     — unmount then mount at new path
test_sb_remount_path_deleted        — unmount, rm path, mount → ENOENT
test_root_sb_cannot_unmount         — sb_id=1 → error
test_root_sb_cannot_destroy         — sb_id=1 → error
```

### Phase 2: Mount crossing

```
test_lookup_into_child_sb           — crosses mount point
test_lookup_changes_fsid            — GETATTR shows new fsid
test_lookupp_from_child_root        — back to parent sb
test_lookupp_from_pseudo_root       — NFS4ERR_NOENT (no parent)
test_readdir_shows_mount_point      — child sb root attrs shown
test_readdir_hides_mounted_on       — hidden dir attrs NOT shown
test_mounted_on_fileid              — returns hidden dir's fileid
test_unmounted_sb_not_in_lookup     — returns mounted-on dir
test_unmounted_sb_fh_still_works    — stale FH works
test_delete_mount_path_blocked      — rm mount dir → EBUSY
test_delete_mount_path_after_unmount— rm succeeds after unmount
test_multi_level_mount              — sb in sb
test_multi_level_lookupp            — traverses multiple boundaries
test_rename_across_exports_xdev     — RENAME → NFS4ERR_XDEV
test_link_across_exports_xdev       — LINK → NFS4ERR_XDEV
test_open_into_child_wrongsec       — OPEN at mount point + wrong flavor
test_secinfo_returns_child_flavors  — SECINFO at mount boundary
test_secinfo_no_name_per_sb         — SECINFO_NO_NAME uses per-sb
```

### Phase 3: Persistence

```
test_registry_persist_load          — save + load round-trip
test_sb_config_persist_load         — per-sb config round-trip
test_sb_survives_restart            — create, mount, restart, verify
test_orphan_detection               — sb dir without registry entry
```

### Phase 4: Security per-sb

```
test_wrongsec_per_sb                — child sb rejects wrong flavor
test_wrongsec_root_accepts_all      — root sb accepts any flavor
test_lint_flavors_warns             — unreachable flavor detected
test_lint_flavors_clean             — consistent config, no warnings
```

## Implementation Order

1. **Per-sb security** — move `ss_flavors` to per-sb, update
   `nfs4_check_wrongsec`, `nfs4_build_secinfo`, `SECINFO_NO_NAME`
2. **Superblock state machine** — enum, transitions, validation
3. **Phase 1 unit tests** — write failing state machine tests
4. **Implement state machine** — make Phase 1 tests pass
5. **Mount point infrastructure** — `RD_MOUNTED_ON` flag,
   path resolution, path protection, `mounted_on_fileid`
6. **Cross-export guards** — RENAME/LINK NFS4ERR_XDEV checks
7. **Phase 2 unit tests** — write failing mount-crossing tests
8. **Implement mount crossing** — LOOKUP/LOOKUPP/READDIR/OPEN
9. **Make Phase 2 tests pass**
10. **Registry + per-sb persistence**
11. **Phase 3 unit tests** — persistence tests
12. **Probe protocol extensions** — SB ops
13. **Config loader** — create sbs from `[[export]]` at startup
14. **Phase 4 unit tests** — security per-sb tests
15. **Root pseudo-export** — auto-generate with all flavors

## Key Files

| File | Change |
|------|--------|
| `lib/include/reffs/super_block.h` | State enum, per-sb flavors, mount fields |
| `lib/fs/super_block.c` | State transitions, mount/unmount |
| `lib/include/reffs/dirent.h` | `RD_MOUNTED_ON` flag |
| `lib/nfs4/server/dir.c` | LOOKUP/LOOKUPP crossing, RENAME/LINK XDEV |
| `lib/nfs4/server/attr.c` | READDIR mount points, mounted_on_fileid |
| `lib/nfs4/server/security.c` | Per-sb WRONGSEC + SECINFO |
| `lib/nfs4/server/filehandle.c` | PUTROOTFH unchanged |
| `lib/fs/ns.c` | Multi-sb init from config |
| `lib/config/config.c` | Already parses [[export]] |
| `lib/probe1/` | New SB management ops |
| `lib/fs/tests/` | State machine + crossing tests |
