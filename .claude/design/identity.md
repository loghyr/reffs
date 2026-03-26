<!--
SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Identity Management Design

## Problem

reffs stores `uint32_t i_uid/i_gid` on inodes — pure UNIX semantics.
This breaks in multi-source environments:

- Kerberos principals don't map 1:1 to UNIX uids without external config
- Windows SIDs carry domain context that UNIX uids don't
- NFSv3 has no owner strings — only numeric uid/gid on the wire
- RFC 2307bis assigns POSIX uids to AD accounts, but isn't universal
- Users can belong to hundreds of groups; individual lookups don't scale

## Internal Identity: `reffs_id`

A `uint64_t` with structured fields.  Stored on inodes, in ACLs, in
the delegation and layout tables — everywhere the current `uint32_t`
uid/gid lives.

```
 63    60 59       32 31            0
 +------+-----------+---------------+
 | type |  domain   |    local_id   |
 +------+-----------+---------------+
   4 bit   28 bit       32 bit
```

### Accessor macros

```c
typedef uint64_t reffs_id;

#define REFFS_ID_TYPE(id)    ((uint16_t)((id) >> 60))
#define REFFS_ID_DOMAIN(id)  ((uint32_t)(((id) >> 32) & 0x0FFFFFFF))
#define REFFS_ID_LOCAL(id)   ((uint32_t)((id) & 0xFFFFFFFF))

#define REFFS_ID_MAKE(type, domain, local) \
    (((uint64_t)(type) << 60) | \
     ((uint64_t)((domain) & 0x0FFFFFFF) << 32) | \
     (uint64_t)(local))
```

### Identity types

```c
enum reffs_id_type {
    REFFS_ID_UNIX     = 0,  /* local UNIX uid/gid (domain=0) */
    REFFS_ID_SID      = 1,  /* Windows SID RID (domain=idx) */
    REFFS_ID_KRB5     = 2,  /* Kerberos principal (domain=realm_idx) */
    REFFS_ID_NAME     = 3,  /* name-mapped (user@domain string) */
    REFFS_ID_SYNTH    = 4,  /* synthetic/fencing credential */
    REFFS_ID_RFC2307  = 5,  /* UNIX uid sourced from 2307bis LDAP */
    /* 6-14 reserved */
    REFFS_ID_NOBODY   = 15, /* anonymous/unmapped (local=65534) */
};
```

### Why type 0 with domain 0 is backward-compatible

`REFFS_ID_MAKE(0, 0, 1000)` = `0x0000_0000_0000_03E8`.  The low
32 bits are the UNIX uid.  Any code that truncates to `uint32_t`
gets the correct uid.  This means existing on-disk inodes (which
store 32-bit uid/gid) can be widened to 64-bit without migration —
zero-extend.

## Domain Table

Maps domain index → external domain descriptor.  Persisted in the
state directory as `identity_domains`.

```c
struct reffs_id_domain {
    uint32_t rd_index;          /* 0 = local, 1..N = remote */
    char     rd_name[256];      /* "EXAMPLE.COM", "S-1-5-21-..." */
    enum reffs_id_type rd_type; /* what kind of identities live here */
    char     rd_ldap_uri[256];  /* ldap://dc.example.com (if applicable) */
    uint32_t rd_flags;          /* RD_HAS_RFC2307, RD_TRUST_TRANSITIVE */
};
```

Domain 0 is always the local UNIX namespace.  Domains are
auto-created on first authentication from a new realm/SID authority.

## Identity Mapping Table

Bidirectional mapping between `reffs_id` values of different types.
A Kerberos principal `alice@EXAMPLE.COM` might map to:

```
reffs_id(KRB5, 1, 42)  ↔  reffs_id(UNIX, 0, 1000)
reffs_id(KRB5, 1, 42)  ↔  reffs_id(SID, 2, 501)
```

### Schema

```c
struct reffs_id_mapping {
    reffs_id rm_primary;      /* canonical identity for this user */
    reffs_id rm_aliases[];    /* equivalent identities */
    char     rm_name[256];    /* display name: "alice@EXAMPLE.COM" */
    uint32_t rm_flags;        /* RM_FROM_LDAP, RM_FROM_AUTH, ... */
    time_t   rm_created;
    time_t   rm_last_seen;    /* for cache eviction */
};
```

### Storage

- In-memory: `cds_lfht` keyed by `reffs_id` (fast path)
- On-disk: append-only log in state directory (`identity_map`)
- Write-ahead: new mappings written to temp, fsynced, renamed

### When mappings are created

Lazily, at authentication time:

1. **AUTH_SYS**: uid/gid → `REFFS_ID_UNIX` directly, no mapping needed
2. **RPCSEC_GSS**: principal → look up in mapping table.  If not found,
   query external source (see below), assign `reffs_id`, persist.
3. **NFSv4 SETATTR owner string**: `user@domain` → look up or create

## NFSv3: No Owner Strings

NFSv3 carries only numeric uid/gid in `fattr3` and AUTH_SYS
credentials.  Two cases:

### Inbound (client → server)

AUTH_SYS arrives with `aup_uid=1000, aup_gid=1000`.
Convert directly: `reffs_id(UNIX, 0, 1000)`.

No external lookup needed — the uid IS the identity.

### Outbound (GETATTR response)

Inode stores `reffs_id`.  Need to convert to `uint32_t` for the wire:

- **Type UNIX**: `REFFS_ID_LOCAL(id)` — direct, zero-cost
- **Type SID/KRB5/NAME**: look up UNIX alias in mapping table.
  If no UNIX alias exists, return `nobody` (65534).
- **Type RFC2307**: `REFFS_ID_LOCAL(id)` — the local_id IS the
  POSIX uidNumber from LDAP, by definition.

### Mixed NFSv3/v4 environments

An inode created over NFSv4 by `alice@EXAMPLE.COM` stores
`reffs_id(KRB5, 1, 42)`.  When an NFSv3 client does GETATTR:

1. Look up `reffs_id(KRB5, 1, 42)` in mapping table
2. Find alias `reffs_id(UNIX, 0, 1000)` (assigned from 2307bis or sssd)
3. Return `uid=1000` on the wire

If no alias: return 65534.  The NFSv3 client sees `nobody`.  This
is the standard NFS behavior for unmapped users.

## RFC 2307bis Integration

RFC 2307bis defines LDAP attributes for POSIX identity:

```
objectClass: posixAccount
uid: alice
uidNumber: 1000
gidNumber: 1000
homeDirectory: /home/alice

objectClass: posixGroup
cn: developers
gidNumber: 2000
memberUid: alice
memberUid: bob
```

### When available (RD_HAS_RFC2307 flag on domain)

- LDAP lookup returns `uidNumber`/`gidNumber` alongside the SID
- Store as `reffs_id(RFC2307, domain_idx, uidNumber)` — the
  local_id IS the POSIX uid, directly usable for NFSv3
- Create bidirectional mapping: `SID ↔ RFC2307 ↔ UNIX`
- Group membership from `memberUid` or `member` attributes

### When not available

- Allocate synthetic UNIX uids from a configured range (e.g.,
  100000-200000) for NFSv3 compatibility
- Persist the allocation in the mapping table
- Same uid always assigned to same SID (idempotent)

### Why not just use libnfsidmap for everything

libnfsidmap is a good runtime mapper, but:

1. It has no batch API — one lookup at a time
2. It doesn't persist mappings across restarts
3. It doesn't handle SID ↔ uid bidirectional mapping
4. It doesn't expose group membership lists

We use libnfsidmap as ONE input source (the fallback for
`gss_ctx_map_to_unix`), but the reffs mapping table is the
authority.

## Group Membership

### The scaling problem

An AD user can be a member of 500+ groups (nested group
expansion).  AUTH_SYS carries at most 16 supplementary gids
(RFC 5531).  RPCSEC_GSS carries zero — group info comes from
the server's lookup.

For a READDIR of 1000 entries, each needing an access check
against the caller's groups, individual LDAP queries per entry
would be catastrophic.

### Design: group cache with batch refresh

```c
struct reffs_group_cache {
    struct cds_lfht *gc_ht;       /* reffs_id → group_entry */
    pthread_mutex_t  gc_mutex;
    uint32_t         gc_ttl_sec;  /* cache lifetime (default: 300) */
};

struct reffs_group_entry {
    struct cds_lfht_node ge_node;
    reffs_id    ge_user;           /* who this entry is for */
    reffs_id   *ge_groups;         /* array of group reffs_ids */
    uint32_t    ge_ngroups;
    time_t      ge_fetched;        /* when the batch was fetched */
    time_t      ge_expires;        /* ge_fetched + ttl */
    uint32_t    ge_flags;          /* GE_VALID, GE_REFRESHING */
};
```

### Batch lookup strategy

On first access check for a user (or cache expiry):

1. **Single LDAP query** for all group memberships:
   ```
   (&(objectClass=posixGroup)(memberUid=alice))
   ```
   or for AD with nested groups:
   ```
   (&(objectClass=group)(member:1.2.840.113556.1.4.1941:=<user-dn>))
   ```
   The `1.2.840.113556.1.4.1941` OID is the AD "LDAP_MATCHING_RULE_IN_CHAIN"
   operator — returns all groups including nested membership in one query.

2. **Parse results** into `ge_groups[]` array of `reffs_id` values.

3. **Cache** with TTL.  Subsequent access checks are O(1) hash lookup
   + linear scan of group array (or secondary hash for very large
   group sets).

4. **Background refresh**: when `ge_expires - now < ttl/4`, trigger
   async refresh.  The stale entry remains valid until the refresh
   completes.  This avoids blocking the request path.

### AUTH_SYS group augmentation

AUTH_SYS carries up to 16 gids.  For local UNIX users this is often
sufficient.  But if the server has LDAP/AD integration:

1. Use the AUTH_SYS uid to look up the user in the mapping table
2. If a mapping exists with a domain that has group info, merge the
   LDAP group list with the AUTH_SYS gid list
3. Cache the merged result

This is optional and configured per-export (`manage_gids` in
NFS server parlance).  When enabled, the server's group list
overrides the client's 16-gid limit.

## Timeout and Failure Handling

### Per-lookup timeout

All external lookups (LDAP, Kerberos, nsswitch) go through a
single function with a configurable timeout:

```c
#define REFFS_ID_LOOKUP_TIMEOUT_MS  3000  /* 3 seconds */
```

Implementation: LDAP uses `ldap_set_option(LDAP_OPT_NETWORK_TIMEOUT)`.
Kerberos/nsswitch use a wrapper thread with `pthread_timedjoin_np`.

### Circuit breaker

When an external source fails N times in a row (default: 3),
mark it as DOWN for a cooldown period (default: 30 seconds).
During cooldown:

- Return cached results if available (even if expired)
- Return NOBODY for uncached identities
- Log a single warning, not per-request spam

```c
struct reffs_id_source {
    uint32_t rs_consecutive_failures;
    time_t   rs_down_until;         /* 0 = healthy */
    uint32_t rs_failure_threshold;  /* default: 3 */
    uint32_t rs_cooldown_sec;       /* default: 30 */
};
```

After cooldown expires, the next request tries the source again.
If it succeeds, reset the failure counter.

### Behavior on lookup failure

| Scenario | Behavior |
|----------|----------|
| New principal, LDAP up | Query, create mapping, proceed |
| New principal, LDAP down | Map to NOBODY, log warning |
| Known principal, cache valid | Use cache, no query |
| Known principal, cache expired, LDAP up | Background refresh |
| Known principal, cache expired, LDAP down | Use stale cache |
| Group lookup timeout | Use AUTH_SYS gids only |

Never block an NFS operation waiting for LDAP.  Stale data is
better than a hung mount.

## On-Disk Format

### Inode change

```c
/* Current */
struct inode_disk {
    uint32_t id_uid;
    uint32_t id_gid;
    ...
};

/* New */
struct inode_disk_v2 {
    uint64_t id_uid;    /* reffs_id */
    uint64_t id_gid;    /* reffs_id */
    ...
};
```

Migration: when reading a v1 inode, zero-extend `uint32_t` to
`reffs_id(UNIX, 0, uid)`.  Write always uses v2 format.  The
inode version field (`id_version`) distinguishes.

### State files

| File | Content |
|------|---------|
| `identity_domains` | Domain table (index → name, type, LDAP URI) |
| `identity_map` | Mapping table (reffs_id ↔ aliases, names) |
| `identity_groups` | Group cache snapshot (optional, for fast restart) |

All use the existing write-temp/fsync/rename pattern.

## Configuration

```toml
[identity]
# Local UNIX namespace is always domain 0.
# Additional domains auto-discovered from authentication,
# or pre-configured here.

manage_gids = true          # server-side group lookup (override AUTH_SYS)
lookup_timeout_ms = 3000    # per-lookup timeout
cache_ttl_sec = 300         # group/mapping cache lifetime
circuit_breaker_threshold = 3
circuit_breaker_cooldown_sec = 30

# Synthetic UID range for unmapped SID/Kerberos users (NFSv3 compat)
synth_uid_min = 100000
synth_uid_max = 200000

[[identity.domain]]
name = "EXAMPLE.COM"
type = "krb5"               # or "ad", "ldap"
ldap_uri = "ldap://dc.example.com"
rfc2307 = true              # this domain has posixAccount/posixGroup
base_dn = "dc=example,dc=com"
```

## Implementation Order

1. **`reffs_id` type + macros** — `lib/include/reffs/identity.h`
2. **Domain table** — persist/load in state directory
3. **Mapping table** — in-memory lfht + on-disk persistence
4. **Inode widening** — `uint32_t` → `reffs_id` in struct inode
5. **AUTH_SYS path** — trivial: `REFFS_ID_MAKE(UNIX, 0, aup_uid)`
6. **RPCSEC_GSS path** — mapping lookup/create at auth time
7. **NFSv4 owner strings** — `user@domain` ↔ reffs_id conversion
8. **NFSv3 numeric path** — reffs_id → uint32_t for fattr3
9. **Group cache** — batch LDAP, TTL, background refresh
10. **Circuit breaker** — failure counting, cooldown
11. **RFC 2307bis** — LDAP posixAccount/posixGroup queries
12. **manage_gids** — server-side group augmentation for AUTH_SYS
