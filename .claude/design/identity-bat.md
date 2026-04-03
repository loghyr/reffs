<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Identity BAT: Kerberos → UNIX mapping with persistence

## Goal

A Kerberos user authenticating via RPCSEC_GSS gets a stable UNIX uid
that survives server restarts.  The mapping is owned by reffs, not
delegated to libnfsidmap's runtime lookup.

## What Exists Today

- `reffs_id` type, macros, enums (identity_types.h)
- Inode uid/gid stored as `reffs_id` (uint64_t) on disk
- AUTH_SYS: `REFFS_ID_MAKE(UNIX, 0, aup_uid)` — trivial
- Kerberos auth works (TLS, krb5 keytabs, libnfsidmap)
- `idmap_principal_to_ids()` maps principal → uid/gid at auth time
- On restart, libnfsidmap re-queries (no server-side persistence)
- RocksDB namespace CF stubs: `identity_domains`, `identity_map`
  (reserved but not implemented)

## What This Plan Adds

1. **Domain table**: maps realm name → domain index (persisted)
2. **Mapping table**: maps reffs_id(KRB5) ↔ reffs_id(UNIX) (persisted)
3. **Lookup-or-create at auth time**: on RPCSEC_GSS, check mapping
   table first; fall back to libnfsidmap; persist the result
4. **Reload on restart**: mappings restored from persistence

## Design

### Domain Table

```c
struct reffs_id_domain {
    uint32_t rd_index;       /* 0 = local UNIX, 1..N = remote */
    char     rd_name[256];   /* "EXAMPLE.COM" */
    uint8_t  rd_type;        /* REFFS_ID_KRB5, REFFS_ID_SID, etc. */
};
```

- Domain 0 is always local UNIX (implicit, never persisted)
- Auto-created on first auth from a new realm
- Persisted in flatfile (`identity_domains` in state_dir) or
  RocksDB CF (`identity_domains`)

In-memory: array indexed by `rd_index`.  Small (< 16 domains
expected).  Protected by a mutex for writes; reads are
copy-on-write or atomic pointer swap.

### Mapping Table

```c
struct reffs_id_mapping {
    reffs_id rm_from;        /* e.g., KRB5(1, 42) */
    reffs_id rm_to;          /* e.g., UNIX(0, 1000) */
    char     rm_name[256];   /* "alice@EXAMPLE.COM" */
    uint64_t rm_created_ns;  /* CLOCK_REALTIME */
};
```

Bidirectional: given a krb5 reffs_id, find the UNIX uid.  Given
a UNIX uid, find the krb5 principal (for NFSv4 GETATTR owner
strings — deferred for BAT, libnfsidmap handles it).

In-memory: `cds_lfht` keyed by `rm_from`.  Reverse lookup
(rm_to → rm_from) is a linear scan (OK for BAT scale; index
later if needed).

Persistence: append-only log in state_dir (`identity_map`) or
RocksDB CF (`identity_map`).  Write-temp/fsync/rename for
flatfile.  Each mapping is one record.

### Auth-Time Flow

```
RPCSEC_GSS context established → principal = "alice@EXAMPLE.COM"

1. Parse principal: user="alice", realm="EXAMPLE.COM"

2. Domain lookup: find_or_create_domain("EXAMPLE.COM", KRB5)
   → domain_index = 1

3. Mapping lookup: find_mapping(KRB5(1, hash("alice")))
   → if found: return UNIX uid from mapping
   → if not found: step 4

4. Fallback to libnfsidmap:
   idmap_principal_to_ids("alice@EXAMPLE.COM") → uid=1000, gid=1000

5. Create mapping:
   KRB5(1, hash("alice")) → UNIX(0, 1000)
   name = "alice@EXAMPLE.COM"
   Persist to disk.

6. Return uid=1000, gid=1000 to compound processing.
```

### Principal Hashing

The `local_id` field of `reffs_id` is 32 bits.  For Kerberos
principals, use a deterministic hash of the username portion
(not the realm — that's encoded in the domain index):

```c
uint32_t principal_local_id(const char *username)
{
    return XXH32(username, strlen(username), 0);
}
```

Hash collisions: the mapping table stores the full name string.
On collision (same hash, different name), allocate a synthetic
local_id (counter per domain).

### Persistence API

Extend `struct persist_ops`:

```c
/* Domain table */
int (*identity_domain_save)(void *ctx,
                            const struct reffs_id_domain *domains,
                            uint32_t count);
int (*identity_domain_load)(void *ctx,
                            struct reffs_id_domain *domains,
                            uint32_t max, uint32_t *count);

/* Mapping table */
int (*identity_map_append)(void *ctx,
                           const struct reffs_id_mapping *mapping);
int (*identity_map_load)(void *ctx,
                         int (*cb)(const struct reffs_id_mapping *, void *),
                         void *arg);
```

Flatfile: `identity_domains` (header + fixed-size records),
`identity_map` (append-only log with header).

RocksDB: `identity_domains` CF keyed by `idom:<index BE>`,
`identity_map` CF keyed by `imap:<reffs_id BE>`.  Both stubs
already exist.

### Restart Recovery

```
server_state_init:
  1. Load domain table → rebuild in-memory array
  2. Load mapping table → rebuild in-memory lfht

RPCSEC_GSS auth:
  1. Look up mapping in memory (fast path — no disk IO)
  2. If miss, fall back to libnfsidmap + persist
```

### Integration Point

The auth-time mapping happens in `idmap_gss_to_ids()` or
equivalent.  Let me check:

Currently, `compound->c_ap` (authunix_parms) is populated from
RPCSEC_GSS context via `idmap_principal_to_ids`.  The integration
point is where this function is called — replace with
"check mapping table first, fall back to libnfsidmap, persist".

## Test Plan

### Unit Tests

| Test | Intent |
|------|--------|
| `test_domain_create_and_find` | Create domain, find by name, verify index |
| `test_domain_persist_roundtrip` | Save domains, load, compare |
| `test_mapping_create_and_find` | Create mapping, look up by reffs_id |
| `test_mapping_persist_roundtrip` | Append mapping, load, verify |
| `test_mapping_survives_restart` | Save, destroy, reload, lookup succeeds |
| `test_principal_local_id_stable` | Same username always produces same hash |

### Functional Test

- Start reffsd with krb5
- Authenticate as `alice@REFFS.TEST`
- Create a file → inode stores `REFFS_ID_MAKE(KRB5, 1, hash("alice"))`
- GETATTR returns `uid=1000` (from mapping)
- Restart reffsd
- GETATTR still returns `uid=1000` (from persisted mapping)
- No libnfsidmap query on the second GETATTR (cache hit)

### Test Impact

| Existing test | Impact |
|---------------|--------|
| All existing tests | PASS — AUTH_SYS path unchanged |
| `nfs4_client_persist.c` | PASS — identity persist is separate |
| `idmap_test.c` | May need extension for mapping table |

## Implementation Steps

### Step 1: Domain table structures + persistence

**Files**: `lib/include/reffs/identity_domain.h` (NEW),
`lib/utils/identity_domain.c` (NEW),
`lib/backends/flatfile_persist.c`, `lib/include/reffs/persist_ops.h`

- `struct reffs_id_domain`
- In-memory array + mutex
- `identity_domain_find_or_create(name, type)` → index
- Flatfile save/load
- Persist_ops wiring
- Unit tests

### Step 2: Mapping table structures + persistence

**Files**: `lib/include/reffs/identity_map.h` (extend existing),
`lib/utils/identity_map.c` (extend existing),
`lib/backends/flatfile_persist.c`

- `struct reffs_id_mapping`
- In-memory `cds_lfht` keyed by `rm_from`
- `identity_map_find(reffs_id)` → mapping
- `identity_map_add(from, to, name)` → persist
- Append-only flatfile
- Persist_ops wiring
- Unit tests

### Step 3: Auth-time lookup-or-create

**Files**: `lib/utils/idmap.c` (extend)

- `idmap_gss_to_ids()`: check mapping table first
- On miss: call `idmap_principal_to_ids()` (libnfsidmap)
- Create domain if new realm
- Create mapping
- Persist both

### Step 4: Restart recovery

**Files**: `lib/utils/server.c`

- In `server_state_init`: load domains, load mappings
- Wire into the persist_ops dispatch (flatfile or RocksDB)

### Step 5: RocksDB backend (if time)

**Files**: `lib/backends/rocksdb_namespace.c`

- Fill in the identity_domains and identity_map CF stubs
- Same API as flatfile

## Files Summary

| File | Change |
|------|--------|
| `lib/include/reffs/identity_domain.h` | NEW — domain struct + API |
| `lib/include/reffs/identity_map.h` | Extend — mapping struct + API |
| `lib/utils/identity_domain.c` | NEW — domain table implementation |
| `lib/utils/identity_map.c` | Extend — mapping table with lfht |
| `lib/include/reffs/persist_ops.h` | Add domain + mapping ops |
| `lib/backends/flatfile_persist.c` | Flatfile domain + mapping persistence |
| `lib/utils/idmap.c` | Auth-time lookup-or-create |
| `lib/utils/server.c` | Load domains + mappings at init |
| `lib/utils/tests/identity_map_test.c` | Extend with persistence tests |

## Risks

- **Hash collisions**: XXH32 has ~1 in 4 billion collision rate per
  domain.  For BAT scale (< 100 users) this is negligible.  The
  full name is stored in the mapping record for collision detection.

- **libnfsidmap fallback**: if libnfsidmap is unavailable at auth time
  (no nsswitch, no LDAP), the mapping creation fails.  Return
  `REFFS_ID_NOBODY` and log a warning.  The circuit breaker (step 10,
  deferred) handles repeated failures gracefully.

- **Concurrent auth**: two threads authenticating the same principal
  simultaneously could create duplicate mappings.  The lfht add is
  atomic — second add detects the existing entry and uses it.

## NOT in scope for BAT

- NFSv4 owner string lookup via mapping table (libnfsidmap handles it)
- Group cache / batch LDAP
- Circuit breaker
- RFC 2307bis
- manage_gids
- Windows SID support
- Synthetic uid allocation for unmapped principals
