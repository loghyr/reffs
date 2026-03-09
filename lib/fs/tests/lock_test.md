<!--
SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: GPL-2.0+
-->

# Lock Unit Tests

This document describes the unit tests for `lib/fs/lock.c`, which implements
the byte-range locking and share reservation logic used by the NLM and NFSv4
protocol layers.

## Overview

The locking code provides two largely independent subsystems:

- **Byte-range locks** (`reffs_lock_*`): POSIX-style locks with owner identity,
  range arithmetic, and partial-range unlock (split, truncate-start,
  truncate-end, full removal).
- **Share reservations** (`reffs_share_*`): NFSv4 / NLM SHARE deny/access
  mode conflict detection and per-owner upgrade semantics.

The tests are split across four files to keep failure isolation clean and
compile times short.  A shared header provides the test fixture infrastructure.

---

## Test Infrastructure (`lock_test.h`)

`lock_test.h` is a header-only fixture library included by the three inode-
level test files (`lock_conflict_2.c`, `lock_remove_3.c`, `lock_share_4.c`).
It provides everything needed to construct owners, locks, and shares without
touching the NLM or NFSv4 protocol layers.

### `struct test_lock_owner`

A minimal wrapper around `reffs_lock_owner` that satisfies the reference-
counting and callback API:

```c
struct test_lock_owner {
    struct reffs_lock_owner base;  /* embedded — cast-compatible */
    int                     id;   /* human-readable label for assertions */
};
```

`urcu_ref_init` is called at allocation time.  `lo_release` is wired to a
simple `free()` thunk.  `lo_match` is left `NULL` by default and set
per-test when the match-callback path needs to be exercised.

**`test_owner_alloc(id)`** — allocates and initialises a `test_lock_owner`.

**`test_owner_put(o)`** — releases one reference; frees when the count reaches
zero.

### `test_lock_alloc` / `test_share_alloc`

Both helpers allocate the target struct, call `inode_get` to hold a reference
on the inode, and call `urcu_ref_get` to hold a reference on the owner — exactly
what the production code expects.  The caller is responsible for eventual
cleanup via `reffs_lock_free` / `reffs_share_free`, or by passing ownership to
`reffs_lock_add` / `reffs_share_add`.

### `lock_count` / `share_count`

Walk `inode->i_locks` / `inode->i_shares` respectively and return the number
of entries.  Used throughout the tests to verify list lengths without relying
on internal implementation details.

### `test_inode_cleanup`

Drains `i_locks` and `i_shares` with `cds_list_for_each_entry_safe`, calling
`reffs_lock_free` / `reffs_share_free` on each entry, then calls `inode_put`
and `test_owner_put` on each owner in the supplied array.

This is necessary because the tests use `REFFS_STORAGE_RAM` superblocks and
the superblock teardown path calls `super_block_remove_all_inodes`, which
asserts that no inodes remain in the hash table.  Every inode allocated during
a test must have its reference count reach zero before the superblock is
released.

### Share mode / access constants

`lock_test.h` defines the four deny-mode and four access-mode constants used
by the share tests, mirroring the `fsh4_mode` / `fsh4_access` enums described
in comments in `lock.c`:

| Constant   | Value | Meaning            |
|------------|-------|--------------------|
| `FSM_DN`   | 0     | deny nothing       |
| `FSM_DR`   | 1     | deny read (bit 0)  |
| `FSM_DW`   | 2     | deny write (bit 1) |
| `FSM_DRW`  | 3     | deny read+write    |
| `FSA_NONE` | 0     | no access          |
| `FSA_R`    | 1     | read access        |
| `FSA_W`    | 2     | write access       |
| `FSA_RW`   | 3     | read+write access  |

### RCU and superblock setup

All three inode-level test files allocate a single `REFFS_STORAGE_RAM`
superblock in `main()` (with a unique `sb_id` per file to avoid collisions in
the global superblock list) and use it for all tests in the file.  Each test
allocates inodes with monotonically increasing inode numbers via an atomic
counter rather than going through `super_block_dirent_create`, so no directory
tree is needed and no `.meta` or `.dir` files are written.

`rcu_register_thread()` is called before any filesystem API is invoked and
`rcu_unregister_thread()` is called after `srunner_free`.  All tests use
`CK_NOFORK` so the RCU thread registration remains valid for the lifetime of
the test run.

---

## Test Files

### 1. `lock_range_1.c` — `reffs_lock_range_overlap`

`reffs_lock_range_overlap` is a pure function with no side effects.  It is the
foundation of both the conflict-detection and the partial-remove logic, so it
is tested exhaustively in isolation.  No superblock or inode is needed.

**Semantics:** a range is described by `(offset, len)`.  `len == 0` means
"from offset to end of file" and is represented internally as
`end = UINT64_MAX`.  Two ranges overlap iff their `[offset, end]` intervals
share at least one byte.

| Test | Description |
|------|-------------|
| `test_overlap_no_before` | `[0,9]` vs `[10,19]` — strictly before, no touch |
| `test_overlap_no_after` | `[100,199]` vs `[200,299]` — strictly after |
| `test_overlap_adjacent` | `[0,4]` vs `[5,9]` — adjacent at boundary, must NOT overlap |
| `test_overlap_partial` | `[0,9]` vs `[5,14]` — partial overlap at `[5,9]` |
| `test_overlap_one_contains_other` | `[0,99]` contains `[10,19]` |
| `test_overlap_identical` | Same range always overlaps itself |
| `test_overlap_single_byte` | `len=1` ranges: same offset overlaps, adjacent does not |
| `test_overlap_len0_vs_finite_overlapping` | `[1000,EOF)` overlaps any range starting at ≥ 1000 |
| `test_overlap_len0_vs_finite_before` | `[1000,EOF)` vs `[0,999]` — finite ends before EOF range starts |
| `test_overlap_both_len0` | Two to-EOF ranges always overlap |
| `test_overlap_len0_adjacent_boundary` | `[0,999]` vs `[1000,EOF)` — adjacent, must NOT overlap |

The `adjacent` tests are particularly important: the formula
`end = off + len - 1` makes adjacent ranges non-overlapping by one byte, which
is the correct POSIX semantics but easy to get wrong with an off-by-one in the
inequality direction.

---

### 2. `lock_conflict_2.c` — `reffs_lock_find_conflict` and `reffs_lock_add`

#### `reffs_lock_find_conflict`

Searches `inode->i_locks` for a lock that conflicts with a proposed
`(offset, len, exclusive)` request from a given owner.  Conflict rules:

- Shared vs shared: no conflict (two readers do not block each other).
- Shared vs exclusive, exclusive vs shared, exclusive vs exclusive: conflict.
- Same owner pointer: lock is skipped (an owner never blocks itself).
- `lo_match` returns true: treated as the same owner, skipped.
- No range overlap: no conflict regardless of lock type.

| Test | Description |
|------|-------------|
| `test_conflict_no_locks` | Empty lock list always returns NULL |
| `test_conflict_shared_vs_shared` | Two shared locks on same range: no conflict |
| `test_conflict_shared_vs_exclusive` | Shared lock blocks an exclusive request |
| `test_conflict_exclusive_vs_shared` | Exclusive lock blocks even a shared request |
| `test_conflict_exclusive_vs_exclusive` | Exclusive blocks exclusive |
| `test_conflict_same_owner_no_conflict` | Owner's own exclusive lock does not conflict with itself |
| `test_conflict_no_overlap` | Exclusive lock on `[0,99]` does not conflict with request for `[200,299]` |
| `test_conflict_lo_match_no_conflict` | `lo_match` returning true suppresses the conflict for an exclusive lock |

The `lo_match` test uses a file-scope static callback `match_always_true` that
unconditionally returns true, simulating the case where two distinct owner
pointers represent the same network client (e.g. an NLM owner and an NFSv4
owner for the same host).

#### `reffs_lock_add`

| Test | Description |
|------|-------------|
| `test_lock_add_new` | Adding a lock increments `lock_count` from 0 to 1 |
| `test_lock_add_relock_upgrades` | Re-locking the same range+owner updates `l_exclusive` in place, no duplicate entry, count stays at 1 |
| `test_lock_add_different_range_not_merged` | Same owner, different non-overlapping ranges: two separate entries |
| `test_lock_add_host_list` | When `host_list` is non-NULL, the lock appears in both `inode->i_locks` and the host list |

The relock-upgrades test is the most subtle: `reffs_lock_add` updates `l_exclusive` on the existing entry in place and returns 0, but does **not** free the passed lock struct. The caller retains ownership of `rl2` on the relock path and must call `reffs_lock_free(rl2)` explicitly after `reffs_lock_add` returns.

---

### 3. `lock_remove_3.c` — `reffs_lock_remove`

`reffs_lock_remove` implements POSIX partial-range unlocking.  For each lock
owned by the specified owner that overlaps the unlock range `[u_off, u_end]`,
it takes one of four actions determined by how the ranges relate:

| Case | Condition | Action |
|------|-----------|--------|
| Full removal | unlock covers entire lock | delete and free the entry |
| Truncate start | unlock covers the beginning of the lock | advance `l_offset` to `u_end + 1` |
| Truncate end | unlock covers the tail of the lock | reduce `l_len` to `u_off - l_off` |
| Split | unlock punches a hole in the middle | shorten existing entry (left), allocate new entry (right) |

Locks belonging to a different owner and non-overlapping locks for the same
owner are never touched.

| Test | Description |
|------|-------------|
| `test_remove_full_exact` | Unlock `[0,99]` removes lock `[0,99]` exactly |
| `test_remove_full_superset` | Unlock `[0,99]` removes lock `[10,59]` (unlock is a superset) |
| `test_remove_truncate_end` | Lock `[0,99]`, unlock `[50,99]` → remaining `[0,49]` |
| `test_remove_truncate_start` | Lock `[0,99]`, unlock `[0,49]` → remaining `[50,99]` |
| `test_remove_split_middle` | Lock `[0,99]`, unlock `[30,59]` → two fragments `[0,29]` and `[60,99]` |
| `test_remove_split_inherits_exclusivity` | Both fragments after a split retain the original `l_exclusive` flag |
| `test_remove_wrong_owner_no_effect` | Unlock by owner B does not touch owner A's lock |
| `test_remove_no_overlap_no_effect` | Unlock `[200,299]` does not touch lock `[0,99]` |
| `test_remove_len0_unlock_removes_finite_lock` | To-EOF unlock `[0,0)` removes a finite lock `[100,199]` |
| `test_remove_len0_unlock_truncates_start_of_eof_lock` | Unlock `[0,99]` applied to a to-EOF lock `[0,EOF)` leaves `[100,EOF)` |
| `test_remove_multiple_locks_only_matching_owner` | Two owners share the same range; removing owner A leaves owner B's lock |

The split test checks both possible orderings of the two resulting fragments in
the list, since the insertion order (`cds_list_add` places the right fragment
immediately after the left fragment) is an internal detail.

The `len=0` truncate-start test verifies that after removing `[0,99]` from a
to-EOF lock, the remainder is `(offset=100, len=0)` — still a to-EOF range,
not a finite range ending at `UINT64_MAX`.  This exercises the
`l_len == 0 ? 0 : ...` branch in the truncate-start path.

---

### 4. `lock_share_4.c` — `reffs_share_add` and `reffs_share_remove`

#### Conflict model

A conflict between two share reservations S1 and S2 exists when:

```
(S2.access & S1.mode) != 0   // S2 wants access that S1 denies
(S1.access & S2.mode) != 0   // S1 has access that S2 denies
```

Both directions are checked, so the relation is symmetric even though the
second opener triggers the check.

#### `reffs_share_add`

Returns 0 on success, `-EACCES` on conflict.  On conflict the passed share
struct is **not** freed; the caller retains ownership and must call
`reffs_share_free` if the share is no longer needed.  On a same-owner
re-open (upgrade), the existing entry is updated in place and the new share
struct is freed by `reffs_share_add`.

| Test | Description |
|------|-------------|
| `test_share_deny_none_vs_deny_none` | Both deny nothing: no conflict, two shares added |
| `test_share_deny_read_vs_write_only_access` | S1 denies read; S2 only wants write: no conflict |
| `test_share_conflict_deny_read_vs_read_access` | S1 denies read; S2 wants read: `-EACCES` |
| `test_share_conflict_deny_write_vs_write_access` | S1 denies write; S2 wants write: `-EACCES` |
| `test_share_conflict_reverse_direction` | S2 denies read; S1 already has read access: `-EACCES` |
| `test_share_conflict_deny_rw_vs_any_access` | S1 denies everything; any access request conflicts |
| `test_share_upgrade_same_owner` | Same owner re-opens with new mode/access: updated in place, count stays at 1 |

The reverse-direction test (`test_share_conflict_reverse_direction`) checks the
second half of the conflict condition — that a new share can be blocked because
it would deny access the *existing* owner already holds.  This is the case that
is easy to miss when reading the conflict formula left-to-right.

#### `reffs_share_remove`

Removes all shares for the specified owner.  Silently succeeds if the owner has
no shares.

| Test | Description |
|------|-------------|
| `test_share_remove_existing` | Normal removal: count goes from 1 to 0 |
| `test_share_remove_wrong_owner_no_effect` | Removing a non-present owner does not touch other owners' shares |
| `test_share_remove_only_own_entry` | Two owners; removing one leaves the other's share intact |
| `test_share_remove_nonexistent_no_error` | Removing when no shares exist returns 0 and does not crash |
| `test_share_host_list_populated` | `host_list` is non-empty after `reffs_share_add` with a non-NULL host list |
| `test_share_remove_cleans_host_list` | `reffs_share_remove` with a non-NULL host list removes the entry from the host list |

---

## Known Gaps

The following scenarios are not covered by the current test suite and are
candidates for future tests:

- **Concurrent access**: all tests are single-threaded.  The `i_lock_mutex`
  that callers are expected to hold around `reffs_lock_add` /
  `reffs_lock_remove` is not exercised under concurrent writers.

- **Memory pressure**: `reffs_lock_remove` allocates a new `reffs_lock` for
  the right fragment during a split.  The `calloc` failure path (returning
  `-ENOMEM`) is not tested.  Injecting allocation failure would require a
  wrapper or a compile-time substitution.

- **Multiple overlapping locks for the same owner**: the current remove tests
  use one lock per owner.  A scenario where the same owner holds several
  overlapping ranges (e.g. after two `lock_add` calls with different ranges
  that are then partially unlocked with a single remove spanning both) is not
  exercised.

- **NLM-layer integration**: `lock_test.h` provides a synthetic owner.  The
  `nlm4_lock_owner` struct that embeds `reffs_lock_owner` is not tested here;
  that is covered by the NLM protocol tests in `lib/nlm/tests/` (not yet
  written).
