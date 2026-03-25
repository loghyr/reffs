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
