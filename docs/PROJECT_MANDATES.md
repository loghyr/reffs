<!--
SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Reffs: Reference File System

Reffs is a high-performance Reference File System project focused on providing reference implementations for NFSv3, NFSv4.1 (with Flex Files support), and FUSE. It leverages modern Linux kernel features like `io_uring` for asynchronous I/O and `liburcu` for scalable, lock-free synchronization.

## Core Mandates

1.  **Reference Quality:** Implementations must prioritize protocol compliance and serve as a clear reference for NFS and FUSE protocols.
2.  **Performance & Scalability:** Utilize `io_uring` for high-performance asynchronous I/O and `liburcu` for lock-free data structures.
3.  **Rigorous Validation:** Every change must be validated with memory and thread sanitizers (ASAN, TSAN, LSAN, UBSAN).
4.  **Modern C Standards:** Adhere to the C11 standard. Use Clang as the primary compiler.
5.  **Failing Test First:** Before fixing any POSIX violation or bug, a new unit test MUST be added that reproduces the failure. The fix is only complete when the new test (and all existing tests) pass.

## Engineering Standards

-   **Coding Style:** Strictly follow the project's `clang-format` configuration.
    -   Run `make style` to check compliance.
    -   Run `make fix-style` to automatically format code (local build tree).
    -   Run `make -f Makefile.reffs fix-style` to format via the Docker sandbox (preferred before committing).
    -   **Config Headers:** All C source files (`.c`) MUST include the standard autotools configuration header:
    ```c
    #ifdef HAVE_CONFIG_H
    #include "config.h"
    #endif
    ```
    -   **Static Analysis:** Regularly use `clang-tidy` and `scan-build` to identify potential issues.
    -   `make tidy` (requires `bear`).
    -   `make scanbuild`.
-   **License Compliance:** All source files MUST carry a valid SPDX header. Run the license checker before committing:
    ```bash
    make -f Makefile.reffs license-check
    ```
-   **Commit Requirements:**
    -   **DCO:** All commits MUST be signed off (`git commit -s`) to certify the Developer Certificate of Origin.
    -   **Style first:** Run `make -f Makefile.reffs fix-style` before staging files.
    -   **Tests pass:** Run `cd build && make check` (or `make -f Makefile.reffs build-in-docker`) and confirm all unit tests pass before committing.
    -   **Pre-push Hook:** Install the pre-push hook via `./install-hooks.sh` (or `make -f Makefile.reffs install-hooks`). This hook runs style, license, and out-of-tree build checks before each push.
-   **Result Pointer Allocation:** Pointers returned inside NFS result structs MUST be allocated with `calloc()`. Do NOT use `malloc()` (leaves fields uninitialized) and do NOT use static buffers (not thread-safe). XDR frees these pointers via `xdr_free()`; they must be individually heap-allocated.
-   **Memory Safety:** Always build with sanitizers during development:
    ```bash
    ./configure --enable-asan --enable-tsan --enable-lsan --enable-ubsan
    ```

## Development Workflows

### Build & Test (Local)
1.  **Configure:** `cd build && ../configure` (add sanitizer flags as needed).
2.  **Build:** `cd build && make -j$(nproc)`
3.  **Test:** `cd build && make check` (runs C unit tests in `lib/*/tests/`).
4.  **Protocol Probing:** Use `scripts/reffs-probe.py` for protocol-level verification.

### Docker Sandbox (Recommended)
To avoid host-level conflicts with NFS services (`rpcbind`, `lockd`), use the isolated Docker environment:
-   **Build Image:** `make -f Makefile.reffs image`
-   **Build in Docker:** `make -f Makefile.reffs build-in-docker`
-   **Run Server:** `make -f Makefile.reffs run-image`
-   **Restart Server:** `make -f Makefile.reffs test-image` (stops any existing sandbox first)
-   **Clean Sandbox:** `make -f Makefile.reffs mrproper`

## Key Technologies

-   **I/O:** `io_uring` (via `liburing`)
-   **Synchronization:** `liburcu` (Userspace RCU)
-   **Networking/RPC:** `libtirpc`, OpenSSL (for TLS)
-   **File System:** FUSE (Filesystem in Userspace)
-   **Utilities:** `xxhash` (hashing), `zstd` (compression), `HdrHistogram_c` (latency monitoring), `libuuid`.

## Architectural Decisions & Lessons Learned

### Storage Backend Abstraction (March 2026)
-   **Driver Model:** Storage logic is abstracted into `struct reffs_storage_ops` (defined in `lib/include/reffs/backend.h`).
-   **Encapsulation:** Implementation-specific code (RAM, POSIX) is isolated in `lib/backends/`. Core filesystem components (`lib/fs/`) must be backend-agnostic and dispatch via the ops structure.
-   **Atomic POSIX Sync:** The POSIX backend implements an atomic "write-to-tmp-then-rename" pattern for metadata updates to ensure robustness against crashes.

### nlink Management (March 2026)
-   **Strict POSIX Invariants:**
    -   Parent directory `nlink` is incremented/decremented **ONLY** when a subdirectory is added or removed.
    -   Regular files, symlinks, and special nodes do not affect parent `nlink`.
    -   New inodes are initialized with `nlink=1` (`inode_alloc`). Subdirectories are initialized with `nlink=2`.
-   **Safety Floor:** Directories have a mandatory safety floor of 2 links. Any operation attempting to drop below this is blocked and logged as a WARNING to prevent filesystem corruption and "Stale File Handle" errors.
-   **Atomic Creation:** `rd_inode` MUST be fully allocated and populated *before* being attached to the parent directory to ensure link counts are incremented correctly.

### io_uring Concurrency
-   **Rewind for get_sqe:** To prevent blocking the Heartbeat thread (which handles I/O completions), worker threads must drop the `rc_mutex` before sleeping if the ring is full. Once `io_uring_get_sqe` succeeds, the thread is "committed" and should hold the lock through `prep` and `submit`.
-   **Submission Retries:** Use `REFFS_IO_RING_RETRIES` (100) for SQE acquisition to handle transient ring-full states without failing the operation.
-   **VM Optimization:** Use `sched_yield()` instead of `usleep()` in retry loops when running in VM environments.

### Locking & Thread Pool Resilience (March 2026)
-   **Minimizing Mutex Hold Time:** Do NOT hold `i_attr_mutex` during blocking I/O (e.g., `pread`/`pwrite` in the POSIX backend). This prevents thread pool exhaustion and deadlocks when high-concurrency operations (like `git clone`) saturate the worker threads. The `i_db_rwlock` is sufficient for data integrity during the I/O itself.
-   **Worker Pool Sizing:** Use a sufficiently large worker pool (default 64) when backends perform blocking operations to prevent starvation of the RPC layer.
-   **Trace Resilience:** Never call `LOG()` (which emits a trace event) from within the trace rotation logic to avoid recursive deadlocks on `trace_mutex`. Use `fprintf(stderr)` for rotation-level errors.
-   **FD Capacity:** Ensure `MAX_CONNECTIONS` (65,536) matches system `ulimit` and Docker settings, as the POSIX backend maintains open file descriptors for active data blocks.

## Technical Integrity & Validation

-   **Unit Tests:** Located in `lib/*/tests/`. Use the `check` library.
-   **FUSE Tests:** Specific FUSE behavior tests in `lib/fs/tests/fuse_*.c`.
-   **Trace System:** Utilize the built-in trace system for debugging (configured via `--enable-all-trace-categories`).
-   **CI/CD:** The pre-push hook ensures that only code passing a full build and test suite is pushed.
