<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Proxy Server Phase 6b: Allowlist + Bypass + Squat-Guard + mTLS

## Context

Slice 6a (`d0fba1fd4f2c`, on origin/main) landed the bare bones of
`PROXY_REGISTRATION`: bit-flag validation, `EXCHGID4_FLAG_USE_NON_PNFS`
session check, AUTH_SYS rejection (via `c_gss_principal == NULL`), and
the `nc_is_registered_ps` flag on `struct nfs4_client`.  Three things
were explicitly deferred:

1. The flag is set but **never consumed** -- LOOKUP / PUTFH / GETFH
   still apply the normal export-rule filter to a registered PS.
2. There is **no allowlist** -- any session whose EXCHANGE_ID set
   `USE_NON_PNFS` and that authenticated via RPCSEC_GSS gets the
   privilege, regardless of which principal it is.
3. **TLS-only sessions are over-rejected** -- the slice 6a comment
   acknowledges the `c_gss_principal == NULL` test wrongly excludes
   future mTLS-PS deployments.

Phase 6b closes those gaps in four sub-slices, ordered by blast radius.

## Sub-slicing

| Slice | Adds | Touches | Risk |
|-------|------|---------|------|
| **6b-i** | `[[allowed_ps]]` config + GSS principal allowlist check on `PROXY_REGISTRATION` | `lib/include/reffs/settings.h`, `lib/config/config.c`, `lib/nfs4/server/proxy_registration.c`, `lib/utils/server.c` | low -- one new config block, one new compound check |
| **6b-ii** | Bypass wiring: `nc_is_registered_ps` consumed by `nfs4_check_wrongsec()` and audit-logged | `lib/nfs4/server/security.c`, `lib/nfs4/server/dispatch.c` | medium -- changes the security-check fast path |
| **6b-iii** | Squat-guard + lease tracking + renewal via matching `prr_registration_id` | `lib/nfs4/include/nfs4/client.h`, `lib/nfs4/server/proxy_registration.c` | medium -- new per-client state + lease semantics |
| **6b-iv** | mTLS auth-context: extend allowlist to allowlist client-cert fingerprints; relax `c_gss_principal == NULL` rejection when TLS auth is present | `lib/io/tls.c`, `lib/nfs4/include/nfs4/compound.h`, `lib/nfs4/server/proxy_registration.c` | high -- pulls TLS auth identity into the NFSv4 layer |

This document plans **6b-i** in detail (the next slice to ship) and
sketches 6b-ii / 6b-iii / 6b-iv enough to validate the allowlist
data structure won't need rework.

---

## 6b-i: Allowlist + RPCSEC_GSS principal check

### Goal

Reject `PROXY_REGISTRATION` from any GSS-authenticated session whose
principal is not on the MDS's `[[allowed_ps]]` list.  Continue to
reject AUTH_SYS sessions (slice 6a behavior, unchanged).  TLS-only
identity check stays deferred to 6b-iv.

### TOML config

New block, repeatable, on the MDS:

```toml
[[allowed_ps]]
principal = "host/ps.example.com@REALM"

[[allowed_ps]]
principal = "host/ps2.example.com@REALM"
```

`principal` is a Kerberos display name, matched as an exact string
against `compound->c_gss_principal`.  Empty list (zero blocks)
means: every GSS-authenticated PROXY_REGISTRATION is rejected
(`NFS4ERR_PERM`).  This is the secure default -- no one is trusted
unless explicitly named.

### Data structures

```c
/* lib/include/reffs/settings.h */

#define REFFS_CONFIG_MAX_ALLOWED_PS    8
#define REFFS_CONFIG_MAX_PRINCIPAL     256

struct reffs_allowed_ps_config {
    char principal[REFFS_CONFIG_MAX_PRINCIPAL];
};

struct reffs_config {
    /* ...existing fields... */

    /* [[allowed_ps]] -- MDS-only allowlist for PROXY_REGISTRATION */
    struct reffs_allowed_ps_config allowed_ps[REFFS_CONFIG_MAX_ALLOWED_PS];
    unsigned int nallowed_ps;
};
```

The runtime mirror lives on `struct server_state` so the proxy_
registration handler can read it without touching the config struct
directly:

```c
/* lib/include/reffs/server.h */

struct server_state {
    /* ...existing fields... */

    /* Snapshot of [[allowed_ps]] from config; immutable after init. */
    char ss_allowed_ps[REFFS_CONFIG_MAX_ALLOWED_PS]
                     [REFFS_CONFIG_MAX_PRINCIPAL];
    unsigned int ss_nallowed_ps;
};
```

`reffsd.c main()` populates `ss_allowed_ps` from `cfg.allowed_ps`
right after `server_state_init()` returns.  No reload-at-runtime
support -- changing the allowlist requires a server restart.  This
matches existing patterns for security-sensitive config (e.g.,
`[backend] type` is also restart-only).

### Lookup helper

```c
/* lib/nfs4/server/proxy_registration.c (static, file-scope) */

/*
 * Returns true if `principal` exactly matches any entry in the
 * server-state allowlist.  Empty allowlist -> always false (deny).
 */
static bool ps_principal_allowed(const struct server_state *ss,
                                 const char *principal)
{
    if (!principal || principal[0] == '\0')
        return false;
    for (unsigned int i = 0; i < ss->ss_nallowed_ps; i++) {
        if (strcmp(ss->ss_allowed_ps[i], principal) == 0)
            return true;
    }
    return false;
}
```

### Handler change

After the existing `c_gss_principal == NULL` check (slice 6a, line
96-99 of `proxy_registration.c`), add:

```c
struct server_state *ss = server_state_get();
if (!ss || !ps_principal_allowed(ss, compound->c_gss_principal)) {
    LOG("PROXY_REGISTRATION: principal '%s' not on allowlist",
        compound->c_gss_principal ? compound->c_gss_principal : "(null)");
    *status = NFS4ERR_PERM;
    return 0;
}
```

The LOG (not TRACE) is intentional -- a rejected registration is an
operator-actionable event (either misconfiguration or attempted squat
from an unprivileged identity).

### Tests (TDD -- write first)

Extend `lib/nfs4/tests/proxy_registration_test.c`:

| Test | Intent |
|------|--------|
| `test_proxy_registration_reject_not_allowlisted` | GSS-authenticated session with principal NOT on the test allowlist -> `NFS4ERR_PERM`, `nc_is_registered_ps` stays false |
| `test_proxy_registration_accept_allowlisted` | GSS principal that matches an allowlist entry -> `NFS4_OK`, flag set |
| `test_proxy_registration_reject_empty_allowlist` | Allowlist has zero entries -> any GSS principal rejected with `NFS4ERR_PERM` |
| `test_proxy_registration_principal_exact_match` | Allowlist entry is `"host/ps.example.com@REALM"`; principal `"host/ps.example.com@OTHER"` -> rejected (exact-match, not realm-match) |

The existing `test_proxy_registration_success` test must be updated:
its principal `"host/ps.example.com@REALM"` must be added to the
test's allowlist via a small `pr_allowlist_set()` helper (added to
the test file -- writes directly into the singleton server_state).

The test fixture must reset `ss_nallowed_ps = 0` in `teardown()` so
allowlist state from one test doesn't bleed into the next.

### Test impact on existing tests

| File | Impact |
|------|--------|
| `lib/nfs4/tests/proxy_registration_test.c` | **UPDATE** -- success test gains one allowlist setup line; no semantic change to other tests |
| `lib/config/tests/config_test.c` | **EXTEND** -- add a test that parses `[[allowed_ps]]` correctly (count, principal, empty case) |
| All other `make check` tests | **PASS** -- additive only |

### Files changed

| File | Change |
|------|--------|
| `lib/include/reffs/settings.h` | Add `reffs_allowed_ps_config` struct, `REFFS_CONFIG_MAX_ALLOWED_PS` constant, `allowed_ps[]` + `nallowed_ps` fields on `reffs_config` |
| `lib/config/config.c` | Parse `[[allowed_ps]]` from TOML (mirrors existing `[[proxy_mds]]` parser) |
| `lib/utils/server.h` | Add `ss_allowed_ps` + `ss_nallowed_ps` fields on `server_state` |
| `lib/utils/server.c` | Initialize allowlist fields to zero in `server_state_init()`; add `server_state_set_allowed_ps()` setter called from `reffsd.c` |
| `src/reffsd.c` | After `server_state_init()`, copy `cfg.allowed_ps[]` into the server state via the new setter |
| `lib/nfs4/server/proxy_registration.c` | Add `ps_principal_allowed()` helper + allowlist check in handler |
| `lib/nfs4/tests/proxy_registration_test.c` | Add 4 new tests + `pr_allowlist_set()` helper + teardown reset |
| `lib/config/tests/config_test.c` | Add `[[allowed_ps]]` parse test |

### Security model

- **Default deny**: zero entries means zero PROXY_REGISTRATIONs accepted.
- **Exact match**: principal string is compared verbatim.  No realm
  fuzz, no DNS canonicalization, no glob.  An allowlist entry binds
  to one Kerberos identity.
- **Restart-only changes**: avoids racing config reload with an
  in-flight registration; an admin who needs to remove an attacker
  restarts reffsd, which also invalidates the attacker's session.
- **LOG on reject**: every rejected PROXY_REGISTRATION fires a
  LOG-level line.  Operators tail the log and notice repeated
  rejects from one principal, which is the primary attack signal.

### RFC references

- RFC 8881 §13.1: `EXCHGID4_FLAG_USE_NON_PNFS` semantics (slice 6a
  already enforces this; 6b-i layers identity on top).
- draft-haynes-nfsv4-flexfiles-v2-data-mover §sec-security: mandate
  that MDS<->PS sessions use RPCSEC_GSS or RPC-over-TLS.
- RFC 8178 §4.4.3: `prr_flags` reserved-bit rejection (already done
  in slice 6a; no change here).

---

## 6b-ii sketch (next-after-next)

Add to `nfs4_check_wrongsec()`:

```c
/* Bypass export-rule filter when the calling session belongs to a
 * registered PS.  PUTROOTFH / PUTFH / LOOKUP / LOOKUPP / GETFH /
 * SEQUENCE only -- every other op still applies the normal check
 * against the forwarded client credentials. */
if (compound->c_nfs4_client &&
    compound->c_nfs4_client->nc_is_registered_ps &&
    op_is_namespace_discovery(compound->c_curr_op)) {
    TRACE("PS-bypass: principal=%s op=%s path=%s",
          compound->c_gss_principal,
          op_name(compound->c_curr_op),
          ...);
    return NFS4_OK;
}
```

The audit log uses TRACE (not LOG) because a healthy PS triggers
this on every namespace-discovery compound -- LOG would flood the
operator log.  The trace category is a new
`REFFS_TRACE_CAT_PS_BYPASS` so operators can selectively enable it.

Tests: `test_registered_ps_bypasses_export_filter_on_lookup`,
`test_registered_ps_does_not_bypass_root_squash` (proves the bypass
is namespace-discovery-only), `test_registered_ps_lookup_audit_logged`.

## 6b-iii: squat-guard + lease + renewal (this slice)

### Goal

Refuse a PROXY_REGISTRATION from a different identity-pair (principal,
registration_id) while an existing PS registration with the same
principal still holds a valid lease, returning `NFS4ERR_DELAY`.
Distinguish a *renewal* (same `prr_registration_id`) from a *squat*
(different id) and let renewal succeed without waiting for the prior
lease to expire.

### Threat model

A PS opens a session, sends `PROXY_REGISTRATION`, gets the privilege.
A second peer opens its own session, authenticates with the same GSS
principal (compromise of the keytab, or a benign restart of the PS),
and sends a fresh `PROXY_REGISTRATION`.  Without the squat-guard the
second registration silently succeeds and the first PS keeps thinking
it owns the relationship.  With the squat-guard the second
registration fails fast with `NFS4ERR_DELAY` until the first lease
expires (default: NFSv4 lease period, ~90s).

### Renewal vs squat

The data-mover draft requires that a PS pick a `prr_registration_id`
once at startup and reuse it across reconnects.  A reconnecting PS
sends the *same* id; an attacker (or a restarted PS that lost its
state) sends a different id.  Same id == renewal == always allowed.
Different id while the prior lease is valid == squat == DELAY.

### Per-client state added

Add to `struct nfs4_client`:

```c
/* PROXY_REGISTRATION state -- only meaningful when
 * nc_is_registered_ps == true.  The principal is captured at
 * registration time so the squat-guard can look up "any other
 * registered client with the same identity".  The lease is in
 * CLOCK_MONOTONIC ns per the dual-clock strategy in standards.md;
 * concurrent renewal writers and scan readers require _Atomic.
 */
char nc_ps_principal[REFFS_CONFIG_MAX_PRINCIPAL];
char nc_ps_registration_id[PROXY_REGISTRATION_ID_MAX];
uint32_t nc_ps_registration_id_len;
_Atomic uint64_t nc_ps_lease_expire_ns;
```

`REFFS_CONFIG_MAX_PRINCIPAL` already exists from slice 6b-i;
`PROXY_REGISTRATION_ID_MAX` (= 64) is the XDR-pinned limit.

### Scan helper

```c
/*
 * nfs4_client_find_other_registered_ps -- scan for another in-memory
 * client (not `self`) holding nc_is_registered_ps with the given
 * GSS principal.  Returns ref-bumped match or NULL.  Caller drops
 * the ref via nfs4_client_put().  Does NOT filter by lease expiry --
 * the caller decides squat vs expired-treat-as-fresh.
 */
struct nfs4_client *
nfs4_client_find_other_registered_ps(struct server_state *ss,
                                     const struct nfs4_client *self,
                                     const char *principal);
```

Implementation iterates `ss->ss_client_ht` under `rcu_read_lock`,
takes a `urcu_ref_get_unless_zero` on each candidate, exact-matches
`nc_ps_principal`, returns the first hit.  Rule 6 lifecycle.

### Handler change

After the slice 6b-i allowlist check (which sets up that the principal
is trusted), but before setting `nc_is_registered_ps`:

```c
struct nfs4_client *self = compound->c_nfs4_client;
struct nfs4_client *other = nfs4_client_find_other_registered_ps(
    compound->c_server_state, self, compound->c_gss_principal);

if (other) {
    uint64_t now_ns = reffs_now_ns();
    uint64_t expire_ns = atomic_load_explicit(
        &other->nc_ps_lease_expire_ns, memory_order_acquire);
    bool same_id =
        other->nc_ps_registration_id_len == args->prr_registration_id.len &&
        memcmp(other->nc_ps_registration_id,
               args->prr_registration_id.val,
               other->nc_ps_registration_id_len) == 0;

    if (expire_ns > now_ns && !same_id) {
        LOG("PROXY_REGISTRATION: squat blocked -- principal '%s' "
            "already registered with different registration_id",
            compound->c_gss_principal);
        nfs4_client_put(other);
        *status = NFS4ERR_DELAY;
        return 0;
    }
    /* Same id (renewal) OR expired (treat as fresh): drop the
     * other ref and fall through to register self normally.
     * Renewal also refreshes the prior client's lease so the still-
     * connected old session does not lose its privilege between
     * "renewed on new session" and "old session expires". */
    if (expire_ns > now_ns && same_id) {
        atomic_store_explicit(&other->nc_ps_lease_expire_ns,
            now_ns + lease_period_ns,
            memory_order_release);
    }
    nfs4_client_put(other);
}

/* Capture identity + lease on self */
strncpy(self->nc_ps_principal, compound->c_gss_principal,
        REFFS_CONFIG_MAX_PRINCIPAL - 1);
self->nc_ps_principal[REFFS_CONFIG_MAX_PRINCIPAL - 1] = '\0';
if (args->prr_registration_id.len > 0)
    memcpy(self->nc_ps_registration_id,
           args->prr_registration_id.val,
           args->prr_registration_id.len);
self->nc_ps_registration_id_len = args->prr_registration_id.len;
atomic_store_explicit(&self->nc_ps_lease_expire_ns,
    reffs_now_ns() + lease_period_ns,
    memory_order_release);
self->nc_is_registered_ps = true;
```

`lease_period_ns` is `(uint64_t)server_lease_time(ss) * 1000000000ULL`
-- reuse the configured NFSv4 lease (45s default).

### Tests

Extend `proxy_registration_test.c`:

| Test | Intent |
|------|--------|
| `test_proxy_registration_renewal_same_id` | Two PROXY_REGISTRATION calls, same principal, same registration_id, both succeed; the first client's lease expire is bumped on the renewal |
| `test_proxy_registration_squat_blocked` | Two calls, same principal, different registration_id, second returns NFS4ERR_DELAY |
| `test_proxy_registration_after_expiry_succeeds` | Two calls, same principal, different ids; between them artificially expire the first client's lease; second succeeds |

The first ctx's pr_free MUST be called AFTER the second registration
runs, otherwise the first client is destroyed and the squat-guard
finds nothing to compare against.  Use a small `pr_free_after` helper
or just stash the first ctx and free in reverse order.

### Test impact

| File | Impact |
|------|--------|
| `lib/nfs4/tests/proxy_registration_test.c` | EXTEND -- 3 new tests, no change to existing |
| All other tests | PASS (additive) |

### Files changed (6b-iii)

| File | Change |
|------|--------|
| `lib/nfs4/include/nfs4/client.h` | New struct fields (4: principal, registration_id, len, _Atomic expire) |
| `lib/nfs4/include/nfs4/client.h` | Declare `nfs4_client_find_other_registered_ps()` |
| `lib/nfs4/server/client.c` | Implement the scan helper |
| `lib/nfs4/server/proxy_registration.c` | Squat-guard + lease assignment in handler |
| `lib/nfs4/tests/proxy_registration_test.c` | 3 new tests |

### Deferred

- Renewal across an MDS restart (would require persisting registered-PS
  state to the namespace DB).  Acceptable: an MDS restart kicks every
  PS through fresh registration, so the lease starts over.
- Server-driven lease expiry sweep (no need: the squat check looks at
  expire_ns lazily, so an expired-but-still-in-table client just looks
  unregistered to the next squat probe).

---

## 6b-iii sketch

Add per-client lease tracking:

```c
struct nfs4_client {
    /* ...existing fields... */

    /* PROXY_REGISTRATION state (only meaningful when
     * nc_is_registered_ps == true) */
    char     nc_ps_registration_id[PROXY_REGISTRATION_ID_MAX];
    uint32_t nc_ps_registration_id_len;
    uint64_t nc_ps_lease_expire_ns; /* CLOCK_MONOTONIC */
};
```

On `PROXY_REGISTRATION`:

1. Look for an existing client (by GSS principal) with
   `nc_is_registered_ps == true`.
2. If found AND `nc_ps_lease_expire_ns > now_ns`:
   - If `prr_registration_id` matches: refresh lease, return NFS4_OK.
   - Else: return `NFS4ERR_DELAY` + LOG the squat attempt.
3. If found but lease expired: treat as fresh.
4. If not found: fresh registration, lease = now + lease_period.

Lease expiry uses `CLOCK_MONOTONIC` per the existing dual-clock
strategy in standards.md.

Tests: `test_proxy_registration_rejects_squat`,
`test_proxy_registration_accepts_renewal`.

## 6b-iv sketch

The hardest of the four because it crosses the io / nfs4 layer
boundary.  When a TLS handshake completes, the client cert
fingerprint (SHA-256 hex) needs to flow into the `compound` struct
so the PROXY_REGISTRATION handler can match it against a TLS
allowlist:

```toml
[[allowed_ps]]
tls_cert_fingerprint = "AB:CD:..."
```

Today the `compound` struct has `c_gss_principal` but no
`c_tls_fingerprint`.  Adding it requires:

- `struct rpc_trans` exposes the SSL session.
- A new helper `tls_get_peer_cert_fingerprint()` in `lib/io/tls.c`.
- `nfs4_op_proxy_registration` consults `c_tls_fingerprint` when
  `c_gss_principal == NULL`.
- The `c_gss_principal == NULL` rejection from slice 6a softens to
  "reject only if BOTH `c_gss_principal == NULL` AND
  `c_tls_fingerprint == NULL`".

Defer the design detail to its own document at slice-6b-iv start
time.

---

## Deferred from 6b entirely

- Multi-MDS PS (`[[allowed_ps]]` does not need an MDS-id field
  because one PS proxies one MDS).
- Hot-reload of `[[allowed_ps]]` (would require a registration-time
  vs config-snapshot race story).
- Allowlist via probe protocol op (admin tool would be cleaner than
  TOML; deferred until probe-server-management infrastructure
  catches up).
- RPCSEC_GSSv3 structured-privilege assertions (the long-term path
  to credential forwarding under Kerberos; orthogonal to identity
  check).
- LOG aggregation -- repeated rejects from the same principal
  produce one LOG line per attempt; rate-limiting is a follow-up.

## Verification

1. `make -j$(nproc)` -- zero errors, zero warnings.
2. `make check` -- all existing + new tests pass; LSan-clean on Linux.
3. `make -f Makefile.reffs license` + `style` -- clean.
4. Reviewer agent on the slice diff -- BLOCKER-free.
5. Push to `origin/ps-phase6b-i` topic branch, verify on dreamer
   worktree, then ff-merge to `origin/main`.
