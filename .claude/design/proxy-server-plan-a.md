<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Plan A: Production-wire identity contexts + PS-side PROXY_REGISTRATION call

## Why

Slices 6a / 6b-i / 6b-iii / 6b-iv landed the MDS-side
PROXY_REGISTRATION machinery: bit-flag validation, allowlist
check, squat-guard, mTLS auth-context support.  The handler
works correctly in unit tests by populating
`compound->c_gss_principal` / `compound->c_tls_fingerprint`
from test mocks.

In production, both fields are NULL because no one calls
PROXY_REGISTRATION (no PS-side caller exists) and even if they
did, the fields aren't populated from the live RPC stack.  Plan A
closes both gaps so PS registration works end-to-end against a
real MDS.

## Sub-slices

| Slice | Adds | Risk |
|-------|------|------|
| **A.i** (this slice) | Production wiring of `compound->c_gss_principal` from the RPC GSS context cache | medium -- MDS-side only; touches dispatch path |
| **A.ii** | Production wiring of `compound->c_tls_fingerprint` from the TLS handshake | medium -- crosses lib/io <-> lib/nfs4 boundary |
| **A.iii** | PS-side PROXY_REGISTRATION call at startup, using mds_session as a registered NFSv4 client | medium -- PS uses existing mds_session GSS auth path; just adds the PROXY_REGISTRATION compound |

A.i and A.ii are independent.  A.iii depends on at least one of
them succeeding (otherwise the MDS handler rejects with
`NFS4ERR_PERM` because both identity contexts are NULL).

This document plans **A.i** in detail and sketches A.ii / A.iii.

## A.i: GSS principal wiring (this slice)

### Goal

When an NFSv4.2 compound arrives on a session whose client is
authenticated via RPCSEC_GSS, populate
`compound->c_gss_principal` with the GSS display name of the
client principal.  Applies to all NFSv4 compounds, not only
PROXY_REGISTRATION; consumers are PROXY_REGISTRATION (slice 6b-i
allowlist), TRUST_STATEID's `te_principal` validation (slice
trust-stateid), and any future op gated on identity.

### Where the principal lives today

`lib/rpc/gss_context.c` maintains a hash table of established
GSS contexts keyed by handle.  Each entry holds
`gc_client_name` (GSS_NAME_T) plus a `gss_ctx_principal()`
helper that calls `gss_display_name()` to produce a printable
string.  The string is cached in `entry->gc_principal_str` after
first call (NULL-safe).

The RPC layer parses incoming RPCSEC_GSS credentials in
`lib/rpc/rpc.c`, looks up the GSS context by handle, and stores
the lookup result.  The context is reachable from the rpc_trans
via the GSS credential header, but currently not stashed where
the NFSv4 compound layer can find it.

### Where the principal needs to land

`compound_alloc()` in `lib/nfs4/server/compound.c` is the entry
point.  It already calls `rpc_cred_to_authunix_parms()` to
populate `c_ap`.  Add a parallel call that populates
`c_gss_principal` from the RPCSEC_GSS context, when the
compound's RPC credential is RPCSEC_GSS.

### Plan

1. **Helper in lib/rpc**: add `rpc_cred_get_gss_principal(const
   struct rpc_info *, const char **out)` -> 0 on success with
   *out set (string lifetime tied to the GSS context cache
   entry's lifetime, which outlives the compound), -ENOENT if
   credential is not RPCSEC_GSS, other -errno on lookup failure.
2. **Compound init**: in `compound_alloc()`, after
   `rpc_cred_to_authunix_parms()`, call
   `rpc_cred_get_gss_principal()`.  On success store the pointer
   on `compound->c_gss_principal` (still a `const char *`; no
   ownership transfer because the GSS context cache outlives the
   compound).
3. **Lifetime check**: a GSS context destroy
   (RPCSEC_GSS_DESTROY) frees the context entry and the
   principal string with it.  A compound with a cached
   `c_gss_principal` pointer outliving the GSS context would be
   a UAF.  Mitigations:
   - GSS context destroy is rare (per-client, on session
     teardown).
   - A compound's lifetime is bounded by its session, which is
     bounded by the GSS context.  If RPCSEC_GSS_DESTROY runs
     while the compound is in flight, the session is going down
     and the in-flight compound's reply will be discarded.
   - Conservative option: copy the principal into the compound
     (`compound->c_gss_principal_buf[]` + `c_gss_principal`
     pointer points into the buf).  ~256 bytes per compound.
     Pinned for slice A.i to avoid the lifetime concern.

### Implementation

```c
/* lib/include/reffs/rpc.h */
int rpc_cred_get_gss_principal(const struct rpc_info *info,
                               char *out_buf, size_t out_buf_len);

/* lib/rpc/rpc.c (or gss_context.c if more natural) */
int rpc_cred_get_gss_principal(const struct rpc_info *info,
                               char *out_buf, size_t out_buf_len)
{
    if (!info || info->ri_cred.rc_flavor != RPCSEC_GSS)
        return -ENOENT;
    /* The GSS context cache lookup happens in the RPC parse
     * path; the cached entry pointer is reachable via
     * info->... -- TBD field to be located.  Copy the
     * principal string out so the caller doesn't depend on
     * cache lifetime. */
    struct gss_ctx_entry *ctx = ri_gss_context(info);
    if (!ctx)
        return -ENOENT;
    char *principal = gss_ctx_principal(ctx);
    if (!principal)
        return -ENOENT;
    strncpy(out_buf, principal, out_buf_len - 1);
    out_buf[out_buf_len - 1] = '\0';
    return 0;
}
```

```c
/* lib/nfs4/include/nfs4/compound.h -- add a buffer field */
struct compound {
    /* ... existing fields ... */
    const char *c_gss_principal; /* points into c_gss_principal_buf */
    char        c_gss_principal_buf[REFFS_CONFIG_MAX_PRINCIPAL];
    /* ... */
};

/* lib/nfs4/server/compound.c -- compound_alloc() */
ret = rpc_cred_to_authunix_parms(&rt->rt_info, &compound->c_ap);
if (ret) { ... }

if (rpc_cred_get_gss_principal(&rt->rt_info,
                               compound->c_gss_principal_buf,
                               sizeof(compound->c_gss_principal_buf)) == 0) {
    compound->c_gss_principal = compound->c_gss_principal_buf;
}
```

### Tests

A.i is hard to unit-test without real GSS infrastructure.  Two
test layers:

1. **Helper unit test** (`lib/rpc/tests/`): mock a `struct
   rpc_info` with RPCSEC_GSS credential pointing at a fake
   gss_ctx_entry containing a known principal; verify
   `rpc_cred_get_gss_principal()` returns the principal string.
2. **Integration**: existing `nfs_krb5_test` exercises the GSS
   path end-to-end.  Add an assertion that
   `compound->c_gss_principal` is non-NULL during a Kerberos-
   authenticated compound.  Probably needs probe surface to
   inspect compound state, or a TRACE log line that the test
   greps.

For first slice, ship the helper unit test only; integration
test waits for A.iii (when there's a real consumer).

### Files changed

| File | Change |
|------|--------|
| `lib/include/reffs/rpc.h` | Declare `rpc_cred_get_gss_principal()` |
| `lib/rpc/rpc.c` (or gss_context.c) | Implement helper |
| `lib/include/reffs/rpc.h` | Add `ri_gss_context` helper if not already exposed |
| `lib/nfs4/include/nfs4/compound.h` | Add `c_gss_principal_buf[]` field |
| `lib/nfs4/server/compound.c` | Populate `c_gss_principal` in `compound_alloc()` |
| `lib/rpc/tests/...` | Helper unit test |

## A.ii sketch (next slice)

Mirror of A.i but for TLS:

- Helper: `tls_get_peer_cert_fingerprint(int fd, char *out_buf,
  size_t out_len)` -> SHA-256 of peer cert DER, formatted as
  colon-separated hex (95 chars + NUL = 96).
- Compound init: in `compound_alloc()`, if the connection has
  TLS enabled (`io_conn_is_tls_enabled()`), call the helper and
  populate `c_tls_fingerprint_buf[]` + `c_tls_fingerprint`.
- Tests: mini-CA fixture (already in tls_test.c) provides a
  test peer cert; verify the helper returns the expected
  fingerprint.

## A.iii sketch (next-next slice)

PS-side caller for PROXY_REGISTRATION:

- `lib/nfs4/client/mds_session.c` already opens the MDS session
  with optional GSS auth.  Add a method
  `mds_session_send_proxy_registration(ms, registration_id,
  flags)` that builds and sends the PROXY_REGISTRATION
  compound, parses the reply, surfaces `nfsstat4` to the
  caller.
- `src/reffsd.c` startup: after each PS listener's mds_session
  is established (existing path at ~line 671), call
  `mds_session_send_proxy_registration()`.  On success, log a
  TRACE line; on `NFS4ERR_PERM` (allowlist miss) or
  `NFS4ERR_DELAY` (squat blocked), log a LOG-level error and
  retry on a backoff loop (or just fail PS init -- TBD).
- `prr_registration_id`: needs to be persistent across PS
  process restarts so the squat-guard recognises a renewal.
  Store it in the PS state directory as a one-shot file written
  on first registration; reread on subsequent boots.  16 random
  bytes is plenty.

## Deferred from Plan A entirely

- Multi-PS coordination of registration_id (each PS chooses its
  own; collision space is 16 random bytes ~= 2^128).
- TLS-only deployments where the PS doesn't speak GSS at all
  (need A.ii + A.iii adapted to use fingerprint instead of
  principal).
- Lease renewal of the PROXY_REGISTRATION (today the squat-
  guard's lease is bumped by re-issuing PROXY_REGISTRATION;
  a future slice could add a renew-only path that doesn't
  re-do the allowlist check).
- LAYOUTGET-based migration work (Plan A only does the
  registration; the actual move/repair work is the slice 6c-x
  ladder in proxy-server-phase6c.md).
