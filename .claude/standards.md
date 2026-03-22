# reffs Coding Standards and Rules

## Build System

### Build in a subdirectory — never in the source tree
```bash
mkdir -p m4 && autoreconf -fi
mkdir build && cd build
../configure [options]
make -j$(nproc)
```

### configure options
| Flag | Purpose |
|------|---------|
| `--enable-asan` | AddressSanitizer (`-fsanitize=address`) |
| `--enable-ubsan` | UndefinedBehaviorSanitizer (`-fsanitize=undefined`) |
| `--enable-tsan` | ThreadSanitizer (`-fsanitize=thread`) |
| `--enable-lsan` | LeakSanitizer (`-fsanitize=leak`) |
| `--enable-debug` | Disable NDEBUG |
| `--enable-noopt` | `-O0` instead of `-O2` |
| `--with-cc=COMPILER` | Compiler selection (default: clang) |
| `--enable-linux-io_uring` | io_uring support (auto-detected) |
| `--enable-verbose-debug` | High-volume debug output |
| `--enable-strict-posix` | Strict POSIX permissions (breaks git-over-NFS) |

### Standard development build
```bash
../configure --enable-asan --enable-ubsan
```

### Useful Makefile.reffs targets
| Target | Purpose |
|--------|---------|
| `make -f Makefile.reffs build` | Incremental build |
| `make -f Makefile.reffs check` | Run unit tests |
| `make -f Makefile.reffs style` | Check clang-format compliance |
| `make -f Makefile.reffs fix-style` | Auto-fix style violations |
| `make -f Makefile.reffs license` | Check SPDX headers |
| `make -f Makefile.reffs check-ci` | Full CI pipeline in Docker |
| `make -f Makefile.reffs image` | Build dev Docker image |
| `make -f Makefile.reffs run-image` | Run NFS server in Docker |
| `make -f Makefile.reffs shell` | Shell into running container |

---

## Style

### Always run fix-style before committing
```bash
make -f Makefile.reffs fix-style
```
Or check first without modifying:
```bash
make -f Makefile.reffs style
```

### clang-format rules (from .clang-format)
- Indentation: **tabs**, width 8
- Line length: **80 columns**
- Pointer alignment: right (`int *ptr`, not `int* ptr`)
- Function opening brace: new line
- No space after C-style casts: `(type)value`
- liburcu iteration macros listed in `ForEachMacros`

---

## Git Commits

- Always sign off: `git commit -s`
- **Never** add `Co-Authored-By:` lines
- Run `fix-style` and `license` checks before committing

---

## SPDX License Headers

Every source file must begin with:
```c
/* SPDX-FileCopyrightText: YEAR Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */
```
Shell/Makefile/YAML use `#` comment equivalents. Enforced by `check_license.sh`.

---

## Error Code Conventions

### NFSv3 op handlers
- Return type: `int` (not `nfsstat3`)
- Success: `0`
- Errors: **negative errno** (e.g., `-ENOENT`, `-ESTALE`, `-EIO`)
- Async: **`-EINPROGRESS`** — never positive `EINPROGRESS`
- `errno_to_nfs3(ret)` converts to wire status at the `out:` label

### NFSv4 op handlers
- Return type: `void` (result written into `compound->c_res->resarray`)
- Errors: `nfsstat4` written via `NFS4_OP_RES_SETUP` / `*status = errno_to_nfs4(...)`
- **Never** set `*status = NFS4_OK;` — result structs are `calloc`'d, NFS4_OK (0) is the default

### Custom errno values
Defined in `lib/include/reffs/errno.h`, base `REFFS_ERR_BASE` (1024) to avoid collision with system errno:
`EBADHANDLE`, `ENOTSYNC`, `EBADTYPE`, etc.

### rpc_protocol_op_call normalization
`-EINPROGRESS` returned by an NFSv3 op is normalized to positive `EINPROGRESS` in
`rpc_protocol_op_call()` — the single chokepoint — so `rpc_process_task()` needs
no special knowledge of protocol-specific sign conventions.

---

## Async PAUSE/RESUME Contract

### Core rule: after task_pause() succeeds, DO NOT touch rt, compound, or the task

`task.h` is explicit:
> "After this returns true the caller MUST NOT touch the task, rpc_trans, or compound again."

A fast io_uring CQE can cause another worker to dequeue, process the resume, and
free `rt` before the original worker reaches any subsequent line.

### Correct NFSv3 async pattern
```c
bool went_async = false;

// Transfer inode/sb ownership to ph BEFORE pausing
ph->ph_inode = inode;  inode = NULL;
ph->ph_sb    = sb;     sb    = NULL;
rt->rt_next_action = my_op_resume;

went_async = task_pause(rt->rt_task);
if (went_async) {
    if (submit_async_io(...) < 0) {
        /* submit failed: cancel pause, restore ownership */
        rt->rt_next_action = NULL;
        task_resume(rt->rt_task);
        went_async = false;
        inode = ph->ph_inode;  ph->ph_inode = NULL;
        sb    = ph->ph_sb;     ph->ph_sb    = NULL;
        ret = -EIO;
        goto out;
    }
    return -EINPROGRESS;   /* <-- return immediately; do not touch rt */
}
/* task_pause failed (shouldn't happen): restore and fall through */
inode = ph->ph_inode;  ph->ph_inode = NULL;
sb    = ph->ph_sb;     ph->ph_sb    = NULL;
rt->rt_next_action = NULL;

out:
    res->status = errno_to_nfs3(ret);
    inode_active_put(inode);   /* safe: NULL-tolerant */
    super_block_put(sb);       /* safe: NULL-tolerant */
    return ret;
```

### Why the local `went_async` flag, not task_check_and_clear_went_async in the op
`task_check_and_clear_went_async()` is for `dispatch_compound()`'s loop (NFSv4),
which holds `t` as a function parameter that outlives `rt`.  In NFSv3 op handlers
`t` is only reachable via `rt->rt_task`; dereferencing `rt` after the pause is UAF.

### NFSv4 async pattern
NFSv4 op handlers call `task_pause(compound->c_rt->rt_task)` and return normally.
`dispatch_compound()` calls `task_check_and_clear_went_async(t)` *outside* the op
handler, using `t` (a function parameter that is never freed by `rpc_process_task`).
Do not replicate the NFSv3 local-flag approach in NFSv4 ops.

### ph_inode / ph_sb
`struct protocol_handler` carries `ph_inode` and `ph_sb` for NFSv3 async state.
These hold active refs across pause/resume because NFSv3 has no `struct compound`
to store them.  Always set the local pointer to NULL after transferring ownership,
so the `out:` label's `inode_active_put(inode)` / `super_block_put(sb)` are no-ops.

---

## NULL-Safe Put Functions

`inode_active_put()`, `inode_put()`, and `super_block_put()` all check for NULL
internally.  Call them unconditionally — no guard needed at call sites.

---

## XDR Sizing

**Always use `xdr_sizeof()`, never `sizeof()`** when allocating or measuring XDR-encoded structs.

`bitmap4` and other variable-length fields diverge from their C struct sizes on the wire:
```c
// WRONG:
size_t sz = sizeof(COMPOUND4res);

// CORRECT:
size_t sz = xdr_sizeof((xdrproc_t)xdr_COMPOUND4res, res);
```

---

## bitmap4 Copy Guard

The early-return guard must check **`src->bitmap4_len`**, not `dst->bitmap4_len`.
Destinations are often zero-initialized (calloc'd); checking `dst` silently skips
all copies into such destinations.

```c
// WRONG:
if (dst->bitmap4_len == 0)
    return;

// CORRECT:
if (src->bitmap4_len == 0)
    return;
```

---

## NFSv4 Operation Structure

### Compound struct fields used in ops
```c
compound->c_ap          // Parsed AUTH_SYS credential
compound->c_inode       // Current filehandle inode
compound->c_session     // NFS4 session (from SEQUENCE)
compound->c_nfs4_client // Client record
compound->c_args        // COMPOUND4args *
compound->c_res         // COMPOUND4res *
compound->c_curr_op     // Index into argarray/resarray
```

### Setup macros (always use these, never index arrays directly)
```c
TYPE *args  = NFS4_OP_ARG_SETUP(compound, op_OPNAME);
TYPE *res   = NFS4_OP_RES_SETUP(compound, op_OPNAME);
TYPE *resok = NFS4_OP_RESOK_SETUP(res, op_OPNAME, OPNAME_ok);
```

### NFS4_OK default
Result structs are `calloc`'d. `NFS4_OK == 0`.  Never write `*status = NFS4_OK;`.

---

## Protocol Handler Registration

```c
struct rpc_operations_handler ops[] = {
    RPC_OPERATION_INIT(NFSPROC3, READ,
                       xdr_READ3args, READ3args,
                       xdr_READ3res,  READ3res,
                       nfs3_op_read),
    // ...
};
```

---

## Logging

```c
LOG("message %s %d", str, val);   // timestamped, pid:tid, function:line
```

High-volume per-request events use `TRACE()` / `TRC()` (written to trace file,
not stderr).

---

## CI Integration Tests

After unit tests, `ci_integration_test.sh`:
1. Starts reffsd with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=0` and `UBSAN_OPTIONS=halt_on_error=0`
2. Mounts via NFSv4.2, clones the source repo, verifies `md5sum configure.ac`
3. Mounts via NFSv3, clones the source repo, verifies `md5sum configure.ac`
4. Graceful SIGTERM shutdown
5. Checks `$LOG` for `ERROR: AddressSanitizer` or `ERROR: LeakSanitizer`

`detect_leaks=0`: `pmap_set()` TIRPC internals and pthread stacks produce
process-lifetime LSan false positives that are not addressable in reffsd.

---

## NFSv4 Delegation Semantics

### A file is open as long as ANY stateid is held

RFC 5661 §10.4: a file is considered open by a client as long as **either**
an open stateid **or** a delegation stateid is outstanding.  CLOSE releases
only the open stateid — the delegation remains valid and the file is still
open from the client's perspective.

RFC 9754 `OPEN_ARGS_SHARE_ACCESS_WANT_OPEN_XOR_DELEGATION` makes the
independence explicit: a client may hold a delegation **without** any open
stateid at all.

### Never revoke delegations at CLOSE time

The client holds dirty pages covered by the delegation and will flush them
before DELEGRETURN.  Revoking inside the CLOSE handler destroys a stateid
the client legitimately holds, causing dirty data loss (the client can no
longer flush pages it wrote under the delegation).

Without CB_RECALL, the server must rely on the client to DELEGRETURN in its
own time (bounded by the lease period).  The correct long-term fix is to
implement CB_RECALL so the server can request delegation return before the
lease expires.
