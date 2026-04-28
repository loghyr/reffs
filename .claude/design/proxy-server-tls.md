<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# PS-side RPC-over-TLS for the MDS Session (#139)

## Problem

The PS-MDS session is currently a plain libtirpc TCP `clnt_create`
(`lib/nfs4/client/mds_session.c:723-777`).  No TLS, no client cert,
no peer fingerprint.  As a result, every PROXY_REGISTRATION the PS
sends arrives at the MDS without `c_tls_fingerprint` AND without
`c_gss_principal`, so the registration handler at
`lib/nfs4/server/proxy_registration.c:144-148` returns
`NFS4ERR_PERM` -- the data-mover draft mandates RPCSEC_GSS or
RPC-over-TLS with mutual authentication on the MDS<->PS session,
explicitly forbidding AUTH_SYS over plain TCP.

The deploy/sanity demo logs the rejection and continues without the
registered-PS privilege, which is acceptable for the codec smoke
matrix but blocks every downstream feature gated on registration:
the PS cannot consult `SB_GET_CLIENT_RULES`, cannot bypass export
filters during discovery LOOKUP, and cannot receive future
PROXY_PROGRESS work assignments under the production auth model
described in `proxy-server.md` ("Privilege model" section).

The server already has every piece needed to accept a TLS-
authenticated PS:

- Server-side TLS context with peer-cert verification:
  `lib/io/tls.c:102` (`io_tls_init_server_context`).
- Per-connection peer fingerprint:
  `lib/nfs4/server/compound.c:171-176` (calls
  `io_conn_get_peer_cert_fingerprint(rt->rt_fd, ...)` and copies
  the SHA-256 hex into `compound->c_tls_fingerprint_buf`).
- Allowlist-by-fingerprint check:
  `lib/nfs4/server/proxy_registration.c:166-177`.
- Allowlist config: `[[allowed_ps]] tls_fingerprint = "..."` under
  `lib/include/reffs/server.h:ss_allowed_ps_tls_fingerprint[]`.

The work for #139 is entirely on the PS side: stand up the
mutually-authenticated TLS connection before the libtirpc
`clnt_create`, populate `[[proxy_mds]]` config with cert paths,
and drive an end-to-end smoke proving the MDS handler accepts.

## Design choice: libtirpc-compatible custom XPRT

Two viable paths:

**Option A -- custom libtirpc XPRT wrapping SSL_read/SSL_write.**
Build a thin XPRT that registers as the read/write callback, with
`xp_p1` pointing at the `SSL *`.  The rest of the libtirpc
machinery (sequenced calls, RPC framing, CLIENT API) is reused
verbatim.  ~150 lines.

**Option B -- bypass libtirpc, hand-roll on `tls_rpc_send/recv`.**
Replace the entire MDS session client stack with the existing
`lib/include/reffs/tls_client.h` helpers.  Larger refactor (every
COMPOUND build site stops talking to a CLIENT* and instead
hand-encodes), and would diverge the PS path from the kernel-
client tooling that already depends on libtirpc.

**Decision: Option A.**  The libtirpc CLIENT* API is the
established surface for the PS-MDS protocol; the rest of
mds_session.c (EXCHANGE_ID, CREATE_SESSION, SEQUENCE,
PROXY_REGISTRATION, PROXY_PROGRESS, PROXY_DONE, PROXY_CANCEL) all
go through `clnt_call` and stay unchanged.  A custom XPRT is
local to one new file (`lib/nfs4/client/mds_tls_xprt.c`) and has
been done before in libtirpc consumers (e.g., the RPC-over-TLS
patches floating in the Linux kernel client tree).

## Slice plan

Multi-slice initiative.  Land each slice independently on the
xdr-proxy-stateid topic branch with its own reviewer pass.

### Slice plan-1-tls.a -- Custom TLS XPRT skeleton

**Scope.**  New module `lib/nfs4/client/mds_tls_xprt.{c,h}`
that exposes:

```c
struct mds_tls_xprt;

struct mds_tls_xprt *mds_tls_xprt_create(int fd, SSL *ssl,
                                          rpcprog_t prog,
                                          rpcvers_t vers);
CLIENT *mds_tls_xprt_to_client(struct mds_tls_xprt *xp);
void    mds_tls_xprt_destroy(struct mds_tls_xprt *xp);
```

Implementation: a libtirpc CLIENT with an op vector
(`cl_call`, `cl_freeres`, `cl_destroy`, etc.) that:
- Encodes the call via XDR into a heap buffer.
- Sends via `tls_rpc_send(ssl, fd, body, body_len)` (already
  in `lib/include/reffs/tls_client.h`).
- Receives via `tls_rpc_recv(ssl, fd, buf, bufsz)`.
- Decodes the reply into the caller's resp xdr.

The XPRT owns the `SSL *` and the underlying fd.  `cl_destroy`
calls `SSL_shutdown` + `SSL_free` then closes the fd.

**Tests** (`lib/nfs4/client/tests/mds_tls_xprt_test.c`):
- Round-trip: build XPRT against a localhost SSL pair via
  socketpair + `SSL_BIO_dgram` style mocking.  Send a NULL RPC,
  verify reply round-trips.
- Destroy ordering: `mds_tls_xprt_destroy` must `SSL_shutdown`
  before `SSL_free` and only then `close(fd)`.  ASAN/LSAN clean.
- Send failure: simulated `SSL_write` error returns a CLIENT
  status code that callers can interrogate via `clnt_perror`.

**Out of scope for this slice.**  Discovery, allowlist,
mds_session integration -- all in 1-tls.b.

### Slice plan-1-tls.b -- mds_session integration

**Scope.**  Extend `mds_session_clnt_open` to accept a
`tls_client_config` and, if any cert path is set, replace the
plain TCP path with:

```
fd  = tls_tcp_connect(host, port);
ctx = tls_client_ctx_create(&cfg);
ssl = tls_starttls(fd, ctx, /*verbose=*/0);  // or tls_direct_connect
xp  = mds_tls_xprt_create(fd, ssl, NFS4_PROGRAM, NFS_V4);
clnt = mds_tls_xprt_to_client(xp);
```

Caller (`reffsd.c` PS init) supplies the `tls_client_config`
from `[[proxy_mds]]` settings.  If no cert path is set, fall
through to the existing plain-TCP path -- the smoke tests and
unit tests that exercise the non-TLS PS keep working without a
config change.

**STARTTLS vs direct TLS.**  Default to `tls_starttls` (RFC
9289 AUTH_TLS upgrade), since the MDS already accepts STARTTLS
on its NFS port without a separate listener.  Allow
`[[proxy_mds]] tls_mode = "direct"` to opt into
`tls_direct_connect` for deployments that proxy the MDS port
through a TLS-only fronting (haproxy, nginx).

**Config schema** (`lib/include/reffs/settings.h`):

```c
struct reffs_proxy_mds_config {
    /* ... existing fields ... */
    char tls_cert_path[REFFS_CONFIG_MAX_PATH];
    char tls_key_path[REFFS_CONFIG_MAX_PATH];
    char tls_ca_path[REFFS_CONFIG_MAX_PATH];
    enum reffs_proxy_tls_mode tls_mode; /* OFF | STARTTLS | DIRECT */
    /* OFF if all three paths are empty -- the PS stays on plain TCP. */
};
```

**Config TOML**:

```toml
[[proxy_mds]]
address  = "10.1.1.5"
port     = 4098
mds_port = 2049
tls_cert = "/etc/reffs/ps-a.crt"
tls_key  = "/etc/reffs/ps-a.key"
tls_ca   = "/etc/reffs/ca.crt"
tls_mode = "starttls"           # or "direct" or omitted (off)
```

**Tests** (`lib/config/tests/config_test.c` extension):
- TLS fields parse correctly.
- Missing `tls_key` while `tls_cert` is set -> config error
  (mTLS needs both, or neither).
- `tls_mode = "direct"` parses to `REFFS_PROXY_TLS_DIRECT`.

### Slice plan-1-tls.c -- end-to-end smoke

**Scope.**  Stand up a mini-CA fixture (mirroring
`.claude/design/stable-bat.md` WI-0.1's design) and prove
PROXY_REGISTRATION succeeds end-to-end:

1. Generate `ca.crt` + `ca.key`, sign `ps.crt` + `ps.key`.
2. Configure MDS with `[[allowed_ps]] tls_fingerprint = "<hash
   of ps.crt>"`.
3. Configure PS with `[[proxy_mds]] tls_cert/key/ca = ...`.
4. Start MDS, start PS.  PS opens TLS-authenticated session,
   issues PROXY_REGISTRATION.
5. MDS handler reads `c_tls_fingerprint` from the per-fd cert,
   matches the allowlist, transitions PS to registered state.
6. Smoke verifies `nc_is_registered_ps == true` on the MDS
   client record.

Test framework: extend `deploy/sanity/run-ps-demo.sh` (or a
sibling `run-ps-demo-tls.sh`) with a `setup-mini-ca.sh`
prologue that generates the certs and writes the config files
into the docker volume.

**Acceptance criteria.**
- PS log shows `PROXY_REGISTRATION ok` instead of the current
  `NFS4ERR_PERM` rejection.
- MDS log shows `PROXY_REGISTRATION accepted from
  tls_fingerprint=<hash>`.
- Cross-PS demo still PASSes the codec matrix unchanged.

## Test impact analysis

| Existing test | Impact | Reason |
|---------------|--------|--------|
| All `lib/nfs4/tests/*` | PASS | No XDR changes; mds_session changes only the transport layer below clnt_call |
| `proxy_registration_test` | PASS | Already drives both GSS and TLS auth paths via test mocks; production wiring doesn't change the assertion contracts |
| `lib/config/tests/config_test.c` | UPDATE | Add tls_cert/key/ca/tls_mode parsing tests (slice 1-tls.b) |
| `deploy/sanity/run-ps-demo*.sh` | EXTEND | Slice 1-tls.c adds the mini-CA prologue; existing scenarios stay non-TLS by default |
| All ec_demo / kmount paths | PASS | None of them use the PS-MDS session |

## Dependencies

- `lib/include/reffs/tls_client.h` (already shipped) provides
  `tls_client_ctx_create`, `tls_starttls`, `tls_direct_connect`,
  `tls_rpc_send`, `tls_rpc_recv`, `tls_tcp_connect`.
- Server side already wires `c_tls_fingerprint` from the per-fd
  peer cert (`lib/nfs4/server/compound.c:171-176`).
- Allowlist persistence already in place
  (`ss->ss_allowed_ps_tls_fingerprint[]`).

## Deferred / NOT_NOW_BROWN_COW

- RPCSEC_GSSv3 structured privilege assertion for credential
  forwarding (that is the long-term auth path; RFC 9289 mTLS is
  the deployable today path).
- Session resumption / 0-RTT (1-RTT TLS 1.3 handshake is
  acceptable for the once-per-PS-startup PROXY_REGISTRATION
  cost; resumption is an optimisation, not a correctness item).
- Per-listener TLS contexts (one [[proxy_mds]] = one TLS
  context; future multi-MDS PS would lift this).
- Re-registration on cert rotation (current model: restart the
  PS to pick up new cert files).

## Implementation order summary

1. **plan-1-tls.a**: Custom TLS XPRT (skeleton + tests).
2. **plan-1-tls.b**: mds_session integration + config wiring.
3. **plan-1-tls.c**: end-to-end smoke + mini-CA fixture.

Each slice lands as a single commit on `xdr-proxy-stateid`,
reviewer pass before commit, dreamer ASAN/UBSAN required for
1-tls.a and 1-tls.b unit tests; 1-tls.c additionally requires
the docker compose stack with the mini-CA prologue.
