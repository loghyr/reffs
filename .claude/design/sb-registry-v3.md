<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Superblock Registry v3: Persistent IDs, Stable UUIDs, Single Authority

> **Note**: "v3" is the design iteration number, not an on-disk
> version.  The actual on-disk `SB_REGISTRY_VERSION` is 1 (no
> prior deployments exist).  All items in this plan are implemented.

## Context

These issues existed in the initial multi-sb implementation:
`[[export]]` in TOML config duplicated the probe protocol as a
source of truth, sb_id was admin-assigned (collision-prone), and
sb_uuid was regenerated on every restart (violated reviewer rule 8).
All have been fixed.

## Design Decisions

1. **Probe is the sole authority** for export creation. Remove the
   `[[export]]` → sb creation loop from reffsd.c. Root sb flavors
   still come from config. `[[export]]` config parsing remains for
   root flavor config only (cleanup deferred).

2. **sb_id is server-assigned** from a persistent monotonic counter.
   Never reused. Gaps are acceptable (prove no reuse). Counter lives
   in `srh_next_id` in the registry header. IDs 1-2 reserved.

3. **sb_uuid is persistent**. Generated once at creation, persisted
   in the registry, restored on load. `super_block_alloc()` stops
   calling `uuid_generate()` — caller is responsible (generate for
   new sbs, copy from persistence for loaded sbs).

4. **Path prefix detection** uses proper boundary checking:
   `/foo` is a prefix of `/foo/bar` but NOT of `/foobar`. Root `/`
   is exempt (it's the pseudo-root, prefix of everything by design).

5. **Registry format v1** includes `sre_uuid` and `srh_next_id`
   from the start.  No migration code (no prior deployments).

6. **fsck tool** is standalone (works without running server). Uses
   text output for dump (no new JSON dependency). TOML import
   deferred (NOT_NOW_BROWN_COW).

## On-Disk Format

### Registry header (v3)

```c
struct sb_registry_header {
    uint32_t srh_magic;     /* 0x53425247 "SBRG" */
    uint32_t srh_version;   /* 1 */
    uint32_t srh_count;
    uint32_t srh_next_id;   /* monotonic, starts at 3 */
};
```

### Registry entry (v1)

```c
struct sb_registry_entry {
    uint64_t sre_id;
    uint32_t sre_state;
    uint32_t sre_storage_type;
    uuid_t   sre_uuid;                      /* NEW in v3 */
    char     sre_path[SB_REGISTRY_MAX_PATH];
};
```

## Implementation Steps

### Step 1: Stop uuid_generate in super_block_alloc

**File**: `lib/fs/super_block.c`
- Remove `uuid_generate(sb->sb_uuid)` from `super_block_alloc()`
- `sb_uuid` starts as all-zeros after calloc
- Callers that create NEW sbs call `uuid_generate(sb->sb_uuid)` after alloc
- `sb_registry_load` calls `uuid_copy(sb->sb_uuid, entry->sre_uuid)` after alloc

**Test impact**: All existing tests that call `super_block_alloc` directly
(lifecycle, persistence, mount-crossing, security, mkdir_p) will get
all-zero UUIDs. This is fine — no test asserts on UUID content. BUT:
add a `uuid_generate()` call in the `mount_child_at()` helpers and
`persist_setup()` helpers if any test ever checks UUID stability.

### Step 2: Registry v1 format + UUID persistence

**File**: `lib/include/reffs/sb_registry.h`
- `SB_REGISTRY_VERSION` = 1
- `uuid_t sre_uuid` in `sb_registry_entry`

**File**: `lib/fs/sb_registry.c`
- `sb_registry_save()`: `uuid_copy(entries[i].sre_uuid, sb->sb_uuid)`
- `sb_registry_load()`: `uuid_copy(sb->sb_uuid, e->sre_uuid)` after alloc
- Restore `srh_next_id` from header

### Step 3: Remove sca_id from SB_CREATE, server assigns

**File**: `lib/xdr/probe1_xdr.x`
- Remove `sca_id` from `SB_CREATE1args`
- Add `opaque psi_uuid[16]` to `probe_sb_info1`

**File**: `lib/probe1/probe1_server.c`
- `probe1_op_sb_create()`: call `sb_registry_alloc_id()` for ID,
  call `uuid_generate(sb->sb_uuid)` after alloc
- `fill_sb_info()`: `memcpy(psi->psi_uuid, sb->sb_uuid, 16)`

**File**: `lib/probe1/probe1_client.c` + `lib/include/reffs/probe1.h`
- Remove `id` param from `probe1_client_op_sb_create()`

**File**: `scripts/reffs/probe_client.py.in`
- Remove `sb_id` param from `sb_create()`

**File**: `scripts/reffs-probe.py.in`
- Remove `--id` from `sb-create` subparser
- Print assigned ID + UUID from response
- Add `import uuid` for UUID formatting

### Step 4: Remove [[export]] as sb source

**File**: `src/reffsd.c`
- Delete the `for (ei = 1; ei < cfg.nexports; ei++)` loop (lines ~408-448)
- Keep root sb flavor assignment from `cfg.exports[0]`
- Keep registry load + orphan detection

### Step 5: Path conflict detection

**File**: `lib/fs/super_block.c`
```c
int super_block_check_path_conflict(const char *path)
```
- Walk sb list under rcu_read_lock
- For each MOUNTED sb (skip root sb_id=1):
  - Exact match: `strcmp(sb->sb_path, path) == 0` → -EEXIST
  - New is parent of existing: `is_path_prefix(path, sb->sb_path)` → -EBUSY
- `is_path_prefix(parent, child)`:
  ```
  if parent == "/" return true (but we skip root)
  len = strlen(parent)
  return strncmp(parent, child, len) == 0
         && (child[len] == '/' || child[len] == '\0')
  ```

**File**: `lib/include/reffs/super_block.h`
- Declare `super_block_check_path_conflict()`

**File**: `lib/probe1/probe1_server.c`
- Call in `probe1_op_sb_create` and `probe1_op_sb_mount`

### Step 6: fsck/repair tool

**File**: `src/reffs_registry_tool.c` (NEW)
- `dump`: read registry, print text (id, state, uuid, path per line)
- `check`: validate magic, version, counter vs max id, no dups, UUIDs nonzero
- `repair-counter`: scan sb_<id>/ dirs, set next_id = max + 1

**File**: `src/Makefile.am`
- Add `reffs_registry_tool` to `bin_PROGRAMS`

Deferred: JSON/TOML import (NOT_NOW_BROWN_COW — no cJSON dependency).

## Unit Tests (TDD)

### New in `lib/fs/tests/sb_persistence_test.c`

| Test | Intent |
|------|--------|
| `test_registry_uuid_persisted` | Create sb with uuid_generate, save, destroy, load. UUID matches. |
| `test_registry_uuid_stable_across_restarts` | Save, destroy, load, save, destroy, load. UUID identical all 3 times. |
| `test_alloc_id_monotonic` | 3 allocs return 3, 4, 5 (or sequential from current counter). |
| `test_alloc_id_persists_across_restart` | Alloc, save, load, alloc. Second > first. |
| `test_alloc_id_never_reuses` | Alloc id=N, create sb, destroy sb, alloc. Returns N+1, not N. |

### New file: `lib/fs/tests/sb_path_conflict_test.c`

| Test | Intent |
|------|--------|
| `test_path_conflict_exact_match` | Mount at /alpo, check /alpo → -EEXIST |
| `test_path_conflict_parent_of_mounted` | Mount at /foo/bar/garbo, check /foo/bar → -EBUSY |
| `test_path_conflict_child_of_mounted` | Mount at /foo, check /foo/bar/deeper → 0 (allowed) |
| `test_path_conflict_no_conflict` | Mount at /alpo, check /bravo → 0 |
| `test_path_conflict_unmounted_no_conflict` | Mount at /alpo, unmount, check /alpo → 0 |
| `test_path_conflict_not_false_prefix` | Mount at /foo, check /foobar → 0 (not a real prefix) |

### Updated: `scripts/test_sb_probe.py`

- `sb_create` no longer passes id — reads assigned id from response
- Assert `psi_id >= 3`
- Assert `psi_uuid` is 16 bytes, not all zeros
- Two creates get different IDs

## Test Impact on Existing Tests

| File | Impact |
|------|--------|
| `sb_lifecycle_test.c` | Calls `super_block_alloc` directly — UUID is zero, no test checks it. **PASS** |
| `sb_mount_crossing_test.c` | Same. **PASS** |
| `sb_security_test.c` | Same. **PASS** |
| `sb_persistence_test.c` | Registry format changes but tests do round-trips in same version. Add uuid_generate after alloc in test helpers. **MINOR UPDATE** |
| `sb_mkdir_p_test.c` | No sb involvement. **PASS** |
| `test_sb_probe.py` | **MUST UPDATE** — no sca_id, read assigned id. |

## Verification

1. `make -j$(nproc)` — zero errors, zero warnings
2. `make check` — all existing + new tests pass
3. Start reffsd, `reffs-probe.py sb-create --path /test --storage ram`
   — returns assigned id + uuid
4. `reffs-probe.py sb-list` — shows uuid column
5. Restart reffsd, `reffs-probe.py sb-list` — same uuid
6. `reffs_registry_tool dump /tmp/state` — readable output
7. `reffs_registry_tool check /tmp/state` — exit 0
8. `scripts/test_sb_probe.py` — all pass
