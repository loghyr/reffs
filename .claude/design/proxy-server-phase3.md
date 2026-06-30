<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# PS Phase 3: `REFFS_DATA_PROXY` Backend + Client READ

Active-slice plan for proxy-server.md Phase 3.  See parent for
context and the broader 8-phase roadmap.

## Goal

Route NFSv4.2 READ on proxy SBs through `ec_pipeline` (LAYOUTGET +
CHUNK_READ + decode) instead of forwarding the read transparently
to the upstream MDS.

Today, `nfs4_op_read` on a proxy SB calls
`ps_proxy_forward_read()` which sends a plain READ to the upstream
MDS; the MDS does its own InBand I/O proxying to one mirror, no EC
encoding visible.  After this slice, the PS itself acquires a
layout, fans out CHUNK_READs to the mirror DSes, and decodes the
EC payload before returning bytes to the client.

This is the gate -- with Phase 4 (WRITE) -- for chunk-collision
Track 2: Linux NFS clients (or IOR via N PSes) drive real EC reads
through PS, and ec_demo's role as the only chunk-protocol driver
ends.

## Tests first

Per `.claude/roles.md` planner rule 1, tests identified before
implementation.

### New unit test file: `lib/backends/tests/proxy_data_test.c`

| Test | Intent |
|------|--------|
| `test_proxy_backend_compose_ok` | `reffs_backend_compose(REFFS_MD_RAM, REFFS_DATA_PROXY)` returns a valid ops struct with non-NULL `db_alloc`/`db_read` |
| `test_proxy_backend_compose_rejects_posix_md` | `compose(REFFS_MD_POSIX, REFFS_DATA_PROXY)` -> NULL (proxy data has no on-disk persistence; pairing with a persistent md axis is a constraint violation) |
| `test_proxy_backend_compose_rejects_rocksdb_md` | same for `compose(REFFS_MD_ROCKSDB, REFFS_DATA_PROXY)` |
| `test_proxy_db_alloc_no_fd` | `db_alloc` on a proxy SB succeeds with a stub data_block; `db_get_fd` returns -1 (no on-disk fd; data is fetched on demand) |
| `test_proxy_db_read_routes_to_pipeline` | `db_read` on a proxy SB invokes `ec_pipeline` with the file's upstream FH and the proxy SB's MDS session |
| `test_proxy_db_read_decodes_correctly_rs` | Mock LAYOUTGET + CHUNK_READ shards form a valid RS encoding; decode produces the expected plaintext |
| `test_proxy_db_read_decodes_correctly_mojette_sys` | Same for systematic Mojette |
| `test_proxy_db_read_partial_offset` | `db_read(buf, len, offset != 0)` reads only the requested byte range (stripe math is correct) |
| `test_proxy_db_read_short_read_eof` | `db_read` past EOF returns short read, no error |
| `test_proxy_db_read_session_unavailable` | Proxy SB whose listener has `pls_session == NULL` returns -ENOTCONN, no panic |
| `test_proxy_db_read_layout_stale` | Mock LAYOUTGET returns the same stateid twice, second time the DSes reject with NFS4ERR_BAD_STATEID -- pipeline's existing -ESTALE retry path covers this; verify no leak / no double-decrement of layout refs |

### Functional test: `lib/nfs4/ps/tests/ps_phase3_read_test.c` (new)

Single end-to-end test exercising the full PS read path.  Bring up
a single MDS + 4 DSes in-process (or use existing test harness),
write a known file via ec_pipeline directly, then read it through
the PS's READ op handler.  Assert bytes match.

NOT_NOW_BROWN_COW for in-process bring-up: if existing test harness
can't compose a 4-DS stack in-process, push this functional test
to `scripts/ci_ps_phase3_test.sh` (Docker-based).

### CI smoke: `scripts/ci_ps_phase3_test.sh`

Reuses the existing 1-MDS + 10-DS bench plus a single PS container
(profile from `run-ps-bench-bringup.sh`).  Steps:

1. Bring up bench + PS.
2. Pre-populate file F via `ec_demo write` on the MDS.
3. Mount PS's `:4098` from the bench-client container with NFSv4.2.
4. `cat /mnt/ps/F | sha256sum` matches sha256 of the original input.

Pass criteria: zero-error mount + cat + sha256 match.  Any
NFS4ERR returned to the client is a failure.

### Existing test impact

| File | Impact |
|------|--------|
| `lib/backends/tests/compose_test.c` | UPDATE -- existing constraint "md=RAM iff data=RAM" relaxes to "md=RAM iff data in {RAM, PROXY}".  Add `test_compose_ram_proxy_ok`; existing `test_compose_ram_posix_rejected` stays valid; existing `test_compose_posix_ram_rejected` stays valid. |
| `lib/nfs4/ps/tests/ps_*_test.c` | UPDATE -- READ-forwarder tests in `ps_proxy_ops_test` (if present) need to be re-scoped: today they verify forwarding to upstream; after this slice, READ on proxy SBs takes the `db_read` -> `ec_pipeline` path instead of the forwarder.  Either re-target the tests at the new path, or keep them as a "transparent-mode" test for non-EC-backed proxy SBs (NOT_NOW_BROWN_COW: PS today only mounts EC-backed proxy SBs, so the transparent READ path is reachable only via Phase 5 short-circuit fallback). |
| `make check` | PASS unchanged for everything else. |

## State / data structures

### `enum reffs_data_type` extension

```c
enum reffs_data_type {
    REFFS_DATA_RAM   = 0,
    REFFS_DATA_POSIX = 1,
    REFFS_DATA_PROXY = 2, /* NEW */
};
```

### Composition constraints

Today (`lib/backends/driver.c` `reffs_backend_compose`):
- `md=RAM` iff `data=RAM` (no partial persistence)

After Phase 3:
- `data=RAM` iff `md=RAM` (unchanged)
- `data=POSIX` iff `md in {POSIX, ROCKSDB}` (unchanged)
- `data=PROXY` iff `md=RAM` (new -- proxy SB metadata is RAM cache from upstream MDS; pairing with a persistent md axis is incoherent)

### `proxy_data_ops` template

New `lib/backends/proxy_data.c` exports a `reffs_data_ops` template:

```c
struct reffs_data_ops proxy_data_ops = {
    .db_alloc            = proxy_db_alloc,
    .db_free             = proxy_db_free,
    .db_release_resources= proxy_db_release_resources,
    .db_read             = proxy_db_read,
    .db_write            = proxy_db_write,    /* Phase 4: returns -ENOSYS today */
    .db_resize           = proxy_db_resize,   /* returns 0; size tracked on inode */
    .db_get_size         = proxy_db_get_size,
    .db_get_fd           = proxy_db_get_fd,   /* returns -1; no on-disk fd */
    .data_inode_cleanup  = proxy_data_inode_cleanup, /* no-op; nothing on disk */
};
```

### Proxy data_block

Stub struct that holds the per-inode coordinates needed by
`ec_pipeline`:

```c
struct proxy_data_block {
    struct inode *pdb_inode;       /* back-pointer for sb / FH lookup */
    size_t pdb_size;               /* logical size (set by db_write / extends; tracked) */
};
```

The actual upstream FH lives on `inode->i_sb->sb_proxy_binding`
(per Phase 2 ps_inode discovery); we don't duplicate it here.

The MDS session lives on `ps_state_find(binding->psb_listener_id)
->pls_session`; `proxy_db_read` looks it up at call time.

## RFC compliance

- Per draft-haynes-nfsv4-flexfiles-v2-proxy-server, the PS issues
  LAYOUTGET against the upstream MDS using its registered-PS
  privileges; the layout points at the same DSes the original
  end-client would have seen.
- Per RFC 8881 S18.43, LAYOUTGET request-response is synchronous.
- Per draft-haynes-nfsv4-flexfiles-v2 sec-CHUNK_READ, CHUNK_READ
  pulls bytes from the DS by chunk offset.
- Per draft-haynes-nfsv4-flexfiles-v2 sec-LAYOUTERROR, decode
  failures (CRC mismatch, BAD_STATEID after fence) flow back to
  the upstream MDS via LAYOUTERROR; existing pipeline retry path
  in `ec_pipeline` (commit `085012716b19`) handles this.

## State machines

No new state machines.  Existing pipeline state (layout grant
-> in-flight CHUNK_READ -> decode -> success/retry) is reused
verbatim.

## Persistence

None.  Proxy SB metadata is RAM (from Phase 2's discovery
cache); proxy data has no on-disk format.  Crash safety is on the
upstream MDS, not the PS.

## Security model

Forwarded credentials per RPC: end-client AUTH_SYS uid/gid forwards
into LAYOUTGET / CHUNK_READ via the `creds` parameter on
`mds_compound_send_with_auth` (already plumbed for transparent
forwarders, reused).

Squash + rule matching:
- `rpc_cred_squash()` runs before `db_read` (existing
  per-export-dstore plumbing).
- The matched client rule's `scr_root_squash` decides whether
  uid=0 squashes to 65534 in the outgoing compound.
- `client_rule_match()` against the proxy SB's
  `sb_client_rules[]` runs at PS receive time, before any
  forwarding decision.  Already plumbed.

Failure modes per draft sec-credential-forwarding rules:
- AUTH_NONE on inbound compound -> reject locally with
  NFS4ERR_ACCESS, no MDS round-trip.  Already enforced for
  forwarded ops; same hook covers `db_read`.
- Forwarded credentials use `REFFS_ID_MAKE(UNIX, 0, uid)` so the
  identity-refactor (when it lands) does not silently strip
  type/domain bits.

## Deferred / NOT_NOW_BROWN_COW

- **Phase 4: client WRITE.**  `proxy_db_write` returns `-ENOSYS`
  in this slice; Linux client WRITE on a proxy SB returns
  NFS4ERR_NOTSUPP until Phase 4 ships.  This is intentional --
  decoupling the read path lets us validate it independently.
- **Phase 5: co-resident DS short-circuit.**  Layout points at
  one of our own DSes -> bypass RPC, call local VFS.  Future
  optimization.
- **Read caching.**  Every `db_read` fires a fresh LAYOUTGET +
  CHUNK_READ (no cache).  delstid (RFC 9754) caching is the
  stretch goal in proxy-server.md action item 2.
- **Layout stateid lifecycle.**  Today the PS will get a layout,
  use it once, return it.  Multi-read reuse is NOT_NOW_BROWN_COW;
  it's an optimization on top of the basic path.
- **Async I/O.**  Synchronous via worker thread -- the dispatch
  thread blocks on `ec_pipeline_read`.  task_pause / task_resume
  for real async is in proxy-server.md deferred section.
- **AUTH_GSS forwarding.**  Forwarding RPCSEC_GSS credentials
  needs RPCSEC_GSSv3 structured privilege assertion (action item
  3 in proxy-server.md).  AUTH_SYS only for Phase 3.

## Admin interface

No new probe ops.  Proxy SB lifecycle stays as-is (created via
`[[proxy_mds]]` discovery; flushed at server shutdown).

## Implementation steps

### Step 1: Backend scaffolding

- Add `REFFS_DATA_PROXY` to `enum reffs_data_type` in
  `lib/include/reffs/backend.h`.
- Update `reffs_backend_compose()` constraint check in
  `lib/backends/driver.c` to allow `RAM/PROXY`.
- Add `proxy_data_templates[REFFS_DATA_PROXY] = &proxy_data_ops;`
  registration.
- Update `lib/backends/Makefile.am` to compile `proxy_data.c`.

### Step 2: `proxy_data.c` -- data_block primitives

Implement `proxy_db_alloc`, `proxy_db_free`,
`proxy_db_release_resources`, `proxy_db_resize`,
`proxy_db_get_size`, `proxy_db_get_fd` (returns -1),
`proxy_data_inode_cleanup` (no-op).

`db_write` is a stub returning `-ENOSYS` (Phase 4 territory).

### Step 3: `proxy_db_read` (the meat)

```c
ssize_t proxy_db_read(struct data_block *db, void *buf, size_t len,
                      off_t offset, struct rpc_creds *creds)
{
    struct inode *inode = ((struct proxy_data_block *)db)->pdb_inode;
    struct super_block *sb = inode->i_sb;
    const struct ps_sb_binding *binding = sb->sb_proxy_binding;

    if (!binding)
        return -EINVAL; /* not a proxy SB */

    const struct ps_listener_state *pls =
        ps_state_find(binding->psb_listener_id);
    if (!pls || !pls->pls_session)
        return -ENOTCONN;

    /* Resolve the upstream FH for THIS file from the inode's
     * per-inode binding (set by ps_inode at LOOKUP time).  Falls
     * back to the SB's anchor FH if the inode does not yet have
     * its own (root inode case). */
    const uint8_t *up_fh;
    uint32_t up_fh_len;
    ps_inode_get_upstream_fh(inode, &up_fh, &up_fh_len);

    /* Resolve a path string for ec_pipeline.  ec_read_encoding takes
     * a path; the existing PS code already maintains a string
     * representation per inode for forwarder use. */
    const char *path = ps_inode_path(inode);

    size_t out_len = 0;
    int ret = ec_read_encoding(pls->pls_session, path, buf, len,
                            &out_len, /* k */ 4, /* m */ 2,
                            EC_ENCODING_RS,    /* read encoding from
                                             * pre-computed sb_ec_encoding
                                             * (set at SB discovery) */
                            LAYOUT4_FLEX_FILES_V2,
                            /* skip_ds_mask */ 0,
                            /* shard_size */ 4096);
    if (ret)
        return ret;

    return (ssize_t)out_len;
}
```

Open issues for Step 3 to resolve in implementation:
- `ec_read_encoding` takes `(k, m, encoding, layout, shard_size)`.
  These are file-level properties.  Phase 2 discovery does NOT
  yet record k/m/encoding on the proxy SB or inode.  Either:
  (a) Derive at first read by issuing a LAYOUTGET-only-no-data
  to inspect the layout's `ffl_stripe_unit` and encoding fields, OR
  (b) Hard-code RS 4+2 v2 at 4096-byte shards for Phase 3 and
  push the parameter discovery to a follow-up.
  The plan picks (b) -- it's a Phase 3 vs file-attribute-cache
  scope split.
- Forwarded creds: `ec_read_encoding` doesn't take a `creds`
  parameter today.  It uses the session's default AUTH_SYS auth.
  For end-client cred forwarding, the pipeline needs a
  per-compound auth override (similar to
  `mds_compound_send_with_auth`).  Either:
  (a) Add a `creds` parameter to `ec_read_encoding` and thread it
  to all internal `mds_compound_send_with_auth` calls, OR
  (b) Set the session's default auth to the forwarded creds for
  the duration of the call (single-threaded session today, so
  serialized -- safe).
  The plan picks (a) -- (b) is a footgun if the session ever
  becomes shared.

### Step 4: SB allocator hook (post-alloc, no signature change)

`super_block_alloc()` for proxy SBs (called from
`lib/nfs4/ps/ps_sb_alloc.c`) needs to compose with
`md=RAM, data=PROXY` instead of the default `md=RAM, data=RAM`.

Approach: keep the existing `super_block_alloc()` signature.
Add a new public function:

```c
/*
 * Re-compose the SB's storage ops for a different data axis.
 * Caller must hold the SB exclusively (no other thread observes
 * sb_ops yet).  Used by the proxy-server subsystem to flip a
 * freshly allocated proxy SB from RAM/RAM to RAM/PROXY before
 * the dirent tree is created.  Returns -EINVAL if the (md, data)
 * pair violates the composition constraints.
 */
int super_block_set_data_axis(struct super_block *sb,
                              enum reffs_data_type data);
```

`ps_sb_alloc.c` calls this immediately after `super_block_alloc()`
and before `super_block_dirent_create()`.  The SB is single-owner
at that point; no concurrency.

### Step 5: `nfs4_op_read` routing -- keep the op-handler shape

The proxy_sb branch in `nfs4_op_read` (`lib/nfs4/server/file.c`
~line 1665) owns several concerns that don't belong in a
`db_read`:

- GSS-on-proxy rejection (currently NFS4ERR_WRONGSEC)
- Upstream FH resolution via `ps_inode_get_upstream_fh()`
- Listener / pls_session presence check (NFS4ERR_DELAY on miss)
- count clamping to NFS4_MAX_RW_SIZE
- `ps_proxy_read_reply` -> response bytes mapping
- `errno_to_nfs4(fret, OP_READ)` failure mapping

The cleanest factoring is to KEEP the proxy_sb branch and
replace JUST the inner forwarder call:

```c
/* before */
fret = ps_proxy_forward_read(
    pls->pls_session, upstream_fh, upstream_fh_len,
    args->stateid.seqid,
    (const uint8_t *)args->stateid.other, args->offset,
    req_count, &compound->c_ap, &reply);

/* after */
fret = ps_proxy_pipeline_read(
    pls->pls_session, upstream_fh, upstream_fh_len,
    args->offset, req_count, &compound->c_ap,
    reply.data, &reply.bytes_read);
```

Where `ps_proxy_pipeline_read()` is a new thin wrapper in
`lib/nfs4/ps/ps_proxy_ops.c` that translates the op-handler-
shaped call into an `ec_read_encoding()` call with the hard-coded
RS 4+2 v2 4096-byte parameters (see Risk #1 below).

The `db_read` side of `proxy_data_ops` is wired but is NOT
called by `nfs4_op_read` directly in this slice -- it's
reachable via `data_block_read()` from other paths
(GETATTR/SETATTR truncate, etc.) and exists for completeness.
The PRIMARY consumer for Phase 3 read traffic is the
`ps_proxy_pipeline_read()` shim invoked from the proxy_sb branch.

Tradeoff: `db_read` is wired but redundant for the main path in
this slice.  Phase 4 (WRITE) will collapse the same redundancy
on `db_write`.  Cleaner refactor possible in a follow-up once
both paths stabilise.

### Step 6: Wire tests

`lib/backends/tests/proxy_data_test.c` per the test inventory
above.  Use `compose_test.c` patterns + a stubbed ec_pipeline
mock (compile-time `__attribute__((weak))` override).

### Step 7: ci_ps_phase3_test.sh

End-to-end smoke per the CI Smoke section above.

### Step 8: Update parent design doc

`.claude/design/proxy-server.md` Phase 3 status -> DONE; cross-
reference this slice doc.

## Files to change

| File | Change |
|------|--------|
| `lib/include/reffs/backend.h` | Add `REFFS_DATA_PROXY` |
| `lib/backends/driver.c` | Composition constraint update; data_templates entry |
| `lib/backends/proxy_data.c` | NEW |
| `lib/backends/Makefile.am` | Wire `proxy_data.c` |
| `lib/fs/super_block.c` | Allocator hook for proxy SBs (md=RAM, data=PROXY) |
| `lib/nfs4/server/file.c` | Drop proxy short-circuit on READ |
| `lib/nfs4/ps/ec_pipeline.c` | No change in this slice; cred forwarding deferred to Phase 3.5 (see Risk #2) |
| `lib/nfs4/ps/ps_proxy_ops.c`, `ps_proxy_ops.h` | NEW `ps_proxy_pipeline_read` shim |
| `lib/nfs4/ps/ps_inode.c`, `ps_inode.h` | Expose `ps_inode_get_upstream_fh()` and `ps_inode_path()` if not already public |
| `lib/backends/tests/compose_test.c` | Add `test_compose_ram_proxy_ok` |
| `lib/backends/tests/proxy_data_test.c` | NEW |
| `scripts/ci_ps_phase3_test.sh` | NEW |
| `.claude/design/proxy-server.md` | Phase 3 status -> DONE |

## Reviewer checklist (planner self-review)

Per `.claude/roles.md` reviewer rule 1, "where are the unit
tests" -- listed up front in "Tests first".

Per rule 2 design compliance: this slice maps 1:1 to Phase 3 in
proxy-server.md.  No unplanned items.

Per rule 3 standards compliance: atomic ops are not in scope (no
new shared state; the proxy_data_block lives per-inode).  RCU is
not in scope.  Lock ordering is not in scope (the `ec_pipeline`
call holds no reffs locks; it owns its own MDS-session call
mutex).

Per rule 4 security review: forwarded creds are end-client creds
(not PS service creds); root_squash and AUTH_NONE rejection
happen at the receive layer before `db_read`; covered explicitly
above.

Per rule 5 RFC compliance: cited above.

Per rule 6 concurrency: synchronous, no task_pause/resume, no
new async path.

Per rule 8 on-disk format versioning: no on-disk format
introduced.

Per rule 9 XDR file review: no protocol XDR changes (probe XDR
unaffected; nfsv42_xdr.x unaffected; no new probe op).

Per rule 10 UUID stability: no new long-lived objects.

## Risks

1. **Hard-coded `k=4 m=2 v2 RS 4096`** (BLOCKER for Phase 3 to
   be useful, not just a risk).  If the bench writes Mojette but
   PS reads RS, every read fails with a decode error that LOOKS
   like a correctness bug.

   Decision for this slice: explicitly pin the Phase 3 test matrix
   to RS 4+2 v2 4096 only.  The `ps_proxy_pipeline_read()` shim
   passes those constants verbatim.  An incoming layout whose
   parameters differ (k != 4, m != 2, encoding != RS) returns
   NFS4ERR_NOTSUPP with a `LOG()` line citing the mismatch.

   Parameter discovery (read encoding/k/m from the layout's first
   LAYOUTGET response and cache on inode) is its own slice -- it
   cuts across PS, ec_pipeline, and the inode-attribute cache,
   and shouldn't be wedged into Phase 3.  Tracked as a
   NOT_NOW_BROWN_COW in `proxy-server.md` action items.
2. **End-client cred forwarding deferred to Phase 3.5.**  The
   existing transparent forwarder `ps_proxy_forward_read` passes
   `&compound->c_ap` to a `mds_compound_send_with_auth` call --
   end-client AUTH_SYS creds reach the upstream MDS verbatim.

   The `ec_pipeline` path is structurally different: it issues
   multiple compounds (LAYOUTGET, GETDEVICEINFO, CHUNK_READ,
   sometimes LAYOUTERROR + retry) via internal helpers
   (`mds_getdeviceinfo`, `mds_layout_error`, etc.) that all
   default to the session's `cl_auth`.  Threading a `creds`
   parameter end-to-end is a deeper refactor than the rest of
   Phase 3.

   Decision for this slice: use the PS session's default auth
   (PS service identity, registered-PS-allowlisted) for all
   pipeline compounds.  Bench testing is acceptable because:
   - Trust-stateid mode: DS-side authorization is via the
     layout stateid the MDS issued, not the connecting cred --
     end-client creds don't enter the DS picture.
   - MDS-side authorization for LAYOUTGET: registered-PS sessions
     bypass export-rule filtering for namespace traversal per
     proxy-server.md security section; LAYOUTGET / CHUNK_READ
     fall through into normal authorization where PS service
     identity is what the MDS sees.

   This is a deviation from proxy-server.md "PS forwards
   end-client credentials" for the EC-decode path.  Tracked as
   a NOT_NOW_BROWN_COW: Phase 3.5 will add a `creds` parameter
   to `ec_read_encoding` and thread it through the helpers.  Spec
   compliance for non-trust-stateid DSes lands then.
3. **`super_block_alloc()` signature change** -- avoided via
   the post-alloc hook approach in Step 4.  No callers outside
   `ps_sb_alloc.c` are affected.
4. **Functional CI test depends on the bench being up.**
   `ci_ps_phase3_test.sh` runs in the bench container, mounts
   `:4098`, requires Linux NFSv4.2 client.  Mitigation: gate
   the CI test on a `ci-ps` Make target so it doesn't break the
   default `ci-check` flow if the bench infrastructure is not
   available.

## Cross-references

- `.claude/design/proxy-server.md` -- parent (Phase 3 section)
- `.claude/design/dstore-vtable-v2.md` -- backend composition
- `.claude/design/chunk-collision-validation.md` -- Track 2
  unblocks after Phase 4
- `lib/nfs4/ps/ec_pipeline.c` -- existing pipeline implementation
- `lib/nfs4/ps/ps_sb.c`, `ps_inode.c` -- existing PS infrastructure
