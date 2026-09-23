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

### Python prerequisites
```bash
pip install reply-xdr@git+https://github.com/loghyr/reply.git xdrlib3
```
`configure` will fail with a clear error if `xdr-parser` is not in `$PATH`.

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
| `make -f Makefile.reffs ci-check` | Full CI pipeline in Docker |
| `make -f Makefile.reffs image` | Build dev Docker image |
| `make -f Makefile.reffs run-image` | Run NFS server in Docker |
| `make -f Makefile.reffs shell` | Shell into running container |

---

## Style

### ASCII only in source and commits

Source files (`.c`, `.h`, `.sh`, `.py`, `.toml`, `.x`) and
**commit messages** must be pure ASCII (bytes 0x00–0x7F).
No em-dashes, curly quotes, non-breaking spaces, or other
Unicode characters in code, comments, or string literals.

Markdown files (`.md`) are exempt — use Unicode freely in
documentation.

Kernel developers will dismiss code with Unicode violations.
Use `--` for dashes, straight quotes, and `>=`/`<=` for
comparison operators in code.

```bash
# Check for non-ASCII in staged source files (skip .md):
git diff --cached --name-only | grep -v '\.md$' | xargs grep -Pn '[^\x00-\x7F]'
```

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

## Git Safety

### Never change branches in the main workspace

**Never use `git checkout` or `git switch`** to change branches.
Always use `git worktree` for new branches or context switches:

```bash
git worktree add ../topic-branch topic-branch
```

Changing the active branch in the main workspace disconnects the
AI session from its active instructions and context (CLAUDE.md,
.claude/ directory, open file state).

### Build before handoff

For any workflow that modifies code, run the build and tests before
committing or pushing, unless explicitly told not to.  Unit tests
must be at 100% pass rate — CI gates on `make check`.

---

## Branch and Commit Methodology

### Core Rule

**Never commit directly to `main` for feature work.** All development
happens on a named branch.  `main` receives only clean, reviewed,
squashed changesets.

### Workflow

1. **Start a branch**
   ```
   git checkout -b <topic>    # e.g., tls-stress-tool, fix-idirent-uaf
   ```
   Use short, descriptive names.  `wip-` prefix for exploratory work.

2. **Commit freely on the branch**
   "WIP", "fix typo", "debug" are fine on topic branches.  Push to
   origin for cross-machine sync: `git push origin <topic>`

3. **Clean up before merging**
   ```
   git rebase -i main
   ```
   Each commit on `main` should be a coherent unit of work.  Single
   squash for small features; a few well-named commits for larger work.

4. **Run the reviewer before merging**
   Invoke `/review` on the cleaned branch.  Address BLOCKERs.

5. **Merge to main (fast-forward only)**
   ```
   git checkout main
   git merge --ff-only <topic>
   ```
   If `--ff-only` fails, rebase topic onto main first.  No merge
   commits on `main`.

6. **Push `main` only when clean**
   `main` at origin must always build, pass tests, pass license.

### Prohibited on `main`

- Direct `git commit` for active development
- `git push --force` (sole-developer exception only)
- Merge commits (use `--ff-only`)
- "WIP" or "debug" commit messages

### Syncing dev work across machines

```
git fetch origin
git checkout -b <topic> origin/<topic>
```
Topic branches at origin are scratch space.  Only `main` is canonical.

### Commit Message Format

```
<subsystem>: <imperative summary under 72 chars>

Optional body explaining *why*, not *what*.  Reference
NOT_NOW_BROWN_COW items for partial fixes.  Cite RFC sections
for protocol-relevant changes.
```

Rules:
- Always sign off: `git commit -s`
- **Never** add `Co-Authored-By:` lines
- Run `fix-style` and `license` checks before committing
- One concern per commit (don't mix refactoring with features)

### RFC References

Always cite the most recent RFC that supersedes earlier versions:

| Topic | Use | Not |
|-------|-----|-----|
| NFSv4.1/4.2 | RFC 8881 | ~~RFC 5661~~ |
| pNFS Flex Files | RFC 8435 | ~~RFC 7862~~ (ops are in 7862) |
| RPC-over-TLS | RFC 9289 | |
| NFSv4.2 ops | RFC 7862 | |
| pNFS FF v2 | draft-haynes-nfsv4-flexfiles-v2 | |

When a section reference appears in code comments or commit messages,
use the format `RFC 8881 §18.36.3` or `RFC 8881 S18.36.3`.

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

## XDR Proc Indirection and UBSan Suppression

`xdrproc_t` is `bool_t (*)(XDR *, ...)` -- variadic for historical reasons.
Real XDR procs are non-variadic (e.g. `bool_t xdr_FOO(XDR *, FOO *)`).
Calling them through `xdrproc_t` is ABI-safe (the variadic shape was always
called with two args in practice and every libtirpc / Apple-RPC consumer
relies on that), but it is a strict function-pointer-type mismatch that
UBSan's `-fsanitize=function` flags at runtime.

The reffs convention: isolate every indirect call through `xdrproc_t` in a
single `__attribute__((no_sanitize("function")))` wrapper per TU.  Existing
wrappers:

| File | Wrapper | Notes |
|------|---------|-------|
| `lib/rpc/rpc.c` | `rpc_call_xdr` | Server-side dispatch.  Branches Linux/Darwin (Darwin's `xdrproc_t` carries a third recursion-depth arg). |
| `lib/nfs4/client/mds_tls_xprt.c` | `mds_tls_call_xdrproc` | Client-side TLS xprt encode/decode.  Linux-only path. |

Rules:

- **Do not add a fresh `(xdrproc_t)foo` cast at a new call site.**  Route
  through the existing wrapper for that TU.  If a TU does not yet have one,
  add a narrow `__attribute__((no_sanitize("function")))` static helper
  alongside an explanatory comment matching the two above.
- The `xdr_sizeof` call below is the documented exception -- it is a
  measurement primitive, not a real XDR call, and libtirpc itself casts to
  `xdrproc_t` at the API boundary.
- For test code that needs an `xdrproc_t`-shaped function that always
  returns FALSE (encode-failure injection), declare the test helper as
  variadic (`bool_t f(XDR *, ...)`) so the implicit conversion to
  `xdrproc_t` is clean without a cast.  Cast `xdr_void` (libtirpc declares
  it `bool_t xdr_void(void)`) through `(xdrproc_t)(void *)` to bypass
  `-Wcast-function-type-mismatch`.

Lock-step with libtirpc and Apple's RPC: both projects use the same
`-fno-sanitize=function` carve-out for the same reason.  The suppression
is a strict-typing accommodation, not evidence of an actual bug.

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

## Unit Test Discipline

### Existing tests are sacred

New code must not break existing passing tests.  If a change breaks
tests, **stop and question the design** before modifying the test.
The test may be correct and the design wrong.

### When a test must change

The only acceptable reasons:
1. **Mechanical**: function signature changed globally, type widened
2. **Intent changed**: the new design intentionally changes the
   tested behavior — requires explicit rationale and approval

In both cases, the reviewer must ask: what design decision caused
this? Is the design decision correct?

### Test comments

Every test should explain its **intent** — what behavior it validates
and why.  When a test uses a specific value (e.g., `SUPER_BLOCK_ROOT_ID`),
comment whether that value is a hard requirement or convention.

### Design review checklist

When reviewing a new design or plan, the reviewer must ask:
1. Where are the unit tests?
2. Are functional tests planned?
3. Are CI tests planned?
4. Do any existing tests need to change? If so, why?

### Test infrastructure

`reffs_ns_init()` in the test harness is a convenience, not a mandate.
Some tests need a pre-built namespace (fs tests), others need a bare
environment (recovery tests).  The test infrastructure must support
both modes.

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

### Unit test time budget

Individual unit tests must complete in **under 2 seconds**.  The full
`make check` suite should run in under 30 seconds.

The `scripts/timed-test.sh` LOG_COMPILER records per-test wall-clock
time to `$REFFS_TEST_TIMING` (default `/tmp/reffs-test-timing.txt`).

Common causes of slow tests:
- Thread `fini()` joining a sleeping thread — use condvar + signal
- Grace period timers at production timeouts — check for early exit
- Repeated `synchronize_rcu` / `rcu_barrier` in loops

---

## Security Flavor Graceful Degradation

### NFS4ERR_DELAY, not NFS4ERR_WRONGSEC, for broken backends

When a security flavor is configured on an export but its backend is
unavailable (missing keytab, KDC unreachable, TLS certs missing):

- **Never** return `NFS4ERR_WRONGSEC` — that tells the client to try
  a different flavor, potentially bypassing the security policy.  A DoS
  on the KDC must not grant access.
- **Return `NFS4ERR_DELAY`** — tells the client the problem is transient
  and to retry with the same flavor.
- **SECINFO** always advertises configured flavors (policy is unchanged).
- **Multi-flavor export** (e.g., `["sys", "krb5"]`): SYS clients still
  work; krb5 clients get DELAY until the KDC/keytab is restored.
- **LOG once** when a flavor backend becomes unavailable.
- Same rule applies to TLS: missing certs → DELAY for TLS clients.

### Availability tracking

Track `gss_server_cred_available()` and `tls_available()` flags.
Check in the NFS compound path (not at the RPC GSS INIT layer).
GSS INIT failures propagate naturally via GSS_S_FAILURE — rpc.gssd
retries automatically.

---

## NFSv4 Delegation Semantics

### A file is open as long as ANY stateid is held

RFC 8881 §10.4: a file is considered open by a client as long as **either**
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

### Two atomic APIs in use

reffs uses two atomic APIs.  **New code** must use C11.  Existing
GCC-builtin code is grandfathered and must not be mixed with C11
on the same field.

#### C11 `<stdatomic.h>` — required for new code

`_Atomic`-qualified types, `atomic_*_explicit()` functions, and
`memory_order_*` constants.

```c
#include <stdatomic.h>

_Atomic uint64_t os_calls;
atomic_fetch_add_explicit(&s->os_calls, 1, memory_order_relaxed);
atomic_load_explicit(&sb->sb_state, memory_order_acquire);
```

Rules for C11 fields:
- `_Atomic` on both struct fields and file-scope statics
- Always use `_explicit` variants with explicit memory order
- `#include <stdatomic.h>` in every file that uses atomic operations
- For fields that are only accessed from a single thread during init
  (e.g., `inode->i_nlink = 1` in `inode_alloc`), plain writes before
  the object is visible to other threads are acceptable

#### GCC `__atomic_*` builtins — grandfathered fields only

The following fields use GCC `__atomic_*` builtins on plain (non-`_Atomic`)
types.  Converting them to C11 `_Atomic` was attempted and failed (ABI
incompatibility with liburcu's `urcu_ref` on the same structs).  These
fields are **grandfathered**: existing code using `__atomic_*` on them
is correct and must not be flagged by the reviewer.

| Struct | Field | Builtin pattern | Files |
|--------|-------|----------------|-------|
| `struct inode` | `i_active` (`int64_t`) | `__atomic_fetch_add`, `__atomic_load_n`, `__atomic_store_n` | inode.c, super_block.c, fs_test_lru.c |
| `struct inode` | `i_state` (`uint64_t`) | `__atomic_fetch_or`, `__atomic_fetch_and`, `__atomic_load_n` | inode.c, super_block.c, stateid.c |
| `struct inode` | `i_stateid_next` (`uint64_t`) | `__atomic_fetch_add` | stateid.c |
| `struct reffs_dirent` | `rd_active` (`int64_t`) | `__atomic_fetch_add`, `__atomic_sub_fetch`, `__atomic_load_n`, `__atomic_store_n` | dirent.c, super_block.c |
| `struct reffs_dirent` | `rd_state` (`uint64_t`) | `__atomic_fetch_or`, `__atomic_fetch_and`, `__atomic_load_n` | dirent.c, vfs.c, attr.c, dir.c, file.c |
| `struct nfs4_session` | `ns_state` (`uint64_t`) | `__atomic_fetch_or`, `__atomic_fetch_and` | session.c |
| `struct nfs4_client` | `nc_session_count` | `__atomic_fetch_add`, `__atomic_fetch_sub` | session.c |
| `struct nfs4_client` | `nc_last_renew_ns` | `__atomic_store_n`, `__atomic_load_n` | session.c, lease_reaper.c |
| `struct super_block` | `sb_inode_lru_count`, `sb_dirent_lru_count` (`size_t`) | plain reads outside lock (data-race tolerated for heuristic LRU pressure check) | inode.c, dirent.c, evictor.c |
| `struct rpc_program_handler` | `rph_calls`, `rph_flags`, etc. | `__atomic_fetch_add`, `__atomic_fetch_or`, `__atomic_load_n` | rpc.c |

**Do not** add new GCC-builtin atomic fields.  **Do not** convert
grandfathered fields to C11 without explicit approval — the previous
attempt caused build failures.  **Do not** flag existing usage of
these fields as a standards violation in reviews.

### Memory ordering rules

| Pattern | Ordering | Example |
|---------|----------|---------|
| Ref-count increment | `memory_order_acq_rel` | `atomic_fetch_add_explicit(&i_active, 1, memory_order_acq_rel)` |
| Ref-count decrement | `memory_order_acq_rel` | `atomic_fetch_sub_explicit(&i_active, 1, memory_order_acq_rel)` |
| State flag set (publish) | `memory_order_release` | `atomic_fetch_or_explicit(&i_state, FLAG, memory_order_release)` |
| State flag read (consume) | `memory_order_acquire` | `atomic_load_explicit(&i_state, memory_order_acquire)` |
| Statistics counters | `memory_order_relaxed` | `atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed)` |
| Sequence numbers (seqid) | `memory_order_acq_rel` | `atomic_fetch_add_explicit(&s_seqid, 1, memory_order_acq_rel)` |

### Fields accessed atomically MUST be read atomically

If a field is ever written with `atomic_*_explicit`, all reads must also
use `atomic_load_explicit`.  **Never** do a plain read of an `_Atomic` field:

```c
// WRONG — plain read of _Atomic field:
nattr->numlinks = inode->i_nlink;

// CORRECT:
nattr->numlinks = atomic_load_explicit(&inode->i_nlink, memory_order_relaxed);
```

Exception: initialization before the object is visible to other threads
may use plain writes (e.g., `inode->i_nlink = 1` in `inode_alloc`
before the inode is hashed).
