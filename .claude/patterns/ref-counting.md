<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Ref-Count Patterns in reffs

## Rule 1: Error paths must unref

Every early return after inode_ref or dirent_ref must call the matching unref.

```c
inode_ref(inode);
if (error_condition)
    goto out_unref;
/* ... */
return 0;
out_unref:
    inode_unref(inode);
    return -EINVAL;
```

## Rule 2: Superblock ref released in inode_release only

inode_release is the RCU-deferred destructor. Do not release the sb ref earlier
(e.g. in a teardown path that also calls inode_unref — the unref triggers
inode_release which releases the sb ref, causing a double-free or leak).

**Symptom:** LSAN leak on superblock, or ASAN double-free on superblock.

## Rule 3: dirent_parent_release nlink accounting

nlink is decremented only when a dirent is destroyed (refcount → 0, removed from
tree). NOT on rename. Treat it as "dirent_died_in_parent".

```c
/* WRONG in rename path */
dirent_parent_release(old_parent);   /* dirent still exists, nlink wrong */
```

## Rule 4: hdr_close vs free for HdrHistogram

```c
free(histogram);      /* WRONG: leaks internal allocations, LSAN hit */
hdr_close(histogram); /* CORRECT */
```

## Rule 5: POSIX backend nlink post-teardown

After namespace teardown with the POSIX backend, nlink on the root dirent is 0,
not 1. reffs_life_action_shutdown skips nlink accounting. Tests must expect 0.

## Rule 6: RCU-protected hash table entry lifecycle

Objects stored in `cds_lfht` (liburcu lock-free hash table) with
refcounting (`urcu_ref`) must follow this lifecycle:

### Creation
1. `calloc` the entry (refcount starts at 1 — the **creation ref**)
2. Initialize all fields
3. `cds_lfht_add` under `rcu_read_lock`
4. The creation ref represents "this entry is alive in the hash table"

### Lookup
1. `cds_lfht_lookup` under `rcu_read_lock`
2. `urcu_ref_get_unless_zero` to take a **find ref** (skip if dying)
3. `rcu_read_unlock`
4. Use the entry
5. `put()` to drop the find ref when done

### Destruction (refcount → 0 callback)
1. `cds_lfht_del` under `rcu_read_lock` (remove from hash table FIRST)
2. `call_rcu` to schedule deferred free
3. **Never** free an entry that is still in the hash table
4. `cds_lfht_del` is idempotent — safe to call on already-removed nodes

### Critical rules

**The creation ref keeps the entry in the hash table.** Do not drop it
until the entry should be destroyed (explicit destroy, reaper expiry,
or shutdown drain). Dropping the creation ref after a successful
operation (e.g., after sending a GSS INIT reply) destroys the entry
immediately.

**Every `put()` that can reach refcount 0 must remove from the hash
table.** The release callback (refcount → 0) must call `cds_lfht_del`
before `call_rcu`. Otherwise iterators find freed entries.
`cds_lfht_del` on an already-removed node returns negative but does
not corrupt — safe for the case where explicit removal preceded the
final `put()`.

**`put()` may invoke the release callback synchronously.** The
release callback acquires `rcu_read_lock` and calls `call_rcu`.
Do not call `put()` from a `call_rcu` callback context (nesting
`call_rcu` inside `call_rcu` can deadlock `synchronize_rcu`).

**Iterator threads must call `rcu_register_thread()`.** Without RCU
registration, `rcu_read_lock` is a no-op and provides no protection.
`call_rcu` callbacks can fire while the unregistered thread is
"inside" a read-side critical section.

**Drain at shutdown uses `put()`, not direct `release()`.** Calling
the release callback directly bypasses the refcount. If another thread
holds a find ref, the entry gets freed while still referenced —
double-free via double `call_rcu`.  Drain must also advance the
iterator before calling `put()` (same rule as the reaper).
Ensure all RPC processing is stopped before draining.

```c
/* WRONG: bypasses refcount */
cds_lfht_for_each(...) {
    cds_lfht_del(ht, node);
    release(&entry->ref);       /* double-free if someone holds a ref */
}

/* CORRECT: manual iterator, advance before put */
rcu_read_lock();
cds_lfht_first(ht, &iter);
while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
    entry = caa_container_of(node, ...);
    cds_lfht_next(ht, &iter);  /* advance BEFORE put */
    put(entry);                /* if last ref, release does del + call_rcu */
}
rcu_read_unlock();
synchronize_rcu();             /* wait for all call_rcu frees */
cds_lfht_destroy(ht, NULL);
```

### Explicit removal with outstanding refs

If you want to remove an entry from the hash table immediately
(so it is no longer findable) but defer the free until all refs
drain:

```c
rcu_read_lock();
cds_lfht_del(ht, &entry->node);   /* unfindable now */
rcu_read_unlock();
put(entry);  /* drop creation ref; release's cds_lfht_del is a safe no-op */
```

### Reaper thread pattern

A reaper thread scanning for expired entries must:
1. Call `rcu_register_thread()` at thread start
2. Hold `rcu_read_lock` during the scan
3. `urcu_ref_get_unless_zero` before accessing any entry field
4. Advance the iterator (`cds_lfht_next`) before dropping refs
5. Drop find ref + creation ref to expire an entry
6. Call `rcu_unregister_thread()` at thread exit
