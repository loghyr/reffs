<!--
SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# POSIX Recovery Unit Tests

This document outlines the unit tests for verifying the POSIX backing store recovery logic in `reffs`.

## Objectives
The goal is to ensure that the server can reliably restore its state from a POSIX-based backing store after a restart, handling various edge cases and ensuring data integrity.

## Test Framework

### Files

- `lib/fs/tests/posix_recovery.h`: Common definitions and helper functions for setting up a POSIX backend.
- `lib/fs/tests/posix_recovery.c`: Implementation of the setup, teardown, and file-writing helpers.

Each test lives in its own `posix_recovery_N.c` file and is a standalone executable that links against `libreffs_fs_test.la` (which contains the helpers) and `libreffs_fs.la`.

### Test Lifecycle

Each test follows this pattern:

1. Call `test_setup(&ctx)` — allocates a temporary directory under `/tmp/reffs_test_XXXXXX`, creates `sb_1/` inside it, allocates a `super_block` with `REFFS_STORAGE_POSIX`, and calls `super_block_dirent_create()` to create root inode 1.
2. Manually populate the backing store by writing `.meta`, `.dat`, `.lnk`, and `.dir` files using the helper functions.
3. Call `reffs_fs_recover(ctx.sb)` to trigger the recovery path.
4. Assert expected in-memory state using the `ck_assert*` family of macros from the [Check](https://libcheck.github.io/check/) unit testing library.
5. Call `test_teardown(&ctx)` — releases the superblock and recursively deletes the temporary directory.

All tests call `rcu_register_thread()` / `rcu_unregister_thread()` around the test runner because the filesystem internals use userspace RCU (liburcu).  Tests are run with `CK_NOFORK` to keep the RCU thread registration and the assertion machinery in the same process.

### RCU Note

The reffs filesystem uses lock-free data structures from liburcu (`cds_lfht`, `cds_list_head`, `urcu_ref`).  Tests must be linked with `$(URCU_LIBS) $(URCU_CDS_LIBS)` and must call `rcu_register_thread()` before any filesystem API is invoked.  Forgetting this causes silent undefined behaviour in the hash table lookups.

### Helper API

```c
/* Create a temporary directory and initialize an empty superblock */
int test_setup(struct test_context *ctx);

/* Release the superblock and delete the temporary directory */
void test_teardown(struct test_context *ctx);

/* Write a binary inode_disk struct to sb_1/ino_N.meta */
int test_write_meta(struct test_context *ctx, uint64_t ino,
                    struct inode_disk *id);

/* Write raw bytes to sb_1/ino_N.dat */
int test_write_dat(struct test_context *ctx, uint64_t ino,
                   const void *data, size_t size);

/* Write a symlink target string to sb_1/ino_N.lnk */
int test_write_lnk(struct test_context *ctx, uint64_t ino,
                   const char *target);

/* Open sb_1/ino_N.dir for writing, emit the v2 header (cookie_next u64),
 * and return the open fd so the caller can append entries. */
int test_write_dir_header(struct test_context *ctx, uint64_t ino,
                          uint64_t cookie_next, int *fd_out);

/* Append one entry to an open .dir fd: [cookie u64][ino u64]
 *                                       [name_len u16][name bytes] */
int test_write_dir_entry(int fd, uint64_t cookie, uint64_t ino,
                         const char *name);
```

### .dir File Format

The v2 `.dir` format is a flat binary file:

```
[cookie_next : uint64_t]          ← next cookie to assign on dirent_alloc
[entry 0 : cookie u64 | ino u64 | name_len u16 | name bytes]
[entry 1 : ...]
...
```

`cookie_next` is written by `test_write_dir_header()`; entries are written one at a time by `test_write_dir_entry()`.  The caller must `close(fd)` after the last entry.

### Inode Number Conventions

`test_setup()` creates the root dirent whose inode number is assigned by `super_block_dirent_create()` starting from `sb_next_ino = 1`.  Tests that write their own `.meta` files use inode numbers starting at 2.  The root inode's `.meta` is always written with `test_write_meta(&ctx, 1, ...)`.

---

## Test Inventory

### 1. Inode Number Gaps (`posix_recovery_1.c`)
- **Scenario**: The backing store contains `.meta` files with non-sequential inode numbers (e.g., 1, 2, 5, 10). Some of these inodes might not be linked into the directory tree.
- **Verification**: `sb_next_ino` must be correctly set to the maximum found inode number + 1 (in this case, 11), ensuring no collisions for future allocations.

### 2. Cookie Persistence (`posix_recovery_2.c`)
- **Scenario**: A `.dir` file contains multiple entries with specific `rd_cookie` values and a persisted `rd_cookie_next`.
- **Verification**: After recovery, each `reffs_dirent` must have its original cookie restored, and the parent's `rd_cookie_next` must match the persisted value.

### 3. Link Count Restoration (`posix_recovery_3.c`)
- **Scenario**: A file has multiple hard links (multiple dirents pointing to the same inode). The `.meta` file contains the correct `nlink` count.
- **Verification**: `reffs_fs_recover` must not double-count links during recovery. The in-memory `i_nlink` must exactly match the value from the `.meta` file.

### 4. Symlink Target Restoration (`posix_recovery_4.c`)
- **Scenario**: The backing store contains a symlink inode and its corresponding `.lnk` file.
- **Verification**: After recovery, the inode's `i_symlink` field must correctly contain the symlink target string.

### 5. Data Block Size and Integrity (`posix_recovery_5.c`)
- **Scenario**: A `.dat` file exists with a specific size and content.
- **Verification**: `data_block_alloc` during recovery must not truncate the file. The in-memory `db_size` must be correctly initialized via `fstat()`, and the data must be readable.

### 6. Atomic Write Robustness (`posix_recovery_6.c`)
- **Scenario**: Verify that the recovery process handles existing files correctly and that the new "write-temp-then-rename" logic doesn't leave stray `.tmp` files in a way that interferes with subsequent operations.
- **Verification**: After `inode_sync_to_disk()`, the `.meta` file exists and no `.meta.tmp` file remains.

### 7. Missing `.lnk` File for Symlink (`posix_recovery_7.c`)
- **Scenario**: A symlink inode's `.meta` file is present on disk but the corresponding `.lnk` file is absent (server crashed between `inode_sync_to_disk` and writing the symlink target).
- **Verification**: Recovery must not crash or dereference a garbage pointer. `i_symlink` must be `NULL`; callers (e.g., `nfs3_op_readlink`) are responsible for mapping this to an appropriate error.
- **Issue covered**: *Symlink target (`i_symlink`) not persisted* — crash between meta and lnk writes.

### 8. Truncated `.dir` File (`posix_recovery_8.c`)
- **Scenario**: A `.dir` file contains the header and two complete entries, followed by a partial third entry (only the `cookie` field, missing `ino` and `name`). This simulates a crash mid-write of a directory update.
- **Verification**: Recovery loads all complete entries before the truncation point, does not crash on the partial entry, and does not expose the partial entry in the in-memory dirent tree.
- **Issue covered**: *`.dir` and `.meta` writes are not atomic* — crash mid-write leaves truncated file; recovery silently loses entries on short reads.

### 9. `sb_next_ino` from Independent `.meta` Scan (`posix_recovery_9.c`)
- **Scenario**: A high-numbered orphaned inode (e.g., ino 99) has a `.meta` file on disk but is not reachable from the root directory tree (its parent `.dir` was never updated before the crash). The directory traversal would never visit it.
- **Verification**: The pre-traversal full-directory scan in `reffs_fs_recover()` must still find the orphaned `.meta` file and set `sb_next_ino = 100`. The linked inodes accessible via traversal must still be recoverable normally.
- **Issue covered**: *`sb_next_ino` recovery depends on complete traversal* — fix is to independently scan all `ino_*.meta` files in `sb_N/` at startup.

### 10. Deep Nested Directory Tree (`posix_recovery_10.c`)
- **Scenario**: A four-level tree (root → A → B → C → leaf file). All directories and files have `.meta` and `.dir` files. Directory `nlink` values are set correctly in the `.meta` files.
- **Verification**: All nodes are findable via `dirent_find()` after recovery. `i_parent` pointers are set correctly at each level. `sb_next_ino` is the max inode + 1 across the entire tree. Directory `i_nlink` values match the `.meta` file exactly (no double-counting from `dirent_parent_attach` increments before `load_inode_attributes` runs).
- **Issues covered**: *nlink handling during recovery is accidentally correct*; *inode_sync_to_disk called before load_inode_attributes during recovery*.

### 11. `sb_bytes_used` Accounting After Recovery (`posix_recovery_11.c`)
- **Scenario**: Two files with known sizes (100 bytes and 5000 bytes) have their `.dat` files present on disk. `data_block_alloc()` is called with `size=0` during recovery.
- **Verification**: `data_block_get_size()` returns the true on-disk size (populated via `fstat()` inside `data_block_alloc()`). `sb_bytes_used` reflects the sum of both files rounded up to block boundaries, not zero.
- **Issue covered**: *`data_block_alloc` with `size=0` leaves `db_size=0`* — `sb_bytes_used` accounting is incorrect until the first write after restart.

### 12. Stray `.tmp` File Ignored During Recovery (`posix_recovery_12.c`)
- **Scenario**: A stray `ino_2.meta.tmp` (containing zeroed data) is present from a previous crashed write attempt. The actual `ino_2.meta` contains valid committed data.
- **Verification**: Recovery reads the committed `.meta`, not the `.tmp`. The in-memory inode has the correct `uid`/`gid`. A subsequent `inode_sync_to_disk()` must not leave a `.tmp` file behind.
- **Issue covered**: *`.dir` and `.meta` writes are not atomic* — temp-then-rename pattern must not be confused by stray temporaries from a previous crash.

### 13. `.meta` Not Overwritten During Recovery (`posix_recovery_13.c`)
- **Scenario**: A child inode with specific attributes (uid=1001, gid=2002, mode=0640) is in the backing store. During recovery, `dirent_parent_attach()` is called before `load_inode_attributes()`.
- **Verification**: After `reffs_fs_recover()`, the `.meta` file on disk still contains the original attributes (not zeroes). The in-memory inode also reflects the correct attributes. This directly tests that `inode_sync_to_disk()` is gated on `rla != reffs_life_action_load` and does not fire prematurely on a zeroed in-memory inode during the load path.
- **Issue covered**: *`inode_sync_to_disk` called before `load_inode_attributes` during recovery* — fix is to gate sync calls in `dirent_parent_attach` on `rla != reffs_life_action_load`.
