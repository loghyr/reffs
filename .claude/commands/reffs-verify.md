<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

Pre-push verification checklist for reffs. Run before pushing a branch.

**CI gate (required)**
```
make -f Makefile.reffs check-ci
```
This must pass clean. No skipped tests, no ASAN/LSAN errors.

**RCU / locking audit**
- [ ] No new rcu_read_lock sections that contain blocking calls
- [ ] No new call_rcu callbacks that access memory still reachable by live readers
- [ ] rd_inode nulled before call_rcu in any new dirent teardown path

**Ref-count audit**
- [ ] inode_ref/inode_unref balanced on all new code paths including error paths
- [ ] dirent_ref/dirent_unref balanced on all new code paths including error paths
- [ ] dirent_parent_release nlink logic: death path only, not rename

**Persistence**
- [ ] Any new on-disk state uses write-to-temp / rename (no direct overwrites)
- [ ] New persistent fields have corresponding load/validate logic

**NFSv4 protocol**
- [ ] New EXCHANGE_ID paths are complete (no partial decision tree branches)
- [ ] clientid4 partitioning (boot_seq | incarnation | slot) preserved
- [ ] Wire inputs go through utf8string validation before use

**Deferred work**
- [ ] Any new NOT_NOW_BROWN_COW comment has a description of what it defers and why
- [ ] No existing NOT_NOW_BROWN_COW item is silently depended on by new code

**IETF hygiene** (if touching protocol behavior)
- [ ] Change is consistent with draft-haynes-nfsv4-flexfiles-v2 or draft-ietf-nfsv4-uncacheable
- [ ] Any protocol deviation is intentional and commented

Report: PASS or list of failing items with file/line.
