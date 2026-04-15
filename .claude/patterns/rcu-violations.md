<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# RCU Violation Patterns in reffs

## Pattern 1: Blocking under rcu_read_lock

**Symptom:** Deadlock, RCU stall, or liburcu assertion.

**Wrong:**
```c
rcu_read_lock();
cds_list_for_each_entry_rcu(de, &parent->rd_children, rd_sibling) {
    pthread_mutex_lock(&de->rd_lock);   /* blocks */
}
rcu_read_unlock();
```

**Fix:** Collect refs under rcu_read_lock, drop lock, then acquire mutex.

```c
rcu_read_lock();
cds_list_for_each_entry_rcu(de, &parent->rd_children, rd_sibling) {
    dirent_ref(de);
    pending[n++] = de;
}
rcu_read_unlock();
for (int i = 0; i < n; i++) {
    pthread_mutex_lock(&pending[i]->rd_lock);
    /* ... */
    dirent_unref(pending[i]);
}
```

## Pattern 2: rd_inode nulled after call_rcu

**Symptom:** UAF in RCU callback.

```c
/* WRONG */
call_rcu(&inode->i_rcu, inode_rcu_free);
rcu_assign_pointer(de->rd_inode, NULL);

/* CORRECT */
rcu_assign_pointer(de->rd_inode, NULL);   /* null first */
call_rcu(&inode->i_rcu, inode_rcu_free);  /* then queue free */
```

## Pattern 3: rcu_read_lock nesting around a held mutex

**Symptom:** Deadlock if call path hits call_rcu or rcu_barrier.

```c
pthread_mutex_lock(&some_lock);
rcu_read_lock();
/* ... */
rcu_read_unlock();
pthread_mutex_unlock(&some_lock);
/* anything between lock/unlock that calls rcu_barrier → deadlock */
```

**Fix:** Invert or separate the critical sections.

## Pattern 4: lfht traversal without rcu_read_lock

```c
/* WRONG */
cds_lfht_for_each_entry(ht, &iter, node, field) { ... }

/* CORRECT */
rcu_read_lock();
cds_lfht_for_each_entry(ht, &iter, node, field) { ... }
rcu_read_unlock();
```

## Pattern 5: Spurious rcu_read_unlock

An extra rcu_read_unlock() without a matching rcu_read_lock() corrupts the
per-thread nesting counter. Subsequent legitimate rcu_read_lock sections behave
as if in a quiescent state. Symptom: UAF only under load or specific scheduling.
Audit with grep for paired rcu_read_lock/rcu_read_unlock in any function touching RCU.

## Pattern 6: synchronize_rcu inside a call_rcu callback

**Symptom:** Deadlock (liburcu's RCU thread hangs waiting for itself).

`call_rcu` callbacks run in the liburcu callback thread context.
`synchronize_rcu()` blocks until all pending callbacks complete -- including
the one that called it. This is a self-referential deadlock.

```c
/* WRONG: deadlock */
static void inode_rcu_free(struct rcu_head *head)
{
    struct inode *inode = caa_container_of(head, struct inode, i_rcu);
    synchronize_rcu();   /* hangs: waits for this callback to finish */
    free(inode);
}

/* CORRECT: grace period has already elapsed when the callback fires */
static void inode_rcu_free(struct rcu_head *head)
{
    struct inode *inode = caa_container_of(head, struct inode, i_rcu);
    free(inode);
}
```

The same applies to `rcu_barrier()` and any function that internally calls
`synchronize_rcu`. Grep for those calls inside functions registered with
`call_rcu`.

## Pattern 7: lfht traversal -- iterator not advanced before put()

When iterating a `cds_lfht` and calling `put()` (or any function that may
drop the creation ref) on the current entry, the iterator must be advanced
to the next node BEFORE the put.

`put()` may reach refcount zero, invoke the release callback, which calls
`cds_lfht_del` on the current node. After deletion, calling `cds_lfht_next`
from the deleted node produces undefined behavior.

```c
/* WRONG: put() may delete current node before next() */
rcu_read_lock();
cds_lfht_first(ht, &iter);
while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
    entry = caa_container_of(node, struct nc_gss_ctx, ngc_node);
    cds_lfht_next(ht, &iter);   /* WRONG if already after put */
}
/* ... put() called inside loop body above the next() ... */
rcu_read_unlock();

/* CORRECT: advance before put */
rcu_read_lock();
cds_lfht_first(ht, &iter);
while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
    entry = caa_container_of(node, struct nc_gss_ctx, ngc_node);
    cds_lfht_next(ht, &iter);   /* advance first */
    put(entry);                 /* now safe: current node already skipped */
}
rcu_read_unlock();
```

Applies to all lfht drain loops (shutdown, reaper, bulk revoke).
