# PJDFSTEST Individual Execution Playbook

Use this checklist to isolate and debug the specific POSIX violations identified in the global test runs. Execute these from the mount point of the filesystem.

## Prerequisites
1.  **Mount the filesystem:**
    ```bash
    sudo mount -o tcp,mountproto=tcp,vers=3,nolock 127.0.0.1:/ /mnt/reffs
    cd /mnt/reffs/
    ```
2.  **Define the test path:**
    ```bash
    export PJDFSTEST="/home/loghyr/reffs/pjdfstest/tests"
    ```

## Priority 1: Rename & Sticky Bit Logic
These tests failed because the server was granting access when it should have returned `EACCES` (verified by the "got 0" results in `run12.txt`).

*   **Test 09.t (Sticky Bit Renames):** 
    ```bash
    sudo prove -rv $PJDFSTEST/rename/09.t
    ```
*   **Test 23.t (Multiply Linked Destination):**
    ```bash
    sudo prove -rv $PJDFSTEST/rename/23.t
    ```

## Priority 2: Unlink & Rmdir (Refcount/Leak issues)
`run12.txt` showed `ENOTEMPTY` errors and `nlink` mismatches (got 1, expected 0). This suggests directory entries aren't being removed from memory or link counts are drifting.

*   **Test 00.t (Basic Unlink):**
    ```bash
    sudo prove -rv $PJDFSTEST/unlink/00.t
    ```
*   **Test 14.t (Unlink Open File):**
    ```bash
    sudo prove -rv $PJDFSTEST/unlink/14.t
    ```

## Priority 3: Symlinks & Timestamps
*   **Test 03.t (Symlink permission/path):**
    ```bash
    sudo prove -rv $PJDFSTEST/symlink/03.t
    ```
*   **Test 06.t & 09.t (Timestamp updates):**
    ```bash
    sudo prove -rv $PJDFSTEST/utimensat/06.t
    sudo prove -rv $PJDFSTEST/utimensat/09.t
    ```

## Deep Debugging (Manual Execution)
If `prove -v` doesn't show enough detail for a specific subtest (e.g., test 2266 in `09.t`), run the script manually and look for the specific `expect` call:
```bash
sudo ksh $PJDFSTEST/rename/09.t | grep "not ok 2266"
```
*(Note: Run scripts from the filesystem mount point.)*
