<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# EID5f: Unconfirmed Client Must Not Destroy Confirmed Client

## Problem

pynfs test EID5f (st_exchange_id.testNoUpdate101) fails because
our EXCHANGE_ID immediately expires the old confirmed client when
a new verifier arrives from the same principal.

RFC 8881 §18.35.4 Table 11 case 7 (confirmed + different verifier +
same principal): the server MUST return a new clientid but MUST NOT
destroy the old confirmed client's state until the new client is
confirmed via CREATE_SESSION.

### Test sequence

```
1. c1 = EXCHANGE_ID(owner, verf1) → clientid1, confirmed
2. sess1 = CREATE_SESSION(clientid1) → confirmed
3. c2 = EXCHANGE_ID(owner, verf2) → clientid2, unconfirmed
4. COMPOUND(sess1, []) → should succeed (old client still alive)
5. sess2 = CREATE_SESSION(clientid2) → confirms c2, expires c1
6. COMPOUND(sess1, []) → NFS4ERR_BADSESSION
```

Step 4 fails because `replace_client()` in step 3 calls
`nfs4_client_expire()` immediately, destroying sess1.

## Current Flow

```
nfs4_client_alloc_or_find()
  → case 7: replace_client(old_nc, ...)
    → nfs4_client_expire(old_nc)  ← WRONG: destroys immediately
    → nfs4_client_alloc(new)
    → return new (unconfirmed)
```

## Proposed Fix

### 1. replace_client: keep old alive

Don't call `nfs4_client_expire` in `replace_client`.  Instead,
link the old client as the new client's **predecessor**:

```c
static struct nfs4_client *replace_client(...)
{
    /* ... allocate new client ... */

    /* Link old as predecessor — will be expired on confirm. */
    nc->nc_predecessor = old_nc;  /* takes the find ref */

    return nc;
}
```

The old client stays in the incarnation table and its sessions
remain valid.  The caller (`nfs4_op_exchange_id`) returns the
new client's clientid.

### 2. CREATE_SESSION: expire predecessor on confirm

When CREATE_SESSION confirms a client that has a predecessor,
expire the predecessor:

```c
/* In nfs4_op_create_session, after successful session creation: */
if (!nc->nc_confirmed) {
    nc->nc_confirmed = true;

    if (nc->nc_predecessor) {
        nfs4_client_expire(ss, nc->nc_predecessor);
        nfs4_client_put(nc->nc_predecessor);
        nc->nc_predecessor = NULL;
    }
}
```

### 3. Lease expiry: clean up unconfirmed clients

An unconfirmed client that is never confirmed (no CREATE_SESSION
within 1 lease period) must be cleaned up by the lease reaper.
The unconfirmed client holds a ref on the predecessor — if the
unconfirmed client expires, release the predecessor ref WITHOUT
expiring it (the predecessor is still a valid confirmed client).

### 4. nc_predecessor field

Add to `struct nfs4_client`:

```c
struct nfs4_client *nc_predecessor;  /* confirmed client to expire on confirm */
```

### 5. Second EXCHANGE_ID with yet another verifier

If a third EXCHANGE_ID arrives with verf3 while c2 (verf2) is
still unconfirmed:
- Expire c2 (unconfirmed)
- c2's predecessor (c1) is released (not expired — c1 is still confirmed)
- Create c3 with c1 as predecessor
- c1 stays alive until c3 is confirmed

This matches RFC 8881: an unconfirmed client can be replaced
freely; only confirmed clients are protected.

## Test Impact

### Existing tests affected

| Test | Impact |
|------|--------|
| `nfs4_session.c` | PASS — tests don't exercise the replace path |
| `nfs4_client_persist.c` | PASS — persistence tests create fresh clients |
| `delegation_lifecycle.c` | PASS — uses a single client |

### New tests needed

| Test | Intent |
|------|--------|
| `test_eid_replace_keeps_old_session` | EXCHANGE_ID(new verf) → old session still works → CREATE_SESSION → old session returns BADSESSION |
| `test_eid_replace_unconfirmed_expires_predecessor_ref` | EXCHANGE_ID(verf2) → EXCHANGE_ID(verf3) → c1 still alive, c2 expired |
| `test_eid_unconfirmed_lease_expiry` | Unconfirmed client expires by lease reaper → predecessor not expired |

These can go in `lib/nfs4/tests/nfs4_session.c` or a new
`eid_lifecycle.c`.

## Files to Change

| File | Change |
|------|--------|
| `lib/nfs4/include/nfs4/client.h` | Add `nc_predecessor` field |
| `lib/nfs4/server/client_persist.c` | `replace_client`: don't expire, link predecessor |
| `lib/nfs4/server/client.c` | `nfs4_client_find_by_owner`: find highest incarnation, not first |
| `lib/nfs4/server/session.c` | `nfs4_op_create_session`: expire predecessor on confirm |
| `lib/nfs4/server/lease_reaper.c` | Clean up unconfirmed client → release predecessor ref |

## RFC References

- RFC 8881 §18.35.4 Table 11: EXCHANGE_ID decision tree
- RFC 8881 §18.36.3: CREATE_SESSION confirms the client
- RFC 8881 §18.35.4 case 7: "The server ... MUST NOT destroy
  the confirmed client's state"

## Risks

- **Ref-counting**: nc_predecessor holds a ref on the old client.
  Must be released on: confirm (expire old), unconfirmed expiry
  (release without expire), and shutdown drain.
- **Incarnation table ordering**: Both old and new clients are in
  the incarnation table simultaneously with the same slot but
  different incarnation numbers.  `nfs4_client_find_by_owner`
  currently takes the **first** match — it must find the
  **highest incarnation** instead.  Otherwise a second
  EXCHANGE_ID finds the old confirmed client and loops.
  Fix: scan all incarnations for the slot, take max.
- **Persistence**: The predecessor link is transient (not persisted).
  On restart, all clients are unconfirmed and go through the
  reclaim path — no predecessor linkage needed.
