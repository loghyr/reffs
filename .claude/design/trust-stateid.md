<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# TRUST_STATEID / REVOKE_STATEID / BULK_REVOKE_STATEID Implementation

## Context

The protocol design is in
`~/Documents/ietf/flexfiles-v2/stateids.md`.  This document covers
the reffs implementation plan.

**Tight coupling** replaces the anonymous-stateid + fencing model
for NFSv4.2 DSes:

- MDS issues a layout stateid to the client (existing LAYOUTGET path).
- MDS fans out TRUST_STATEID to each DS, registering the stateid in
  a per-DS trust table.
- Client uses the real layout stateid (not the anonymous stateid) for
  CHUNK_WRITE / CHUNK_READ.
- DS validates every I/O request against the trust table.
- MDS revokes access via REVOKE_STATEID (per-file) or
  BULK_REVOKE_STATEID (per-client or all) instead of fencing.

`ffdv_tightly_coupled = true` in `ff_device_versions4` (GETDEVICEINFO
response) signals the client to use the layout stateid.

The three new ops are MDS-to-DS only.  Clients MUST NOT send them.
The DS enforces this by checking `nc_exchgid_flags` on the session's
owning client (see Phase 1 Step 0 -- this field does not yet exist
and must be added before the op handlers can be written).

### XDR modification approval

Adding TRUST_STATEID, REVOKE_STATEID, and BULK_REVOKE_STATEID to
`lib/xdr/nfsv42_xdr.x` is explicitly approved: the user is the
draft author (draft-haynes-nfsv4-flexfiles-v2) and requested this
implementation.  The upstream tracking reference is stateids.md in
the flexfiles-v2 repository.  Open Question 3 (op number range:
CHUNK range vs. separate MDS control-plane range) is unresolved;
op numbers are assigned as TBD constants and will be updated when
the draft finalises.

## Test Plan (TDD -- tests before implementation)

### New test file: `lib/nfs4/tests/trust_stateid_test.c`

#### Group A: trust table lifecycle

| Test | Intent |
|------|--------|
| `test_trust_alloc_free` | Alloc table, free table -- no leak |
| `test_trust_insert_find` | Insert entry, find by stateid.other -- entry returned with ref |
| `test_trust_insert_find_miss` | Find non-existent stateid -- NULL |
| `test_trust_insert_duplicate` | Second TRUST_STATEID for same stateid.other -- te_expire_ns updated atomically, entry still valid |
| `test_trust_revoke` | Insert, revoke, find -- NULL |
| `test_trust_expired` | Insert with te_expire_ns in the past; find -- entry returned (caller checks expiry); CHUNK hook returns NFS4ERR_BAD_STATEID |
| `test_trust_bulk_revoke_client` | Insert 3 entries for client A and 2 for client B, bulk_revoke(A), verify A entries gone, B intact |
| `test_trust_bulk_revoke_all` | Insert 5 entries for 2 clients, bulk_revoke(all-zeros), all gone |
| `test_trust_fini_drain` | Insert 2 entries, trust_table_fini() -- no UAF, no leak (ASAN/LSAN clean) |

#### Group B: TRUST_STATEID op handler (DS-side)

| Test | Intent |
|------|--------|
| `test_op_trust_stateid_ok` | Valid layout stateid on real FH, session has PNFS_MDS flag -- NFS4_OK, entry in table |
| `test_op_trust_stateid_no_fh` | No current FH -- NFS4ERR_NOFILEHANDLE |
| `test_op_trust_stateid_anon_rejected` | Anonymous stateid on real FH -- NFS4ERR_INVAL |
| `test_op_trust_stateid_dir_rejected` | Real layout stateid on directory FH -- NFS4ERR_INVAL |
| `test_op_trust_stateid_probe_response` | Anonymous stateid + PUTROOTFH -- NFS4ERR_INVAL (capability probe correct response) |
| `test_op_trust_stateid_not_from_mds` | Session without EXCHGID4_FLAG_USE_PNFS_MDS on nc_exchgid_flags -- NFS4ERR_PERM |
| `test_op_trust_stateid_update_expire` | Re-issue same stateid with new expire -- te_expire_ns updated, NFS4_OK |
| `test_op_trust_stateid_expire_validation` | tsa_expire.nseconds >= 1000000000 -- NFS4ERR_INVAL |

#### Group C: REVOKE_STATEID op handler (DS-side)

| Test | Intent |
|------|--------|
| `test_op_revoke_stateid_ok` | Revoke a trusted entry -- NFS4_OK, entry gone |
| `test_op_revoke_stateid_no_fh` | No current FH -- NFS4ERR_NOFILEHANDLE |
| `test_op_revoke_stateid_not_found` | Revoke unknown stateid -- NFS4_OK (idempotent) |
| `test_op_revoke_stateid_anon_rejected` | Anonymous stateid -- NFS4ERR_INVAL |

#### Group D: BULK_REVOKE_STATEID op handler (DS-side)

| Test | Intent |
|------|--------|
| `test_op_bulk_revoke_by_client` | Revoke all entries for one client -- NFS4_OK, entries gone |
| `test_op_bulk_revoke_all` | All-zeros clientid -- all entries gone |
| `test_op_bulk_revoke_empty` | No entries for client -- NFS4_OK |
| `test_op_bulk_revoke_not_from_mds` | Session without EXCHGID4_FLAG_USE_PNFS_MDS -- NFS4ERR_PERM |

#### Group E: CHUNK stateid validation hook

| Test | Intent |
|------|--------|
| `test_chunk_anon_stateid_allowed` | CHUNK_WRITE with anonymous stateid -- not checked against trust table |
| `test_chunk_trusted_stateid_allowed` | CHUNK_WRITE with stateid in trust table, not expired -- proceeds |
| `test_chunk_untrusted_stateid_rejected` | CHUNK_WRITE with stateid not in trust table -- NFS4ERR_BAD_STATEID |
| `test_chunk_expired_stateid_rejected` | CHUNK_WRITE with expired trust entry -- NFS4ERR_BAD_STATEID |
| `test_chunk_pending_stateid_delay` | CHUNK_WRITE with TRUST_PENDING entry -- NFS4ERR_DELAY |
| `test_chunk_principal_match` | Kerberos: stateid in table, principal matches -- allowed (requires compound_gss_principal accessor, Phase 1 deliverable) |
| `test_chunk_principal_mismatch` | Kerberos: stateid in table, wrong principal -- NFS4ERR_ACCESS |

### Test impact on existing tests

| File | Impact |
|------|--------|
| `lib/nfs4/tests/reflected_getattr_test.c` | PASS -- independent |
| `lib/nfs4/tests/delegation_lifecycle.c` | PASS -- independent |
| All `lib/nfs4/server/chunk.c` paths | PASS -- anonymous stateid path unchanged |
| All other `make check` tests | PASS |

## Data Structures

### Trust table entry

```c
/* lib/nfs4/include/nfs4/trust_stateid.h */

#define TRUST_PRINCIPAL_MAX 256

/* te_flags values */
#define TRUST_ACTIVE  (1u << 0) /* entry is valid for I/O */
#define TRUST_PENDING (1u << 1) /* MDS rebooted, awaiting re-issue */

struct trust_entry {
    struct cds_lfht_node te_ht_node;               /* MUST be first */
    struct urcu_ref      te_ref;
    uint8_t              te_other[NFS4_OTHER_SIZE]; /* hash key: stateid.other */
    uint64_t             te_ino;                    /* DS file inode */
    clientid4            te_clientid;               /* MDS client that issued */
    layoutiomode4        te_iomode;                 /* READ or RW */
    _Atomic uint64_t     te_expire_ns;              /* CLOCK_MONOTONIC ns */
    _Atomic uint32_t     te_flags;
    char                 te_principal[TRUST_PRINCIPAL_MAX]; /* "" or GSS name */
};
```

`te_expire_ns` is `_Atomic uint64_t` because the renewal path writes
it concurrently with CHUNK op readers.  Always read with
`atomic_load_explicit(..., memory_order_acquire)`.

`te_principal` is written once at insert time (before the entry is
hashed) and never updated after; renewal updates only `te_expire_ns`.
No synchronization needed for `te_principal` reads after insertion.

`trust_stateid_find()` returns expired entries -- the caller (CHUNK
hook) checks `te_expire_ns` and returns `NFS4ERR_BAD_STATEID` for
expired entries.  Callers always drop the ref via `trust_entry_put()`.

### Global trust table

One `cds_lfht` keyed by `te_other[NFS4_OTHER_SIZE]`
(XXH3_64bits on 12 bytes).  Initialized in `trust_table_init()`,
torn down in `trust_table_fini()`.  Volatile by design: DS crash
clears the table; re-layout path restores entries.

### Rule 6 lifecycle for trust entries

**Creation** (trust_stateid_register):
1. `calloc` the entry (urcu_ref starts at 1 -- the creation ref)
2. Initialize all fields before hashing
3. `cds_lfht_add` under `rcu_read_lock` (creation ref = "alive in table")

**Lookup** (trust_stateid_find):
1. `cds_lfht_lookup` under `rcu_read_lock`
2. `urcu_ref_get_unless_zero` to take a find ref
3. `rcu_read_unlock`
4. Caller uses entry, then calls `trust_entry_put()` to drop find ref

**Revoke** (trust_stateid_revoke):
1. `cds_lfht_lookup` under `rcu_read_lock`, take find ref
2. `cds_lfht_del` under `rcu_read_lock` (entry no longer findable)
3. `rcu_read_unlock`
4. `trust_entry_put(te)` -- drops find ref
5. `trust_entry_put(te)` -- drops creation ref → triggers release
6. Release callback: `cds_lfht_del` (idempotent for already-removed), then `call_rcu`

**Bulk revoke** (trust_stateid_bulk_revoke):
```
rcu_read_lock()
cds_lfht_first(ht, &iter)
while (node = cds_lfht_iter_get_node(&iter)) != NULL:
    te = container_of(node, ...)
    if te matches clientid:
        urcu_ref_get_unless_zero(&te->te_ref)  /* find ref */
        cds_lfht_next(ht, &iter)               /* advance BEFORE put */
        cds_lfht_del(ht, node)                 /* unfindable */
        rcu_read_unlock()
        trust_entry_put(te)                    /* drop find ref */
        trust_entry_put(te)                    /* drop creation ref */
        rcu_read_lock()
        cds_lfht_first(ht, &iter)             /* restart after unlock */
    else:
        cds_lfht_next(ht, &iter)
rcu_read_unlock()
```
(Restart from the beginning after each removal because advancing past
a removed node in liburcu-lfht is not safe after `rcu_read_unlock`.)

**Drain at fini** (trust_table_fini):
Same iterator pattern as bulk revoke with all-zeros clientid (matches
all entries).  After draining: `synchronize_rcu()` then
`cds_lfht_destroy()`.

### Per-dstore capability flag

```c
/* Add to struct dstore (lib/include/reffs/dstore.h): */
bool ds_tight_coupled; /* TRUST_STATEID probe returned NFS4ERR_INVAL */
```

### Client EXCHANGE_ID flags (new field)

```c
/* Add to struct nfs4_client (lib/nfs4/include/nfs4/client.h): */
uint32_t nc_exchgid_flags; /* eia_flags from client's EXCHANGE_ID */
```

Populated at EXCHANGE_ID time:
```c
/* In nfs4_op_exchange_id(), after nc is allocated/found: */
nc->nc_exchgid_flags = args->eia_flags;
```

The op handlers check:
```c
if (!(compound->c_nfs4_client->nc_exchgid_flags
        & EXCHGID4_FLAG_USE_PNFS_MDS)) {
    *status = NFS4ERR_PERM;
    return 0;
}
```

Note: the pre-existing NOT_NOW_BROWN_COW in `ds_session.c` (MDS
sends `EXCHGID4_FLAG_USE_PNFS_MDS` when it should send
`USE_NON_PNFS`) means the MDS control session will pass the check
correctly.  Fixing the flag is tracked in dstore-vtable-v2.md.

## Phase 1: DS-side (new ops + validation hook)

### Step 1.0: Add nc_exchgid_flags to struct nfs4_client

**File**: `lib/nfs4/include/nfs4/client.h` -- add field
**File**: `lib/nfs4/server/exchange_id.c` -- populate on EXCHANGE_ID

This unblocks the op handler guard in Step 1.3.

### Step 1.1: XDR definitions

**File**: `lib/xdr/nfsv42_xdr.x`

Add after the CHUNK ops block.  Use named constants for op numbers
(TBD pending draft finalisation, Open Question 3):

```xdr
const OP_TRUST_STATEID        = 89;  /* TBD: pending draft op range decision */
const OP_REVOKE_STATEID       = 90;
const OP_BULK_REVOKE_STATEID  = 91;

struct TRUST_STATEID4args {
    stateid4      tsa_layout_stateid;
    layoutiomode4 tsa_iomode;
    nfstime4      tsa_expire;
    utf8str_cs    tsa_principal;
};
struct TRUST_STATEID4res {
    nfsstat4      tsr_status;
};

struct REVOKE_STATEID4args {
    stateid4      rsa_layout_stateid;
};
struct REVOKE_STATEID4res {
    nfsstat4      rsr_status;
};

struct BULK_REVOKE_STATEID4args {
    clientid4     brsa_clientid;
};
struct BULK_REVOKE_STATEID4res {
    nfsstat4      brsr_status;
};
```

Add the three XDR structs to the `nfs_argop4` / `nfs_resop4` unions
and to `OP_MAX` bounds.  Regenerate C stubs with `rpcgen`.

### Step 1.2: Trust table implementation

**Files**: `lib/nfs4/server/trust_stateid.c` (NEW),
`lib/nfs4/include/nfs4/trust_stateid.h` (NEW)

Public API:

```c
int  trust_table_init(void);
void trust_table_fini(void);

/*
 * Register or update a trust entry.  If an entry already exists for
 * stateid.other, updates te_expire_ns atomically (renewal path).
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
int trust_stateid_register(const stateid4 *stid, uint64_t ino,
                           const clientid4 *cid, layoutiomode4 iomode,
                           uint64_t expire_ns, const char *principal);

/*
 * Remove a trust entry by stateid.other.  Idempotent (not found = OK).
 */
void trust_stateid_revoke(const stateid4 *stid);

/*
 * Remove all trust entries for the given clientid4.  If clientid is
 * all-zeros, removes all entries in the table.
 */
void trust_stateid_bulk_revoke(const clientid4 *cid);

/*
 * Lookup a trust entry.  Returns a reference the caller MUST drop
 * via trust_entry_put().  Returns NULL if not found.
 * Returns expired entries -- caller must check te_expire_ns.
 */
struct trust_entry *trust_stateid_find(const stateid4 *stid);
void trust_entry_put(struct trust_entry *te);
```

### Step 1.3: tsa_expire conversion (two-clock pattern)

When processing TRUST_STATEID, convert the wire `nfstime4 tsa_expire`
(wall clock, seconds since epoch) to a `CLOCK_MONOTONIC` nanosecond
deadline stored in `te_expire_ns`:

```c
/* Validate nfstime4 first (RFC 8881 nfstime4 overflow rule) */
if (args->tsa_expire.nseconds >= 1000000000U) {
    *status = NFS4ERR_INVAL;
    return 0;
}

/* Snapshot both clocks at the same instant */
struct timespec rt_now, mono_now;
clock_gettime(CLOCK_REALTIME,  &rt_now);
clock_gettime(CLOCK_MONOTONIC, &mono_now);

int64_t expire_wall_ns = (int64_t)args->tsa_expire.seconds * 1000000000LL
                         + (int64_t)args->tsa_expire.nseconds;
int64_t now_wall_ns    = (int64_t)rt_now.tv_sec * 1000000000LL
                         + (int64_t)rt_now.tv_nsec;
int64_t remaining_ns   = expire_wall_ns - now_wall_ns;
if (remaining_ns <= 0) {
    /* tsa_expire is already in the past -- reject */
    *status = NFS4ERR_INVAL;
    return 0;
}

uint64_t mono_now_ns  = (uint64_t)mono_now.tv_sec * 1000000000ULL
                        + (uint64_t)mono_now.tv_nsec;
uint64_t expire_ns    = mono_now_ns + (uint64_t)remaining_ns;
```

### Step 1.4: Op handlers

**File**: `lib/nfs4/server/trust_stateid.c`

**nfs4_op_trust_stateid**:
1. Check `compound->c_nfs4_client->nc_exchgid_flags & EXCHGID4_FLAG_USE_PNFS_MDS` -- else NFS4ERR_PERM
2. Check current FH set -- else NFS4ERR_NOFILEHANDLE
3. Check `tsa_layout_stateid` is not special (anonymous / read-bypass / current / invalid) -- else NFS4ERR_INVAL (this is the correct capability probe response)
4. Check current FH is not a directory -- else NFS4ERR_INVAL
5. Validate `tsa_expire.nseconds < 1e9` -- else NFS4ERR_INVAL
6. Two-clock conversion (Step 1.3)
7. Extract clientid from `nfs4_client_to_client(compound->c_nfs4_client)`
8. Call `trust_stateid_register()`
9. Return NFS4_OK

**nfs4_op_revoke_stateid**:
1. Check session flag (PNFS_MDS), FH set, stateid not special -- same guards as TRUST_STATEID
2. Check stateid.other matches te_ino from current FH inode (validates caller has a FH for the right file)
3. Call `trust_stateid_revoke()` -- idempotent
4. Return NFS4_OK

**nfs4_op_bulk_revoke_stateid**:
1. Check session flag (PNFS_MDS) -- else NFS4ERR_PERM
2. (No FH check: operates on entire table for client, not a per-file op)
3. Call `trust_stateid_bulk_revoke(&args->brsa_clientid)`
4. Return NFS4_OK

### Step 1.5: compound_gss_principal() accessor

**File**: `lib/nfs4/server/compound.c` (or compound.h inline)

```c
/*
 * Returns the caller's GSS display name if the compound was
 * authenticated via RPCSEC_GSS, NULL for AUTH_SYS.
 */
static inline const char *compound_gss_principal(const struct compound *c)
{
    /* compound->c_ap is the parsed AUTH_SYS credential.
     * For RPCSEC_GSS compounds, the GSS display name is stored
     * in compound->c_gss_name (or equivalent -- check actual field). */
    return c->c_gss_name; /* NULL if AUTH_SYS */
}
```

(Exact field name to be confirmed when reading the compound struct.)

### Step 1.6: CHUNK stateid validation hook

**File**: `lib/nfs4/server/chunk.c`

In `nfs4_op_chunk_write()` and `nfs4_op_chunk_read()`, after the
initial argument extraction and before the `i_attr_mutex` acquire:

```c
if (!stateid4_is_special(&args->cwa_stateid)) {
    struct trust_entry *te = trust_stateid_find(&args->cwa_stateid);
    if (!te) {
        *status = NFS4ERR_BAD_STATEID;
        return 0;
    }
    uint32_t flags = atomic_load_explicit(&te->te_flags,
                                          memory_order_acquire);
    if (flags & TRUST_PENDING) {
        trust_entry_put(te);
        *status = NFS4ERR_DELAY;
        return 0;
    }
    uint64_t now_ns = reffs_now_ns();
    uint64_t expire_ns = atomic_load_explicit(&te->te_expire_ns,
                                              memory_order_acquire);
    if (expire_ns <= now_ns) {
        trust_entry_put(te);
        *status = NFS4ERR_BAD_STATEID;
        return 0;
    }
    if (te->te_principal[0] != '\0') {
        const char *caller = compound_gss_principal(compound);
        if (!caller || strcmp(te->te_principal, caller) != 0) {
            trust_entry_put(te);
            *status = NFS4ERR_ACCESS;
            return 0;
        }
    }
    trust_entry_put(te);
}
```

This validation is synchronous (no async, no task_pause) -- it is a
hash-table lookup and a few comparisons, well within the CHUNK op's
existing synchronous dispatch path.

### Step 1.7: Op dispatch registration

**File**: `lib/nfs4/server/dispatch.c`

Add to the `op_table` array:

```c
[OP_TRUST_STATEID]       = nfs4_op_trust_stateid,
[OP_REVOKE_STATEID]      = nfs4_op_revoke_stateid,
[OP_BULK_REVOKE_STATEID] = nfs4_op_bulk_revoke_stateid,
```

Extend `OP_MAX` if the new op numbers exceed the current table size.

**File**: `lib/nfs4/include/nfs4/ops.h`

Declare the three new handler functions.

### Step 1.8: Unit tests

**File**: `lib/nfs4/tests/trust_stateid_test.c` (NEW)

All Groups A through E from the Test Plan above.
`setup()` calls `trust_table_init()` and `nfs4_protocol_register()`.
`teardown()` calls `trust_table_fini()` and `nfs4_protocol_unregister()`.

## Phase 2: MDS-side

### Step 2.1: dstore_ops extension

**File**: `lib/include/reffs/dstore_ops.h`

Add to `struct dstore_ops`:

```c
/*
 * probe_tight_coupling -- sends the capability probe:
 *   SEQUENCE + PUTROOTFH + TRUST_STATEID(anon_stid, READ, expire=0, "")
 *
 * Return values:
 *   0           -- NFS4ERR_INVAL received: tight coupling supported
 *   0           -- NFS4_OK received (DS bug): log anomaly, treat as supported
 *   -ENOTSUP    -- NFS4ERR_NOTSUPP: tight coupling not available
 *   -errno      -- other error: DS unavailable
 *
 * NULL for NFSv3 DSes (always -ENOTSUP).
 */
int (*probe_tight_coupling)(struct dstore *ds);

/* MDS-to-DS trust ops (NULL for NFSv3 DSes) */
int (*trust_stateid)(struct dstore *ds,
                     const uint8_t *fh, uint32_t fh_len,
                     const stateid4 *layout_stid, layoutiomode4 iomode,
                     uint64_t expire_ns, const char *principal);
int (*revoke_stateid)(struct dstore *ds,
                      const uint8_t *fh, uint32_t fh_len,
                      const stateid4 *layout_stid);
int (*bulk_revoke_stateid)(struct dstore *ds, const clientid4 *clientid);
```

For `probe_tight_coupling`, `NFS4_OK` from the DS is a bug.  The
implementation logs the anomaly and returns 0 (tight coupling
confirmed) per the protocol spec.

### Step 2.2: NFSv4 dstore vtable

**File**: `lib/nfs4/dstore/dstore_ops_nfsv4.c`

Implement all four using the existing `add_seq_putfh()` /
`send_and_check()` helpers.  The probe uses `PUTROOTFH` instead of
`PUTFH`.

### Step 2.3: Local dstore vtable

**File**: `lib/nfs4/dstore/dstore_ops_local.c`

`probe_tight_coupling`: return 0 directly (local DS always supports it).

`trust_stateid` / `revoke_stateid` / `bulk_revoke_stateid`: call the
trust table functions directly (same process, no RPC):
```c
trust_stateid_register(stid, ino, cid, iomode, expire_ns, principal);
trust_stateid_revoke(stid);
trust_stateid_bulk_revoke(clientid);
```

These are synchronous (no fanout threads needed for combined mode).

### Step 2.4: Capability probe at dstore session setup

**File**: `lib/nfs4/dstore/dstore.c`

In `dstore_alloc()` after NFSv4.2 session is established:

```c
if (do_mount && protocol == REFFS_DS_PROTO_NFSV4) {
    if (ds_session_create(ds) == 0 &&
        ds->ds_ops->probe_tight_coupling) {
        int r = ds->ds_ops->probe_tight_coupling(ds);
        ds->ds_tight_coupled = (r == 0);
        TRACE("dstore[%u]: tight coupling %s", id,
              ds->ds_tight_coupled ? "enabled" : "disabled");
    }
}
```

### Step 2.5: ffdv_tightly_coupled in GETDEVICEINFO

**File**: `lib/nfs4/server/layout.c`

Replace the hardcoded `ver.ffdv_tightly_coupled = false` (line ~145):

```c
struct dstore *ds = dstore_find(device_id_to_dstore_id(...));
ver.ffdv_tightly_coupled = ds && ds->ds_tight_coupled;
if (ds)
    dstore_put(ds);
```

### Step 2.6: TRUST_STATEID fan-out at LAYOUTGET

**File**: `lib/nfs4/dstore/dstore_fanout.h`

Add to the `fanout_op` enum: `FANOUT_TRUST_STATEID`.

Add to the `struct dstore_fanout` union (shared across all slots, not per-slot):

```c
union {
    uint64_t df_size;               /* FANOUT_TRUNCATE */
    struct { uint32_t df_fence_min; uint32_t df_fence_max; };
    struct {                        /* FANOUT_TRUST_STATEID */
        stateid4  df_ts_stateid;
        layoutiomode4 df_ts_iomode;
        uint64_t  df_ts_expire_ns;
        char      df_ts_principal[TRUST_PRINCIPAL_MAX]; /* copied before launch */
    };
};
```

`df_ts_principal` is copied from the XDR-decoded string before
`dstore_fanout_launch()` so the compound's XDR buffer can be freed
after the fanout is queued without racing with fanout threads.

**File**: `lib/nfs4/server/layout.c`

After runway pop, before returning layout to client:

```c
for each DS in mirror set:
    if ds->ds_tight_coupled:
        add to FANOUT_TRUST_STATEID fan-out
        
if fan-out has entries:
    set df_ts_stateid, df_ts_iomode, df_ts_expire_ns, df_ts_principal
    dstore_fanout_launch(df, task)
    task_pause(rt->rt_task)
    /* fanout result check on resume */
```

Failure handling per stateids.md:
- NFS4ERR_DELAY: retry before completing LAYOUTGET
- Unreachable: exclude DS from layout
- NFS4ERR_NOTSUPP: clear `ds_tight_coupled`, set `ffdv_tightly_coupled = false`
- All DSes fail: return NFS4ERR_LAYOUTTRYLATER

### Step 2.7: REVOKE_STATEID triggers

**File**: `lib/nfs4/server/layout.c` (LAYOUTERROR handler)

- NFS4ERR_ACCESS / NFS4ERR_PERM: for `ds_tight_coupled` DSes, fan out REVOKE_STATEID before fencing.
- NFS4ERR_BAD_STATEID (trust-gap recovery): retry TRUST_STATEID; if retry succeeds, return NFS4_OK for LAYOUTERROR.

**File**: `lib/nfs4/server/lease_reaper.c` (client lease expiry)

- On lease expiry: fan out BULK_REVOKE_STATEID(clientid) to all DSes.

### Step 2.8: TRUST_STATEID renewal

NOT_NOW_BROWN_COW: Track `tsa_expire` per layout segment and re-issue
TRUST_STATEID within `lease_period / 2` of expiry.

## Phase 3: ec_demo client

**File**: `lib/nfs4/client/layout_decode.c`
- Decode `ffdv_tightly_coupled` from `ff_device_versions4`, store on resolved mirror.

**File**: `lib/nfs4/client/chunk_io.c`
- If `mirror->m_tight_coupled`: use real `layout_stateid` in `cwa_stateid` / `cra_stateid`.
- On NFS4ERR_BAD_STATEID from DS: send LAYOUTERROR to MDS, retry (max 3, exponential backoff), then LAYOUTRETURN on failure.

## Implementation Order

1. Step 1.0: nc_exchgid_flags (prerequisite for security guard)
2. Steps 1.1 + 1.2: XDR + trust table data structure
3. Step 1.8 Group A: trust table unit tests (write first, then implement)
4. Steps 1.3-1.4: tsa_expire conversion + op handlers
5. Step 1.8 Groups B-D: op handler unit tests
6. Step 1.5: compound_gss_principal()
7. Step 1.6: CHUNK validation hook
8. Step 1.8 Group E: CHUNK hook unit tests
9. Step 1.7: dispatch table registration -- Phase 1 complete
10. Steps 2.1-2.4: dstore vtable + capability probe
11. Steps 2.5-2.6: GETDEVICEINFO flag + LAYOUTGET fanout -- Phase 2 core
12. Step 2.7: REVOKE triggers
13. Phase 3: ec_demo client

## Deferred / NOT_NOW_BROWN_COW

- TRUST_STATEID renewal (Step 2.8)
- MDS persistence of trust table (Open Question 4 in stateids.md)
- DS detection of MDS reboot via EXCHANGE_ID epoch change and auto-pending
  of trust entries
- Fix MDS-to-DS session flag (USE_NON_PNFS vs USE_PNFS_MDS, tracked in
  dstore-vtable-v2.md)
- Full Kerberos integration test (requires mini-KDC fixture)

## Lock continuity on REVOKE_STATEID (draft-coordinated)

The draft (see
`~/Documents/ietf/flexfiles-v2/draft-haynes-nfsv4-flexfiles-v2.md`,
sections `sec-chunk_guard_mds` and `sec-CHUNK_LOCK`) now requires
that when REVOKE_STATEID revokes a stateid that holds one or more
chunk locks on a DS, the locks MUST NOT be dropped.  The DS
atomically transfers lock ownership on each held range to the
MDS-escrow owner (reserved `chunk_guard4.cg_client_id ==
CHUNK_GUARD_CLIENT_ID_MDS == 0xFFFFFFFF`).  The lock remains
held until the client selected via CB_CHUNK_REPAIR issues
CHUNK_LOCK with the new `CHUNK_LOCK_FLAGS_ADOPT` flag, which
atomically transfers ownership from MDS-escrow to the repair
client.

Implementation implications when Step 2.7 is wired:

- `trust_stateid_revoke()` in `lib/nfs4/server/trust_stateid.c`
  MUST check whether the revoked stateid holds chunk locks (via
  `chunk_store_find_locks_by_owner()` -- new) and, for each
  held range, rewrite the chunk block's owner field to the
  MDS-escrow sentinel rather than clearing it.
- The chunk block's on-disk owner field (`cb_owner_id` etc.)
  MUST be updated under the same write-temp/fdatasync/rename
  boundary as the revocation itself, so the transfer is crash-safe.
- The `cg_client_id == 0xFFFFFFFF` value MUST be rejected by
  `nfs4_op_chunk_write` and `nfs4_op_chunk_lock` on any
  client-originated request (return NFS4ERR_INVAL); it is only
  producible by the DS internally.
- `nfs4_op_chunk_lock` MUST accept a new `cla_flags` field and
  the `CHUNK_LOCK_FLAGS_ADOPT` bit; the ADOPT path performs an
  atomic owner rewrite regardless of whether the prior owner is
  MDS-escrow (tight coupling) or a still-authenticated
  orphaned client (loose coupling).

These are tracked as work items for the same phase that wires
`trust_stateid_revoke()`.  Do not ship REVOKE_STATEID-with-locks
without the escrow semantics -- dropping a chunk lock is a
write-hole generator.

## Key Files

| File | Change |
|------|--------|
| `lib/nfs4/include/nfs4/client.h` | Add `nc_exchgid_flags` |
| `lib/nfs4/server/exchange_id.c` | Populate `nc_exchgid_flags` |
| `lib/xdr/nfsv42_xdr.x` | Add 3 new op structs + op numbers |
| `lib/nfs4/server/trust_stateid.c` | NEW -- trust table + op handlers |
| `lib/nfs4/include/nfs4/trust_stateid.h` | NEW -- public API |
| `lib/nfs4/server/chunk.c` | CHUNK_WRITE/READ stateid validation hook |
| `lib/nfs4/server/dispatch.c` | Register 3 new ops |
| `lib/nfs4/include/nfs4/ops.h` | Declare 3 new handler functions |
| `lib/include/reffs/dstore_ops.h` | Add 4 new vtable function ptrs |
| `lib/include/reffs/dstore.h` | Add `ds_tight_coupled` bool |
| `lib/nfs4/dstore/dstore_ops_nfsv4.c` | Implement 4 new vtable ops |
| `lib/nfs4/dstore/dstore_ops_local.c` | Direct-call vtable ops |
| `lib/nfs4/dstore/dstore.c` | Capability probe at session setup |
| `lib/nfs4/dstore/dstore_fanout.c` | FANOUT_TRUST_STATEID op |
| `lib/nfs4/dstore/dstore_fanout.h` | Enum + union fields |
| `lib/nfs4/server/layout.c` | ffdv_tightly_coupled + LAYOUTGET fan-out |
| `lib/nfs4/client/layout_decode.c` | Decode ffdv_tightly_coupled |
| `lib/nfs4/client/chunk_io.c` | Use real stateid when tight coupled |
| `lib/nfs4/tests/trust_stateid_test.c` | NEW -- all unit tests |
