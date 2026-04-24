<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Proxy Server (PS) Implementation

## Context

`draft-haynes-nfsv4-flexfiles-v2-data-mover` defines a Proxy Server
role: a peer of the MDS and DSes that carries out whole-file
operations (move, repair) and codec translation on behalf of clients
that cannot speak the file's native codec.  The protocol surface is
four new callback ops (`CB_PROXY_MOVE`, `CB_PROXY_REPAIR`,
`CB_PROXY_STATUS`, `CB_PROXY_CANCEL`), two new fore-channel ops
(`PROXY_REGISTRATION`, `PROXY_PROGRESS`), a new layout flag
(`FFV2_DS_FLAGS_PROXY`), and a set of credential-forwarding rules.

This document plans the reffs implementation.  The client library
half of the PS already exists in `ec_demo`: MDS session, COMPOUND
builder, layout decode, CHUNK I/O, and codecs.  The new work is the
server surface that wraps that library and the registration
mechanism that binds it to an MDS.

The reffs implementation doubles as a proof-of-concept for the
draft: punching sharp numbers into benchmark reports, exercising
the protocol end-to-end against a real MDS and DSes, and surfacing
any corners the draft under-specifies.

## Non-goals (initial scope)

- RPCSEC_GSSv3 structured privilege assertion for credential
  forwarding under Kerberos.  The initial implementation uses
  AUTH_SYS only.
- Multi-MDS PS.  One PS proxies exactly one MDS.  The current HS
  equivalent has the same constraint.
- Delta-journaling moves.  `CB_PROXY_MOVE` in the first iteration
  is quiesced or dual-write only, matching the draft's scope.
- Partial-range moves.  Whole-file only.
- Inter-server COPY via COPY_NOTIFY.
- Automated load balancing across registered PSes.  The MDS
  picks one PS per operation.

## Key decisions

### Architecture: two listeners on one reffs

Alternatives considered:

| Option | Pro | Con |
|--------|-----|-----|
| FH-flag-marks-proxied-SB, one listener | No new listener plumbing | FH rewrite everywhere; single MDS forever; no per-client namespace |
| Separate reffs instances for PS and DS | Clean FH story | Co-resident DS unreachable except via loopback RPC; throws away the combined-deployment win |
| Two listeners, one process (chosen) | Clean SB-id scoping per listener; short-circuit to local DS works; composes with existing `role` config | New listener plumbing in `lib/rpc`; per-compound listener tag |

The PS surface binds a configurable port (default 4098) that serves
a proxy namespace.  SB ids are scoped to the listener — `sb_id == 1`
on `:2049` is the local root; `sb_id == 1` on `:4098` is the
proxied root, a different address space.  An FH minted on one
listener presented on the other fails the SB lookup and returns
`NFS4ERR_STALE`.

### Config schema is additive

The existing `role` enum is unchanged.  A new `[[proxy_mds]]`
config block is independent of `role`:

```toml
[[proxy_mds]]
address   = "10.1.1.5"
port      = 4098                # PS listener port
mds_port  = 2049                # MDS NFS port (default 2049)
mds_probe = 20490               # MDS probe port (default 20490)
```

Composition follows:

| `role` | `[[proxy_mds]]` | Result |
|--------|-----------------|--------|
| absent | present | Pure PS |
| `"ds"` | present | Combined DS + PS (the demo default) |
| `"mds"` | absent | Pure MDS (today) |
| `"ds"` | absent | Pure DS (today) |

One PS proxies one MDS; `[[proxy_mds]]` appears at most once.
Listener port is fixed per PS instance.

### Proxy SB = new backend composition

Add one axis value on the data side:

```c
enum reffs_data_type {
    REFFS_DATA_RAM    = 0,
    REFFS_DATA_POSIX  = 1,
    REFFS_DATA_PROXY  = 2,  /* NEW */
};
```

A proxy SB is composed as `md = RAM` + `data = PROXY`:

- Metadata (inode attrs, directory entries, symlink targets) lives
  in RAM as a cache of values pulled from the MDS.
- Data ops (`db_read`, `db_write`) redirect into the `ec_pipeline`
  library (see the refactor section below) which does LAYOUTGET +
  CHUNK I/O + codec transform.

This slots into the existing `reffs_backend_compose()` scaffold.
No new discriminant in `reffs_storage_type` on the wire; a proxy
SB is distinguished by its `ops` pointer (composed
`md=RAM, data=PROXY`) plus a new `sb_proxy_mds` field linking it
back to the per-PS MDS state.

### Privilege model

The PS's MDS-facing session starts unprivileged.  `EXCHANGE_ID` +
`CREATE_SESSION` just set up the session.  The first fore-channel
op is `PROXY_REGISTRATION`, which is authenticated by whatever the
deployment provides (GSS machine principal preferred; dedicated
AUTH_SYS uid on an allowlist acceptable for AUTH_SYS-only
deployments).  The MDS has a config-driven allowlist of PS
identities.

On successful registration, the MDS marks the session's client
record as `nc_is_registered_ps = true`.  That flag grants **one**
narrow privilege: LOOKUP / LOOKUPP / PUTFH / PUTROOTFH / GETFH /
SEQUENCE on this session bypass export-rule / ACL / perm checks,
regardless of the client credentials on the compound.  This lets
the PS walk the namespace to discover FHs.

Every other op applies **normal** authorization using the RPC
credentials on the compound.  The PS forwards the end-client's
credentials on proxied I/O (LAYOUTGET, GETATTR, OPEN, etc.) and
the MDS authorizes against those credentials exactly as if the
client were connected directly.  root_squash applies in the
normal way based on the client-rule for the PS's source address
and the forwarded credentials.

This model does not require the MDS to trust the PS's credentials
at all -- only to trust that the PS forwards the client's
credentials truthfully.  The PS's only escalated capability is
"can see the namespace shape"; it has no escalated data access.

**Audit logging.**  Every LOOKUP / LOOKUPP / GETFH / PUTFH
compound on a registered-PS session that benefits from the
bypass (i.e., would have returned `ACCES` / `PERM` under the
PS's own RPC credentials absent the privilege) MUST be recorded
at `TRACE` level.  The log line includes the compound's
operation list and the path being traversed.  This makes the
namespace-shape disclosure reviewable after the fact; the
privilege is deliberate but it is a real disclosure of names
that the PS's source IP would otherwise not see, and the audit
trail is the only way to spot unexpected traversal patterns
(e.g., a rogue PS codebase enumerating every export on the
MDS).

### Export-rule mirror via probe protocol

The PS does its own per-client export-rule enforcement locally so
that bad requests are rejected before they hit the MDS.  To do
this it needs a copy of each proxied SB's client-rule list.

Rather than admin-curated TOML (which is throwaway code that
doesn't scale beyond the demo), extend the probe protocol:

- New op `SB_GET_CLIENT_RULES` (probe op 27) -- takes `sb_id`,
  returns the full rule list.  Internal-only protocol; ships C
  and Python clients simultaneously per the probe-SB-management
  design.
- The probe op is authenticated against the same
  `[[allowed_ps]]` allowlist as `PROXY_REGISTRATION` -- it
  returns `PROBE1ERR_PERM` to any caller whose transport-security
  identity (GSS machine principal or mTLS client cert) is not on
  the allowlist.  Rule data is materially sensitive (discloses
  per-client export policy) and the probe transport has no
  default authentication of its own.
- The PS calls `SB_GET_CLIENT_RULES` for each discovered SB at
  registration time, and re-pulls on a timer (default 5 minutes)
  or on-demand when the PS's own enforcement returns `ACCES` for
  a client it hasn't seen before.

For the earliest implementation phases (before the probe op is
plumbed), the PS accepts **every** client and lets the MDS do all
authorization.  That is correct but slower: bad clients hit the
MDS before being rejected.  Acceptable while the other pieces
come up.

### Discovery: startup + on-demand

At PS startup, after PROXY_REGISTRATION succeeds:

1. PS sends NFSv3 `MOUNT` / `EXPORT` procedure to the MDS
   (showmount-e over the wire).
2. For each exported path returned, PS traverses the MDS's
   namespace on its NFSv4.2 session:
   `PUTROOTFH + LOOKUP "component1" + LOOKUP "component2" + ... + GETFH`
3. PS allocates a proxy SB per discovered path, mounts it at the
   matching path in the :4098 namespace, stashes the MDS FH on
   the SB's root inode.

On-demand triggers:

- Client LOOKUP into a path not yet in the PS's SB table -->
  PS re-runs `MOUNT/EXPORT` (with a 5-second hold-down to avoid
  hammering) and traverses any new paths.
- Client PUTFH with `sb_id` matching an SB the PS doesn't have -->
  `NFS4ERR_STALE`.  FHs are not portable across PS restarts.
- PS receives `NFS4ERR_NOENT` or `NFS4ERR_STALE` from the MDS
  during proxied I/O --> re-run `MOUNT/EXPORT`; if the export is
  gone, destroy the proxy SB.  Clients holding cached FHs for
  that SB get `NFS4ERR_STALE`, which is correct.

### GETATTR is forwarded to the MDS

For the first implementation, every GETATTR the PS receives from a
client is forwarded to the MDS as a COMPOUND with the client's
credentials.  The MDS does its own reflected-GETATTR fan-out to
DSes if it needs to; the PS does no caching beyond what NFSv4
already caches at the client.

The win is delstid (RFC 9754): a PS that holds a delegation with a
`OPEN_ARGS_SHARE_ACCESS_WANT_OPEN_XOR_DELEGATION` bit can answer
GETATTR locally for attrs covered by the delegation.  Stretch goal,
tracked in the action-items section.

### Short-circuit for co-resident DS

When LAYOUTGET returns a layout, each `ffv2_data_server4` entry
has a `ffv2ds_deviceid`.  The PS resolves that to an address via
GETDEVICEINFO.  If the resolved address matches one of the PS's
own bound addresses (maintained as a small set in the PS's
runtime state), the PS bypasses RPC entirely and calls the local
DS SB's `sb_ops->db_read` / `db_write` directly via the local
inode.  No loopback RPC, no codec round-trip beyond the codec
transform itself.

**The short-circuit MUST NOT skip the per-request authorization
check that the RPC path would have done.**  Before calling
`db_read` or `db_write` on the local DS inode, the short-circuit
path MUST validate that the forwarded client credentials satisfy
the local DS file's synthetic uid/gid (the same check NFSv3
WRITE or CHUNK_WRITE would perform on the RPC path) and, once
tight-coupling trust tables land, MUST validate the forwarded
layout stateid against the local DS's trust-stateid table.  A
short-circuit path that skips either check turns the narrow
privilege model into a blanket ACL override for every
co-resident file; the phase 5 tests enforce both checks with
`test_shortcircuit_rejects_wrong_creds` and
`test_shortcircuit_rejects_unknown_stateid`.

Detection uses the PS's own `getsockname` set plus any
config-declared equivalent addresses (e.g., container-internal
IPs that map to the same reffs process).  Complex NAT or overlay
networks are out of scope; the demo uses flat container
networking.

## Data flows

### Client WRITE on a proxied file

1. Client issues WRITE on an FH in a proxy SB, bound to the PS's
   :4098 listener.
2. PS's `nfs4_op_write` detects `sb_proxy_mds != NULL` on the
   inode's SB and delegates to the proxy data path.
3. Proxy data path looks up (or acquires) a layout for the file
   via LAYOUTGET on the PS's MDS session, carrying the forwarded
   client credentials.
4. Codec layer encodes the payload using the layout's coding type
   (from `ec_pipeline`).
5. For each mirror in the layout, the proxy data path fans out
   CHUNK_WRITE (and CHUNK_FINALIZE / CHUNK_COMMIT as the layout
   requires) to the real DSes.
6. If the layout points at a DS that resolves to a local address,
   use the short-circuit VFS path for that mirror.
7. On success, PS returns NFS4_OK to the client; on codec error,
   LAYOUTERROR to the MDS and NFS4ERR_IO to the client.

### Client READ on a proxied file

1. Client READ on a proxy-SB FH.
2. PS acquires layout (cached from a prior LAYOUTGET or fresh).
3. PS issues CHUNK_READ to sufficient DSes to reconstruct the
   requested byte range.
4. Codec decodes to plaintext.
5. PS returns plaintext in the READ reply.

### Client GETATTR on a proxied file

PS builds a COMPOUND: `SEQUENCE + PUTFH(<mds-fh>) + GETATTR(<bits>)`
with the client's forwarded credentials, sends it to the MDS,
returns the result verbatim to the client.

No caching in this iteration.  Each client GETATTR = one PS-to-MDS
round-trip.

### Client LOOKUP crossing into a proxy SB

1. Client does `PUTROOTFH + LOOKUP "somepath"` on PS :4098.
2. PS's root pseudo-SB sees `somepath` mounted as a proxy SB.
3. Compound crosses into the proxy SB (existing mount-crossing
   mechanism in reffs).
4. GETFH returns the proxy-SB-scoped FH (containing the PS's
   sb_id for this proxy SB and the root inode number).

No cross-SB RPC to the MDS on LOOKUP itself.  The client-visible
LOOKUP stops at the PS.

### Discovery traversal on the PS-MDS session

When the PS needs to resolve a path like `/a/b/c` to an FH on the
MDS:

```
SEQUENCE
PUTROOTFH
LOOKUP "a"
LOOKUP "b"
LOOKUP "c"
GETFH
```

The PS uses its own RPC credentials on this compound (uid mapped
from the PS's configured service identity).  Because this is on a
registered-PS session, the MDS permits the LOOKUPs / GETFH
regardless of export-rule filtering for the PS's source address.
The FH returned is cached on the proxy SB's root inode.

## PROXY_REGISTRATION authentication

The data-mover draft (`sec-security`) mandates that the MDS <-> PS
session MUST use RPCSEC_GSS or RPC-over-TLS with mutual
authentication; AUTH_SYS on that session is explicitly forbidden.
An earlier draft of this plan offered AUTH_SYS + source-IP
allowlist as a demo shortcut; that option has been removed to
stay conformant.  The two supported authenticators are:

1. **RPCSEC_GSS machine principal.** The PS uses Kerberos with
   `host/ps.example.com@REALM`.  The MDS config has
   `[[allowed_ps]] principal = "host/ps.example.com@REALM"`.
   Requires a KDC.  Preferred for multi-host production
   deployments.
2. **RPC-over-TLS with allowlisted client certificate.** The PS's
   session runs over RPC-over-TLS ({{RFC9289}}) with mutual
   authentication; the MDS allowlists the client cert
   fingerprint.  Simpler than Kerberos for CI and container
   deployments -- mTLS with a self-signed CA is the demo
   default.

The MDS applies the allowlist check on receipt of
PROXY_REGISTRATION, before recording `nc_is_registered_ps`.  A
registration from a non-allowlisted identity returns
`NFS4ERR_PERM`.

**Guard against registration squatting.**  Because one PS proxies
one MDS, a successful rogue registration displaces the legit PS
and returns `NFS4ERR_STALE` to every client holding a cached FH
against the previous PS (the discovery section documents why FHs
are not portable across PS restarts).  The MDS MUST therefore
refuse a new `PROXY_REGISTRATION` while an existing registration
from the same allowlisted identity still holds a valid lease,
returning `NFS4ERR_DELAY` and logging the conflict.  Registration
*renewal* from the same identity is distinguished by
`prr_registration_id` matching the existing one and is permitted
(it refreshes the lease).  The demo setup script additionally
installs a host firewall rule restricting the MDS port to the
PS's container-internal network so that only an already-exposed
attacker on that network could even attempt registration.

Config: `[[allowed_ps]]` on the MDS side, repeatable for multiple
allowed identities.  The demo uses (2) with mTLS over a
container-internal network.

## `ec_demo` refactor

Phase 2 (below) requires a callable library for the MDS-session +
CHUNK-io + codec pipeline.  Two refactor depths:

- **(a) Minimal: extract the glue.** Move ec_demo's orchestration
  (MDS session setup, compound chaining, layout resolve, CHUNK
  fan-out, codec transform) into `lib/nfs4/ps/ec_pipeline.c` with
  a clean entry API.  ec_demo becomes a thin CLI that configures
  ec_pipeline and calls it.  Leaves the underlying modules in
  `lib/nfs4/client/` alone.
- **(b) Deeper: reshape `lib/nfs4/client/`.** Audit the existing
  MDS-client / compound-builder / layout-decoder interfaces,
  remove ec_demo-specific leaks, document re-entrancy
  requirements.  A day or two of plumbing.

Plan: do (a) first (one refactor commit before any PS phase
starts).  (b) is tracked as an action item and completed before
the PS is declared "done" — having `ec_pipeline` as a second
consumer of the underlying modules is a forcing function for (b),
surfacing interfaces that need to be generalised.

## Phases

Each phase lists tests up front per `.claude/roles.md` "tests first"
discipline.  All phases require the phase's unit tests to be in
place and failing before implementation starts, and all phases
require `ci-full` to pass before moving on.

### Phase 0: Refactor `ec_demo` into `lib/nfs4/ps/ec_pipeline.c`

**Unit tests** (new file `lib/nfs4/ps/tests/ec_pipeline_test.c`):

| Test | Intent |
|------|--------|
| `test_pipeline_init_fini` | Create and destroy a pipeline handle; no leaks |
| `test_pipeline_mds_session_create` | Pipeline opens an MDS session; teardown closes cleanly |
| `test_pipeline_layoutget_decode` | LAYOUTGET returns an `ffv2_layout4`; pipeline decodes to per-mirror DS lists |
| `test_pipeline_write_roundtrip` | Pipeline writes a buffer, reads it back, bytes match (against a combined MDS+DS reffs instance) |
| `test_pipeline_read_plain_codec` | FFV2_CODING_MIRRORED roundtrip |
| `test_pipeline_read_rs_codec` | FFV2_ENCODING_RS_VANDERMONDE roundtrip |
| `test_pipeline_read_mojette_sys_codec` | FFV2_ENCODING_MOJETTE_SYSTEMATIC roundtrip |

**Functional test**: ec_demo still passes all its existing tests
after the refactor.  No behavioural regression.

**CI**: `ci-full` runs the new unit tests and the unchanged
ec_demo tests.

### Phase 1: Second listener + empty proxy namespace

Plumb a per-listener SB set in `lib/rpc`.  `[[proxy_mds]]`
present --> additional listener binds on the configured port.

**Unit tests** (`lib/rpc/tests/listener_test.c`):

| Test | Intent |
|------|--------|
| `test_listener_alloc_free` | Bind a listener to a port, tear down, port released |
| `test_two_listeners_different_sb_sets` | Two listeners, each with its own SB list; SB list access routes by listener tag on the compound |
| `test_listener_unknown_sb_id` | PUTFH on listener B with sb_id from listener A returns `NFS4ERR_STALE` |
| `test_listener_shutdown_drain_then_teardown` | In-flight compounds on a listener drain before any SB owned by that listener is torn down; no UAF under ASAN; listener shutdown is explicitly drain -> SB teardown, in that order |
| `test_listener_shutdown_order` | Fini shuts down both listeners cleanly; no leaks |

**Functional test**: mount `:2049` and `:4098` from the same client,
stat `/`, each returns its own root.

**CI**: `scripts/ci_ps_listener_test.sh` starts a reffsd with
`[[proxy_mds]]` configured (but MDS absent — PS startup must
tolerate unavailable MDS and retry), verifies both listeners are
bound.

### Phase 2: Static proxy SB with showmount discovery

At PS startup, after PROXY_REGISTRATION (phase 5 plumbs this in
fully; for phase 2 the registration op is stubbed and the PS
assumes privilege), run showmount against the MDS, traverse each
path, allocate a proxy SB per discovered export.  No I/O yet;
READ / WRITE on a proxy-SB file return `NFS4ERR_NOTSUPP`.

**Unit tests** (`lib/nfs4/ps/tests/ps_discovery_test.c`):

| Test | Intent |
|------|--------|
| `test_discovery_showmount_parse` | Parse a stock showmount reply, yield a list of paths |
| `test_discovery_traversal` | Given a path and a mock MDS session, traverse and extract the FH |
| `test_discovery_creates_sb` | Successful traversal allocates a proxy SB with correct backend composition (md=RAM, data=PROXY) |
| `test_discovery_ondemand_hold_down` | Two consecutive LOOKUP misses within 5s trigger only one showmount |
| `test_discovery_stale_export` | MDS returns NOENT on traversal --> proxy SB is destroyed; existing FHs return STALE |

**Functional test**: a client does `ls /` on the PS, sees the same
paths as `ls /` on the MDS.  `stat somepath` on the PS returns
metadata forwarded from the MDS.  `cat somepath` fails with
`NFS4ERR_NOTSUPP` (expected, no I/O yet).

**CI**: extend the scripted test to set up 1 MDS + 1 PS, create
`/data` via probe, verify the client can see `/data` via the PS.

### Phase 3: `REFFS_DATA_PROXY` backend, client READ

Plumb `db_read` through the proxy data backend.  READ on a
proxy-SB file triggers pipeline LAYOUTGET + CHUNK_READ + decode.
Write still returns NOTSUPP.

The `sb_ops->db_read` dispatch check `sb_proxy_mds != NULL` is
cached as a scalar bit on `sb_state` (C11 `_Atomic` per
`.claude/standards.md`) so the hot path is one atomic load, not
a struct dereference through the SB.

**Unit tests** (`lib/backends/tests/proxy_data_test.c`):

| Test | Intent |
|------|--------|
| `test_proxy_backend_compose` | `reffs_backend_compose(REFFS_MD_RAM, REFFS_DATA_PROXY)` returns a valid ops struct |
| `test_proxy_db_read_routes_to_pipeline` | `db_read` on a proxy SB invokes the pipeline with the file's MDS FH |
| `test_proxy_db_read_decodes_correctly` | Mock LAYOUTGET + mock CHUNK_READ returning shards; pipeline decodes to expected plaintext |
| `test_proxy_db_read_cred_forwarding_auth_sys` | `db_read` call includes the caller's forwarded AUTH_SYS credentials (uid/gid) in the outgoing LAYOUTGET compound |
| `test_proxy_db_read_cred_forwarding_uid_zero_squash` | uid=0 + root_squash=true --> squashed to 65534 end-to-end |
| `test_proxy_db_read_cred_forwarding_uid_nonzero_passthrough` | uid=1000 --> passes through unchanged |
| `test_proxy_db_read_cred_forwarding_auth_none_rejected` | AUTH_NONE on inbound compound --> PS rejects before any MDS round-trip (rule 5 of sec-credential-forwarding) |
| `test_proxy_db_read_cred_forwarding_uses_reffs_id` | Forwarded credentials go through `REFFS_ID_MAKE(UNIX, 0, uid)` and accessor macros so the identity-refactor does not later strip type/domain bits |
| `test_proxy_db_read_stale_layout` | Expired layout triggers re-LAYOUTGET transparently |

**Functional test**: file written directly to the MDS (via an ec_demo
run), client mounts the PS, `cat` that file, bytes match.  This is
the first visible demo milestone.

**CI**: extend `ci_integration_test.sh` with a PS-read variant.
Measure latency and throughput as a baseline.

### Phase 4: Client WRITE

Plumb `db_write` similarly.  End-to-end: client writes to PS --> PS
encodes --> CHUNK_WRITE to DSes; client reads back --> PS decodes.

**Unit tests** (extend `proxy_data_test.c`):

| Test | Intent |
|------|--------|
| `test_proxy_db_write_routes_to_pipeline` | `db_write` invokes pipeline with correct FH and credentials |
| `test_proxy_db_write_encode_chunks` | Pipeline encodes a buffer and issues CHUNK_WRITE + FINALIZE + COMMIT to each DS in layout |
| `test_proxy_db_write_roundtrip` | Write via PS, read via PS, bytes match |
| `test_proxy_db_write_layouterror` | A CHUNK_WRITE error triggers LAYOUTERROR to MDS and propagates as NFS4ERR_IO |

**Functional test**: `cp largefile client:PS-mount/file` and
`diff largefile <(cat PS-mount/file)` on the client.

**CI**: extend benchmark harness with a PS-write variant; compare
against direct-to-MDS throughput as the "codec-translation cost"
number for the draft.

### Phase 5: Short-circuit for co-resident DS

When the layout points at a DS whose address is one of the PS's
own bound addresses, bypass RPC and call the local DS sb's
`db_read` / `db_write` directly.

**Unit tests**:

| Test | Intent |
|------|--------|
| `test_shortcircuit_address_detection` | Given a set of PS addresses and a DS deviceinfo reply, correctly identify match vs non-match |
| `test_shortcircuit_taken_on_read` | When deviceinfo matches local, the CHUNK_READ RPC is not sent; local VFS returns the same bytes |
| `test_shortcircuit_taken_on_write` | Same, for CHUNK_WRITE |
| `test_shortcircuit_rejects_wrong_creds` | Short-circuit path must reject a forwarded credential that would fail the DS file's synthetic-uid/gid check on the RPC path; no bypass |
| `test_shortcircuit_rejects_unknown_stateid` | Once tight-coupling trust tables land, short-circuit also rejects a forwarded layout stateid absent from the local DS's trust table |
| `test_shortcircuit_partial` | Layout with 2 mirrors, 1 local + 1 remote; local takes shortcircuit, remote takes RPC; reconstruction succeeds |

**Functional test**: measure read latency with 1 local + 1 remote
mirror; verify the local mirror serves reads with VFS-level
latency, not RPC latency.

**CI**: benchmark extension with combined-DS+PS vs pure-remote
variants.  Expect noticeable speedup on the local fast path.

### Phase 6: PROXY_REGISTRATION + minimal CB back-channel

Wire the real `PROXY_REGISTRATION` fore-channel op and the
`CB_PROXY_STATUS` receive path (the simplest CB op to stub —
the PS replies with `"idle"` always).  `CB_PROXY_MOVE` and
`CB_PROXY_REPAIR` return `NFS4ERR_NOTSUPP` for now.  `PROXY_PROGRESS`
is still unused until phase 7 since no MDS-initiated ops exist.

Also add the MDS's `[[allowed_ps]]` allowlist check and the
`nc_is_registered_ps` session flag.

**Unit tests** (`lib/nfs4/tests/proxy_registration_test.c`):

| Test | Intent |
|------|--------|
| `test_proxy_registration_success` | Allowlisted PS sends PROXY_REGISTRATION; MDS records it; session flagged |
| `test_proxy_registration_reject_not_allowlisted` | Non-allowlisted identity --> NFS4ERR_PERM |
| `test_proxy_registration_reject_bad_prr_flags` | Non-zero prr_flags --> NFS4ERR_INVAL (per RFC 8178) |
| `test_proxy_registration_rejects_without_use_non_pnfs` | PROXY_REGISTRATION on a session whose EXCHANGE_ID did not set `EXCHGID4_FLAG_USE_NON_PNFS` --> NFS4ERR_PERM (per data-mover draft sec-PROXY_REGISTRATION) |
| `test_proxy_registration_rejects_auth_sys_session` | PROXY_REGISTRATION over an AUTH_SYS (non-GSS, non-TLS) session --> NFS4ERR_PERM; the MDS <-> PS session MUST use RPCSEC_GSS or RPC-over-TLS |
| `test_proxy_registration_rejects_squat` | Second PROXY_REGISTRATION from the same allowlisted identity while an existing one holds a valid lease --> NFS4ERR_DELAY; log entry present |
| `test_proxy_registration_accepts_renewal` | Second PROXY_REGISTRATION with matching `prr_registration_id` --> NFS4_OK, lease refreshed |
| `test_registered_ps_bypasses_export_filter_on_lookup` | LOOKUP on a registered-PS session succeeds on an export the PS's creds wouldn't normally see |
| `test_registered_ps_does_not_bypass_root_squash` | LAYOUTGET with forwarded uid=0 and root_squash=true --> squashed, same as any other client |
| `test_registered_ps_lookup_audit_logged` | LOOKUP / GETFH on a registered-PS session that benefits from the bypass emits a TRACE-level audit line naming the traversed path |
| `test_cb_proxy_status_idle_response` | CB_PROXY_STATUS on the back-channel --> PS responds with cpsr_status=NFS4_OK, cpsr_op_status=NFS4_OK, idle |
| `test_cb_proxy_status_lifecycle_tsan_clean` | Two threads driving CB_PROXY_STATUS concurrent with session teardown -- TSAN must be clean; catches races between cb_pending / cb_timeout and PS-side CB receive path |

**Functional test**: end-to-end startup: MDS allowlists PS IP,
PS registers, PS shows up in `reffs-probe sb-list-ps`.  An MDS
probe `cb-proxy-status-poll <ps-id>` returns "idle".

**CI**: `ci_ps_registration_test.sh`.

### Phase 7: `sb-get-client-rules` probe op + PS-side enforcement

Extend the reffs probe protocol with op 27, implement PS-side
pull at registration time, and replace "accept everything" with
real per-client rule enforcement on the PS.

**Unit tests** (extend `lib/probe1/tests/` and
`lib/nfs4/ps/tests/`):

| Test | Intent |
|------|--------|
| `test_sb_get_client_rules_probe` | Probe op returns the exact rule list set on the SB |
| `test_ps_pulls_rules_at_registration` | After PROXY_REGISTRATION, PS has the rule list for every discovered SB |
| `test_ps_enforces_rules_before_mds` | Client hits a rule mismatch at the PS; PS returns NFS4ERR_WRONGSEC before any MDS round-trip |
| `test_ps_rule_refresh_on_ACCES` | PS hits MDS-side NFS4ERR_ACCESS it didn't expect; PS re-pulls rules and retries or rejects |

**Functional test**: per-rule scenarios — AUTH_SYS-only rule
rejects a GSS client at the PS; `*` rule accepts any client.

### Phase 8: Stretch -- `CB_PROXY_MOVE` handler

First real MDS-initiated op.  The PS receives `CB_PROXY_MOVE`,
kicks off a whole-file move via the pipeline (read from source
layout, write to destination layout), emits interim
`PROXY_PROGRESS` and a terminal `PROXY_PROGRESS` on completion.

**Unit tests**:

| Test | Intent |
|------|--------|
| `test_cb_proxy_move_receive_parse` | Decode CB_PROXY_MOVE args, set up operation state |
| `test_cb_proxy_move_executes_move` | Mock source + dest layouts; verify read from source, write to dest, terminal PROXY_PROGRESS emitted |
| `test_cb_proxy_move_progress_reports` | Multi-chunk move emits interim PROXY_PROGRESS at expected checkpoints |
| `test_cb_proxy_move_cancel` | CB_PROXY_CANCEL interrupts an in-progress move; PS emits cancelled PROXY_PROGRESS |

**Functional test**: admin triggers a move via `reffs-probe
file-move`; the file's layout on the MDS updates to point at the
destination DSes; client re-reads and gets the same bytes.

## Systematic testing

Beyond the per-phase unit and functional tests, the PS needs a
set of always-on systematic checks:

### Sanitizer coverage

- **ASAN** on every PS path.  `ci-full` runs PS tests with ASAN.
- **UBSAN** likewise.
- **TSAN** specifically for the dual-listener concurrency.  Two
  client threads driving reads on `:2049` (DS direct) and `:4098`
  (PS proxy) simultaneously; TSAN flags any data race on shared
  state.  Add to `ci-full`.
- **LSAN** is part of CI; PS shutdown must clean up every
  per-SB and per-pipeline allocation.

### Fuzz testing

The PS's `:4098` listener is the one end of the reffs surface
facing potentially-hostile NFSv4 clients (codec-ignorant clients
that the PS is translating for).  Add a fuzzer for the NFSv4.2
compound parser on the PS side.  Harness follows the
`lib/rpc/tests/rpc_fuzz.c` pattern (if exists; else create).

Target: the PS must not crash or leak on malformed compounds.
Should return NFS4ERR_BADXDR / NFS4ERR_INVAL for the entire
malformed-compound class.

The fuzzer also targets the MDS-side decode of `PROXY_REGISTRATION`
and `PROXY_PROGRESS` argument structs.  Those ops are only
reachable from allowlisted PS identities, but parser bugs behind
an allowlist are still blocker-grade: an allowlisted but
compromised PS is a realistic threat model.

### Soak testing

`scripts/ci_ps_soak_test.sh`:

- 30-minute run (CI), 8-hour run (manual BAT)
- Two concurrent client threads: one reads a set of files, one
  writes a set of files, both via the PS
- Restart the PS every 5 minutes (CI) / 15 minutes (BAT)
- Separately, a BAT-only companion run restarts the *MDS*
  every 15 minutes while the PS stays up; the PS must
  re-register within one lease period and resume service
  without client-visible errors beyond the expected
  NFS4ERR_DELAY window during grace
- After each restart: MOUNT/LOOKUP must succeed within 30s;
  clients with cached FHs for destroyed SBs must get STALE, not
  crash or hang
- Pass criteria (parallels the main soak acceptance in
  stable-bat.md):
  - Zero ASAN / UBSAN errors
  - RSS at end within 2x of RSS at 5-minute mark
  - Open FD count within 10% of 5-minute mark
  - Every restart-and-reconnect succeeds within 30s

### Co-residency correctness

For any file served by a combined-DS+PS box, the bytes returned
on a local-shortcircuit read must be byte-identical to the bytes
returned when the shortcircuit is disabled (forcing RPC to the
local DS).  `ci_ps_shortcircuit_test.sh` runs the same read
request with shortcircuit on and off; `cmp` must report zero
differences.

**Write-path symmetry** is the corresponding test.
`ci_ps_shortcircuit_write_test.sh` issues identical CHUNK_WRITE
sequences once with the local-VFS short-circuit enabled and
once with it forced through RPC.  The post-write state must be
byte-identical: same content, same CRC32, same FINALIZE/COMMIT
verifier behaviour, same chunk-store metadata on disk.  Any
divergence is a bug in the short-circuit path's auth / lock /
FINALIZE handling.

### Cross-codec correctness

For every codec in the draft (mirrored, RS, mojette-sys,
mojette-nonsys), the PS's client-visible bytes must match the
bytes written via ec_demo-direct-to-MDS.  Matrix coverage in a
dedicated test script that runs every phase 3+4 is declared
complete.

## Demo topology

```
         +-------------------------+
         |  reffs MDS              |
         |  bare metal or VM       |
         |  role = "mds"           |
         |  :2049 NFS              |
         |  :20490 probe           |
         +-----------+-------------+
                     |
     +---------------+-------------+
     |                             |
+----+-----------+   +-------------+---+
|  reffs container  | ... 4/6/8 copies |
|  role = "ds"      |                  |
|  [[proxy_mds]]    |                  |
|  :2049 DS         |                  |
|  :4098 PS         |                  |
+-----+-------------+                  |
      |                                |
      +------ clients mount any :4098 -+
```

The client can live on the base OS or in a container.  Mount any
of the containers on port 4098 — all expose the same proxied
namespace.  Writes round-robin across the PS surfaces naturally
if the client uses a service address that DNS-round-robins or a
load balancer.

For the `ci-full` integration test, the simple topology is
1 MDS + 2 (DS+PS) + 1 client.  For benchmark reports, the full
10-node topology already present in `deploy/benchmark/` is
reused with PS enabled.

## Action items (tracked, not in the phase plan above)

1. **Deeper `lib/nfs4/client` refactor (phase 0 option b).** Do
   before declaring the PS demo done.  Remove ec_demo-specific
   leaks; document re-entrancy.
2. **delstid (RFC 9754) support in the PS.** PS acquires an
   XOR-DELEGATION that carries `mtime`, `ctime`, `size`; answers
   GETATTR locally for those attrs; returns the delegation on
   conflicting operations or MDS recall.  Win is substantial for
   READ-heavy workloads.
3. **RPCSEC_GSSv3 structured privilege assertion for credential
   forwarding under Kerberos.** Required to proxy Kerberos-
   authenticated clients.  Out of scope for the first iteration
   (AUTH_SYS only).
4. **Multi-MDS PS.** Allow `[[proxy_mds]]` to appear multiple
   times; one listener per MDS or one listener multiplexed by
   top-level subpath.  Deferred.
5. **PS-side GETATTR caching.** After delstid lands, add a layer
   of PS-local GETATTR cache with delegation-driven invalidation.

## Deferred / NOT_NOW_BROWN_COW

- Inter-server COPY via COPY_NOTIFY through a PS.
- Delta-journaling CB_PROXY_MOVE.
- Partial-range CB_PROXY_MOVE.
- Multi-proxy pipelined moves for very large files.
- Automated PS load-balancing policy at the MDS.
- Probe visibility into PS registration state (new
  `probe1_op_ps_list` or similar) -- admin's view of "who's
  registered" is currently inferable from existing SB probes
  but a dedicated op would be cleaner.

## RFC references

- RFC 8881 Section 13.1: EXCHGID4 flag semantics (PS does NOT set
  USE_PNFS_MDS; it sets USE_NON_PNFS on its MDS-facing session).
- RFC 8178 Section 8, 4.4.3: flag-bit-not-known discovery rule
  (applies to `prr_flags`).
- RFC 7862 Section 11: NFSv4.2 operation registry (PROXY_*,
  CB_PROXY_* op numbers per the data-mover draft).
- RFC 9754: delstid / XOR delegation (stretch goal).
- RFC 9289: RPC-over-TLS (option for PROXY_REGISTRATION
  authentication).
- RFC 7861: RPCSEC_GSSv3 (long-term credential-forwarding path).
- draft-haynes-nfsv4-flexfiles-v2-data-mover: authoritative
  source for PS semantics, PROXY_REGISTRATION/PROGRESS and
  CB_PROXY_* op behaviour.
