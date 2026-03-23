<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Mount Hang Debug — 2026-03-23

## Symptom

`sudo mount -o vers=4.2 127.0.0.1:/ /mnt/reffs` hangs indefinitely.
The `mount.nfs` process enters D (uninterruptible sleep) state and
cannot be killed even with `SIGKILL`.  Multiple attempts accumulate
stuck processes.  Only a reboot clears them.

NFSv3 mounts work fine.

## Timeline

- The hang appeared during the session where we added pNFS layout
  advertisement (`FATTR4_FS_LAYOUT_TYPES`, `FATTR4_LAYOUT_TYPES`)
  to the supported_attributes bitmap and GETATTR responses.
- NFSv4.2 mounts were working earlier in the same session (before
  layout attrs were advertised).
- The hang persists even after reverting the `nfs4_attribute_init`
  changes — D-state processes from earlier attempts hold the kernel
  NFS client in a bad state.

## What tshark shows

The NFS protocol exchange completes normally:
1. SECINFO_NO_NAME → OK
2. PUTROOTFH + GETATTR → OK (350 bytes)
3. Multiple GETATTRs for different attr groups → all OK
4. 30-second gap, then SEQUENCE heartbeat

The server responds to everything.  The client's NFS protocol layer
is satisfied.  But `mount.nfs` never returns.

## Theories

### 1. Layout attrs with empty values confuse the client

When `FATTR4_FS_LAYOUT_TYPES` and `FATTR4_LAYOUT_TYPES` are in
`supported_attributes` but `inode_to_nattr` returns empty arrays
(len=0, val=NULL) in standalone mode, some Linux client versions
may enter a retry loop or block trying to initialize the flexfiles
layout driver.

**Status:** Partially confirmed.  The attrs are now conditionally
advertised only when the server role includes MDS.  But the hang
occurred on a combined-role server where the arrays are populated
correctly with `[LAYOUT4_FLEX_FILES]`.

### 2. `server_state_find()` in `nfs4_attribute_init` triggers early grace

`server_state_find()` calls `server_state_get()` which transitions
`GRACE_STARTED → IN_GRACE`.  When called during `nfs4_attribute_init`
(before the server is ready), this could put the server in an
unexpected lifecycle state.

**Status:** Possible.  The fix is to avoid `server_state_find()` in
init and use a separate `nfs4_attr_enable_layouts()` call after
protocol registration.  Not yet confirmed as the root cause.

### 3. Stale sessions from previous mount attempts

Multiple failed mount attempts leave kernel-side NFS sessions that
are never properly torn down.  New mounts try to reuse stale
sessions and block.  The server may have stale session state that
doesn't match what the kernel expects.

**Status:** Likely a contributing factor.  The D-state processes
from earlier attempts hold kernel locks that block new mounts.

### 4. Runway pool file creation fails on restart

On restart, `runway_batch_create` tries to create `pool_000001.dat`
which already exists.  The local VFS `vfs_create` returns `EEXIST`.
The runway ends up empty.  LAYOUTGET would fail with
`NFS4ERR_LAYOUTUNAVAILABLE` for all files.

**Status:** Confirmed bug.  Separate from the mount hang but will
block pNFS testing.

## Known bugs to fix

1. **`nattr_release` memory leak:** `fs_layout_types` and
   `layout_types` arrays are calloc'd in `inode_to_nattr` but
   never freed in `nattr_release`.

2. **Runway EEXIST on restart:** `vfs_create` in the local vtable
   returns `EEXIST` for existing pool files.  Either use UNCHECKED
   semantics (create-or-open) or LOOKUP first.

3. **Layout attr timing:** `nfs4_attr_enable_layouts()` must be
   called AFTER `nfs4_protocol_register()` to avoid accessing
   uninitialized state.  Do NOT call `server_state_find()` inside
   `nfs4_attribute_init()`.

4. **Shutdown double-put:** DS super_block was double-put (fixed
   in this session but should be verified).

5. **Shutdown refcount assertion:** `urcu_ref_put` assertion
   `res >= 0` fires on Ctrl-C.  Likely related to the double-put
   fix or runway inode cleanup.

## Resolution plan

1. Reboot the VM to clear D-state mount processes.
2. Fix `nattr_release` to free layout type arrays.
3. Fix runway to handle existing pool files (LOOKUP or UNCHECKED).
4. Use `nfs4_attr_enable_layouts()` called from reffsd.c AFTER
   `nfs4_protocol_register()` — do not use `server_state_find()`
   in `nfs4_attribute_init()`.
5. Test with clean state: `make -f Makefile.reffs clean-combined`
   then `make -f Makefile.reffs run-combined`.
6. Mount with: `sudo mount -o vers=4.2 127.0.0.1:/ /mnt/reffs`
7. Verify LAYOUTGET in tshark.
8. Test data integrity: `cp LICENSE /mnt/reffs/ && md5sum LICENSE /mnt/reffs/LICENSE`
