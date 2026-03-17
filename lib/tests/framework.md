# Reffs Unit Test Framework

The reffs test framework provides a consistent way to initialize the environment, run tests using the Check library, and ensure clean teardown (essential for ASAN/leak detection).

## Core Framework (`libreffs_test`)

The core framework in `lib/test` provides global initialization and the main test runner.

### `reffs_test_run_suite(Suite *s, void (*setup)(void), void (*teardown)(void))`

This is the primary entry point for a test program's `main()`. It:
1.  Calls `reffs_test_global_init()` (RCU thread registration, tracing, logging).
2.  Calls the optional per-suite `setup` function.
3.  Runs all tests in the provided `Suite` using an `SRunner` in `CK_NOFORK` mode.
4.  Calls the optional per-suite `teardown` function.
5.  Calls `reffs_test_global_fini()` (RCU synchronization, tracing close).

## Modular Harnesses

Modules provide their own harnesses that build upon the core framework.

### Standalone Runners (Utility tests)

For tests that don't need a filesystem or server state (e.g., string manipulation), use `reffs_test_run_suite` directly:

```c
#include "libreffs_test.h"

int main(void) {
    return reffs_test_run_suite(my_suite(), NULL, NULL);
}
```

### File System Tests (`lib/fs/tests`)

Use `fs_test_harness.h` which provides:
-   `fs_test_setup()`: Initializes namespace with RAM backend, sets root UID/GID.
-   `fs_test_teardown()`: Finalizes namespace.
-   `fs_test_run(suite)`: A wrapper for the core runner.

Most FS tests use a per-test fixture:
```c
static void setup(void) { fs_test_setup(); }
static void teardown(void) { fs_test_teardown(); }

Suite *my_suite(void) {
    TCase *tc = tcase_create("Core");
    tcase_add_checked_fixture(tc, setup, teardown);
    // ...
}
```

### NFSv4 Tests (`lib/nfs4/tests`)

Use `nfs4_test_harness.h` which adds:
-   `nfs4_test_setup()`: Calls `reffs_test_setup_server()` (temp state directory), `fs_test_setup()`, and `nfs4_protocol_register()`.
-   `nfs4_test_run(suite)`: Bare runner (same as `fs_test_run`).

### FUSE Shim Tests (`lib/fs/tests/fuse_*.c`)

Use `fuse_harness.h` which provides `fuse_test_run()`. This runner automatically sets `REFFS_FUSE_UNIT_TEST=1` to ensure the FUSE layer doesn't overwrite the test's security context with UID 0.

## Implementation Details

### Weak Symbols
`libreffs_test.la` declares module setup functions (like `reffs_test_setup_fs`) as **weak symbols**. This allows the core library to be linked without requiring all module libraries to be present. If a module harness (like `libreffs_fs_test.la`) is linked, its implementation will override the weak symbol.

### State Directory
`reffs_test_create_state_dir()` creates a unique directory under `/tmp/reffs.state.XXXXXX` using `mkdtemp`. This allows tests to run in parallel without colliding on persistent state files. `reffs_test_remove_state_dir()` recursively removes the directory and its contents.
