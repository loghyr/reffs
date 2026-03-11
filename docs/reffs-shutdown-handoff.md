<!--
SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# reffs: NFS3 correctness debugging — session handoff

**Date:** 2026-03-08 (second session)
**Session scope:** NFS3 READDIR/READDIRPLUS correctness, CREATE→WRITE active-ref fix, deadlock diagnosis
**Status:** cthon04 basic tests pass. git clone passes. Both resolved in Session 4 (io_uring FD capacity fix — MAX_CONNECTIONS raised to 65536). LRU fault-in unit tests WIP pending i_dirent back-pointer (§8d/§10.5).

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

## 4. ~~Open investigation: cthon04 re-hang~~ — RESOLVED (see §8a)

**RESOLVED in Session 4.** Root cause was io_uring FD capacity exhaustion
(`MAX_CONNECTIONS=1024`), not a dispatch bug. See §8a for details.

Historical notes: After the Bug H fix was deployed, cthon04 hung again immediately
at test start. The server was not deadlocked — gdb showed all threads idle. No RPCs
were received after startup, suggesting the hang was at the io_uring dispatch layer.
The next-steps investigation below was superseded by the FD capacity discovery.

~~**Next steps:**~~
~~1. Check the trace file (`/logs/reffs.trc`)~~
~~2. Run `tcpdump -i lo port 2049`~~
~~3. Check if the hang is on MOUNT or first NFS op~~
~~4. The `script -q -c` wrapper in `run-image`~~

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

### 6a. cthon04 re-hang after Bug H fix — RESOLVED
See §8a. Fixed in Session 4 by raising MAX_CONNECTIONS to 65536.

### 6b. git clone hang — RESOLVED
See §8b. Bug H fix plus MAX_CONNECTIONS increase both applied; git clone passes.

### 6c. `reffs_fs_*` access `rd_inode` without `inode_active_get` (from prior session §6b)
All `reffs_fs_*` operations access `nm_dirent->rd_inode` as a raw pointer. Under LRU
pressure the inode can be evicted between `dirent_find` and the field access.
Fix: `name_match_get_inode(nm)` helper returning active ref; `inode_active_put` before
`name_match_free`.

### 6d. `dirent_ensure_inode` `!rd->rd_inode` removal — verify no callers relied on it
The `!rd->rd_inode` bail guard was removed (Bug E). Confirm no other caller passed a
dirent with `rd_ino != 0` but `rd_inode == NULL` intending to get NULL back.

---

## 7. Session 3 — LRU eviction unit tests (2026-03-08)

**Session scope:** Writing `fs_test_lru.c` and `fs_test_ns_teardown.c`; debugging
three successive ASAN crashes that exposed a structural invariant violation in
`release_dirents_recursive`.

### 7.1 New test files

**`lib/fs/tests/fs_test_lru.c`**

Tests the inode LRU lifecycle directly via `reffs_fs_*()` and internal APIs.
Three suites:

- *Eviction accounting and pressure* (always-run, must be green): lru_count
  invariants, active-ref pinning, no-leak on rmdir/unlink, burst creates, Bug G
  regression.
- *Structural eviction probe* (always-run): verifies `i_active == -1` after organic
  eviction, `i_ino` stability until `inode_free_rcu`, `inode_find` behaviour on a
  fully-unhashed ino.
- *Eviction fault-in* (non-blocking known-bug suite): tests that `reffs_fs_*()`
  operations correctly reload an evicted inode via `dirent_ensure_inode`. All six
  tests fail until §6c (`name_match_get_inode`) is fixed. They live in a separate
  `SRunner` so failures appear in output but do not affect the exit status.

**`lib/fs/tests/fs_test_ns_teardown.c`**

Tests `reffs_ns_fini()` / `release_all_fs_dirents()` directly. Four suites:
basic teardown (empty/file/dir/deep/wide/mixed tree), direct `rafd()` calls,
ordering and RCU safety, idempotency.

### 7.2 Critical invariant discovered: `release_dirents_recursive` and `rd_inode`

`release_dirents_recursive()` in `super_block.c` dereferences
`rd_parent->rd_inode->i_children` to walk the dirent tree during teardown.
Combined with the fact that **`rd_inode` is never nulled by the LRU eviction path**
(architecture doc §3), this creates a hard precondition:

> **Every `rd_inode` in the dirent tree must point to live (non-freed) memory when
> `release_dirents_recursive` runs.**

After `super_block_evict_inodes` fires and the subsequent RCU grace period elapses,
`inode_free_rcu` has run and the inode struct is freed memory. Any test that forces
LRU eviction and leaves those dirents in the tree will UAF in teardown.

Three successive crash attempts (2026-03-08) each found a new variant of this:

| Attempt | Crash site | Why it failed |
|---------|-----------|---------------|
| Direct dereference (original code) | `super_block.c:159` READ 8 bytes | `rd_inode` is freed memory |
| `dirent_ensure_inode` guard | `urcu/ref.h:83` READ 8 bytes in `urcu_ref_get_unless_zero` | fast path calls `inode_active_get(rd_inode)` without verifying struct is allocated; same UAF one frame deeper |
| Phase-0 drain before dirent walk | `super_block.c:159` same crash | `super_block_drain` calls `rcu_barrier()` internally, which completes `inode_free_rcu`; walk runs on freed memory |

**Root cause:** There is no back-pointer from inode to dirent. `inode_release` cannot
null `rd_inode` on all dirents pointing at it because it does not know which dirents
those are. `i_parent` covers directory inodes only; file inodes have no back-pointer.

**Long-term fix (TODO):** Add `i_dirent` as a weak back-pointer on `struct inode`.
Set it in `dirent_parent_attach` and `super_block_dirent_create`. Null `rd_inode` in
`inode_release()` before `call_rcu`. This makes the invariant self-enforcing without
any test-side workarounds.

### 7.3 Test-side workaround for the invariant

Until `i_dirent` lands, any test that forces LRU eviction must null the dangling
`rd_inode` pointers before teardown. The safe window is **after `synchronize_rcu()`
but before `rcu_barrier()`**:

- After `synchronize_rcu()`: evicted inode structs are queued for `inode_free_rcu`
  but not yet freed. `inode_active_get(rd_inode)` returns NULL safely (tombstone
  check; `i_active == -1`). That NULL return is the signal to null `rd_inode`.
- After `rcu_barrier()`: `inode_free_rcu` has run; the struct is freed memory.
  Any dereference — even inside `rcu_read_lock`, even `inode_active_get` — is a UAF.

**`fs_test_lru.c`** (known probe dirent): saves `probe_rd` via `dirent_find` before
eviction, then after `inode_put + synchronize_rcu`:
```c
rcu_assign_pointer(probe_rd->rd_inode, NULL);
synchronize_rcu();   /* flush NULL store */
rcu_barrier();       /* now safe: inode_free_rcu completes after NULL */
```

**`fs_test_ns_teardown.c`** (unknown set of evicted dirents): `null_evicted_rd_inodes()`
walks the full dirent tree after `synchronize_rcu()`, calling `inode_active_get` on
each `rd_inode`. NULL return → `rcu_assign_pointer(rd->rd_inode, NULL)`. Then
`synchronize_rcu() + rcu_barrier()`.

### 7.4 `release_dirents_recursive` comment

The function in `super_block.c` now carries a full explanation of the precondition,
the two failed fix attempts, and the `i_dirent` TODO. The function body is
**unchanged** from the original — the correct fix is the back-pointer, not a guard
inside the walker.

### 7.5 `test_teardown_with_pinned_inode` — nlink and leak

**Assertion bug:** The test checked `pinned->i_nlink == 1` after `reffs_ns_fini()`.
This is wrong: `dirent_parent_release(reffs_life_action_death)` subtracts nlink as
part of the dirent walk, so `i_nlink == 0` is the correct post-teardown value.
Fixed to assert `i_nlink == 0` and `i_ino == st.st_ino` (the latter proves the
struct is still addressable, which is the actual invariant being tested).

**Leak as consequence:** The wrong assertion caused libcheck to longjmp before
`inode_active_put(pinned)` was called, leaving `i_ref = 1` permanently. LeakSanitizer
reported 504 bytes (one inode struct). Once the assertion was fixed the leak
disappeared — it was never a ref-counting bug in the production code.

**Ref accounting for pinned-inode pattern** (for future reference):
```
reffs_fs_create → inode_alloc → i_ref=1(hash), i_active=1
vfs returns → inode_active_put → i_active=0, i_ref=1(hash only)
inode_find → inode_active_get → i_ref=2, i_active=1
reffs_ns_fini → super_block_drain → inode_unhash+inode_put → i_ref=1
inode_active_put(pinned) → i_active=0, inode_put → i_ref=0 → inode_release → call_rcu
synchronize_rcu + rcu_barrier → inode_free_rcu → clean
```

### 7.6 Known-bug suite pattern

For tests that document unfixed bugs without blocking CI, the correct libcheck
idiom (with `CK_NOFORK`) is a **separate `SRunner`** whose failure count is not
added to `failed`:

```c
/* Primary: controls exit status */
SRunner *sr = srunner_create(fs_lru_suite());
srunner_set_fork_status(sr, CK_NOFORK);
srunner_run_all(sr, CK_NORMAL);
failed = srunner_ntests_failed(sr);
srunner_free(sr);

/* Known bugs: visible in output, does not affect exit status */
SRunner *sr_bugs = srunner_create(fs_lru_known_bugs_suite());
srunner_set_fork_status(sr_bugs, CK_NOFORK);
srunner_run_all(sr_bugs, CK_VERBOSE);
srunner_free(sr_bugs);

return failed ? EXIT_FAILURE : EXIT_SUCCESS;
```

`tcase_skip` is not usable with `CK_NOFORK` (it relies on `exit(77)` which kills
the process). `ck_assert_msg(false, ...)` reports FAIL in the primary suite and
blocks CI. The two-srunner split is the correct approach.

---

## 8. Updated pending issues

### 8a. cthon04 re-hang (was §6a) — RESOLVED
Fixed in Session 4. Root cause: io_uring FD capacity exhaustion. The POSIX backend
holds one data-block FD open per inode; under high concurrency (cthon04, git clone)
this exceeded the old `MAX_CONNECTIONS=1024` limit in `io.c`. Fix: raised
`MAX_CONNECTIONS` to 65536 and removed the restrictive `fd >= MAX_CONNECTIONS`
checks in `io_request_read_op` / `io_request_write_op`. Both cthon04 and git clone
pass consistently with the new binary.

### 8b. git clone hang (was §6b) — RESOLVED
Resolved by the same io_uring FD capacity fix as §8a. Bug H (rd_rwlock deadlock)
was also a contributor but FD exhaustion was the primary cause of the hang under
git clone's high concurrency. Both fixes are in place and git clone passes.

### 8c. Raw `rd_inode` UAF in `reffs_fs_*()` — `name_match_get_inode` (was §6c)
All `reffs_fs_*()` operations and `vfs_remove_common_locked()` access `rd_inode`
without `inode_active_get`. This is the blocker for the fault-in test suite.

**Fix:** `name_match_get_inode(nm)` helper in `fs.c` that calls
`dirent_ensure_inode(nm->nm_dirent)` and stores the result; `inode_active_put`
before `name_match_free`. Same fix needed in `vfs_remove_common_locked` in `vfs.c`.

Until this lands: the structural probe and `raw_rdino_bug_present()` in
`fs_test_lru.c` use organic eviction + `rcu_assign_pointer` to null `rd_inode`
before teardown (see §7.3).

### 8d. `i_dirent` back-pointer — self-enforcing teardown invariant (new)
`release_dirents_recursive` requires that no `rd_inode` in the tree is freed memory
when it runs. Currently enforced only by test-side workarounds (§7.3).

**Fix:** Add `i_dirent` weak back-pointer to `struct inode`. Set in
`dirent_parent_attach` and `super_block_dirent_create`. In `inode_release()`,
before `call_rcu`:
```c
if (inode->i_dirent)
    rcu_assign_pointer(inode->i_dirent->rd_inode, NULL);
```
Once this lands, remove `null_evicted_rd_inodes()` from `fs_test_ns_teardown.c`,
remove the `rcu_assign_pointer` blocks from `fs_test_lru.c`, and remove the TODO
comment in `super_block.c`.

### 8e. `dirent_ensure_inode` fast path not safe after `rcu_barrier` (new)
`dirent_ensure_inode`'s fast path calls `inode_active_get(rd->rd_inode)` inside
`rcu_read_lock`. This is safe during normal operation (the RCU grace period prevents
struct freeing while a reader holds the lock). It is NOT safe after `rcu_barrier()`
has already completed, because `inode_free_rcu` has run and the struct is freed
memory before the RCU reader lock is acquired.

Do not call `dirent_ensure_inode` on a potentially-evicted dirent after `rcu_barrier`.
The safe window is after `synchronize_rcu()` and before `rcu_barrier()`.

### 8f. `dirent_ensure_inode` `!rd->rd_inode` removal — verify callers (was §6d)
Unchanged.

---

## 9. Session 4 — LRU teardown UAF final resolution; pinned-inode test fix (2026-03-08)

**Session scope:** Resolving four additional ASAN crashes encountered while running
the session-3 test files for the first time; fixing a wrong assertion in
`test_teardown_with_pinned_inode`.

### 9.1 Crash 1: `reffs_fs_unlink` after `drain_lru` — UAF in `vfs_remove_common_locked`

The tombstone probe test originally ended with `reffs_fs_unlink("/tombstone_probe")`
after `inode_put(inode) + rcu_barrier()`. LTTng trace:

```
reffs_fs_getattr:402  ret=0           ← succeeded; inode still live at this point
inode_release:133     ino=2           ← drain_lru fired 10ms later during inode_put chain
reffs_fs_unlink:613   path=/tombstone_probe
vfs_remove_common_locked:163  READ 8 bytes  ← UAF on freed inode
```

`vfs_remove_common_locked` has the same raw `rd_inode` dereference as `fs.c:387` —
the §6c / §9 bug affects every `vfs_*` function that touches inodes after a path walk,
not just `reffs_fs_*()` in `fs.c`. **The fix scope is wider than originally described.**

The rule: after `drain_lru()` (or any path that calls `rcu_barrier()` after eviction),
no `reffs_fs_*()` call may be made on any path whose inode was evicted. The unlink
calls were deleted from `raw_rdino_bug_present()` and `test_lru_eviction_produces_tombstone`;
those files are left for teardown to clean up.

**Additional finding from the trace:** `reffs_fs_getattr` succeeded (ret=0) even
though `drain_lru()` fired 10ms later. This means `raw_rdino_bug_present()`'s earlier
design — probing via `reffs_fs_getattr` before the `inode_find/inode_get` sequence —
would have returned false even with the bug present, because timing prevented the
tombstone from being visible. The atomic `i_active` read approach in the final
implementation is the only reliable probe.

### 9.2 Crash 2: organic eviction still UAFs in `release_dirents_recursive`

After removing `drain_lru()` and replacing it with organic eviction (`lru_max=1` +
one extra create), the tombstone probe still crashed:

```
inode_release:133  ino=2          ← triggered by /tombstone_trigger create
dirent_get:156     ref=5          ← teardown starts walking
release_directs_recursive:159  READ UAF  ← rd_inode on probe dirent is freed
```

The leaf-file assumption was wrong: `release_dirents_recursive` dereferences `rd_inode`
on **every node it visits**, not just directories. Line 159 reads through `rd_inode`
unconditionally before deciding whether to recurse. Even a leaf file's `rd_inode` is
read during the dirent walk.

This means organic eviction is just as unsafe as `drain_lru()` for any test that
leaves an evicted inode's dirent in the tree.

### 9.3 Safe window: after `synchronize_rcu()`, before `rcu_barrier()`

The resolution is a test-side workaround using a narrow timing window:

- After `synchronize_rcu()`: `inode_free_rcu` is queued but not yet run. The struct
  is still allocated. `inode_active_get(rd_inode)` returns NULL safely — the tombstone
  check (`i_active == -1`) fires before any memory access that could fault, and the
  NULL return signals "evicted."
- After `rcu_barrier()`: `inode_free_rcu` has run. The struct is freed. Any access —
  even `inode_active_get` inside `rcu_read_lock` — is a UAF.

**Critical ordering rule:** `rcu_assign_pointer(rd->rd_inode, NULL)` must be called
after `synchronize_rcu()` but before `rcu_barrier()`.

`fs_test_lru.c` uses this for the known probe dirent:
```c
inode_put(inode);
synchronize_rcu();
rcu_assign_pointer(probe_rd->rd_inode, NULL);  /* BEFORE rcu_barrier */
synchronize_rcu();                              /* flush the NULL store */
rcu_barrier();                                  /* now safe: struct freed after NULL */
dirent_put(probe_rd);
```

`fs_test_ns_teardown.c` uses `null_evicted_rd_inodes()` for an unknown set:
```c
synchronize_rcu();   /* structs queued but live */
/* walk tree: inode_active_get → NULL → rcu_assign_pointer(rd->rd_inode, NULL) */
synchronize_rcu();
rcu_barrier();
```

The `null_evicted_rd_inodes_recursive` walker loads `rd_parent->rd_inode` inside
`rcu_read_lock` to protect the pointer load itself (the struct may still be freed
between the load and `inode_active_get`; `rcu_read_lock` ensures the RCU grace period
hasn't completed, so the struct is still allocated at load time).

### 9.4 `dirent_ensure_inode` fast path: not safe after `rcu_barrier`

The initial attempt to fix `release_directs_recursive` by adding a
`dirent_ensure_inode` guard failed because `dirent_ensure_inode`'s fast path is:

```c
rcu_read_lock();
inode = rd->rd_inode;
if (inode)
    inode = inode_active_get(inode);  /* calls urcu_ref_get_unless_zero */
rcu_read_unlock();
```

`inode_active_get` calls `urcu_ref_get_unless_zero` which reads `inode->i_ref`.
If `inode_free_rcu` has already run (i.e., `rcu_barrier()` has completed), the
inode struct is freed memory and this read is a UAF — ASAN crashes at
`urcu/ref.h:83`. `rcu_read_lock` does not help here because `rcu_barrier()` waited
for all RCU callbacks to complete before the lock was acquired.

Rule: Do not call `dirent_ensure_inode` on a potentially-evicted dirent after
`rcu_barrier()` has run.

### 9.5 `drain_lru` safe vs unsafe call sites

`drain_lru()` remains in the file but is only safe in specific contexts:

**Safe:**
- Called *before* any creates (establishing a baseline lru_count).
- Called *after* `rmdir` or `unlink` — those operations remove the dirent from the
  tree, so no in-tree dirent has a dangling `rd_inode` after the drain.
- Called inside fault-in tests (guarded by `SKIP_IF_RAW_RDINO_BUG()`) — only
  reached when the `name_match_get_inode` fix is in place, at which point
  `dirent_ensure_inode` works correctly.

**Unsafe:**
- Called after creates that leave files in the tree, when teardown will subsequently
  call `reffs_ns_fini()`.

### 9.6 `test_teardown_with_pinned_inode` — wrong assertion, leak as consequence

**Assertion bug:** `ck_assert_uint_eq(pinned->i_nlink, 1)` failed with `i_nlink == 0`.
`reffs_ns_fini()` calls `release_all_fs_dirents()` → `dirent_parent_release(reffs_life_action_death)`,
which subtracts nlink as part of the dirent walk. `i_nlink == 0` is the correct
post-teardown value. Fixed to:
```c
ck_assert_uint_eq(pinned->i_ino, st.st_ino);  /* memory validity check */
ck_assert_uint_eq(pinned->i_nlink, 0);         /* correct post-teardown value */
```

**Leak as consequence:** The failing assertion caused libcheck's longjmp before
`inode_active_put(pinned)` was called. `i_ref` stayed at 1 permanently → LeakSanitizer
reported 504 bytes. Not a ref-counting bug in production code — fixing the assertion
eliminated the leak entirely.

**Ref accounting for pinned-inode pattern:**
```
reffs_fs_create → i_ref=1(hash), i_active=1
vfs returns      → inode_active_put → i_active=0, i_ref=1(hash only)
inode_find       → inode_active_get → i_ref=2, i_active=1
reffs_ns_fini    → super_block_drain → inode_unhash+inode_put → i_ref=1
inode_active_put → i_active=0, inode_put → i_ref=0 → inode_release → call_rcu
synchronize_rcu + rcu_barrier → inode_free_rcu → clean
```

### 9.7 Updated pending issues (carry-forward from §8)

**§8c (raw rd_inode UAF)** — wider than previously described. The bug is in every
function that touches `rd_inode` without `inode_active_get`:
- `reffs_fs_*()` in `fs.c` (all ops via `name_match_get_inode` path)
- `vfs_remove_common_locked()` in `vfs.c` (confirmed by crash 1 above)
- Likely other `vfs_*` functions as well

The fix (`name_match_get_inode` + `inode_active_get`) must be applied to `vfs.c`
as well as `fs.c`.

**§8d (`i_dirent` back-pointer)** — still the cleanest long-term fix. Once it lands,
all test-side workarounds (`null_evicted_rd_inodes`, `rcu_assign_pointer` blocks,
`raw_rdino_bug_present`, `SKIP_IF_RAW_RDINO_BUG`) can be deleted. See §8d for the
full transition checklist.

**New — `dirent_children_release()` in `dirent.c:478`**: same raw `rd_inode->i_children`
dereference pattern as `release_dirents_recursive`. Same latent bug; same fix needed.

---

## Session 5 — 2026-03-09: rd_inode UAF fix + super_block_release_dirents repair

### Summary

Applied the fs.c / vfs.c `inode_active_get` fix from Session 4 and then drove the
fault-in LRU tests to passing. This required fixing three additional bugs discovered
during testing, all in the `release_dirents_recursive` / `inode_active_get` stack.
The fault-in tests now run — but crash at `lru_max=1` due to a fundamental RCU
limitation. The tests are left **WIP** pending `i_dirent` back-pointer (§8d).

---

### 10.1 fs_test_lru.c — known-bug scaffolding removed

`fs_test_lru.c` was updated to remove all scaffolding that guarded against the
now-fixed raw `rd_inode` UAF:

- `raw_rdino_bug_present()` deleted
- `fault_in_tcase_skip` flag deleted
- `SKIP_IF_RAW_RDINO_BUG()` macro and all six call sites deleted
- `fs_lru_known_bugs_suite()` and second `SRunner` in `main()` deleted
- `fault_in_setup()` deleted; `tc_faultin` now uses plain `setup`/`teardown`
- `test_lru_eviction_produces_tombstone` simplified: `probe_rd` /
  `rcu_assign_pointer` / `dirent_put` workaround removed; files cleaned up
  with `reffs_fs_unlink` before return
- Header comment updated to reflect fix status

---

### 10.2 Bug: `release_dirents_recursive` — raw `rd_inode` dereference at teardown

**Symptom:** ASAN heap-use-after-free at `super_block.c:188` inside
`release_dirents_recursive`, triggered by `test_lru_eviction_fires` (organic
eviction under `TEST_LRU_MAX=4` pressure evicts the root inode before teardown).

**Root cause:** `release_dirents_recursive` uses `rd_parent->rd_inode->i_children`
to walk the child list. After organic eviction + RCU grace period completion,
the inode struct is freed while `rd_inode` still points at it.

**Fix:** `release_dirents_recursive` now takes `struct super_block *sb` as a
parameter and uses `inode_active_get` + `inode_alloc` fallback to safely pin the
inode for the walk:

```c
static void release_dirents_recursive(struct super_block *sb,
                                       struct reffs_dirent *rd_parent)
{
        struct inode *inode;

        if (!rd_parent)
                return;

        rcu_read_lock();
        inode = rcu_dereference(rd_parent->rd_inode);
        if (inode)
                inode = inode_active_get(inode);
        rcu_read_unlock();

        if (!inode && rd_parent->rd_ino)
                inode = inode_alloc(sb, rd_parent->rd_ino);

        if (!inode)
                return;

        cds_list_for_each_entry_safe(rd, tmp, &inode->i_children, rd_siblings) {
                release_dirents_recursive(sb, rd);
                dirent_parent_release(rd, reffs_life_action_death);
        }
        inode_active_put(inode);
}
```

**Comment block** before the function replaced with a short accurate version
documenting the `rd_inode` weak pointer nature and the `i_dirent` TODO.

---

### 10.3 Bug: `rcu_barrier()` in `release_all_fs_dirents` — fires before dirent walk

**Symptom:** Crash moved to `urcu_ref_get_unless_zero` inside `inode_active_get`
called from `release_dirents_recursive`. Even with the §10.2 fix in place,
`inode_active_get` was reading freed memory.

**Root cause:** `release_all_fs_dirents()` in `ns.c` called `rcu_barrier()` as a
preamble "to drain pending RCU callbacks before touching rd_inode." This fires
`inode_free_rcu` for all tombstoned inodes, freeing their structs — before the
dirent walk starts. The `inode_active_get` inside `release_dirents_recursive` then
reads the freed `urcu_ref` field.

**Fix:** Remove the `rcu_barrier()` preamble from `release_all_fs_dirents()`.
Each `super_block_release_dirents()` ends with `super_block_drain + rcu_barrier()`
which is the correct place to drain. The preamble comment is replaced with a
warning explaining why `rcu_barrier()` must NOT be called before the walk.

Similarly, the `rcu_barrier()` that was at the top of `super_block_release_dirents`
was also removed — it was moved to after the dirent walk (already in Phase 2).

---

### 10.4 Bug: `inode_active_get_from_dirent` — RCU window too late

**Symptom:** After removing spurious `rcu_barrier()` calls, crash persisted in
`test_lru_inode_find_after_evict`. `drain_lru()` calls `rcu_barrier()`, completing
`inode_free_rcu`. Then `reffs_fs_getattr` loads `rd_inode` (outside RCU), calls
`inode_active_get_from_dirent` — but by then the struct is already freed.

**Investigation:** Added trace points to `inode_free_rcu` and
`inode_active_get_from_dirent`. Confirmed: `inode_free_rcu` logs the address as
freed on thread 1470075, then 10ms later the test thread loads the same address
from `rd_inode` and crashes — the `rcu_read_lock` inside `inode_active_get_from_dirent`
is acquired *after* the grace period already completed.

**Root cause of original fix being wrong:** The original approach put `rcu_read_lock`
inside `inode_active_get(struct inode *)`. But `inode_active_get` receives the
already-loaded pointer — it cannot retroactively protect against a grace period that
completed between the load and the call.

**Fix:** New function `inode_active_get_from_dirent(struct reffs_dirent *rd)` that
loads `rd_inode` *and* calls `inode_active_get` within the same unbroken
`rcu_read_lock` critical section:

```c
struct inode *inode_active_get_from_dirent(struct reffs_dirent *rd)
{
        struct inode *inode;
        if (!rd)
                return NULL;
        rcu_read_lock();
        inode = rcu_dereference(rd->rd_inode);
        if (inode)
                inode = inode_active_get(inode);
        rcu_read_unlock();
        return inode;
}
```

All call sites in `fs.c` and `vfs.c` updated from
`inode_active_get(rd->rd_inode)` → `inode_active_get_from_dirent(rd)`.

`inode_active_get(struct inode *)` reverted to its original form — no internal
`rcu_read_lock`. Naming convention enforced: bare pointer → `inode_active_get`;
dirent weak pointer → `inode_active_get_from_dirent`.

---

### 10.5 WIP: fault-in tests still crash at `lru_max=1` — fundamental RCU limit

**Symptom:** `test_lru_inode_find_after_evict` still crashes after all the above
fixes. Detailed tracing confirms:

1. `reffs_fs_create("/evict_me")` with `lru_max=1`:
   - New inode allocated, `i_ref=2`, `i_active=1`
   - `inode_active_put` inside `vfs_create_common_locked`: `i_active→0`, LRU add
   - Immediately evicted (LRU full at max=1): `inode_unhash` → `i_ref→0` →
     `inode_release` → `call_rcu(inode_free_rcu)`
2. `drain_lru()` calls `rcu_barrier()`: `inode_free_rcu` runs, struct freed
3. `reffs_fs_getattr("/evict_me")`:
   - `find_matching_directory_entry` returns `nm->nm_dirent` pointing at `/evict_me`
   - `nm->nm_dirent->rd_inode` = freed struct pointer (never nulled by eviction path)
   - `inode_active_get_from_dirent` enters `rcu_read_lock`, loads freed pointer,
     crashes in `urcu_ref_get_unless_zero`

**Why `rcu_read_lock` cannot help here:** The inode was freed by `rcu_barrier()` in
`drain_lru()` — a completed grace period. `rcu_read_lock` only blocks *future* grace
periods. Once `rcu_barrier()` has run, the struct is freed memory regardless of any
subsequently acquired read-side lock.

**Root cause — fundamental:** `rd_inode` is never nulled by the eviction path.
After eviction + `rcu_barrier()`, `rd_inode` is a non-NULL pointer to freed memory.
`inode_active_get_from_dirent` cannot distinguish this from a live pointer —
`rcu_dereference` returns it, and the `inode_active_get` call crashes before it can
check `i_active`.

**The only correct fix:** The eviction path must null `rd_inode` on the dirent
atomically when it tombstones the inode. This requires `i_dirent` back-pointer on
`struct inode` (§8d) so `super_block_evict_inodes` can do:
```c
rcu_assign_pointer(inode->i_dirent->rd_inode, NULL);
```
before `call_rcu`. Without this, any `rd_inode` dereference after a completed grace
period is a potential UAF.

---

### 10.6 Commit message (WIP)

```
fs/super_block,ns,inode: fix release_dirents_recursive UAF + inode_active_get RCU window

Three bugs fixed in the teardown / inode-get path, exposed by the new
LRU fault-in unit tests:

1. release_dirents_recursive (super_block.c): added struct super_block *sb
   parameter; now uses inode_active_get + inode_alloc fallback to safely
   pin rd_parent's inode before walking i_children.  Previously accessed
   rd_parent->rd_inode->i_children as a raw pointer; after organic LRU
   eviction + RCU grace period completion the struct is freed memory.

2. release_all_fs_dirents (ns.c): removed rcu_barrier() preamble that was
   firing inode_free_rcu callbacks (freeing inode structs) before the dirent
   tree walk.  Each super_block_release_dirents call ends with
   super_block_drain + rcu_barrier() which is the correct drain point.

3. inode_active_get_from_dirent (inode.c): new function that loads rd_inode
   and calls inode_active_get within the same unbroken rcu_read_lock critical
   section.  The previous pattern of loading rd_inode outside RCU and passing
   the raw pointer to inode_active_get is unsafe: a grace period can complete
   between the load and the call, freeing the struct.  All fs.c / vfs.c call
   sites updated.

WIP — fault-in LRU tests (test_lru_inode_find_after_evict and friends) still
crash when lru_max=1.  Root cause: the eviction path never nulls rd_inode on
the dirent, so after drain_lru() + rcu_barrier() the pointer is non-NULL but
freed.  inode_active_get_from_dirent cannot safely distinguish this from a
live pointer — rcu_read_lock does not protect against a completed grace period.

Fix requires i_dirent back-pointer on struct inode (see architecture doc §8d)
so super_block_evict_inodes can rcu_assign_pointer(inode->i_dirent->rd_inode,
NULL) before call_rcu.  Until that lands, the fault-in tests are expected to
fail at lru_max=1 (organic eviction immediately after create).
```

---

### 10.7 Updated pending issues

**§8d (`i_dirent` back-pointer)** — NOW BLOCKING the fault-in LRU tests.
Required changes:
- Add `struct reffs_dirent *i_dirent` weak pointer to `struct inode`
- In `super_block_evict_inodes`, before `call_rcu`:
  `rcu_assign_pointer(inode->i_dirent->rd_inode, NULL);`
- In `inode_active_get_from_dirent`: miss path (NULL from `rcu_dereference`)
  can now call `dirent_ensure_inode` safely since NULL means "evicted" not
  "freed"
- Delete test-side workarounds: `null_evicted_rd_inodes`, `rcu_assign_pointer`
  blocks in `test_lru_eviction_produces_tombstone`
- LRU fault-in tests should pass once this lands

**`release_dirents_recursive` `inode_alloc` in shutdown path:** The fallback
to `inode_alloc` when `inode_active_get` returns NULL (tombstoned) is correct
for the POSIX backend (can reload from disk). Verify this is safe for the
RocksDB backend before shipping.

**`dirent_children_release()` in `dirent.c:478`** — same raw `rd_inode->i_children`
dereference as the old `release_dirents_recursive`. Same fix needed.

**`vfs_is_subdir` ancestor walk (`vfs.c:141`)** — each
`curr->i_parent->rd_parent->rd_inode` hop is a raw weak dereference. Deferred;
low risk under `vfs_lock_dirs` (directory inodes stay hot). Requires `i_dirent`
for a clean fix. TODO comment in place.
