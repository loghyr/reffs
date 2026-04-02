<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Directory Delegations Design

## Context

RFC 8881 §18.39 defines GET_DIR_DELEGATION, but the specification
is underspecified and impractical.  Jeff Layton (Linux) and Rick
Macklem (FreeBSD) collaborated to define a workable interop model
that replaces the rigid CB_NOTIFY specification with negotiated
capabilities.  See `ddir.txt` for the full research summary.

## What Linux Does Today

The Linux NFS client (6.x) sends GET_DIR_DELEGATION during GETATTR
on directories when `directory_delegations` is enabled (module param,
default off as of 6.19).  The request is minimal:

```c
// fs/nfs/nfs4xdr.c:2011
encode_op_hdr(xdr, OP_GET_DIR_DELEGATION, ...);
xdr_stream_encode_bool(xdr, false);          // no CB_RECALLABLE_OBJ_AVAIL
xdr_encode_bitmap4(xdr, {0}, 1);             // zero notification bits
xdr_encode_nfstime4(p, &zero_ts);            // no attr change delay
xdr_encode_nfstime4(p, &zero_ts);
xdr_encode_bitmap4(xdr, {0}, 1);             // zero child attrs
xdr_encode_bitmap4(xdr, {0}, 1);             // zero dir attrs
```

On success (`GDD4_OK`), the client stores a READ delegation on the
directory inode and uses it to cache READDIR results.  On `GDD4_UNAVAIL`,
it proceeds without a delegation.

The client checks `NFS_CAP_DIR_DELEG` (server capability discovered
at mount time from supported_attrs or server_caps) before requesting.

## Response Format

```
GET_DIR_DELEGATION4resok:
    gddr_status:   GDD4_OK or GDD4_UNAVAIL
    (if GDD4_UNAVAIL: gddr_will_signal_deleg_avail bool)
    (if GDD4_OK:)
    gddr_cookieverf:  verifier4
    gddr_stateid:     stateid4   (delegation stateid)
    gddr_notification: bitmap4   (server's notification capabilities)
    gddr_child_attributes: bitmap4
    gddr_dir_attributes: bitmap4
```

## Implementation Plan

### Phase 1: Minimal (Linux-compatible)

Grant directory delegations with zero notification capabilities.
The delegation is purely a "cache valid" token — the client caches
READDIR results and trusts them until the delegation is recalled.

**Grant:** In GET_DIR_DELEGATION handler, allocate a delegation
stateid on the directory inode.  Return `GDD4_OK` with:
- `gddr_cookieverf` = current READDIR verifier for the directory
- `gddr_stateid` = new delegation stateid
- `gddr_notification` = empty bitmap (no CB_NOTIFY)
- `gddr_child_attributes` = empty bitmap
- `gddr_dir_attributes` = empty bitmap

**Recall:** Any directory-modifying operation on an inode that has
an outstanding directory delegation triggers CB_RECALL on that
delegation stateid.  Operations that trigger recall:
- CREATE (file, dir, symlink, mknod, link)
- REMOVE / RMDIR
- RENAME (both source and target directories)

The existing `stateid_inode_find_delegation` + `cb_recall_file`
infrastructure handles this.  Add a call in each mutating op,
after the VFS call succeeds but before returning to the client.

**DELEGRETURN:** Already implemented — the generic delegation
return handler works for directory delegations identically to
file delegations.

**Conflict with OPEN:** No conflict — directory delegations are
READ-only and do not interact with OPEN (which operates on files,
not directories).  Multiple clients can hold directory delegations
on the same directory simultaneously; a mutation by any client
recalls all of them.

### Phase 2: CB_NOTIFY (deferred, NOT_NOW_BROWN_COW)

Instead of recalling on every mutation, send CB_NOTIFY with the
change details (added entry, removed entry).  The client updates
its cache without requiring a full READDIR.

This requires the negotiated capability model from the
Layton/Macklem work:
- `CB_NOTIFY_WANT_VALID` flag
- Per-notification-type capability bits
- Fallback to recall on overflow

Not needed for initial functionality — recall-on-mutate is correct
and sufficient.

### Files to Change

**New handler:** `lib/nfs4/server/delegation.c`
- `nfs4_op_get_dir_delegation()` — allocate delegation, encode
  response.  Currently returns NFS4ERR_NOTSUPP.

**Recall wiring:** `lib/nfs4/server/dir.c`
- `nfs4_op_create` — after success, recall dir delegations
- `nfs4_op_remove` — after success, recall dir delegations
- `nfs4_op_rename` — after success, recall on both dirs
- `nfs4_op_link` — after success, recall dir delegations

**Recall helper:** `lib/nfs4/server/delegation.c`
- `nfs4_recall_dir_delegations(struct inode *dir)` — find all
  delegation stateids on `dir`, send CB_RECALL for each.
  Fire-and-forget (same model as file delegation recall).

**Capability advertisement:**
- Enable `FATTR4_DIR_NOTIF_DELAY` and `FATTR4_DIRENT_NOTIF_DELAY`
  in supported_attributes (set to 0 = no delay).
- Remove `bitmap4_attribute_clear(bm, FATTR4_DIR_NOTIF_DELAY)` etc.

**Dispatch registration:**
- Register `nfs4_op_get_dir_delegation` in the dispatch table
  (currently wired to the NOTSUPP stub).

### READDIR Verifier

The `gddr_cookieverf` must change when the directory changes.
Use the directory's `i_changeid` (truncated to 8 bytes) as the
verifier.  The READDIR handler already uses a verifier — ensure
consistency.

### Concurrency

Multiple clients may hold directory delegations simultaneously.
Recall is fire-and-forget — the server sends CB_RECALL and does
not wait for DELEGRETURN before completing the mutating operation.
The client's cached READDIR results become stale at recall time;
the next READDIR will go to the server.

### Lease Interaction

Directory delegations are subject to the same lease rules as file
delegations.  The lease reaper expires them if the client doesn't
renew (via SEQUENCE on any session).  DELEGRETURN explicitly
releases them.

### Test Plan

**Unit tests:**
- `test_get_dir_deleg_grant`: GET_DIR_DELEGATION returns GDD4_OK
  with a valid stateid
- `test_get_dir_deleg_recall_on_create`: CREATE in delegated dir
  triggers CB_RECALL
- `test_get_dir_deleg_recall_on_remove`: REMOVE triggers CB_RECALL
- `test_get_dir_deleg_delegreturn`: DELEGRETURN releases the
  delegation

**Integration:**
- Mount with `directory_delegations` kernel param, READDIR a
  directory, verify no READDIR on second access.  Create a file
  from another client, verify CB_RECALL fires and next READDIR
  goes to server.

**pynfs:** pynfs has GET_DIR_DELEGATION tests but they are in the
`deleg` group which we currently skip.  Enabling them would test
the wire format.

### Risk

Low.  The implementation reuses existing delegation infrastructure.
The only new code paths are:
1. GET_DIR_DELEGATION handler (~50 lines)
2. Recall helper (~20 lines)
3. Recall calls in 4 mutation handlers (~4 lines each)

If a bug causes incorrect delegation grants, the worst case is
stale READDIR cache — a correctness issue, not data corruption.
CB_RECALL failure (client unreachable) is already handled by the
existing delegation infrastructure (lease-based expiry).

### RFC References

- RFC 8881 §18.39: GET_DIR_DELEGATION
- RFC 8881 §20.4: CB_NOTIFY (Phase 2)
- RFC 8881 §18.49: WANT_DELEGATION (can request dir deleg)
- Layton/Macklem negotiation model: see `ddir.txt`
