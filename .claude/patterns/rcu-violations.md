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
