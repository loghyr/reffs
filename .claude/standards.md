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

`LOG()` is for **fatal or actionable errors** — conditions that cause the
server to hang, abort, or corrupt data.  Every LOG line is an actionable item.

`TRACE()` / `TRC()` is for **informative events** — normal operations, state
transitions, request flow, diagnostic information for triaging issues.
Written to the trace file, not stderr.

```c
LOG("critical error: %s", msg);   // operator must act
TRACE("request completed: %s", msg);  // diagnostic info
```

---

## Unused Parameters

Use `__attribute__((unused))` to annotate unused function parameters.
**Never** use `(void)param;` casts to suppress warnings.

```c
// WRONG:
(void)ds;

// CORRECT:
static int my_func(struct dstore *ds __attribute__((unused)), ...)
```

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

### server_state_find() triggers grace transition

`server_state_find()` → `server_state_get()` transitions
`GRACE_STARTED → IN_GRACE` on the first call.  **Never** call
`server_state_find()` during initialization (before the server is
ready for client connections).  Use direct atomic loads or pass
state as parameters instead.

### Attribute bitmap and supported_attributes

Attributes advertised in `supported_attributes` MUST have working
XDR encode/decode handlers AND return valid data.  An attribute
that is "supported" but returns empty/zero data can cause the Linux
NFS client to loop or hang during mount.

Layout attributes (`FATTR4_FS_LAYOUT_TYPES`, `FATTR4_LAYOUT_TYPES`)
must only be set in `supported_attributes` when the server role
includes MDS.  Use `nfs4_attr_enable_layouts()` AFTER
`nfs4_protocol_register()` — never inside `nfs4_attribute_init()`.

### nattr_release must free all allocated fields

When `inode_to_nattr` allocates memory (calloc, strdup, etc.) for
any `nfsv42_attr` field, `nattr_release` MUST free it.  Missing
frees cause leaks on every GETATTR and READDIR entry.

### Never revoke delegations at CLOSE time

The client holds dirty pages covered by the delegation and will flush them
before DELEGRETURN.  Revoking inside the CLOSE handler destroys a stateid
the client legitimately holds, causing dirty data loss (the client can no
longer flush pages it wrote under the delegation).

Without CB_RECALL, the server must rely on the client to DELEGRETURN in its
own time (bounded by the lease period).  The correct long-term fix is to
implement CB_RECALL so the server can request delegation return before the
lease expires.

---

## Erasure Coding — Patent-Safe Implementation Rules

### US 8,683,296 (StreamScale) — DO NOT infringe

StreamScale's patent covers SIMD-accelerated Galois field arithmetic for
erasure coding.  The settlement with James Plank (2014) pulled Jerasure 2.0
and GF-Complete from public availability and barred Plank from further EC
implementation work.

**NEVER** reference, derive from, or cite:
- Plank's papers or code (Jerasure, GF-Complete, any fork)
- SIMD-optimized GF(2^8) multiplication (pshufb split-table, AVX, etc.)
- ISA-L's GF implementation (unclear patent cross-licensing)

**SAFE** to use (pre-dates StreamScale by decades):
- Reed & Solomon 1960 original paper
- Berlekamp 1968 "Algebraic Coding Theory" (GF arithmetic)
- Peterson & Weldon 1972 "Error-Correcting Codes"
- Plain log/antilog table GF(2^8) multiplication
- Vandermonde matrix construction for systematic RS codes
- Gaussian elimination for matrix inversion (decoding)

### Implementation approach

Use scalar log/antilog table GF(2^8) multiply with a standard irreducible
polynomial.  No SIMD.  Reference only pre-2000 textbook sources.  Document
prior art references in source file headers.

---

## Clock and Time

### Dual-clock strategy

| Use case | Clock | Rationale |
|----------|-------|-----------|
| Inode timestamps (atime, mtime, ctime, btime) | `CLOCK_REALTIME` | Persistent metadata; must survive reboot |
| Lease/grace/callback timers | `CLOCK_MONOTONIC` | Immune to ntpd/admin clock adjustments |
| RPC/op latency measurement | `CLOCK_MONOTONIC` | Interval measurement, not wall-clock |
| Logging and trace timestamps | `CLOCK_REALTIME` | Human-readable, correlate with external logs |
| `pthread_cond_timedwait` | `CLOCK_REALTIME` | POSIX requirement (most implementations) |

### Rules

- Use `reffs_now_ns()` for all monotonic timestamps (returns `uint64_t` nanoseconds)
- Use `clock_gettime(CLOCK_REALTIME, &ts)` for inode time updates
- Never use `gettimeofday()` or `struct timeval`
- Never use `time()` — always `clock_gettime`
- `clock_nanosleep(CLOCK_MONOTONIC, ...)` for duration-based sleeps

---

## Atomic Operations

### API: use GCC builtins (`__atomic_*`)

reffs standardizes on **GCC `__atomic_*` builtins**, not C11 `stdatomic.h`.
The GCC builtins work on plain types without requiring `_Atomic` qualification,
which matters for structs shared with on-disk formats and RCU.

Exception: `_Atomic` is acceptable for **file-scope static variables** (e.g.,
`static _Atomic bool first_foo`) where the type is self-contained.

```c
// CORRECT — GCC builtins on plain types:
__atomic_fetch_add(&inode->i_active, 1, __ATOMIC_ACQ_REL);
__atomic_load_n(&sb->sb_state, __ATOMIC_ACQUIRE);

// WRONG — C11 on struct fields:
atomic_fetch_add_explicit(&inode->i_active, 1, memory_order_acq_rel);
```

### Memory ordering rules

| Pattern | Ordering | Example |
|---------|----------|---------|
| Ref-count increment | `__ATOMIC_ACQ_REL` | `__atomic_fetch_add(&i_active, 1, __ATOMIC_ACQ_REL)` |
| Ref-count decrement | `__ATOMIC_ACQ_REL` | `__atomic_fetch_sub(&i_active, 1, __ATOMIC_ACQ_REL)` |
| State flag set (publish) | `__ATOMIC_RELEASE` | `__atomic_fetch_or(&i_state, FLAG, __ATOMIC_RELEASE)` |
| State flag read (consume) | `__ATOMIC_ACQUIRE` | `__atomic_load_n(&i_state, __ATOMIC_ACQUIRE)` |
| Statistics counters | `__ATOMIC_RELAXED` | `__atomic_fetch_add(&calls, 1, __ATOMIC_RELAXED)` |
| Sequence numbers (seqid) | `__ATOMIC_ACQ_REL` | `__atomic_fetch_add(&s_seqid, 1, __ATOMIC_ACQ_REL)` |

### Fields accessed atomically MUST be read atomically

If a field is ever written with `__atomic_*`, all reads must also use
`__atomic_load_n`.  **Never** do a plain read of an atomically-written field:

```c
// WRONG — plain read of atomically-written field:
nattr->numlinks = inode->i_nlink;

// CORRECT:
nattr->numlinks = __atomic_load_n(&inode->i_nlink, __ATOMIC_RELAXED);
```

Exception: initialization before the object is visible to other threads
may use plain writes (e.g., `inode->i_nlink = 1` in `inode_alloc`
before the inode is hashed).
