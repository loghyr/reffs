<!--
SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# reffs: Inode and Dirent Lifecycle, LRU Eviction, and Reference Counting

## Overview

reffs maintains two independent object caches — **inodes** and **dirents** — each with
its own reference counting model and LRU eviction strategy. The two caches are
intentionally decoupled: a dirent can outlive its inode in memory (the inode is
evicted to the backend while the dirent stays in the tree), and a dirent's
`rd_inode` pointer is a *weak* reference that can become stale without warning.

Understanding when each ref type must be held, and what operations are safe without
holding them, is essential for correct NFS op implementation.

---

## 1. Inode Reference Counting

Each inode has two orthogonal ref counts:

### `i_ref` — memory lifetime

`i_ref` is a `urcu_ref`. As long as `i_ref > 0` the inode struct is valid memory.
When `i_ref` drops to zero, `inode_release` is called synchronously, which unhashes
the inode and schedules `inode_free_rcu` via `call_rcu`. After the RCU grace period,
the memory is freed.

| Who holds `i_ref` | Acquired by | Released by |
|---|---|---|
| Hash table | `inode_alloc` (on hash-insert win) | `super_block_drain` → `inode_unhash` + `inode_put` |
| Caller of `inode_alloc` | `inode_alloc` (second `inode_get`) | `inode_active_put` → `inode_put` |
| Caller of `inode_find` | `inode_find` → `inode_active_get` → `inode_get` | `inode_active_put` → `inode_put` |
| Caller of `inode_get` | `inode_get` explicitly | `inode_put` |

**`rd->rd_inode` holds NO `i_ref`.** It is a plain weak pointer. Storing into
`rd_inode` never calls `inode_get`. Reading from `rd_inode` without an active ref
is only safe inside `rcu_read_lock` — and even then the inode may be a tombstone.
Use `inode_active_get(rd->rd_inode)` to safely promote a weak pointer to a strong one.

### `i_active` — active-use count

`i_active` is an `int64_t` atomic. It tracks whether any caller is actively using
the inode. It has three significant states:

| Value | Meaning |
|---|---|
| `> 0` | Inode is in active use; not on LRU |
| `0` | Inode is idle; on LRU; eligible for eviction |
| `-1` | Tombstone; eviction is in progress; `inode_active_get` will fail |

`inode_active_get` atomically increments `i_active` and checks for the tombstone
race. If `i_active` was already `-1` (eviction in progress), it backs out and
returns NULL. It also pulls the inode off the LRU if `i_active` was 0.

`inode_active_put` decrements `i_active`. If it hits 0, the inode is added to the
LRU tail. It always calls `inode_put` to match the `i_ref` bump from acquisition.

### `inode_alloc` return contract

`inode_alloc` returns with `i_ref = 2` (hash ref + caller ref) and `i_active = 1`
(caller active ref). The caller **must** call `inode_active_put` when done — this
drops `i_active` to 0 (adding to LRU) and calls `inode_put` (dropping caller `i_ref`).

If another thread wins the `cds_lfht_add_unique` race, `inode_alloc` discards the
newly-allocated inode and returns an active ref on the winner — same contract.

### `inode_find` return contract

`inode_find` returns with `i_active` incremented (inode pulled off LRU if needed)
and `i_ref` incremented. Caller **must** call `inode_active_put` when done.

Returns NULL if the inode is not in the hash or if `inode_active_get` fails
(tombstone race).

---

## 2. What to Do After `inode_find` (or `inode_alloc`)

Every NFS operation that looks up an inode by filehandle follows this pattern:

```c
inode = inode_find(sb, nfh->nfh_ino);
if (!inode) {
    ret = -ENOENT;
    goto out;
}

/* ... use inode freely ... */

out:
    res->status = errno_to_nfs3(ret);
    inode_active_put(inode);   /* always, even on error paths */
    super_block_put(sb);
    return res->status;
```

Rules:
- **Always call `inode_active_put`** in every exit path, including error paths.
  Missing it leaks the active ref and the inode never returns to the LRU.
- **Never call `inode_active_put` twice.** Use a NULL-init and single exit label.
- **`inode_active_put(NULL)` is safe** — it's a no-op, so unconditional calls at
  `out:` are fine when the inode pointer may legitimately be NULL.
- **Don't store the raw pointer in `rd_inode` and call `inode_active_put`.**
  `rd_inode` is a weak pointer. If you want to store a just-created inode into
  `rd_inode`, assign it without `inode_get`, then call `inode_active_put` on
  the original pointer you received from `inode_alloc`/`inode_find`.

### The NFS create-op active-ref transfer

When a create op (CREATE, MKDIR, SYMLINK, MKNOD) calls `vfs_create` and receives
`new_inode` back, it holds the active ref that `vfs_create_common_locked`
transferred from `inode_alloc`. This ref must be held for the **entire duration of
the RPC** — including after `res->status` is set — because the NFS client will
immediately follow with a WRITE or GETATTR using the filehandle, and `inode_find`
in those handlers must succeed.

```c
/* vfs_create_common_locked: transfer active ref to caller, not drop it */
if (new_inode) {
    *new_inode = inode;  /* caller owns the active ref */
} else {
    inode_active_put(inode);  /* nobody wants it; drop now */
}
```

The NFS handler drops it at `out:`:
```c
out:
    inode_active_put(new_inode);  /* safe if NULL */
    inode_active_put(inode);      /* parent dir */
```

---

## 3. LRU Eviction — Inodes

### Structure

Each `super_block` has:
- `sb_inode_lru` — doubly-linked list of LRU-eligible inodes (tail = most recently idle)
- `sb_inode_lru_lock` — mutex protecting the list
- `sb_inode_lru_count` — current count
- `sb_inode_lru_max` — eviction threshold (default 64k)

An inode is on the LRU if and only if `i_active == 0` and
`INODE_IS_ON_LRU` is set in `i_state`.

### Eviction trigger

`inode_lru_add` is called when `i_active` hits 0. After adding to the LRU tail
it checks:
```c
if (sb->sb_inode_lru_count > sb->sb_inode_lru_max)
    super_block_evict_inodes(sb, sb->sb_inode_lru_count - sb->sb_inode_lru_max);
```

`super_block_evict_inodes` walks the LRU head (oldest), sets `i_active = -1`
(tombstone), unhashes the inode, and calls `inode_put` to drop the hash ref.
If no other `i_ref` is held, `inode_release` fires; the inode's dirty state was
already flushed to the backend by `inode_sync_to_disk` during the last write.

### Consequences for `rd_inode`

After eviction, `rd->rd_inode` still holds the pointer value but the inode memory
may be freed (after the RCU grace period). Any dereference of `rd_inode` outside
`rcu_read_lock` — or without first calling `inode_active_get` to check the
tombstone — is a use-after-free.

**`rd_inode` is never set to NULL on eviction.** The eviction path does not walk
the dirent tree. Only `dirent_release` nulls `rd_inode`.

---

## 4. Dirent Reference Counting

Each dirent has one ref count: `rd_ref` (`urcu_ref`). When `rd_ref` drops to zero,
`dirent_release` is called, which schedules `dirent_free_rcu`.

| Who holds `rd_ref` | Acquired by | Released by |
|---|---|---|
| Alloc ref | `dirent_alloc` (`urcu_ref_init` → `rd_ref = 1`) | caller `dirent_put` |
| List ref | `dirent_parent_attach` → `dirent_get(rd)` | `dirent_parent_release` (inline in `dirent_release`) |
| Parent back-ref | `dirent_parent_attach` → `rd->rd_parent = dirent_get(parent)` | `dirent_parent_release` → `dirent_put(parent)` |
| `sb->sb_dirent` alloc ref | `urcu_ref_init` in root dirent alloc | `rcu_xchg_pointer + dirent_put` in `super_block_release_dirents` |
| Lookup ref | `dirent_find` / `dirent_get` | caller `dirent_put` |

### `dirent_release` — the double-free trap

`dirent_release` is the `urcu_ref` callback fired when `rd_ref` hits zero. It must
not call `dirent_parent_release` because that function ends with `dirent_put(rd)`,
which would decrement `rd_ref` from 0 to -1 and re-enter `dirent_release`.

The fix is to inline the parent-detach logic *without* the trailing `dirent_put(rd)`:
```c
static void dirent_release(struct urcu_ref *ref)
{
    struct reffs_dirent *rd = caa_container_of(...);
    struct inode *inode = rd->rd_inode;
    rd->rd_inode = NULL;

    rcu_read_lock();
    parent = rcu_xchg_pointer(&rd->rd_parent, NULL);
    if (parent) {
        cds_list_del_rcu(&rd->rd_siblings);
        if (inode && S_ISDIR(inode->i_mode))
            inode->i_parent = NULL;
        dirent_put(parent);
        /* dirent_put(rd) deliberately omitted — rd_ref is already 0 */
    }
    rcu_read_unlock();
    call_rcu(&rd->rd_rcu, dirent_free_rcu);
}
```

### Dirent LRU

Dirents have a parallel LRU (`sb_dirent_lru`). A leaf dirent (one with an empty
`rd_inode->i_children` list) is added to the dirent LRU when its active-use drops.
Eviction via `super_block_evict_dirents` calls `dirent_parent_release` on the
dirent, removing it from the parent's `i_children` list and dropping the list ref.
This is a space optimisation for very flat, large directories.

---

## 5. `dirent_ensure_inode` — Safe Inode Fault-In

`dirent_ensure_inode` is the canonical way to obtain an active-ref inode from a
dirent that may have had its inode evicted. Callers **must** call `inode_active_put`
when done.

```
dirent_ensure_inode(rd):
    fast path:
        rcu_read_lock
        inode = rd->rd_inode
        if inode: inode = inode_active_get(inode)   # handles tombstone
        rcu_read_unlock
        if inode: return inode

    miss path (rd_inode was evicted):
        if !rd->rd_ino: return NULL   # never fully attached
        walk rd->rd_parent chain to find any resident inode -> sb
        if !sb: return NULL
        inode = inode_alloc(sb, rd->rd_ino)  # finds in hash or loads from backend
        if inode:
            rd->rd_inode = inode   # restore weak pointer (benign race)
            if dir: inode->i_parent = rd
        return inode
```

Key constraints:
- **Must NOT be called inside `rcu_read_lock`** — the miss path calls `inode_alloc`
  which takes `sb_inode_lru_lock` (a mutex; blocking).
- **Must NOT be called while holding `rd_rwlock` as reader** if any other thread
  may try to acquire `rd_rwlock` as writer (e.g. via `vfs_lock_dirs` in a create
  op). This is the deadlock that caused the git clone hang (see §7).
- Returns NULL on permanent failure (dirent never fully attached, or sb unreachable).
  Callers must treat NULL as a hard error (EIO/SERVERFAULT), not a soft skip.

---

## 6. The `rd_rwlock` / `i_children` Locking Model

`dir_de->rd_rwlock` (a `pthread_rwlock_t` on the dirent that *owns* a directory
inode) protects the `inode->i_children` linked list:

- **Reader** — safe to traverse `i_children` (e.g. READDIR, READDIRPLUS snapshot phase)
- **Writer** — required to modify `i_children` (e.g. `vfs_lock_dirs` in all create/rename/unlink ops)

`vfs_lock_dirs` acquires `rd_rwlock` as a **writer**, after acquiring `i_attr_mutex`,
in inode-ID order to prevent deadlock between two-directory operations (rename).

`rd_siblings` (the per-dirent list link) is RCU-protected: traversal under
`rcu_read_lock` is safe without holding `rd_rwlock`. Modification requires the
writer lock.

### Root inode special case

The root inode has `i_parent == NULL`. Any code that needs `dir_de` (the dirent
owning the directory) must guard:
```c
struct reffs_dirent *dir_de =
    inode->i_parent ? inode->i_parent : sb->sb_dirent;
```
`sb->sb_dirent` is always present for the lifetime of the superblock.

---

## 7. What Went Wrong — Bug History

### The original problem (prior session)

`vfs_create_common_locked` assigned `rd->rd_inode = inode` (correct) but never set
`rd->rd_ino`. `dirent_parent_attach` runs *before* `rd->rd_inode` is assigned, so
it could not set `rd_ino` there either. Result: every newly created dirent had
`rd_ino = 0`. `dirent_ensure_inode` would see `!rd->rd_ino` and return NULL for
any dirent whose inode was evicted — effectively making all post-eviction lookups
fail with ENOENT or SERVERFAULT.

**Fix:** Set `rd->rd_ino = inode->i_ino` at every site that sets `rd->rd_inode`
in `vfs.c` (three sites: `vfs_create_common_locked`, `vfs_exclusive_create_locked`,
`vfs_link`).

### CREATE→WRITE race (this session, Bug G)

`vfs_create_common_locked` was calling `inode_active_put` before returning,
dropping `i_active` to 0. The inode went immediately onto the LRU. The NFS client
follows CREATE with WRITE in the same TCP stream; by the time the WRITE arrived,
the inode had been evicted. `inode_find` in the WRITE handler returned NULL →
NFS3ERR_NOENT.

**Fix:** Transfer the active ref to the NFS handler via `*new_inode = inode`
instead of dropping it. The handler calls `inode_active_put(new_inode)` at its
`out:` label, keeping `i_active > 0` for the entire RPC.

### READDIR/READDIRPLUS deadlock (this session, Bug H)

READDIR and READDIRPLUS held `dir_de->rd_rwlock` (reader) across phase 2, which
included calls to `dirent_ensure_inode`. Under load (git clone, cthon04), all
worker threads queued as writers via `vfs_lock_dirs`. The reader could not release
because `dirent_ensure_inode` was blocked on `sb_inode_lru_lock`. The writers
could not proceed because they were blocked on the reader. Classic RW-lock
starvation deadlock.

**Fix:** Release `rd_rwlock` after phase 1 (the RCU snapshot), before phase 2.
A `dir_de_rdlocked` flag handles early-exit `goto update_wcc` paths that branch
before phase 1 completes.

```
Phase 1:  rcu_read_lock → snapshot rd/rd_ino/rd_cookie/rd_name → rcu_read_unlock
          pthread_rwlock_unlock(&dir_de->rd_rwlock)    ← released here
          dir_de_rdlocked = false

Phase 2:  for each snap entry:
              dirent_ensure_inode(snap[i].rd)           ← safe: no locks held
              build reply entry
```

---

## 8. Quick Reference: Correct Usage Patterns

### NFS op reading a file/directory

```c
inode = inode_find(sb, nfh->nfh_ino);
if (!inode) { ret = -ENOENT; goto out; }

/* read inode fields — i_attr_mutex for attributes */
pthread_mutex_lock(&inode->i_attr_mutex);
/* ... */
pthread_mutex_unlock(&inode->i_attr_mutex);

out:
    res->status = errno_to_nfs3(ret);
    inode_active_put(inode);   /* NULL-safe */
    super_block_put(sb);
    return res->status;
```

### NFS op creating a file

```c
/* parent dir */
ret = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &inode);
if (ret) goto out;

ret = vfs_create(inode, name, mode, &ap, &new_inode);
if (ret) goto update_wcc;

/* new_inode now holds active ref transferred from vfs_create_common_locked */
/* build reply using new_inode ... */

out:
    res->status = errno_to_nfs3(ret);
    inode_active_put(new_inode);   /* holds i_active > 0 until here */
    inode_active_put(inode);
    super_block_put(sb);
    return res->status;
```

### Safely reading rd_inode in a dirent traversal

```c
/* Inside rcu_read_lock — only safe to read, not dereference deeply */
rcu_read_lock();
struct inode *inode = rd->rd_inode;
if (inode)
    inode = inode_active_get(inode);  /* promotes or returns NULL */
rcu_read_unlock();

if (!inode) {
    /* Miss: use dirent_ensure_inode outside any lock */
    inode = dirent_ensure_inode(rd);  /* may hit backend */
    if (!inode) { /* hard error */ }
}
/* ... use inode ... */
inode_active_put(inode);
```

### Things that are NEVER correct

```c
/* ❌ Storing a strong ref in a weak pointer slot */
rd->rd_inode = inode_get(inode);     /* leaks i_ref */

/* ❌ Reading rd_inode without rcu_read_lock or active ref */
size = rd->rd_inode->i_size;         /* UAF if evicted */

/* ❌ Calling dirent_ensure_inode inside rcu_read_lock */
rcu_read_lock();
inode = dirent_ensure_inode(rd);     /* deadlock: takes sb_inode_lru_lock */
rcu_read_unlock();

/* ❌ Calling dirent_ensure_inode while holding rd_rwlock (reader) */
pthread_rwlock_rdlock(&dir_de->rd_rwlock);
inode = dirent_ensure_inode(rd);     /* deadlock under concurrent creates */

/* ❌ Missing inode_active_put on error path */
inode = inode_find(sb, ino);
if (something_failed)
    return -EIO;                     /* leaked active ref */
```

---

## 9. Open Issue: `reffs_fs_*` Unprotected `rd_inode` Access

All `reffs_fs_*` operations (the POSIX backend layer) access
`nm->nm_dirent->rd_inode` as a raw pointer after a path walk, with no
`inode_active_get`. Under LRU pressure the inode can be evicted between
`dirent_find` returning and the field access, leaving a dangling pointer.

Planned fix: a `name_match_get_inode(nm)` helper that calls
`inode_active_get(nm->nm_dirent->rd_inode)` and returns an active ref,
with `inode_active_put` before `name_match_free`. This centralises the
protection across all `reffs_fs_*` callers.

At `lru_max = 64k` (default) this races rarely in practice, but it is a
latent correctness hole under memory pressure.
