# reffs: NFS3 correctness debugging — session handoff

**Date:** 2026-03-08 (second session)
**Session scope:** NFS3 READDIR/READDIRPLUS correctness, CREATE→WRITE active-ref fix, deadlock diagnosis
**Status:** cthon04 basic tests pass. git clone hangs — root cause identified (reader/writer lock ordering), fix applied but not yet verified. cthon04 now re-hanging at basic test start — under investigation.

---

## 1. Bugs fixed this session

### Bug A — READDIRPLUS `..` fileid wrong (`nfs3_server.c`)
`parent_ino` computation used `inode->i_parent->rd_inode->i_ino` (self) instead of
`inode->i_parent->rd_parent->rd_inode->i_ino` (actual parent). Copy-paste divergence
from READDIR. Fixed to match READDIR.

### Bug B — Bare `rd_inode` dereference inside `rcu_read_lock` (`nfs3_server.c`)
Both READDIR and READDIRPLUS child loops called `inode_active_get(rd->rd_inode)` inside
`rcu_read_lock`. `inode_active_get` takes `sb_inode_lru_lock` (blocking) — illegal inside
RCU read-side. Also `dirent_ensure_inode` can hit storage backend (blocking).

**Fix: Two-phase snapshot pattern** in both handlers:
- Phase 1 (inside `rcu_read_lock`): snapshot `rd`, `rd_ino`, `rd_cookie`, `rd_name` into heap array. No inode access.
- Phase 2 (outside RCU): call `dirent_ensure_inode(snap[si].rd)` freely; fault in evicted inodes from disk.
- NULL from `dirent_ensure_inode` → hard EIO/SERVERFAULT (not silent skip).

### Bug C — Wrong list iterated: `inode->i_parent->rd_inode->i_children` (`nfs3_server.c`)
Both loops iterated the *parent directory's* children (siblings of the listed dir) instead
of `inode->i_children` (the listed directory's own children). Fixed to `inode->i_children`.

### Bug D — NULL dereference on root inode `i_parent` (`nfs3_server.c`)
Root inode has `i_parent == NULL`. Both handlers did `inode->i_parent->rd_rwlock` (crash)
and `inode->i_parent->rd_parent->rd_inode->i_ino` (crash).

**Fix:** Introduced `dir_de` in both handlers:
```c
struct reffs_dirent *dir_de =
    inode->i_parent ? inode->i_parent : sb->sb_dirent;
```
Used for rwlock acquire/release and `..` ino computation.

### Bug E — `dirent_ensure_inode` wrong bail condition (`dirent.c`)
```c
if (!rd->rd_ino || !rd->rd_inode) { return NULL; }  // was wrong
```
The `!rd->rd_inode` check incorrectly treated eviction (the case to handle) as
"never attached". Removed `!rd->rd_inode`; only `!rd->rd_ino` is the valid guard.
Also: sb walk started at `rd->rd_parent`; fixed to start at `rd` itself.

### Bug F — `rd_ino` never set on newly created dirents (`vfs.c`)
**Root cause:** `dirent_alloc` → `dirent_parent_attach` runs before `rd->rd_inode` is
assigned. So `rd->rd_ino` was never set — stayed 0 forever.

**Fix:** Added `rd->rd_ino = inode->i_ino` immediately after every `rd->rd_inode = inode`
assignment. Three sites in `vfs.c`:
- `vfs_create_common_locked`
- `vfs_exclusive_create_locked`
- `vfs_link`

### Bug G — CREATE→WRITE race: inode evicted before WRITE arrives (`vfs.c`)
**Root cause:** `vfs_create_common_locked` called `inode_active_put` before returning,
dropping `i_active` to 0. The inode became LRU-eligible immediately. Client follows
CREATE with WRITE using the returned filehandle; `inode_find` in the WRITE handler
returned NULL → NFS3ERR_NOENT.

**Fix:** Transfer active ref to caller instead of dropping it:
```c
if (new_inode) {
    *new_inode = inode;  /* transfer active ref to caller */
} else {
    inode_active_put(inode);
}
```
Applied in `vfs_create_common_locked`, `vfs_exclusive_create_locked`, and `vfs_symlink`
(which had an additional `inode_get`/`inode_put` that was leaking the active ref).

NFS handlers (CREATE, MKDIR, SYMLINK, MKNOD) already call `inode_active_put(new_inode)`
at their `out:` label — they correctly own and drop the ref for the duration of the RPC.

### Bug H — READDIR/READDIRPLUS deadlock under concurrent creates (`nfs3_server.c`)
**Root cause:** `rd_rwlock` was held as a **reader** across all of phase 2 (including
`dirent_ensure_inode`, which can block on `inode_alloc` → LRU → `sb_inode_lru_lock`).
Meanwhile `vfs_lock_dirs` in the create path takes `rd_rwlock` as a **writer**. Under
load (git clone, cthon04), all worker threads queued as writers; the reader couldn't
release because it was blocked in `dirent_ensure_inode`; writers blocked on the reader.
Classic RW-lock starvation deadlock.

**Fix:** Release `rd_rwlock` after phase 1 (snapshot complete), before phase 2:
```c
pthread_rwlock_unlock(&dir_de->rd_rwlock);
dir_de_rdlocked = false;
/* Phase 2: dirent_ensure_inode calls safe here */
```
`dir_de_rdlocked` flag added to both handlers so early-exit `goto update_wcc` paths
(which fire before phase 1 completes) still unlock correctly via conditional at
`update_wcc`.

---

## 2. Files modified this session

| File | Changes |
|------|---------|
| `nfs3_server.c` | Bugs A, B, C, D, H: two-phase snapshot; dir_de for root NULL i_parent; rd_rwlock released after phase 1; dir_de_rdlocked flag |
| `dirent.c` | Bug E: `dirent_ensure_inode` bail condition; sb walk starts from `rd` itself |
| `vfs.c` | Bugs F, G: `rd_ino` set at all three create sites; active-ref transfer to caller in `vfs_create_common_locked`, `vfs_exclusive_create_locked`, `vfs_symlink` |

---

## 3. Current test status

| Test | Status | Notes |
|------|--------|-------|
| `echo foo > /mnt/cthon04/foo` + `ls` | ✅ Pass | CREATE→WRITE→READDIR working |
| `cthon04` basic (first run) | ✅ Pass | All basic tests passed |
| `cthon04` basic (second run, new container) | ❌ Hang | Hangs immediately at "Running basic tests..." — no RPC logged |
| `git clone` to NFS | ❌ Hang | Fixed rd_rwlock deadlock (Bug H); not yet re-tested with fix |

---

## 4. Open investigation: cthon04 re-hang

After the Bug H fix was deployed, cthon04 hangs again immediately at test start.
The server is **not** deadlocked — gdb shows all threads idle (io_uring waiting,
workers on `pthread_cond_timedwait`, RCU threads idle). The TCP connections have
Recv-Q=0, Send-Q=0. The server console shows no RPCs received after startup.

This suggests the RPC is arriving at TCP but not being dispatched by io_uring, or
the mount itself is hanging before any test RPC. The NFS client shows 126K retransmits
but those are cumulative from the prior cthon04 run that passed.

**Next steps:**
1. Check the trace file (`/logs/reffs.trc`) — it will show what RPCs the server
   actually received and whether any op is returning without sending a reply.
2. Run `tcpdump -i lo port 2049` on dreamer during the hang to see if packets are
   arriving and whether replies are sent.
3. Check if the hang is on MOUNT (before any NFS ops) or on the first NFS op
   (likely GETATTR on the root filehandle).
4. The `script -q -c` wrapper in `run-image` — verify it's not buffering stdout
   and preventing the server from seeing its own terminal.

---

## 5. Architecture notes (accumulated)

- `inode->i_parent` is NULL for root inode; use `sb->sb_dirent` as `dir_de` fallback
- `rd_rwlock` on a dirent protects its `i_children` list; must NOT be held across blocking ops
- `rd_siblings` is RCU-protected only (no rwlock on writers)
- `dirent_ensure_inode` opens its own `rcu_read_lock` internally — must NOT be called while holding `rcu_read_lock`
- `rd_ino` is the authoritative stable ino; `rd_inode` is weak/nullable
- `inode_active_get` handles NULL and tombstone (-1) internally
- `vfs_lock_dirs` acquires `i_attr_mutex` + `rd_rwlock` as writer (in inode-ID order for two-dir ops)
- Active ref must be held on `new_inode` for entire duration of NFS create RPC to prevent LRU eviction before client's follow-up WRITE

---

## 6. Pending issues

### 6a. cthon04 re-hang after Bug H fix
See §4 above. Server is live but not dispatching. Trace file investigation needed.

### 6b. git clone hang
Bug H (rd_rwlock deadlock) was the identified cause. Fix applied to `nfs3_server.c`
but git clone not yet re-tested with the new binary.

### 6c. `reffs_fs_*` access `rd_inode` without `inode_active_get` (from prior session §6b)
All `reffs_fs_*` operations access `nm_dirent->rd_inode` as a raw pointer. Under LRU
pressure the inode can be evicted between `dirent_find` and the field access.
Fix: `name_match_get_inode(nm)` helper returning active ref; `inode_active_put` before
`name_match_free`.

### 6d. `dirent_ensure_inode` `!rd->rd_inode` removal — verify no callers relied on it
The `!rd->rd_inode` bail guard was removed (Bug E). Confirm no other caller passed a
dirent with `rd_ino != 0` but `rd_inode == NULL` intending to get NULL back.
