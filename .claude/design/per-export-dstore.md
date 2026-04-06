# Per-Export Dstore Binding

## Problem

The dstore table is global — LAYOUTGET picks dstores from
`dstore_collect_available()` which returns all connected dstores
regardless of which export the inode belongs to.  This means:

- `/ffv1` (flex files v1, loopback DS) gets the same dstores as
  `/files` (file layouts, remote DS on adept)
- There's no way to say "this export uses these DSes"
- Adding a second DS for file layouts breaks flex files exports
  because LAYOUTGET would pick the wrong DS

## Requirements

Each pNFS-enabled export needs:
1. A list of dstore IDs to use for LAYOUTGET
2. The layout type determines how the dstores are used:
   - File layouts: single DS per export (RFC 5661 §13,
     one nfl_deviceid per layout body)
   - Flex files v1: mirror across dstores (CSM)
   - Flex files v2: mirror with CHUNK ops

## Design

### Per-sb dstore list

Add to `struct super_block`:

```c
#define SB_MAX_DSTORES 16
uint32_t sb_dstore_ids[SB_MAX_DSTORES];
uint32_t sb_ndstores;
```

When LAYOUTGET fires, instead of `dstore_collect_available()`,
use `sb->sb_dstore_ids[]` to look up the specific dstores for
this export.  If `sb_ndstores == 0`, fall back to the global
pool (backward compatible).

If a configured dstore is offline (runway empty or NULL),
LAYOUTGET skips it.  If ALL configured dstores are offline,
LAYOUTGET returns NFS4ERR_LAYOUTUNAVAILABLE.

### Constraint: file layouts = single DS per export

`nfsv4_1_file_layout4` has one `nfl_deviceid` — all FHs in the
layout must be on the same DS.  Enforce: when `sb_layout_types`
includes `SB_LAYOUT_FILE`, `sb_ndstores` must be exactly 1.
The `SB_SET_DSTORES` probe handler rejects configurations that
violate this.

### Probe op to set dstore binding (op 25)

```
struct SB_SET_DSTORES1args {
    unsigned hyper  sda_id;
    unsigned int    sda_dstore_ids<SB_REGISTRY_MAX_DSTORES>;
};
```

**Validation**: the handler calls `dstore_find()` for each ID
and returns `PROBE1ERR_NOENT` if any ID is unknown.  Also checks
the file layout single-DS constraint.

CLI:
```bash
reffs-probe.py sb-set-dstores --id 10 --dstores 1
reffs-probe.py sb-set-dstores --id 11 --dstores 2
reffs-probe.py sb-set-dstores --id 12 --dstores 1 2 3
```

### Registry persistence

Add to `sb_registry_entry`:
```c
uint32_t sre_ndstores;
uint32_t sre_dstore_ids[SB_REGISTRY_MAX_DSTORES];
```

Where `SB_REGISTRY_MAX_DSTORES` = 16 (defined in `sb_registry.h`
alongside `SB_REGISTRY_MAX_FLAVORS`).

Round-trip persistence tests still pass because `sre_ndstores == 0`
and `sre_dstore_ids[]` are zero-initialized by the existing calloc
pattern, making behavior identical to the fallback path.

### bat_export_setup.sh wiring

```bash
# /ffv1 uses dstore 1 (loopback, combined mode)
$PROBE sb-set-dstores --id "$FFV1_ID" --dstores 1

# /ffv2 uses dstore 1 (loopback, CHUNK)
$PROBE sb-set-dstores --id "$FFV2_ID" --dstores 1

# /files uses dstore 2 (adept, file layouts — single DS)
$PROBE sb-set-dstores --id "$FILES_ID" --dstores 2
```

### TOML config for data servers

```toml
# Loopback DS for flex files (combined mode)
[[data_server]]
id      = 1
address = "192.168.2.128"
path    = "/"

# Remote DS for file layouts (adept, role = "ds")
[[data_server]]
id      = 2
address = "192.168.2.129"
path    = "/"
```

### LAYOUTGET changes

In `nfs4_op_layoutget()`, replace:

```c
nds = dstore_collect_available(dstores, LAYOUT_SEG_MAX_FILES);
```

with:

```c
struct super_block *sb = compound->c_inode->i_sb;
if (sb->sb_ndstores > 0) {
    nds = 0;
    for (uint32_t i = 0; i < sb->sb_ndstores; i++) {
        struct dstore *ds = dstore_find(sb->sb_dstore_ids[i]);
        if (ds)
            dstores[nds++] = ds;
    }
} else {
    nds = dstore_collect_available(dstores, LAYOUT_SEG_MAX_FILES);
}
```

### GETDEVICEINFO changes

No changes needed — the device ID encodes the dstore ID, and
the dstore has the address regardless of which export uses it.
GETDEVICEINFO already handles all three layout types.

### File layout DS requirements (RFC 5661 §13.1)

The DS must:
1. Be an NFSv4.1+ server (supports sessions)
2. Set `EXCHGID4_FLAG_USE_PNFS_DS` in EXCHANGE_ID
3. Accept layout stateids for I/O

reffsd already supports all NFSv4.2 session operations.  The DS
flag is set via `role = "ds"` in the config, which maps to
`EXCHGID4_FLAG_USE_PNFS_DS` via `reffs_role_exchgid_flags()`.
No new config option needed — `role = "ds"` is the existing
mechanism.

**Layout stateid validation on the DS**: RFC 5661 §13.1 requires
the DS to validate the layout stateid.  For BAT, the DS accepts
any stateid (the stateid was issued by the MDS, not the DS).
Full validation requires MDS→DS stateid propagation which is
NOT_NOW_BROWN_COW.

**RFC note**: File layout format is specified in RFC 5661 §13
and was not re-specified in RFC 8881.  The reference to RFC 5661
is correct for the layout body encoding.

### Stale dstore references

If a dstore is removed from TOML config but still referenced
in `sb_dstore_ids[]` (from the persisted registry), LAYOUTGET
calls `dstore_find()` which returns NULL, skips it, and returns
NFS4ERR_LAYOUTUNAVAILABLE if all dstores are missing.  This is
safe but silent — the admin gets no warning at startup.
NOT_NOW_BROWN_COW: startup validation that cross-references
registry dstore IDs against configured dstores.

## Test plan

### Existing tests affected: NONE

All changes are additive.  The global dstore fallback
(`sb_ndstores == 0`) preserves existing behavior.  Persistence
tests pass because new fields are zero-initialized.

### Unit tests

In `lib/fs/tests/sb_persistence_test.c`:
- `test_registry_dstores_persisted`: set dstore IDs, save, load,
  verify IDs match
- `test_registry_dstores_empty`: sb with no dstores, verify
  fallback to global pool after load

In `lib/nfs4/tests/` (new or extend existing):
- `test_layoutget_per_sb_dstores`: create sb with sb_ndstores=1,
  verify LAYOUTGET uses only that dstore
- `test_set_dstores_validates_ids`: probe handler rejects unknown
  dstore IDs
- `test_set_dstores_file_layout_single`: probe handler rejects
  ndstores > 1 when SB_LAYOUT_FILE is set

### Functional tests

1. `/ffv1` mount → LAYOUTGET → flex files with dstore 1 (loopback)
2. `/files` mount → LAYOUTGET → file layout with dstore 2 (adept)
3. `/` mount → LAYOUTGET → NFS4ERR_LAYOUTUNAVAILABLE

## Implementation order

1. Add `sb_dstore_ids[]`, `sb_ndstores` to super_block.h
2. Update LAYOUTGET to use per-sb dstores
3. Add probe op `SB_SET_DSTORES` with validation
4. Persist in registry (`sre_ndstores`, `sre_dstore_ids[]`)
5. Update bat_export_setup.sh with dstore bindings
6. Update reffsd-bat.toml with second `[[data_server]]` entry
7. Configure adept with `role = "ds"`
8. Test: mage as MDS, adept as DS, file layout I/O

## Deferred / NOT_NOW_BROWN_COW

- Multiple dstores per file layout export (striping across DSes)
- DS layout stateid validation (MDS→DS propagation)
- DS health monitoring and failover
- Dynamic dstore add/remove via probe
- Per-dstore-per-export runway pools
- Startup cross-reference of registry dstore IDs vs config

## Key files

| File | Change |
|------|--------|
| `lib/include/reffs/super_block.h` | `sb_dstore_ids[]`, `sb_ndstores`, `SB_MAX_DSTORES` |
| `lib/nfs4/server/layout.c` | Per-sb dstore selection in LAYOUTGET |
| `lib/xdr/probe1_xdr.x` | `SB_SET_DSTORES` op 25 |
| `lib/probe1/probe1_server.c` | Handler with validation |
| `scripts/reffs/probe_client.py.in` | Python client method |
| `scripts/reffs-probe.py.in` | CLI subcommand |
| `lib/include/reffs/sb_registry.h` | `sre_ndstores`, `sre_dstore_ids[]`, `SB_REGISTRY_MAX_DSTORES` |
| `lib/fs/sb_registry.c` | Save/load dstore IDs |
| `examples/reffsd-bat.toml` | Second `[[data_server]]` for adept |
| `scripts/bat_export_setup.sh` | `sb-set-dstores` calls |
