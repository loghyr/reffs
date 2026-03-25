<!--
SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# reffs — Claude Code Project Instructions

Coding standards, build rules, and error-code conventions are in:

@.claude/standards.md

Project goals and milestones:

@.claude/goals.md

Design documents:

@.claude/design/mds.md
@.claude/design/erasure-coding.md
@.claude/design/terminology.md

Bug pattern encyclopedias (RCU, ref-counting, NFSv4 protocol):

@.claude/patterns/rcu-violations.md
@.claude/patterns/ref-counting.md
@.claude/patterns/nfs4-protocol.md

Use the `review` subagent after making code changes to enforce style and
check for standards violations before committing.

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
make -f Makefile.reffs check-ci
```

Run before push. ASAN/LSAN clean required. A pre-push hook is in
`.git-hooks/pre-push` — install with `git config core.hooksPath .git-hooks`.

## Slash commands

- `/review` — full code review (style, build, standards, tests)
- `/reffs-debug` — failure triage (UAF, deadlock, LSAN)
- `/reffs-verify` — pre-push checklist
