<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Identity BAT: Kerberos -> UNIX mapping with persistence

## Goal

A Kerberos user authenticating via RPCSEC_GSS gets a stable UNIX uid
that survives server restarts.  The mapping is owned by reffs, not
delegated to libnfsidmap's runtime lookup.

## Infrastructure Question: FreeIPA vs MIT + standalone LDAP

The lab runs MIT Kerberos on psyklo (`INTERNAL.EXCFB.COM`).

FreeIPA integrates Kerberos + LDAP + RFC 2307bis in one package but
cannot co-exist with an existing MIT KDC in the same realm without a
full migration.  For psyklo: re-issue all keytabs (5 machines just
enrolled), migrate the KDC database, accept a heavyweight package set
(Dogtag CA, DNS, NTP integrations).

**Recommendation: keep MIT KDC, add standalone OpenLDAP (or 389-DS).**

The split-role setup:
- MIT KDC on psyklo handles authentication (unchanged)
- OpenLDAP on psyklo serves RFC 2307bis POSIX attributes
  (`posixAccount`, `posixGroup`, `uidNumber`, `gidNumber`)
- SSSD on each machine queries both:
  - Kerberos: `krb5_server = psyklo.internal.excfb.com`
  - LDAP: `ldap_uri = ldap://psyklo.internal.excfb.com`
- `libnfsidmap` (via SSSD) resolves `alice@INTERNAL.EXCFB.COM` to
  `uidNumber` from LDAP

This is the standard "MIT KDC + OpenLDAP for POSIX attrs" pattern
used by large RHEL environments that do not want FreeIPA.  The psyklo
LDAP setup is a prerequisite for RFC 2307bis testing but is NOT
required for the BAT plan below -- which uses `libnfsidmap` (falling
back to `getpwnam_r` when LDAP is absent).

## What Exists Today (as of 2026-04-20)

All five implementation steps from the original plan are complete.

- `reffs_id` type, macros, enums (`lib/include/reffs/identity_types.h`)
- Inode uid/gid stored as `reffs_id` (uint64_t) on disk
- AUTH_SYS: `REFFS_ID_MAKE(UNIX, 0, aup_uid)` -- trivial
- Kerberos auth works (TLS, krb5 keytabs, libnfsidmap)

### Step 1 -- Domain table: DONE

`lib/utils/identity_domain.c`:
- In-memory array, mutex-protected
- `identity_domain_find_or_create(name, type)` -- idempotent
- `identity_domain_persist(state_dir)` / `identity_domain_load(state_dir)`

Tests: `test_domain_zero_exists`, `test_domain_create_find`,
`test_domain_persist_load`.

### Step 2 -- Mapping table: DONE

`lib/utils/identity_map.c`:
- `cds_lfht` keyed by `reffs_id`, bidirectional (A->B and B->A)
- `identity_map_add(a, b)`, `identity_map_unix_for(id)`,
  `identity_map_remove(key)`, `identity_map_iterate(cb, arg)`
- `identity_map_persist(state_dir)` / `identity_map_load(state_dir)`

Tests: `test_map_bidirectional`, `test_map_unix_for`,
`test_map_unix_for_nobody`, `test_map_persist_load`.

### Step 3 -- Auth-time lookup-or-create: DONE

`lib/rpc/gss_context.c:gss_ctx_map_to_unix()`:

```
1. Parse "user@REALM" from principal
2. identity_domain_find_or_create(REALM, KRB5) -> domain_idx
3. krb5_id = REFFS_ID_MAKE(KRB5, domain_idx, XXH32(user, ulen, 0))
4. unix_id = identity_map_unix_for(krb5_id)
5. If found: return uid (fast path, no disk IO)
6. If not found: libnfsidmap (nfs4_owner_to_uid) or getpwnam_r
7. identity_map_add(krb, unix_id)   [persist gap -- see Issue 1]
```

Hash: `XXH32(username_only, ulen, 0)`.  Seed 0 and XXH32 are
canonical.  **Never change the hash function or seed** -- the hash is
stored in every inode the principal owns via `reffs_id.local_id`.

### Step 4 -- Restart recovery: DONE

`src/reffsd.c` startup:

```c
identity_domain_load(ss->ss_state_dir);
identity_map_load(ss->ss_state_dir);
```

Shutdown (SIGTERM handler):

```c
identity_domain_persist(ss->ss_state_dir);
identity_map_persist(ss->ss_state_dir);
```

### Step 5 -- Probe ops: DONE

Registered in `lib/probe1/probe1_server.c`:
- Op 21: `IDENTITY_DOMAIN_LIST`
- Op 22: `IDENTITY_MAP_LIST`
- Op 23: `IDENTITY_MAP_REMOVE`

CLI: `reffs-probe.py identity-domain-list`, `identity-map-list`,
`identity-map-remove --from <reffs_id>`.

## Open Issues

### Issue 0 (BLOCKER): identity_map_persist() is unsafe

Two pre-existing bugs in `lib/utils/identity_map.c` must be fixed
before persist-on-add (Issue 1) can be added.

**Bug A: I/O inside rcu_read_lock** (lines 311-327)

```c
rcu_read_lock();
cds_lfht_for_each(map_ht, &iter, node) {
    ...
    n = write(fd, &de, sizeof(de));   /* VIOLATION */
    ...
}
rcu_read_unlock();
```

`write()` is blocking I/O.  Per standards.md "Never block inside
rcu_read_lock (no mutex_lock, no I/O, no allocation)".

**Fix**: Collect entries into a heap array under a single
`rcu_read_lock`, unlock, then write the array to disk outside
the RCU critical section.

**Bug B: Concurrent persist race**

Two workers calling `identity_map_persist()` simultaneously both
open `<state_dir>/identity_map.tmp` with `O_CREAT|O_TRUNC`.
Both file descriptors reference the same inode; concurrent writes
produce corrupted output.

**Fix**: Add a `map_persist_lock` mutex (or reuse the existing
domain mutex pattern) to serialize concurrent persist calls.

### Issue 1 (BLOCKER): Persist-on-add gap

`identity_map_add()` in `gss_ctx_map_to_unix()` adds to the
in-memory table but does NOT immediately persist to disk.  Persist
occurs only at SIGTERM.

**Observable impact**: a crash after a new user authenticates but
before shutdown loses the mapping.  On restart, libnfsidmap is
called again and resolves the same uid (if the backend is stable) --
the mapping is recreated correctly.

**True risk**: if libnfsidmap assigns a different uid between crash
and restart (LDAP `uidNumber` changed, or `/etc/passwd` edited),
inodes owned by that principal now report the wrong uid via GETATTR.
The admin must use `identity-map-remove` via probe to clear the stale
entry and let the correct mapping be recreated on the next auth.

**Fix**: persist immediately after each new mapping is added.  The
slow path fires at most once per unique principal per server lifetime,
so the cost of one write-temp/fsync/rename pair is negligible.

Implementation: add `gss_context_set_state_dir(const char *dir)` --
called from `reffsd.c` at startup.  `gss_ctx_map_to_unix()` calls
`identity_domain_persist()` + `identity_map_persist()` after
`identity_map_add()` using a module-level `state_dir` pointer.  This
mirrors how `g_domain` is set via `idmap_init(domain)` today.

### Issue 2 (BLOCKER): Two tests not yet written

**`test_mapping_survives_restart`**: persist domains + maps, call
`identity_map_fini` + `identity_domain_fini`, reinit both from disk,
verify `identity_map_unix_for` returns the original UNIX uid.

`test_map_persist_load` exists but only tests raw key-value
round-trip, not the full restart sequence with domain table reload.

**`test_principal_local_id_stable`**: assert that
`XXH32("alice", 5, 0)` produces a specific, documented value.
This test locks in the hash invariant and will fail immediately
if the hash function or seed is accidentally changed.

Both belong in `lib/utils/tests/identity_map_test.c`.

## Implementation Steps (Outstanding Only)

Steps 1-5 are complete.

### Fix 0: Repair identity_map_persist() (prerequisite for Fix 1)

**File**: `lib/utils/identity_map.c`

1. Add `static pthread_mutex_t map_persist_lock = PTHREAD_MUTEX_INITIALIZER;`
   alongside `map_ht`.

2. Refactor the persist loop to collect outside RCU:

   ```c
   /* Phase 1: collect under RCU into heap array */
   struct identity_map_disk *entries = NULL;
   uint32_t count = 0, cap = 0;
   rcu_read_lock();
   cds_lfht_for_each_entry(map_ht, &iter, e, im_node) {
       if (count == cap) {
           cap = cap ? cap * 2 : 64;
           rcu_read_unlock();
           entries = realloc(entries, cap * sizeof(*entries));
           if (!entries) goto out_oom;
           rcu_read_lock();
           /* restart iteration -- entries may have changed */
           cds_lfht_first(map_ht, &iter);
           count = 0;
           continue;
       }
       entries[count++] = encode_disk(e);
   }
   rcu_read_unlock();

   /* Phase 2: write outside RCU */
   pthread_mutex_lock(&map_persist_lock);
   /* open tmp, write header + entries[], fsync, rename */
   pthread_mutex_unlock(&map_persist_lock);
   free(entries);
   ```

   Note: if restart-on-realloc silently drops entries, document that
   as acceptable (the map is eventually consistent at SIGTERM; the
   persist-on-add path only needs to be crash-safe, not perfectly
   atomic).

3. `identity_domain_persist()` also writes a flatfile from a
   mutex-protected array; verify it holds the array lock across the
   write (it does, per current code -- no change needed there).

### Fix 1: Persist-on-add

1. Add to `lib/rpc/gss_context.c`:
   ```c
   static const char *g_state_dir;

   void gss_context_set_state_dir(const char *dir)
   {
       g_state_dir = dir;
   }
   ```
2. Declare in `lib/rpc/gss_context.h`
3. Call from `src/reffsd.c` after server_state_init
4. In `gss_ctx_map_to_unix()`, after `identity_map_add(krb, unix_id)`:
   ```c
   if (g_state_dir) {
       identity_domain_persist(g_state_dir);
       identity_map_persist(g_state_dir);
   }
   ```

### Fix 2: Two missing tests

Add `#include <xxhash.h>` at the top of
`lib/utils/tests/identity_map_test.c` (verify `xxhash` appears in
`LDADD` in the test `Makefile.am`).

Add to `lib/utils/tests/identity_map_test.c`:

```c
START_TEST(test_mapping_survives_restart)
{
    int idx = identity_domain_find_or_create("TEST.REALM",
                                             REFFS_ID_KRB5);
    reffs_id krb = REFFS_ID_MAKE(REFFS_ID_KRB5, (uint32_t)idx, 42);
    reffs_id unix_id = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 1000);

    ck_assert_int_eq(identity_map_add(krb, unix_id), 0);
    ck_assert_int_eq(identity_domain_persist(test_dir), 0);
    ck_assert_int_eq(identity_map_persist(test_dir), 0);

    identity_map_fini();
    identity_domain_fini();
    identity_domain_init();
    ck_assert_int_eq(identity_domain_load(test_dir), 0);
    ck_assert_int_eq(identity_map_init(), 0);
    ck_assert_int_eq(identity_map_load(test_dir), 0);

    reffs_id result = identity_map_unix_for(krb);
    ck_assert(!REFFS_ID_IS_NOBODY(result));
    ck_assert_int_eq((int)REFFS_ID_LOCAL(result), 1000);
}
END_TEST

START_TEST(test_principal_local_id_stable)
{
    /*
     * Hash seed 0 and XXH32 are canonical for KRB5 local_id.
     * These values are stored in every inode the principal owns.
     * If any assertion fails, the hash function or seed was
     * changed -- which is a data-corruption bug for deployed inodes.
     */
    ck_assert_int_eq(XXH32("alice",   5, 0), (uint32_t)0x753a727d);
    ck_assert_int_eq(XXH32("bob",     3, 0), (uint32_t)0x02bbe0e7);
    ck_assert_int_eq(XXH32("a",       1, 0), (uint32_t)0x550d7456);
    ck_assert_int_eq(XXH32("user123", 7, 0), (uint32_t)0x1ba25d57);
}
END_TEST
```

The `bob`, `a`, and `user123` vectors are computed identically (compile
and run `printf("0x%08x\n", XXH32(...))` to regenerate if needed).
The canonical source is the XXHash reference implementation.

### Fix 3: Probe remove must persist

**File**: `lib/probe1/probe1_server.c`

`probe1_op_identity_map_remove()` calls `identity_map_remove()` but
never calls `identity_map_persist()`.  After a server restart the
removed mapping reappears from disk, defeating the admin's intent.

Add after the `identity_map_remove()` call:

```c
if (g_state_dir)
    identity_map_persist(g_state_dir);
```

`g_state_dir` is the module-level pointer set by
`gss_context_set_state_dir()` (Fix 1 step 1 above).  The probe
handler must only persist when a state directory is configured.

### Fix 4: Hash invariant comment in gss_context.c

Add near the `XXH32` call in `gss_ctx_map_to_unix()`:

```c
/*
 * Hash seed 0 and XXH32 are canonical for the KRB5 local_id field.
 * This value is stored in every inode owned by this principal.
 * Changing the hash function or seed is a data-corruption bug.
 */
```

## Test Plan (Full)

### Existing unit tests (all passing)

| Test | Intent |
|------|--------|
| `test_domain_zero_exists` | Domain 0 always present |
| `test_domain_create_find` | New realm auto-created |
| `test_domain_persist_load` | Domain table survives disk round-trip |
| `test_map_bidirectional` | A->B and B->A both inserted |
| `test_map_unix_for` | KRB5->UNIX lookup works |
| `test_map_unix_for_nobody` | Unmapped non-UNIX returns NOBODY |
| `test_map_persist_load` | Mapping table survives disk round-trip |

### Tests to add (Fix 2)

| Test | Intent |
|------|--------|
| `test_mapping_survives_restart` | Full reinit from disk; unix_for still works |
| `test_principal_local_id_stable` | XXH32 value is deterministic and documented |

### Functional test

- Start reffsd with Kerberos export
- `kinit alice@INTERNAL.EXCFB.COM`, create file via NFS
- `reffs-probe.py identity-map-list` -- verify mapping present
- SIGTERM + restart reffsd
- GETATTR on the file still returns `uid=1000`
- `reffs-probe.py identity-map-list` -- mapping still present

### Test impact on existing tests

| Test | Impact |
|------|--------|
| All AUTH_SYS path tests | PASS -- unchanged |
| All other `make check` | PASS -- only gss_context.c and test file change |

## RFC 2307bis Path (Deferred)

When psyklo has OpenLDAP with RFC 2307bis:

1. SSSD on each machine queries LDAP for `uidNumber`/`gidNumber`
2. `libnfsidmap` calls `nfs4_owner_to_uid()` via SSSD
3. `gss_ctx_map_to_unix()` fast path is unchanged; the UNIX uid it
   persists is now LDAP-sourced rather than `getpwnam_r`-sourced

No reffs code changes are needed.  The improvement is that the
persisted UNIX uid is authoritative from LDAP rather than from
the local `/etc/passwd`, which eliminates the crash-recovery risk
described in Issue 1 (the uid is stable even if the server is
rebuilt from scratch).

RFC 2307bis support in reffs itself (Step 11 of `identity.md`) is
explicitly out of scope for BAT.

## Files Changed

| File | Change |
|------|--------|
| `lib/rpc/gss_context.c` | `gss_context_set_state_dir()`; persist after add |
| `lib/rpc/gss_context.h` | Declare `gss_context_set_state_dir()` |
| `src/reffsd.c` | Call `gss_context_set_state_dir()` at startup |
| `lib/utils/tests/identity_map_test.c` | Add 2 missing tests |

## Risks

- **Hash stability**: XXH32 seed 0 is canonical.  Documented in code
  (Fix 3) and locked by test (Fix 2).

- **Hash collisions**: ~1 in 4 billion per domain.  Negligible at BAT
  scale.  Collision detection via stored name string.

- **libnfsidmap fallback required**: if the principal is unknown to
  both LDAP and `/etc/passwd`, uid=65534 is persisted.  Admin clears
  via `identity-map-remove`, correct mapping created on next auth.

- **Concurrent domain creation**: `find_or_create` holds the mutex
  for the find-then-create sequence.  No TOCTOU.

- **Concurrent auth for same principal**: `cds_lfht_add_unique` detects
  collision in `map_insert()` -- second thread uses first thread's
  entry.  No duplicate mappings.

## NOT in scope for BAT

- RFC 2307bis LDAP integration in reffs (libnfsidmap handles it)
- Group cache / batch LDAP
- Circuit breaker for libnfsidmap failures
- Windows SID support
- Synthetic uid allocation for unmapped principals
- NFSv4 owner string lookup via mapping table (libnfsidmap handles it)
- `manage_gids`
- RocksDB backend for identity tables (flatfile is sufficient)
