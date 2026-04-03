<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# EID5f: Unconfirmed Client Must Not Destroy Confirmed Client

## Problem

pynfs test EID5f (st_exchange_id.testNoUpdate101) fails because
our EXCHANGE_ID immediately expires the old confirmed client when
a new verifier arrives from the same principal.  The old client's
sessions are destroyed, so SEQUENCE on the old session returns
NFS4ERR_BADSESSION instead of NFS4_OK.

RFC 8881 §18.35.4 Table 11 case 7 requires old sessions to remain
valid until CREATE_SESSION confirms the new client.

## Previous Attempt (reverted)

The predecessor approach (nc_predecessor link) was reverted because:
- Two incarnation records per slot broke incarnation_remove
  (removed both records instead of just the old one)
- Something in the interaction caused pynfs to crash at test 106
  with fd=-1 in select() — root cause not fully identified
- Added complexity: ref-counting, persistence API changes

## New Approach: Zombie Sessions

Instead of keeping the old client alive, expire the client but
preserve its sessions as **zombies** in the session hash table.

### Concept

```
EXCHANGE_ID(new verifier) → replace_client:
  1. Move old client's sessions to new client (re-parent)
  2. Mark moved sessions as ZOMBIE
  3. Expire old client (no sessions left on it)
  4. Allocate and return new unconfirmed client

SEQUENCE(old session) → session lookup succeeds (zombie):
  - Process normally
  - Renew the new client's lease

CREATE_SESSION(new client) → confirm:
  - Destroy all zombie sessions on this client
  - Mark client as confirmed

Lease expiry (unconfirmed client never confirmed):
  - nfs4_client_expire destroys all sessions including zombies
  - Natural cleanup, no special handling
```

### Why This Works

- **Single incarnation record per slot**: no persistence changes
- **No predecessor ref-counting**: old client is fully expired
- **Sessions stay in hash table**: SEQUENCE lookup works
- **No new client fields**: zombie state is on the session
- **Simple cleanup**: CREATE_SESSION destroys zombies, or
  lease expiry destroys them with the unconfirmed client

### Implementation

#### Step 1: Add NFS4_SESSION_IS_ZOMBIE flag

**File**: `lib/nfs4/include/nfs4/session.h`

```c
#define NFS4_SESSION_IS_ZOMBIE (1ULL << 2)
```

#### Step 2: Session re-parent helper

**File**: `lib/nfs4/server/session.c`

```c
void nfs4_session_reparent_for_replace(struct server_state *ss,
                                       struct nfs4_client *old_nc,
                                       struct nfs4_client *new_nc)
```

Iterates session hash table.  For each session belonging to
`old_nc`:
1. Take ref on `new_nc`, swap `ns_client`, drop ref on `old_nc`
2. Transfer session counts between clients
3. Set `NFS4_SESSION_IS_ZOMBIE` on the session

After this, `old_nc->nc_session_count == 0` and the old client
can be expired without destroying any sessions.

#### Step 3: replace_client uses re-parent

**File**: `lib/nfs4/server/client_persist.c`

```c
static struct nfs4_client *replace_client(...)
{
    nc = nfs4_client_alloc(...);

    /* Re-parent old sessions to new client as zombies. */
    nfs4_session_reparent_for_replace(ss, old_nc, nc);

    /* Old client has no sessions left — safe to expire. */
    nfs4_client_expire(ss, old_nc);

    /* Add incarnation record for new client. */
    ...
}
```

#### Step 4: CREATE_SESSION destroys zombies

**File**: `lib/nfs4/server/session.c`

After `nc->nc_confirmed = true`:

```c
nfs4_session_destroy_zombies(ss, nc);
```

Iterates sessions for `nc`, destroys any with zombie flag.

#### Step 5: No lease reaper changes

When unconfirmed client expires, `nfs4_session_destroy_for_client`
destroys all sessions including zombies.  Natural cleanup.

### Test Impact

| Existing test | Impact |
|---------------|--------|
| `nfs4_session.c` | PASS — no structural changes |
| `nfs4_client_persist.c` | PASS — no persistence changes |
| All other tests | PASS — zombie flag is additive |

### New Tests

| Test | Intent |
|------|--------|
| `test_zombie_session_survives_replace` | Old session works after EXCHANGE_ID with new verifier |
| `test_zombie_destroyed_on_confirm` | CREATE_SESSION destroys zombie sessions |

### Files to Change

| File | Change |
|------|--------|
| `lib/nfs4/include/nfs4/session.h` | Add `NFS4_SESSION_IS_ZOMBIE` |
| `lib/nfs4/server/session.c` | `reparent_for_replace`, `destroy_zombies` |
| `lib/nfs4/server/client_persist.c` | `replace_client`: reparent before expire |

### What This Doesn't Change

- No persistence API changes
- No new client struct fields
- No incarnation_remove signature change
- No RocksDB changes
- Single incarnation record per slot (always)

### Why This Won't Crash pynfs

The previous approach left two clients alive simultaneously,
which corrupted the incarnation table when both were removed.
This approach expires the old client immediately (single
incarnation record), just moves its sessions first.  The session
hash table operations are the same ones used by DESTROY_SESSION
and client expiry — no new patterns.

### RFC References

- RFC 8881 §18.35.4 Table 11 case 7
- RFC 8881 §18.36.3: CREATE_SESSION confirms the client
