Triage a reffs failure. Provide the crash output, ASAN/LSAN report, or test failure.

Work through these in order:

**1. Classify the failure**
- ASAN use-after-free → likely RCU callback racing live access, or double-unref
- ASAN heap-buffer-overflow → check dirent list iteration, lfht bucket walk
- LSAN leak → check superblock ref in inode_release, hdr_close vs free for HdrHistogram
- Deadlock / hang → check rcu_read_lock nesting, vfs_lock_dirs on non-directory, rd_lock order
- Assert / wrong value → check nlink accounting in dirent_parent_release, rd_ino never set

**2. Locate the owner**
- UAF on inode: who called inode_unref last? Was it from an RCU callback?
- UAF on dirent: was rd_inode nulled before or after call_rcu?
- Leak on superblock: did inode_release run? Did it release the sb ref?

**3. Identify the test**
- Which fs_test_*.c covers this path?
- If none exists, propose a minimal libcheck test that reproduces it

**4. Propose fix**
- State the invariant being violated
- Show the corrected code
- Note any NOT_NOW_BROWN_COW items that become load-bearing

Be terse. Skip preamble. Lead with the root cause.
