<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Probe Protocol: SB Management Ops + Per-SB Stats

## Context

Multi-superblock Phases 1-4 are complete (state machine, mount-crossing,
persistence, per-sb security). But there's no runtime admin path — the
probe protocol only has stats/diagnostic ops (0-12). An admin cannot
create, mount, or configure exports without restarting the server.

This plan adds 8 SB management ops (13-20), extends 3 existing stats
ops with per-sb breakdowns, and delivers C + Python simultaneously.

**Rule**: All probe functionality ships in C and Python at the same time.

## Design Decisions

- **Wire compat**: Extend existing resok structs (append per-sb arrays).
  Probe is internal-only, client and server ship together.
- **Create flow**: Separate ops. SB_CREATE allocates + creates root
  dirent + ensures mount path exists (creating intermediate dirs).
  Admin calls SB_SET_FLAVORS and SB_MOUNT as separate steps.
- **Python XDR**: Auto-generated from `.x` file — add types, rebuild.
- **Admin interface**: Probe protocol is THE admin interface for
  non-POSIX operations. Normal filesystem ops (touch, mkdir, etc.)
  cannot create superblocks or set security flavors. The probe
  protocol fills this gap. Future consideration: a `.admin` pseudo-
  directory or reserved namespace for filesystem-as-interface admin,
  but probe is the v1 approach.
- **SB_CREATE path creation**: The create op must ensure the mount
  path exists, creating intermediate directories as needed (like
  `mkdir -p`). This avoids requiring the admin to manually create
  the path before creating the export.

## Test Impact Analysis

### Existing tests affected: NONE

All changes are additive — new probe ops and new fields appended to
existing resok structs. No existing C handler logic changes.

### Tests needed

1. **Python integration script** (`scripts/test_sb_probe.py`):
   Start reffsd, exercise all 8 SB ops + per-sb stats via Python.

2. **C unit tests for path creation** (`lib/fs/tests/`):
   `reffs_fs_mkdir_p()` (or equivalent) needs tests for:
   - Single-level path creation
   - Multi-level path creation (`/a/b/c`)
   - Idempotent (path already exists)
   - Partial existence (`/a` exists, `/a/b/c` doesn't)

3. **Existing `make check` must still pass** (zero regressions).

## New XDR Types

**File**: `lib/xdr/probe1_xdr.x`

### Shared enums (mirror C enums, validated by static_assert)

```
probe_storage_type1   { RAM=0, POSIX=1, ROCKSDB=2, FUSE=3 }
probe_sb_lifecycle1   { CREATED=0, MOUNTED=1, UNMOUNTED=2, DESTROYED=3 }
probe_auth_flavor1    { SYS=1, KRB5=390003, KRB5I=390004, KRB5P=390005,
                        TLS=0x40000001 }
```

### SB info struct (reused by SB_LIST, SB_GET, SB_CREATE)

```
probe_sb_info1 {
    psi_id, psi_uuid[16], psi_path, psi_state, psi_storage_type,
    psi_flavors<8>, psi_bytes_max, psi_bytes_used,
    psi_inodes_max, psi_inodes_used
}
```

### 8 new ops (13-20)

| # | Op | Args | Result |
|---|-----|------|--------|
| 13 | SB_LIST | void | `SB_LIST1resok { slr_sbs<> }` |
| 14 | SB_CREATE | `sca_path, sca_storage_type` | `probe_sb_info1` (server assigns id + uuid) |
| 15 | SB_MOUNT | `sma_id, sma_path` | void |
| 16 | SB_UNMOUNT | `sua_id` | void |
| 17 | SB_DESTROY | `sda_id` | void |
| 18 | SB_GET | `sga_id` | `probe_sb_info1` |
| 19 | SB_SET_FLAVORS | `sfa_id, sfa_flavors<8>` | void |
| 20 | SB_LINT_FLAVORS | void | `lfr_warnings, lfr_messages<>` |

### Extended stats (append per-sb arrays to existing resok)

```
NFS4_OP_STATS1resok += probe_sb_nfs4_op_stats1 nosr_per_sb<>;
FS_USAGE1resok      += probe_sb_fs_usage1 fur_per_sb<>;
LAYOUT_ERRORS1resok += probe_layout_error1 ler_sbs<>;
```

## Implementation Steps

### Step 1: `reffs_fs_mkdir_p()` + unit tests
**File**: `lib/fs/fs.c`, `lib/include/reffs/fs.h`
- Add `reffs_fs_mkdir_p(path, mode)` — recursive mkdir
- Walk components, call `reffs_fs_mkdir()` for each, ignore EEXIST
**File**: `lib/fs/tests/sb_mkdir_p_test.c` (NEW)
- test_mkdir_p_single — `/one`
- test_mkdir_p_multi — `/a/b/c`
- test_mkdir_p_exists — path already exists → success
- test_mkdir_p_partial — `/a` exists, create `/a/b/c`
- test_mkdir_p_root — `/` → success (noop)

### Step 2: XDR definitions
**File**: `lib/xdr/probe1_xdr.x`
- Add shared enums, sb_info struct, 8 op args/res types
- Add per-sb stats types, extend 3 existing resok structs
- Add ops 13-20 to program block
- Rebuild to validate (auto-generates C + Python XDR)

### Step 3: C server handlers
**File**: `lib/probe1/probe1_server.c`
- `fill_sb_info()` helper
- 8 new handlers:
  - `probe1_op_sb_list` — walk sb list under rcu_read_lock
  - `probe1_op_sb_create` — mkdir_p + super_block_alloc +
    dirent_create + registry_save
  - `probe1_op_sb_mount` — find + mount + registry_save
  - `probe1_op_sb_unmount` — find + unmount + registry_save
  - `probe1_op_sb_destroy` — find + destroy + release_dirents +
    registry_save
  - `probe1_op_sb_get` — find + fill_sb_info
  - `probe1_op_sb_set_flavors` — find + set_flavors
  - `probe1_op_sb_lint_flavors` — lint_flavors()
- Register all 8, add static_assert for enum sync

### Step 4: Extend existing stats handlers
**File**: `lib/probe1/probe1_server.c`
- `probe1_op_nfs4_op_stats` — add per-sb array from
  `sb->sb_nfs4_op_stats[]`
- `probe1_op_fs_usage` — add per-sb bytes/files
- `probe1_op_layout_errors` — empty `ler_sbs` for now (per-sb
  layout errors need `sb_layout_errors` field, deferred)

### Step 5: C client wrappers + CLI
**File**: `lib/probe1/probe1_client.c`, `lib/include/reffs/probe1.h`
- 8 new `probe1_client_op_sb_*()` functions
**File**: `src/probe1_client.c`
- `--sb-id`, `--sb-path`, `--storage-type`, `--flavors` options
- 8 new `--op` values

### Step 6: Python client + CLI
**File**: `scripts/reffs/probe_client.py.in`
- 8 new methods on Probe1Client
**File**: `scripts/reffs-probe.py.in`
- Enum maps: STORAGE_TYPE_MAP, AUTH_FLAVOR_MAP, SB_LIFECYCLE_MAP
- 8 new subparsers
- Update nfs4-op-stats, fs-usage, layout-errors formatters for
  per-sb breakdown

### Step 7: Python integration test
**File**: `scripts/test_sb_probe.py` (NEW)
- sb-list → root sb present
- sb-create id=42 path=/test/deep/export storage=ram
- sb-get 42 → CREATED, path created
- sb-set-flavors 42 [sys,krb5]
- sb-mount 42
- sb-get 42 → MOUNTED
- sb-list → root + 42
- nfs4-op-stats → per-sb array present
- fs-usage → per-sb array present
- sb-lint-flavors → 0 warnings
- sb-unmount 42
- sb-destroy 42
- sb-list → root only

## Error Mapping

| C errno | Probe status |
|---------|-------------|
| 0 | PROBE1_OK |
| -ENOMEM | PROBE1ERR_NOMEM |
| -EINVAL | PROBE1ERR_INVAL |
| -EBUSY | PROBE1ERR_BUSY |
| -ENOENT | PROBE1ERR_NOENT |
| -ENOTDIR | PROBE1ERR_NOTDIR |
| -EPERM | PROBE1ERR_PERM |
| -EEXIST | PROBE1ERR_EXIST |

## Key Files

| File | Change |
|------|--------|
| `lib/fs/fs.c` | `reffs_fs_mkdir_p()` |
| `lib/include/reffs/fs.h` | declaration |
| `lib/fs/tests/sb_mkdir_p_test.c` | NEW — mkdir_p unit tests |
| `lib/xdr/probe1_xdr.x` | XDR types + ops 13-20 |
| `lib/probe1/probe1_server.c` | 8 new + 3 extended handlers |
| `lib/probe1/probe1_client.c` | 8 new client wrappers |
| `lib/include/reffs/probe1.h` | 8 new declarations |
| `src/probe1_client.c` | CLI sb-* options |
| `scripts/reffs/probe_client.py.in` | 8 new Python methods |
| `scripts/reffs-probe.py.in` | 8 subparsers + per-sb formatters |
| `scripts/test_sb_probe.py` | NEW — integration test |

## Verification

1. `make -j$(nproc)` — zero errors, zero warnings
2. `make check` — all existing + new mkdir_p tests pass
3. Start reffsd, run `reffs-probe.py sb-list` — shows root sb
4. Run `scripts/test_sb_probe.py` — full SB lifecycle via Python
5. Run `reffs_probe1_clnt --op sb-list` — C CLI works
6. `make -f Makefile.reffs fix-style` + `license` — clean
