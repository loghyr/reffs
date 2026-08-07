<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# CHUNK_ESCROW (S2)

Design for the four escrow operations in
`draft-haynes-nfsv4-flexfiles-v2`: `CHUNK_ESCROW_INSTALL`,
`_RELEASE`, `_ENUMERATE`, `_TAKEOVER` (ops 92-95).

## Status: blocked, and not on a decision

The wire is done.  Verified 2026-08-07: reffs has all four ops at
92-95, `escrow_id4`, every args/res struct, all four handlers
registered in `dispatch.c` and present in the S1.5(A) allowlist, and
four deliberate `NFS4ERR_NOTSUPP` stubs in `chunk.c` whose comment
explains the choice -- a capability probe from a compliant metadata
server sees a known operation refusing rather than a decode error.

What is missing is semantics.  And the semantics cannot be written
yet, because **escrow is defined entirely in terms of chunk locks,
and chunk locks do not exist**.

`nfs4_op_chunk_lock` and `nfs4_op_chunk_unlock` are nine-line
`NFS4ERR_NOTSUPP` stubs.  The persistent block record carries a
`CHUNK_BLOCK_LOCKED` bit in `cb_flags` -- declared, persisted through
`cbd_flags`, and never set by anything.

Read the draft's own definition (`sec-system-model-escrow`) with that
in mind:

- Placeholder ownership is "when a client's stateid is revoked while
  it holds chunk locks, the data server MUST NOT drop the locks".
  With no locks to hold, there is nothing to transfer.
- `CHUNK_ESCROW_ENUMERATE` inventories "outstanding escrows", which
  are placeholder-owned *locks*.
- Adoption happens through `CHUNK_LOCK` with
  `CHUNK_LOCK_FLAGS_ADOPT` -- a flag on the operation that is a stub.

So implementing escrow first would produce four operations managing
a lock state no operation can create, testable only against
hand-fabricated state.  **The next slice is CHUNK_LOCK / CHUNK_UNLOCK,
not this one.**

## What escrow needs beyond chunk locks

Assuming locks land first, escrow still needs four things reffs does
not have.

### 1. Escrow identity on the block record

`struct chunk_block` has no escrow id.  It needs one: `escrow_id4` is
16 opaque bytes, and it is what RELEASE names, what ENUMERATE
returns, and what a repair actor names when adopting.

This grows the on-disk record (`struct chunk_block_disk`).  Per the
deployment status recorded in `CLAUDE.md` there is no persistent
deployment, so the format version stays 1 and no migration code is
needed -- but that claim should be re-read at implementation time
rather than trusted from here.

Only placeholder-owned blocks carry one.  An all-zero escrow id is
the natural "not escrowed" value, which means zero must not be a
legal `escrow_id4` the metadata server can choose.  The draft does
not appear to reserve it -- **open question below**.

### 2. Metadata-server epoch, per data server

`ceia_mds_epoch`, `cera_mds_epoch` and `ceea_mds_epoch` appear on
INSTALL, RELEASE and ENUMERATE; TAKEOVER carries
`ceta_expected_prior_epoch` and `ceta_new_epoch`.  The data server
has to hold a current epoch and reject operations from a stale one
with `NFS4ERR_STALE_MDS_EPOCH` (10106, already in reffs's enum).

This is per data server, not per file, and it must survive restart --
otherwise a data server reboot silently unfences a departed metadata
server incarnation.  Server state, not chunk state.

### 3. Proof-profile verification for TAKEOVER

`ceta_proof_profile` + `ceta_proof_data`.  This is the security
boundary of the whole mechanism: whoever satisfies it becomes
authoritative and fences the previous incarnation.  A stub that
accepts any proof is worse than a stub that refuses, because it looks
implemented.

Recommendation: implement TAKEOVER **last**, and until the proof
profile is real, keep it returning `NFS4ERR_NOTSUPP` rather than
accepting unverified proofs.  INSTALL / RELEASE / ENUMERATE are
useful without it in a single-incarnation deployment.

### 4. Enumerate cursor

`ceea_cookie` / `ceer_cookie` / `ceer_eof`, bounded by
`CHUNK_ESCROW_ENUMERATE_MAX4` (256) entries and a 256-byte cookie.
Standard NFSv4 cursor problem: the cookie has to stay meaningful
across a concurrent INSTALL or RELEASE.  Simplest correct answer is
an offset-ordered walk with the cookie encoding the next offset,
which degrades gracefully -- a concurrently released escrow is simply
absent from a later page.

## Ordering

1. **CHUNK_LOCK / CHUNK_UNLOCK** -- prerequisite, its own design.
   Includes `CHUNK_LOCK_FLAGS_ADOPT`, since adoption is the exit
   path for every escrow.
2. **Escrow id on the block record** -- on-disk field plus
   accessors.  Small once locks exist.
3. **INSTALL + RELEASE** -- the pair that creates and retires
   placeholder ownership.  Testable together.
4. **ENUMERATE** -- read-only over state (2) and (3) create.
5. **TAKEOVER** -- last, gated on a real proof profile.

## Tests

Per `.claude/roles.md`, before implementation:

Unit, once locks exist:

| test | intent |
|---|---|
| `escrow_install_marks_range` | INSTALL over a range leaves every block placeholder-owned with the given escrow id |
| `escrow_install_rejects_zero_id` | an all-zero escrow id is refused (pending the open question below) |
| `escrow_install_stale_epoch` | `ceia_mds_epoch` below current gives NFS4ERR_STALE_MDS_EPOCH |
| `escrow_release_matches_id` | RELEASE with the right id clears placeholder ownership |
| `escrow_release_wrong_id` | RELEASE naming a different id is refused, and the escrow survives |
| `escrow_enumerate_pages` | more than `CHUNK_ESCROW_ENUMERATE_MAX4` escrows paginate, `ceer_eof` only on the last |
| `escrow_enumerate_cookie_stable` | a release between pages does not corrupt the walk |
| `escrow_takeover_fences_prior` | after TAKEOVER, an operation at the prior epoch gets NFS4ERR_STALE_MDS_EPOCH |
| `escrow_takeover_epoch_mismatch` | `ceta_expected_prior_epoch` not matching current is refused |

Functional: a revoked client holding locks leaves escrows that
ENUMERATE can find and a repair actor can adopt -- which needs the
lock slice and a fixture that can revoke a stateid mid-operation.

Existing tests affected: none.  All four handlers currently return
NOTSUPP and nothing exercises them; there are no escrow tests today.

## Open questions

1. **Is an all-zero `escrow_id4` reserved?**  The block record needs
   a "not escrowed" encoding, and zero is the natural one, but only
   if the metadata server may never choose it.  If the draft does not
   say, it should -- and the reffs side needs a separate presence
   flag rather than assuming.
2. **Does epoch state belong per data server or per export?**  The
   draft says a data server has one authoritative metadata-server
   incarnation.  reffs can serve several exports from one process,
   and whether those share an epoch is unstated.
3. **What happens to escrows when the data server restarts?**  The
   locks are persistent state; the trust table that authorises the
   metadata server is not (it is rebuilt by re-registration).  The
   window between data-server restart and re-registration is
   unanalysed here.

## References

- Draft escrow model: `draft-haynes-nfsv4-flexfiles-v2`
  `sec-system-model-escrow`; wire mechanics in the New NFSv4.2
  Operations section.
- Op-number allocation: `XDR-MAP.md` in the flexfiles-v2 repo --
  92-95 escrow (base), 96-99 PROXY_* (proxy-server).
- reffs XDR: `lib/xdr/nfsv42_xdr.x`, escrow block around the
  `CHUNK_ESCROW_ENUMERATE_MAX4` constants.
- Stubs: `lib/nfs4/server/chunk.c`, the four
  `nfs4_op_chunk_escrow_*` handlers.
