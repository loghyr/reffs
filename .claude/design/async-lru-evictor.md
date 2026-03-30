# Plan: Async LRU Evictor

## Context

LRU eviction runs synchronously on NFS worker threads — PUTFH
blocked for 41ms doing inode_sync disk I/O under the LRU lock.
This blocks the data path and correlates with the ASAN-only
c_server_state corruption.

## Step 1: Two-phase eviction (refactor, no new threads)

The root improvement — move inode_sync OUTSIDE the LRU lock.
Valuable even without the async thread.

**Phase 1 (under lock):** Walk LRU, set `i_active = -1`
(tombstone), remove from LRU list, collect into local list.
Release lock.

**Phase 2 (no lock):** For each evictee, call `inode_sync`,
`inode_unhash`, `inode_put`.

Safe because `i_active == -1` prevents `inode_active_get` from
succeeding, and the inode is off the LRU list.

Same refactor for `super_block_evict_dirents` (`dirent_sync_to_disk`
under `sb_dirent_lru_lock`).

**Files:** `lib/fs/super_block.c`

## Step 2: Background evictor thread

Single global thread, `lease_reaper.c` pattern.

`inode_lru_add` / `dirent_lru_add` change from calling
`super_block_evict_inodes` to calling `evictor_signal()`.

**Backpressure:** Hard high-water mark at `2 * lru_max`. If
`inode_lru_add` sees `count > high_water`, fall back to
synchronous eviction on the worker thread. Prevents unbounded
LRU growth when evictor can't keep up.

**Evictor thread:**
- `rcu_register_thread()` at start
- Sleep on condvar with 1s timeout
- Wake on signal, scan all sbs (take sb ref inside rcu_read_lock,
  drop rcu_read_lock before evicting)
- Round-robin: evict N per sb per pass (NOT_NOW_BROWN_COW:
  fair scheduling deferred)

**Runtime mode flag:** `evictor_mode` enum (SYNC / ASYNC).
Tests can force SYNC mode. Default ASYNC. Checked in
`inode_lru_add` / `dirent_lru_add`.

**State machine:**
```
STOPPED → RUNNING → IDLE (sleeping) → RUNNING (signaled)
                         ↓
STOPPED ← DRAINING ← RUNNING (fini called)
```

**`evictor_drain()`:** Sets drain flag, signals condvar, waits
on completion condvar until evictor finishes one full pass.
Used by tests and shutdown.

**Lifecycle:**
- `evictor_init()` in `reffs_ns_init()`
- `evictor_fini()` in `reffs_ns_fini()` — BEFORE any
  `super_block_drain()` call

**Files:**
- `lib/fs/evictor.c` (NEW)
- `lib/include/reffs/evictor.h` (NEW)
- `lib/fs/inode.c` (signal instead of sync evict)
- `lib/fs/dirent.c` (signal instead of sync evict)
- `lib/fs/ns.c` (init/fini)
- `lib/fs/Makefile.am` (add evictor.c)

## Step 3: Makefile targets

- `ci-soak`: ASAN + -O2, sync eviction (stress test)
- `ci-soak-async`: ASAN + -O2, async eviction, LRU=4096
- `ci-soak-tsan`: TSAN, async eviction

## Tests

### Unit tests for evictor (NEW file)
- `test_evictor_init_fini`: lifecycle
- `test_evictor_signal_wakes`: signal triggers eviction
- `test_evictor_drain`: synchronous flush works
- `test_evictor_signal_after_fini`: no crash

### Test impact
- `fs_test_lru.c`: tests that rely on synchronous eviction
  inside `inode_lru_add` need `evictor_drain()` calls or
  `evictor_set_mode(SYNC)` in fixture setup
- `fs_test_ns_teardown.c`: test_teardown_rcu_barrier_order
  sets lru_max=2, expects sync eviction — needs analysis
- Existing `drain_lru()` helper calls `super_block_evict_inodes`
  directly — still works, bypasses evictor

### Verification
1. `make check` — 122 tests pass
2. `ci-soak-async` with LRU=4096 — 30 min clean
3. `ci-soak` sync eviction — still available for stress
4. `ci-soak-tsan` — no new races

## Deferred (NOT_NOW_BROWN_COW)
- Per-sb evictor threads
- Round-robin fairness for multi-sb
- Adaptive high/low water marks
- Eviction priority (dirty vs clean inodes)
