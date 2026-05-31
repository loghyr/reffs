<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Krb5 stress harness: SPN selection and multi-transport in ec_demo

A controlled in-process reproducer for the krb5 multi-client
connection load some sites have reported: many simultaneous
mounts from one box, each driving a fresh GSS context
establishment, can saturate the server's directory-lookup path
in ways that single-mount tests do not surface.

The companion `tools/nfs_krb5_multiclient` already covers the
*N distinct identities, one process per identity* variant via
forked workers.  This design adds a complementary axis:
**one process, N simultaneous GSS handshakes**, with optional
control over the target service principal name (SPN) per
transport.  Same wire shape as the multi-mount load, but in a
binary whose timing, identity matrix, and SPN matrix are
controllable from the command line.

## Three commits, each independently usable

### 1. `--spn` and `--canonicalize` on ec_demo

Pure-additive flags on `tools/ec_demo.c`.  Defaults preserve
today's behavior (the krb5 library builds the SPN as
`nfs@<server>`, which canonicalizes to `nfs/<server>@<REALM>`).

`--spn NAME` overrides the target service principal name passed
to `authgss_create_default`.  Three accepted forms:

  * `nfs/host.example.com` -- principal-name form.  Library fills
    in the default realm.
  * `nfs/host.example.com@REALM` -- fully-qualified.
  * `nfs@host.example.com` -- host-based service form; library
    canonicalizes.

API: `mds_session_create_sec_spn` in `lib/nfs4/client/mds_session.c`
takes the SPN override; the existing `mds_session_create_sec`
is now a thin wrapper that passes NULL.

`--canonicalize` (planned for a follow-on within this same
commit series) sets up `KRB5_CONFIG` to a synthesized config
with `dns_canonicalize_hostname = false`, so the server sees
the SPN-form cname rather than the canonicalized one.

### 2. Burst axes: sessions, transports, identities

> **Flag-naming note (resolved).**  This axis was originally named
> `--nconnect`, which overloads the kernel / pd-protod mount option
> of the same name.  Kernel `nconnect=N` is N TCP transports under
> one NFSv4 session (multiplexing).  The N-independent-sessions
> axis was renamed to `--nsessions` to match the wire artifact, and
> `--nconnect` was reclaimed for the kernel-style meaning -- M TCP
> transports per session with `BIND_CONN_TO_SESSION` on transports
> 1..M-1.  Both axes are now exposed independently; total wire
> transport count is `nsessions * nconnect`.  The design below
> still uses "xprt" / "multi-xprt" to describe the per-handshake
> GSS fan-out for historical reasons.

A third axis -- per-worker initiator identity -- is exposed via
`--ccache-dir DIR`, which rotates `KRB5CCNAME` across forked
workers.  The K8s pod model (one pre-baked ccache per pod,
provisioned by the kerberos operator) maps directly onto this
shape: forked workers each have their own envp and their own
libkrb5 context.

`struct mds_session` grows from one `CLIENT *` to an array of N
under `ms_clnts[]`.  `mds_session_create_sec_spn` opens N xprts
in parallel and runs N independent `authgss_create_default`
exchanges, one per xprt.  This is the burst.

The per-xprt GSS contexts are deliberately **not** shared.
Sharing one context across N xprts is a sound client-side
optimization but the stress harness wants the un-shared
behavior so the server sees the full handshake fan-out.  When
shared-context support is added later, a `--share-gss` flag
gates the optimization here so this reproducer keeps the burst
shape.

Per-xprt SLOT4 / sessionid4 bookkeeping is per-xprt today --
each `clnt` has its own AUTH and its own session id, which is
what reproduces the load.  COMPOUND submission picks an xprt
round-robin via a small atomic counter on `struct mds_session`.

### 3. Combined reproducer + docs

ec_demo gets `--spn-list a,b,c,...` -- when paired with
`--nsessions N`, the comma-separated list is round-robined
across xprts.  Length need not match N (modular).

`docs/krb5-multiclient-testing.md` documents the canonical
invocation and the comparison against `nfs_krb5_multiclient`
(forked vs. threaded, when to use each).

The combined invocation looks like:

```
ec_demo write --mds <server> --file /bigdata --input <local> \
              --sec krb5 \
              --nsessions 32 \
              --spn-list nfs/h0,nfs/h1,...,nfs/h31 \
              --canonicalize no
```

This fires 32 simultaneous `EXCHANGE_ID + CREATE_SESSION +
RPCSEC_GSS_INIT` handshakes from one process, each asking the
KDC for a ticket against a different SPN.  Tom's confirmation
of 2026-05-30: same `KRB5CCNAME` on every xprt (one user, N
parallel handshakes); a future commit may add per-xprt ccache
when needed.

## What this does not address

* **Server-side identity from the PAC.**  The reffs server does
  not extract or trust the PAC from inbound krb5 GSS contexts
  today.  That is a separate slice of work; see
  `identity.md` Phase 2.  This document is purely about driving
  the existing server-side identity path harder.
* **Shared GSS context across N transports.**  That is the
  *fix* for the load pattern this reproducer drives; intentionally
  left to a future commit so the stress harness retains the
  un-shared burst shape.
* **LDAP / DNS-SRV SPN resolution on the client.**  The client
  asks the KDC for whatever SPN the operator passed via
  `--spn` / `--spn-list`; it does not resolve SPNs out of a
  directory.

## Cross-reference

`docs/krb5-multiclient-testing.md` documents the operational
side -- KDC setup, ccache management, expected vs. actual
failure shapes.  Read that for invocation; read this for the
ec_demo internals.
