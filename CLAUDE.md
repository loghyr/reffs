<!--
SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# reffs — Claude Code Project Instructions

Coding standards, build rules, and error-code conventions are in:

@.claude/standards.md

Role definitions (planner, programmer, reviewer):

@.claude/roles.md

Project goals and milestones:

@.claude/goals.md

Design documents:

@.claude/design/mds.md
@.claude/design/erasure-coding.md
@.claude/design/terminology.md
@.claude/design/identity.md
@.claude/design/export-management.md
@.claude/design/export-policy.md
@.claude/design/probe-sb-management.md
@.claude/design/sb-registry-v3.md
@.claude/design/rocksdb-backend.md
@.claude/design/rocksdb-database.md
@.claude/design/rocksdb-composition.md
@.claude/design/rocksdb-persistence.md
@.claude/design/rocksdb-implementation.md
@.claude/design/rocksdb-config-and-notes.md
@.claude/design/stable-bat.md
@.claude/design/remove-pynfs.md
@.claude/design/dir-delegations.md
@.claude/design/per-export-dstore.md
@.claude/design/dstore-vtable-v2.md

Bug pattern encyclopedias (RCU, ref-counting, NFSv4 protocol):

@.claude/patterns/rcu-violations.md
@.claude/patterns/ref-counting.md
@.claude/patterns/nfs4-protocol.md

Use the `review` subagent after making code changes to enforce style and
check for standards violations before committing.

## Python / XDR tooling

Python XDR code generation and the RPC client library come from the
**reply** package (`pip install reply-xdr`, source at
https://github.com/loghyr/reply).

- `xdr-parser --lang python foo.x` generates `*_const.py`, `*_type.py`, `*_pack.py`
- `xdr-parser --lang c foo.x` generates `*_xdr.h`, `*_xdr.c`
- `from rpc import rpc` provides the RPC Client/Server
- C XDR generation still uses system `rpcgen`

pynfs has been removed. Do not add imports from pynfs or dependencies
on ply (GPL).

## Deployment Status

**No persistent storage has been deployed.**  All on-disk formats
(registry, chunk store, server state, client state) are version 1
with no migration code.  When the first deployment with persistent
data ships, update this section and all format changes after that
point require version bumps + migration code.

## Architecture

- `lib/fs/` — protocol-agnostic inode/dirent/superblock layer
- `lib/nfs4/` — NFSv4.2-specific; strict one-way dependency on lib/fs (never reverse)
- `lib/nfs3/` — NFSv3 server ops
- `lib/rpc/` — RPC transport, credential parsing, GSS context cache
- `lib/io/` — io_uring event loop, TLS, connection management
- `lib/ec/` — erasure coding (Reed-Solomon, Mojette)
- `src/` — reffsd main; must NOT include `lib/nfs4/include/` (layering violation)
- `tests/` — per-concern `fs_test_*.c` files (libcheck, call `reffs_fs_*()` directly)

## Inode / dirent model

- `struct inode` holds `i_dirent` — weak back-pointer, RCU-nulled before `call_rcu`
- `struct reffs_dirent` holds `rd_children` and `rd_parent` (not on inode)
- `rd_inode` must be nulled *before* `call_rcu`; always read under `rcu_read_lock`

## RCU rules

- Never block inside `rcu_read_lock` (no mutex_lock, no I/O, no allocation)
- `cds_list_for_each_entry_rcu` for dirent iteration
- Mutate dirent list under `rd_parent->rd_lock`; read under `rcu_read_lock`
- `cds_lfht` traversal always under `rcu_read_lock`

## Ref-counting

- `inode_ref` / `inode_unref` — inode lifetime
- `dirent_ref` / `dirent_unref` — dirent lifetime
- `inode_release` runs from RCU callback; must not hold locks; releases sb ref
- `dirent_parent_release` decrements nlink on death only, not rename
- `hdr_close()` not `free()` for HdrHistogram objects
- `inode_active_put()`, `inode_put()`, `super_block_put()` are NULL-tolerant

## NFSv4 state invariants

- `clientid4` = `boot_seq | incarnation | slot` (use accessor macros)
- `nfs4_client_alloc_or_find()` owns EXCHANGE_ID decision tree (5 cases, all required)
- Persistent state: write-to-temp / fsync / rename (never direct overwrite)
- `NOT_NOW_BROWN_COW` marks intentionally deferred work
- Never set `*status = NFS4_OK;` — result structs are `calloc`'d, NFS4_OK (0) is default

## CI gate

```
make -f Makefile.reffs ci-check
```

Run before push. ASAN/LSAN clean required. A pre-push hook is in
`.git-hooks/pre-push` — install with `git config core.hooksPath .git-hooks`.

## Workflow rules

### Review before commit
Always run `/review` BEFORE committing, not after.  The workflow is:
make changes → run reviewer → fix findings → commit.

### RPC wire changes need check-ci
Changes to `lib/rpc/rpc.c` reply encoding, credential parsing, or
verifier handling MUST be verified with `make -f Makefile.reffs ci-check`
(real NFS mount via kernel client), not just `make check` (unit tests).
Unit tests don't catch malformed RPC replies.

### New library dependencies → update both Dockerfiles
Every `PKG_CHECK_MODULES` addition in `configure.ac` requires the
corresponding package in BOTH `Dockerfile` (Fedora, `-devel`) and
`Dockerfile.ci` (Ubuntu, `-dev`).  Do this in the same commit.

### License: AGPL-3.0-or-later
reffs is AGPL-3.0-or-later.  GPL-2.0-only code is NOT compatible
and must NOT be vendored.  `check_license.sh` enforces this.
`make -f Makefile.reffs license` checks headers + compatibility.

### Module init/fini pattern
Subsystem init/fini belongs in the module's own init function
(e.g., `io_handler_init`), not in `reffsd.c main()`.  The module
owns the knowledge of what components need setup.

### Build directory awareness
The build directory is `build/`.  Always use absolute paths for
file edits.  Never assume CWD after `cd build`.

## Slash commands

- `/review` — full code review (style, build, standards, tests, license compat)
- `/reffs-debug` — failure triage (UAF, deadlock, LSAN)
- `/reffs-verify` — pre-push checklist
