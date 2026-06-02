<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# PS Phase 4a: Client WRITE through pipeline -- whole-file mode

Active-slice plan for the first half of `proxy-server.md` Phase 4.
See parent for context and Phase 4b (per-stripe RMW) for the
follow-on that completes the spec-compliant story.

## Status (shipped 2026-05-12)

Phase 4a is **structurally complete**: every client-visible NFSv4
op on a proxy SB (WRITE / COMMIT / CLOSE) now routes through the
buffer-and-flush pipeline.  Slices shipped:

| Slice | Commit          | Step | What                                                                      |
|-------|-----------------|------|---------------------------------------------------------------------------|
| 4a.1  | `8d56911b90df`  | 1    | `ec_write_encoding_with_file` factor-out + creds threading                   |
| 4a.2a | `61aa4b600091`  | 3    | Write-buffer table + per-listener lifecycle (quiesce protocol)            |
| 4a.2b | `b60dabdbfbad`  | 4    | `ps_proxy_pipeline_write` shim                                            |
| 4a.2c | `44274cf98fa8`  | 5    | `ps_proxy_pipeline_commit` shim                                           |
| 4a.3  | `dc2a0d515d5d`  | 6+7  | `ps_proxy_pipeline_close` shim + op-handler dispatch flip                 |
| 4a.4  | `22c30ab24f8c`  | 8    | `ps-write-buffer-stats` probe op + observability counters                 |

Three implementation steps from the design ship as deferred
follow-ons (each is its own slice's worth of work, called out
inline below):

- **Step 2** -- `proxy_data_pipeline_write` helper in
  `lib/backends/proxy_data.c`.  The shim ended up routing directly
  through `ps_proxy_ops.c` without going through `data_block` (the
  pipeline path bypasses the data-block hook entirely).  Helper
  not needed in 4a.
- **Step 9** -- `[[ps]] write_buffer_max_bytes` TOML config.
  Today `REFFS_PS_WRITE_BUFFER_MAX` is a compile-time constant
  (1 GiB).  Operator-tunable runtime cap is a follow-on slice.
- **Step 10** -- `scripts/ci_ps_phase4a_test.sh` end-to-end
  bench test.  Requires standing up the bench docker-compose +
  Linux NFS client mount + `cp + diff` harness; deferred to the
  bench-integration arc.

Unit-test coverage on Fedora 44 / clang 22: 142/142 PASS across
the full `make check` suite, including 30 new tests across 4 new
test files specifically for Phase 4a slices.

## Goal

Wire client WRITE on proxy-SB files through the EC pipeline by
buffering NFSv4 WRITE traffic per `(open stateid, upstream FH)` and
flushing the buffer through `ec_write_encoding_with_file` on COMMIT (or
on CLOSE if the client never issues an explicit COMMIT).  All WRITEs
return `committed = UNSTABLE4` and a PS-minted write verifier; the
encode + DS-side CHUNK_WRITE + FINALIZE + COMMIT happens at flush
time.

This unblocks:
- `cp largefile client:PS-mount/file` end-to-end
- IOR `-F 1 -W -R` (file-per-process) -- each rank gets its own
  file, single writer per file, no chunk-level contention
- ec_demo-equivalent functional matrix on the PS surface

This does **not** unblock Track 2 chunk-collision IOR `-F 0 -W -R -C`
(shared file, multi-writer).  That is Phase 4b.  Calling out the
gap up front because Phase 4a alone is **not** the
chunk-collision-validation gate -- Phase 4b is.

## Tests first

Per `.claude/standards.md` rule 1 (tests-first), the test surface is
the contract.  All tests live in `lib/backends/tests/proxy_data_test.c`
and `lib/nfs4/ps/tests/ps_proxy_pipeline_write_test.c` (NEW).

| Test | Intent |
|------|--------|
| `test_write_buffer_alloc_per_stateid` | First WRITE on `(stateid, fh)` allocates a buffer entry; second WRITE on same key reuses it. |
| `test_write_buffer_appends_in_order` | Two contiguous WRITEs at offsets 0 and 64K land in the buffer at those offsets and read back as a 128K contiguous payload. |
| `test_write_buffer_sparse_holes_zero_filled` | WRITEs at offsets 0 and 1 MiB leave the gap as zeros in the buffer (matches sparse-file POSIX semantics). |
| `test_write_buffer_overwrite_replaces` | WRITE at offset 0 followed by WRITE at offset 0 with different bytes leaves the second writer's bytes in the buffer. |
| `test_write_buffer_size_cap_returns_delay` | A WRITE that would push the buffer past the configured cap returns NFS4ERR_DELAY; the prior buffer state is unchanged. |
| `test_write_returns_unstable_with_verifier` | Every WRITE reply has `committed = UNSTABLE4` and a non-zero PS-listener verifier. |
| `test_commit_flushes_buffer_via_pipeline` | COMMIT on a buffered file invokes `ec_write_encoding_with_file` exactly once with the buffer's contents and the file's upstream FH + stateid. |
| `test_commit_returns_same_verifier_as_writes` | COMMIT's `writeverf` matches the verifier returned on prior WRITEs (within the same listener generation). |
| `test_commit_drops_buffer_on_success` | After successful COMMIT the buffer entry is freed; a subsequent WRITE allocates a fresh buffer. |
| `test_commit_keeps_buffer_on_failure` | On `ec_write_encoding_with_file` failure, COMMIT returns NFS4ERR_IO and the buffer is **not** freed (client may retry COMMIT). |
| `test_close_implicit_flush` | CLOSE on a stateid with a non-empty buffer triggers the same flush as COMMIT; CLOSE returns OK iff flush succeeded. |
| `test_close_drops_buffer_after_implicit_flush` | After CLOSE-driven flush the buffer entry is freed even if the client never sent COMMIT. |
| `test_close_buffer_cleanup_on_error` | If implicit flush fails on CLOSE, the buffer is freed anyway (CLOSE has no retry surface) and a TRACE line records the loss. |
| `test_write_encoding_with_file_smoke` | `ec_write_encoding_with_file` against a mock MDS+DS stack: encodes, sends CHUNK_WRITE + FINALIZE + COMMIT, returns 0. |
| `test_write_encoding_with_file_propagates_creds` | The compound stream sees the supplied `authunix_parms` on every LAYOUTGET / LAYOUTRETURN (mirror of slice b2's read-side test). |
| `test_write_encoding_with_file_null_creds_default_auth` | `creds == NULL` falls back to the session's default auth (PS service identity) -- bit-identical to pre-Phase-3.5 behaviour. |
| `test_listener_shutdown_drains_buffers` | On `ps_listener_stop`, all buffered writes are dropped (no pipeline flush attempted) and the buffer table is emptied; no leaks under valgrind. |
| `test_listener_shutdown_waits_for_inflight_commit` | One thread is mid-`ec_write_encoding_with_file` holding a find ref; another calls `ps_listener_stop`. Teardown blocks on `pls_active_buffer_refs`, completes after the COMMIT thread drops its ref. No UAF under TSAN; verifier or NFS4ERR_DELAY observed by client, never freed memory access. |
| `test_listener_shutdown_quiesce_deterministic` | Companion to the TSAN race test, using a test-only hook (see "Test-hook injection" below): the test thread arms a condvar inside `ec_write_encoding_with_file`, calls `ps_listener_stop` on a second thread, asserts the second thread is blocked in `pthread_cond_wait` on `pls_drain_cv`, signals the encoding to proceed, asserts teardown unblocks within 1 s and the table is empty.  Deterministic counterpart so we have a regression test for the quiesce ordering even without TSAN. |
| `test_listener_toctou_state_check_after_increment` | Whitebox: function-pointer hook (see "Test-hook injection") inserts a CPU pause between `enter_quiesce_or_bail`'s `fetch_add` and the `pls_state` load; teardown sets DRAINING during the pause; verify the op handler observes DRAINING on re-load, decrements via leave_quiesce, and returns NFS4ERR_DELAY without taking a find ref.  Pins the TOCTOU fix from verdict-2 BLOCKER NEW-2. |
| `test_close_flush_timeout_proceeds_to_forward` | Test-only hook stalls `ec_write_encoding_with_file` past `REFFS_PS_FLUSH_TIMEOUT_NS`; verify CLOSE drops the buffer, logs the timeout, increments `close_flush_timeouts_total`, and still invokes `ps_proxy_forward_close` (so the upstream stateid is released). |
| `test_forward_read_on_draining_returns_delay` | After listener teardown sets `pls_state = DRAINING`, a forward READ (non-pipeline path from prior slices) calling `ps_listener_session_borrow` observes NULL and the dispatcher maps to NFS4ERR_DELAY -- pins the new listener-borrow contract for non-pipeline callers. |
| `test_listener_draining_state_returns_delay` | After teardown sets `pls_state = DRAINING`, a fresh WRITE / COMMIT on the listener returns NFS4ERR_DELAY without entering the buffer table. |
| `test_concurrent_writes_same_stateid_serialize` | Two threads issuing WRITEs on the same stateid serialise on the per-buffer mutex; final buffer state matches the well-defined ordering. |
| `test_concurrent_writes_distinct_stateids_documented_loss` | Two clients with distinct stateids write the same FH and COMMIT in sequence; second COMMIT clobbers first. Test asserts the documented Phase-4a-broken outcome so future Phase 4b work has a regression test to flip. |
| `test_listener_id_collision_distinct_buffers` | Two listeners with the same numeric id but distinct boot generations writing the same FH: each gets its own buffer (key includes `pwb_listener_id` + `pwb_listener_gen`); no cross-leak. Pins the listener-id-stays-stable-but-gen-distinguishes contract. |
| `test_write_larger_than_cap_returns_fbig` | A single WRITE with `data.data_len > REFFS_PS_WRITE_BUFFER_MAX` returns NFS4ERR_FBIG; no buffer is allocated. |
| `test_commit_layout_get_retry` | Pipeline COMMIT where the first LAYOUTGET returns NFS4ERR_LAYOUTTRYLATER: pipeline retries, second LAYOUTGET succeeds, COMMIT returns OK. |
| `test_commit_partial_ds_failure` | One of `k+m` DSes times out during CHUNK_WRITE: pipeline reports the DS error to the MDS, retries via re-LAYOUTGET, completes. Pins the existing pipeline behaviour. |
| `test_commit_finalize_partial_failure` | FINALIZE succeeds on `j` of `k+m` DSes then fails: COMMIT returns NFS4ERR_IO; buffer kept (per state machine); upstream is half-finalized -- documented as an inherent limitation pending Phase 4b's per-stripe RMW + cleanup. |
| `test_pipeline_write_gss_refused` | `nfs4_op_write` proxy branch refuses GSS-flavoured compounds with NFS4ERR_WRONGSEC (mirror of READ-path check). |

### Test-hook injection

`ps_write_buffer_internal.h` (the whitebox-only header listed in
"Files to change") declares three `_Atomic` function-pointer
slots, default-initialised to NULL:

```c
extern _Atomic(void (*)(void)) ps_test_hook_pre_state_load;
extern _Atomic(void (*)(void)) ps_test_hook_in_encoding_flush;
extern _Atomic(uint64_t (*)(void)) ps_test_hook_clock_now_ns;
```

Production code at the relevant points loads the slot with
`atomic_load_explicit(..., memory_order_relaxed)` and skips the
call if NULL (one branch, predicted-taken).  Tests that need to
inject behaviour `atomic_store_explicit` a callback before
exercising the path and clear it on teardown.  Using `_Atomic`
rather than plain function pointers avoids a C11 data race --
word-sized loads/stores are atomic on every supported arch, but
the standard says undefined behaviour without `_Atomic` and
sanitiser builds (especially TSAN) will flag it.

This avoids `#ifdef TEST_*` carve-outs (which contaminate
production compiles and break sanitiser builds) and avoids weak
symbols (which break LTO).  Same shape that
`lib/nfs4/ps/ps_renewal_internal.h` already uses for the
renewal-tick test hook.

**Functional test** (`scripts/ci_ps_phase4a_test.sh`, NEW): from a
Linux NFSv4.2 client mounted on `:4098`, `cp /tmp/random_256m
ps-mount/f && diff /tmp/random_256m ps-mount/f`.  Bytes match.

**CI**: `ci_ps_phase4a_test.sh` runs in the bench docker-compose and
is gated on the new `[[ps-phase4a]]` runner block in the bench
config (NOT_NOW_BROWN_COW: hook into the existing PS bring-up runner
in a follow-on once 4a is green on dreamer).

## State / data structures

New:

```c
struct ps_write_buffer {
    /* Key */
    uint8_t pwb_stateid_other[PS_STATEID_OTHER_SIZE];
    uint8_t pwb_upstream_fh[PS_MAX_FH_SIZE];
    uint32_t pwb_upstream_fh_len;
    uint32_t pwb_listener_id;

    /* Bytes */
    uint8_t *pwb_data;        /* malloc, grows as needed */
    size_t pwb_capacity;      /* allocated bytes */
    size_t pwb_high_water;    /* highest (offset+count) seen */

    /* Bookkeeping */
    pthread_mutex_t pwb_mutex;       /* leaf lock; serialises buffer mutation */
    struct urcu_ref pwb_ref;         /* table ref + per-op find refs */
    uint64_t pwb_listener_gen;       /* listener boot generation snapshot */
    struct cds_lfht_node pwb_ht_node;
};
```

Lookup: a per-listener `cds_lfht` keyed by
`hash(stateid_other || upstream_fh)`.  Iteration follows the
patterns/ref-counting.md Rule 6 lifecycle:

- Insert under `rcu_read_lock`; new buffer carries the **table
  ref** (`urcu_ref_init` to 1).
- Lookup takes a **find ref** via `urcu_ref_get_unless_zero`
  (skips entries already being torn down).
- Op-handler unlocks RCU after taking the find ref, then takes
  `pwb_mutex` for the actual buffer mutation.
- On normal drop (successful COMMIT or implicit CLOSE flush): the
  op handler calls `cds_lfht_del` to make the entry unfindable,
  then drops both refs (find ref + table ref); the release
  callback `call_rcu`'s the free.
- Listener teardown (see `ps_listener_stop` in State machines)
  walks the table, advances iterator before dropping refs (per
  rcu-violations.md Pattern 7), and uses `synchronize_rcu` after
  the drain before destroying the lfht.

The `pwb_mutex` is leaf-most among PS locks: nothing else is
acquired while it is held.  See Reviewer checklist rule 4 for the
full ordering.

Listener state gains a `_Atomic enum ps_listener_state pls_state`
with values `{RUNNING, DRAINING, STOPPED}`.  WRITEs and COMMITs
that observe `pls_state != RUNNING` return NFS4ERR_DELAY without
touching the buffer table.  Teardown sets `DRAINING`, waits for
`pls_active_buffer_refs` (see Persistence) to reach zero, then
walks the table to drop table refs.

Sized small (`PS_WRITE_BUFFER_HASH_BUCKETS = 64`).  Files in flight
through PS at any one moment are bounded; oversizing would waste RAM.

Cap: `pwb_capacity <= REFFS_PS_WRITE_BUFFER_MAX` (default 1 GiB,
configurable via `[[ps]] write_buffer_max_bytes` in `reffs.toml`).

Flush timeout: `REFFS_PS_FLUSH_TIMEOUT_NS` (default 30 s,
configurable via `[[ps]] flush_timeout_ms`).  Used by
CLOSE-flush to bound the time spent in
`ec_write_encoding_with_file` before giving up and proceeding to the
forward CLOSE -- prevents the client-CLOSE timeout from leaking
the upstream open stateid (see Risk #7).
Two distinct cap-overflow cases:

1. **Single WRITE byte count > cap**: `args->data.data_len > cap`.
   No amount of waiting can succeed.  Returns
   `-EFBIG` --> NFS4ERR_FBIG.
2. **Buffered total (existing + new) > cap**: transient pressure
   from in-flight buffered writes from other clients.  Returns
   `-EAGAIN` --> NFS4ERR_DELAY; client backs off and retries.

Listener generation: `pls_boot_gen` on `struct ps_listener_state`
(C11 `_Atomic uint64_t`), incremented on every `ps_listener_start`.
Buffers carry the gen at allocation time; a buffer whose gen does
not match the current listener gen at COMMIT/flush time is treated
as orphaned (returns NFS4ERR_STALE rather than flushing stale
bytes upstream).

## RFC compliance

Per `draft-haynes-nfsv4-flexfiles-v2-proxy-server` sec-write-path:

- **All proxy WRITE goes through the pipeline path; no fallback
  to the legacy forward path.**  Phase 4a *replaces*
  `ps_proxy_forward_write` for the WRITE branch.  This avoids the
  PS-listener-verifier vs. MDS-verifier mismatch that a mixed
  dispatch would produce: a single client+file always sees
  exactly one verifier source within a listener generation.  The
  forward path remains used by READDIR, GETATTR, LOOKUP, etc. --
  only WRITE / COMMIT / CLOSE switch to pipeline.
- WRITE with `stable = UNSTABLE4` is the spec's nominal mode for
  proxy writes.  Phase 4a only supports UNSTABLE4.  Clients
  requesting `DATA_SYNC4` or `FILE_SYNC4` get back
  `committed = UNSTABLE4` and a verifier; the spec permits the
  server to downgrade.  Documented as a deviation in Risks.
- Write verifier is per-PS-listener (boot generation bytes packed
  into 8 bytes via the existing `nfs4_write_verf` helper).  A
  listener restart invalidates verifiers, which surfaces to the
  client as a verifier mismatch on COMMIT and the standard
  client-side rewrite kicks in.
- **COMMIT range honouring (Phase 4a model)**: COMMIT flushes the
  *entire* buffered prefix `[0, high_water)` regardless of
  `args->offset` / `args->count`.  Per RFC 8881 S18.3.4 the
  server MAY commit more than the requested range, so this is
  spec-compliant.  The buffer is dropped after a successful flush,
  so a subsequent COMMIT (for an unflushed range) returns success
  with the listener verifier and is a no-op.  Partial-range
  COMMIT semantics with coverage-bitmap tracking is Phase 4b
  territory.
- **No fallback for COMMIT either**: a COMMIT on a proxy SB with
  no buffered writes returns success-with-listener-verifier
  directly (e.g., COMMIT on a file the client only ever read).
  The forward COMMIT path is removed for proxy SBs to keep the
  verifier source uniform.
- For the in-spec partial-stripe and shared-file-multi-writer
  cases, see Phase 4b.  Phase 4a explicitly punts on the multi-
  writer-shared-file case: the second client's WRITEs land in a
  separate buffer keyed by a different stateid, and at COMMIT the
  pipeline encode of "the whole buffer" overwrites the first
  client's stripes.  This is **incorrect** per the spec but is the
  documented Phase 4a limitation.

## State machines

### Quiesce protocol

Three primitives synchronise op handlers with listener teardown:

- `pls_state` (`_Atomic enum {RUNNING, DRAINING, STOPPED}`) --
  the **DRAINING gate**.  Op handlers must observe `RUNNING`
  AFTER they have published their intent (see TOCTOU below).
- `pls_active_buffer_refs` (`_Atomic uint64_t`) -- the **active-op
  counter**: number of op-handler threads that may still touch a
  buffer or look up the table.  This is broader than "find-ref
  count": the counter is incremented BEFORE the state check, so
  during the lookup-or-bail window a thread may hold the counter
  but no buffer ref.  Teardown's invariant is "counter == 0 ⇒
  no in-flight op may race the destroy walk", not "counter == n
  ⇒ n find refs held".  Decremented when the op finishes
  (regardless of whether a buffer was actually touched).
- `pls_drain_cv` + `pls_drain_mutex` -- the wakeup signal so
  `ps_listener_stop` does not spin while waiting for the counter
  to reach zero.

**TOCTOU resolution.**  The naive ordering "check `pls_state`
first, then take a find ref" has a race: an op handler reads
`RUNNING`, the scheduler suspends it, teardown stores `DRAINING`
and drains (sees zero refs because the op had not incremented
yet), the op resumes and grabs a find ref on a buffer that
`ps_listener_stop` is about to free.  The fix mirrors the
lock-free quiesce pattern from `lib/nfs4/server/ps_renewal.c`:

1. `atomic_fetch_add_explicit(&active_refs, 1, memory_order_acq_rel)`.
2. Re-load `pls_state` with `memory_order_acquire`.
3. If state is not `RUNNING`, decrement immediately
   (`acq_rel` so any prior writes are visible) and bail.
4. Otherwise, proceed with `cds_lfht_lookup` /
   `urcu_ref_get_unless_zero`.

Teardown does the symmetric publish-then-observe:

1. `atomic_store_explicit(&pls_state, DRAINING, memory_order_release)`.
2. `atomic_load_explicit(&active_refs, memory_order_acquire)`
   in the wait loop.

Release-on-store + acquire-on-load on the same atomic
(`pls_state`) gives the inter-thread happens-before that the
counter handshake needs.

**RCU registration.**  All threads that enter the `cds_lfht`
read-side (`cds_lfht_lookup`, iteration in `ps_listener_stop`)
must be RCU-registered.  In reffs:

- RPC worker threads (`io_uring` event-loop threads in
  `lib/io/`) are already registered in
  `io_handler_init` -- op handlers inherit registration from
  the worker pool.  No change needed.
- The listener-teardown path runs on the listener-control
  thread that owns `ps_listener_state`.  That thread is
  registered at `ps_listener_start`; no change needed for 4a.

If a future caller invokes `ps_listener_stop` from an
unregistered context (e.g., a probe op driven by a fresh
thread), the teardown's `cds_lfht_first` walk will silently
not see RCU-deferred frees and may fault.  Document at the
public `ps_listener_stop` declaration that callers must be
RCU-registered.

`drop_buffer(buffer)` below is shorthand for: `cds_lfht_del`
under `rcu_read_lock` (idempotent), then drop the table ref;
the release callback `call_rcu`'s the free.  Find refs are
dropped separately via the helpers below.

```
/* Quiesce primitives.  All four call sites in the handlers below
 * route through these so the counter accounting + cv-wake live in
 * one place. */

leave_quiesce(pls):
    new = atomic_fetch_sub_explicit(&pls->pls_active_buffer_refs,
                                    1, memory_order_acq_rel)
    if (new == 1):  /* we were the last */
        /* Standard cv-predicate discipline: teardown LOADS the
         * counter under pls_drain_mutex; we MUST take the same
         * mutex around the broadcast to avoid a lost-wakeup race
         * (teardown reads counter > 0, we fetch_sub to 0 +
         * broadcast before teardown enters cond_wait -- without
         * the mutex teardown would wait forever). */
        pthread_mutex_lock(&pls->pls_drain_mutex)
        pthread_cond_broadcast(&pls->pls_drain_cv)
        pthread_mutex_unlock(&pls->pls_drain_mutex)

take_find_ref(buffer):
    urcu_ref_get(&buffer->pwb_ref)
    /* counter already incremented by enter_quiesce_or_bail();
     * no separate fetch_add here */

drop_find_ref(buffer, pls):
    /* Order: urcu_ref_put FIRST so the release callback (which
     * does cds_lfht_del + call_rcu) runs before we wake teardown.
     * If teardown wakes and walks the table, the just-deleted
     * entry is already invisible (cds_lfht_del is idempotent and
     * concurrent iterators skip removed nodes per liburcu
     * semantics).  This ordering is intentional. */
    urcu_ref_put(&buffer->pwb_ref, release_cb)
    leave_quiesce(pls)

/* Common entry: bump active_refs FIRST, then check the gate.
 * Returns true if the op may proceed; on false the active_refs
 * decrement has already happened (via leave_quiesce) and the
 * caller bails. */
enter_quiesce_or_bail(pls) -> bool:
    atomic_fetch_add_explicit(&pls->pls_active_buffer_refs,
                              1, memory_order_acq_rel)
    state = atomic_load_explicit(&pls->pls_state,
                                 memory_order_acquire)
    if (state != RUNNING):
        leave_quiesce(pls)
        return false
    return true

/* RCU + mutex ordering.  alloc_and_insert opens its own
 * rcu_read_lock for cds_lfht_add and releases it before
 * returning, so callers can safely acquire buffer->pwb_mutex
 * (a blocking mutex) after the call -- never under RCU
 * read-side per patterns/rcu-violations.md Pattern 1.  The
 * lookup paths below also do rcu_read_unlock() BEFORE the
 * pwb_mutex acquisition. */

WRITE arrives:
  if (args.data.data_len > REFFS_PS_WRITE_BUFFER_MAX):
      return NFS4ERR_FBIG       /* no buffer touched yet */
  if (!enter_quiesce_or_bail(pls)):
      return NFS4ERR_DELAY      /* listener draining or stopped */

  rcu_read_lock()
  node = cds_lfht_lookup(table, hash(stateid, fh))
  buffer = NULL
  if (node):
      buffer = container_of(node, ...)
      if (!urcu_ref_get_unless_zero(&buffer->pwb_ref)):
          buffer = NULL          /* losing race with teardown */
  rcu_read_unlock()

  if (!buffer):
      buffer = alloc_and_insert(stateid, fh, current_listener_gen)
      /* urcu_ref_init to 1 (table ref); plus implicit find ref
       * for this op (we just inserted, no urcu_ref_get_unless_zero). */
      urcu_ref_get(&buffer->pwb_ref)
  /* From here, buffer holds 2 refs: table + find.  active_refs
   * counter already incremented by enter_quiesce_or_bail. */

  lock(buffer.pwb_mutex)
  if (buffer.pwb_listener_gen != atomic_load_explicit(
          &pls->pls_boot_gen, memory_order_acquire)):
      /* stale buffer (listener restart between alloc and now);
       * drop and reject -- client will rewrite under the new gen. */
      unlock(buffer.pwb_mutex)
      drop_buffer(buffer)            /* removes table ref */
      drop_find_ref(buffer, pls)     /* removes find ref + decs counter */
      return NFS4ERR_STALE

  if (offset + count > buffer.pwb_capacity):
      if (offset + count > REFFS_PS_WRITE_BUFFER_MAX):
          unlock(buffer.pwb_mutex)
          drop_find_ref(buffer, pls)
          return NFS4ERR_DELAY        /* transient cap pressure */
      grow_or_fail(buffer, offset + count)

  memcpy(buffer.pwb_data + offset, args.data, count)
  buffer.pwb_high_water = max(buffer.pwb_high_water, offset + count)
  unlock(buffer.pwb_mutex)
  drop_find_ref(buffer, pls)
  reply: count=count, committed=UNSTABLE4, verf=listener_verf

COMMIT arrives:
  if (!enter_quiesce_or_bail(pls)):
      return NFS4ERR_DELAY

  rcu_read_lock()
  node = cds_lfht_lookup(table, hash(stateid, fh))
  buffer = NULL
  if (node):
      buffer = container_of(node, ...)
      if (!urcu_ref_get_unless_zero(&buffer->pwb_ref)):
          buffer = NULL
  rcu_read_unlock()

  if (!buffer):
      /* No buffered bytes -- spec-permitted no-op COMMIT.
       * See "COMMIT empty-buffer semantics" note below for the
       * client per-(stateid, fh) serialization assumption. */
      leave_quiesce(pls)
      reply: writeverf=listener_verf
      return

  lock(buffer.pwb_mutex)
  if (buffer.pwb_listener_gen != atomic_load_explicit(
          &pls->pls_boot_gen, memory_order_acquire)):
      unlock(buffer.pwb_mutex)
      drop_buffer(buffer)
      drop_find_ref(buffer, pls)
      return NFS4ERR_STALE

  /* Phase 4a: flush the entire buffered prefix regardless of
   * args.offset / args.count.  Spec permits the server to commit
   * more than the requested range. */
  ret = ec_write_encoding_with_file(ms, &mf, buffer.pwb_data,
                                 buffer.pwb_high_water,
                                 k=4, m=2, EC_ENCODING_RS,
                                 LAYOUT4_FLEX_FILES_V2,
                                 shard_size=4096, creds)

  if (ret == 0):
      unlock(buffer.pwb_mutex)
      drop_buffer(buffer)              /* removes table ref */
      drop_find_ref(buffer, pls)       /* removes find ref + decs counter */
      reply: writeverf=listener_verf
  else:
      unlock(buffer.pwb_mutex)
      drop_find_ref(buffer, pls)       /* keep table ref so retry works */
      return NFS4ERR_IO

CLOSE arrives:
  /* CLOSE has no retry surface; we want the upstream MDS to see
   * the close even if the pipeline flush fails or hangs.  Bound
   * the flush at REFFS_PS_FLUSH_TIMEOUT_NS (default 30s); if
   * exceeded, drop the buffer (bytes lost, operator-visible
   * warning) and proceed to the forward CLOSE so the upstream
   * stateid does not leak.  See Risk #6 / Risk #7. */
  if (!enter_quiesce_or_bail(pls)):
      /* Listener is draining; the underlying upstream session is
       * being torn down too.  ps_listener_session_borrow() returns
       * NULL for non-RUNNING listeners (see "Listener-borrow
       * contract" below), so a forward CLOSE here would itself
       * map to NFS4ERR_DELAY.  Skip the forward and reply
       * NFS4ERR_DELAY directly -- the client retries against
       * whatever the next listener generation looks like. */
      return NFS4ERR_DELAY

  buffer = rcu_lookup_with_find_ref(stateid, fh)
  if (!buffer):
      leave_quiesce(pls)
      goto forward_close

  lock(buffer.pwb_mutex)
  best_effort_flush_with_timeout(buffer, REFFS_PS_FLUSH_TIMEOUT_NS)
  unlock(buffer.pwb_mutex)
  drop_buffer(buffer)
  drop_find_ref(buffer, pls)

forward_close:
  call ps_proxy_forward_close()       /* upstream MDS sees CLOSE */

ps_listener_stop:
  /* Publish: stop accepting new buffer ops. */
  atomic_store_explicit(&pls->pls_state, DRAINING,
                        memory_order_release)

  /* Wait for in-flight find refs to drain.  enter_quiesce_or_bail
   * after this point sees DRAINING and decrements without taking
   * a find ref; existing ops drop their refs as they unwind. */
  pthread_mutex_lock(&pls->pls_drain_mutex)
  while (atomic_load_explicit(&pls->pls_active_buffer_refs,
                              memory_order_acquire) > 0):
      pthread_cond_wait(&pls->pls_drain_cv, &pls->pls_drain_mutex)
  pthread_mutex_unlock(&pls->pls_drain_mutex)

  /* No ref-takers can exist now.  Walk the table dropping table
   * refs.  Iterator advances BEFORE put per
   * patterns/rcu-violations.md Pattern 7. */
  rcu_read_lock()
  cds_lfht_first(table, &iter)
  while ((node = cds_lfht_iter_get_node(&iter)) != NULL):
      buffer = container_of(node, ...)
      cds_lfht_next(table, &iter)
      cds_lfht_del(table, node)
      drop table ref                  /* triggers call_rcu free */
  rcu_read_unlock()
  synchronize_rcu()                   /* wait for all call_rcu frees */
  cds_lfht_destroy(table, NULL)
  atomic_store_explicit(&pls->pls_state, STOPPED,
                        memory_order_release)
```

### Listener-borrow contract

`ps_listener_session_borrow(listener_id)` -- the API used by the
forward path (`ps_proxy_forward_*`) to obtain a per-listener MDS
session -- **MUST return NULL when the listener's `pls_state` is
not `RUNNING`**.  This is the contract that lets the buffer
quiesce above bound the forward path's lifetime too: if a forward
op observes a DRAINING listener, it returns NFS4ERR_DELAY rather
than touching about-to-be-freed session memory.

`ps_listener_session_borrow` already takes `pls_session_rwlock`
(rdlock) per the reconnect-arc work.  Add a single
`atomic_load(pls_state) == RUNNING` check at the top of the borrow
under the rdlock; on mismatch return NULL.  The pls_state store
in `ps_listener_stop` happens BEFORE the table walk, so any
in-flight borrow either observed RUNNING (and held the session
ref long enough to complete its op) or observes DRAINING/STOPPED
and bails cleanly.  The pls_session_rwlock+pls_state ordering
exactly mirrors the pls_state+pls_active_buffer_refs ordering
used for the buffer table.

**Scope note.**  This modifies `ps_listener_session_borrow`,
which lives in the reconnect-arc code (`lib/nfs4/ps/ps_state.c`)
and is also called by the existing forward READ / GETATTR / LOOKUP
paths from previous slices.  After the change, those callers
will also observe NULL on a DRAINING listener and map to
NFS4ERR_DELAY -- this is intentional and matches the contract
above.  The "Files to change" table lists `ps_state.c` for the
borrow modification, and the test inventory adds
`test_forward_read_on_draining_returns_delay` so the
non-pipeline forward callers' new behaviour is pinned.

### CLOSE-DRAINING reply choice

CLOSE-DRAINING returns NFS4ERR_DELAY rather than NFS4_OK-with-
forged-verifier.  Trade-off:

- NFS4ERR_DELAY -- correct per RFC 8881 S15.1.1.2 (transient
  failure, retry).  Cost: kernel client may retry-spam against a
  listener that is on its way to STOPPED.  In practice the
  client times out within `lease_period` and reconnects to the
  next listener generation.  In a planned rolling restart this
  is the operator-visible symptom of "graceful drain in
  progress, please wait".
- NFS4_OK + forged verifier -- some real servers do this during
  shutdown.  Cost: the client believes its bytes are durable but
  the buffer was dropped -- silent data loss if the client did
  not COMMIT before CLOSE.

Phase 4a picks DELAY because correctness > convenience for the
PS test surface.  The forged-OK option is an operator-tunable
follow-on (NOT_NOW_BROWN_COW).

### COMMIT empty-buffer semantics

An NFS client kernel serialises operations per `(open stateid,
file)` via openowner locking, so two ops with the same
`(stateid_other, upstream_fh)` cannot interleave in the kernel
client's RPC stream.  Phase 4a relies on this: a WRITE returning
`listener_verf` and a follow-on COMMIT returning the same
`listener_verf` are guaranteed to be ordered by the client, so a
COMMIT that finds no buffer truly has no preceding-WRITE bytes to
flush.

If a different client (different stateid) is concurrently writing
the same upstream FH, that client has its own buffer keyed by
its stateid and is unaffected.  The multi-writer-shared-file case
(distinct stateids, same FH) is the documented Phase 4a
limitation (the second COMMIT clobbers the first's stripes) --
not a verifier-match race.

If a future scenario relaxes per-(stateid, fh) ordering (e.g., an
out-of-order async client), the empty-buffer COMMIT path will
need to take `pls_table_quiesce_rwlock` or equivalent to
serialise against in-flight inserts.  Not required for 4a.

### Flush timeout interrupt mechanism

`best_effort_flush_with_timeout(buffer, deadline_ns)` is **not**
a thread-cancellation point and cannot interrupt a single
`mds_compound_send_with_auth` mid-RPC.  The 30 s
`REFFS_PS_FLUSH_TIMEOUT_NS` budget is enforced per-compound:

- `ec_write_encoding_with_file` is parameterised with a deadline
  (`uint64_t deadline_ns`); on `deadline_ns == 0` the existing
  unbounded behaviour applies (ec_demo path).
- **Implementation: watchdog-thread close-on-deadline.**
  `mds_compound_send_with_auth` is a libtirpc wrapper and does
  not itself accept a deadline parameter; modifying it would
  ripple through every existing caller (ec_demo, layout fan-out,
  the reconnect-arc work).  Instead, the encoding spawns a
  lightweight watchdog **pthread** at flush entry (Phase 4a
  chooses the pthread over the timerfd-on-io_uring variant for
  simplicity; revisit if profiling shows the thread create/join
  cost dominates).  The watchdog holds a weak reference to the
  session's xprt; on deadline expiry it closes the xprt fd,
  which causes the in-flight `CLNT_CALL` to return RPC_TIMEDOUT.
  The encoding then sees -ETIMEDOUT and bails to the caller.  This
  is the standard libtirpc-cancellation idiom and is already
  used in `ds_session_create` for connection-attempt timeouts.

  **Cleanup discipline.**  The encoding joins the watchdog on the
  way out (both happy and error paths) -- never detached.  The
  watchdog's `close(xprt_fd)` is idempotent: if the encoding
  finished and joined before the deadline, the watchdog wakes,
  checks a "encoding done" flag, and exits without touching the
  fd.  If the watchdog fired and closed the fd first, the encoding
  observes RPC_TIMEDOUT, treats it as a flush failure, and the
  subsequent join is a no-op.  No leaked thread, no
  double-close.
- Each compound checks the remaining budget BEFORE issuing the
  next RPC; if `now >= deadline_ns` the encoding returns -ETIMEDOUT
  without issuing the call, so deadline overshoot is bounded by
  the longest single RPC's actual completion time.
- A single hung CHUNK_WRITE on a wedged DS can still consume the
  full budget once (until the watchdog closes the xprt) --
  that's acceptable: CLOSE returns within ~30 s + close-roundtrip,
  the upstream stateid releases, the operator sees
  `close_flush_timeouts_total` increment.  Without the budget the
  CLOSE could hang indefinitely.

Granularity is therefore "best-effort per RPC, hard cap at the
specified deadline".  Documented in the implementation notes for
step 5 (`ps_proxy_pipeline_commit`).

## Persistence

None.  Write buffers live in PS RAM and are lost on listener restart
or PS process exit.  This is by design -- the buffer is a
COMMIT-deferred scratchpad, not durable storage.  The upstream MDS
+ DSes are authoritative.  A client whose buffer is lost will see
verifier mismatch on COMMIT and rewrite.

## Security model

- Same gating as Phase 3 READ: GSS compounds refused with
  NFS4ERR_WRONGSEC; AUTH_NONE refused at the op handler.
- Forwarded creds (`compound->c_ap`) are passed verbatim to
  `ec_write_encoding_with_file`, which threads them through the
  pipeline compounds (LAYOUTGET / LAYOUTRETURN; CHUNK_WRITE/FINALIZE/
  COMMIT use the DS session's auth as today).  Mirror of slice b2.
- Buffer table is per-listener; one client's buffers are not visible
  to another client.  No cross-listener buffer reuse.
- Buffer cap is a DoS knob: a malicious client cannot OOM the PS by
  buffering arbitrary writes without ever issuing COMMIT.  At cap
  the WRITE is rejected with NFS4ERR_DELAY and the client backs off.

## Deferred / NOT_NOW_BROWN_COW

- **Phase 4b: per-stripe RMW.**  The spec-compliant path that
  preserves multi-writer-shared-file semantics and unblocks
  Track 2 chunk-collision IOR `-F 0`.  Shipped 2026-05-13; the
  finalised design doc is `proxy-server-phase4b.md`.  4b also
  delivered the composed write verifier (4b.4), COMMIT range
  honouring (4b.5), and the FILE_SYNC4 / DATA_SYNC4 inline flush
  (4b.6) -- so the "Phase 4b territory" mentions on
  DATA_SYNC4 / FILE_SYNC4 below are now closed.
- **Direct-to-disk WRITE (no buffering).**  An optimisation for
  writes that perfectly align to stripe boundaries (offset and
  count both multiples of `k * shard_size`).  These could
  encode-and-stripe inline without buffering.  Defer; correctness
  first, perf later.
- **DATA_SYNC4 / FILE_SYNC4 honoured.**  Phase 4a treated every
  write as UNSTABLE4 regardless of client request.  Phase 4b
  slice 4b.6 shipped the inline flush so that a client asking
  for FILE_SYNC4 / DATA_SYNC4 now observes the requested level
  in the reply once the per-stripe flush succeeds.  No longer
  deferred.
- **Buffer spill to disk.**  The 1 GiB RAM cap is restrictive for
  workloads with many parallel large open files.  A spill-to-tmp
  fallback is plausible but not in 4a scope.
- **Async pipeline flush.**  COMMIT today blocks the client
  thread while the encode + per-DS CHUNK_WRITE storm completes.
  An async dispatch with a polling notification model is a perf
  follow-on; correctness path is sync.
- **Adjusting buffer cap dynamically.**  The cap is process-wide;
  per-client or per-listener caps would let the operator give
  high-priority clients more headroom.  Not in 4a.
- **MDS-restart-aware verifier.**  Mix the upstream MDS write
  verifier into the PS-listener verifier so an MDS restart
  surfaces as a verifier mismatch on the next COMMIT (closes
  Risk #3a).  Belongs in Phase 4b alongside per-stripe RMW.
- **Per-(stateid, FH) write-buffer probe op for live debugging.**
  The proposed `ps-write-buffer-stats` reports aggregates only.
  A developer chasing "client says it wrote 256 MiB but COMMIT
  returned 0" needs per-buffer state inspection.  Defer.

## Admin interface

- `[[ps]] write_buffer_max_bytes = 1073741824` in `reffs.toml`
  (default 1 GiB).  Validated at parse time; values < 4 KiB or
  > SIZE_MAX/2 are rejected.
- New probe op `ps-write-buffer-stats` (NEW) returning per-listener:
  `(active_buffers, total_bytes_buffered, peak_bytes_buffered,
  cap_rejections_total, close_flush_timeouts_total,
  fbig_rejections_total)`.  Useful for the bench harness to confirm
  that COMMIT really did flush, the buffer table is empty between
  test runs, and DS-side regressions are surfaced via the
  `close_flush_timeouts_total` counter (Risk #7).

## Implementation steps

Each step is **tests-first** per `roles.md`: write the failing
unit test from the "Tests first" table, then the production code,
then verify the test passes before moving on.  The list below
groups each step with the tests it unblocks, so the implementation
matches the standards rule 1 discipline.

**Per-slice quality gates** (run BEFORE every commit, not just at
step 11; per memory `feedback_reffs_preslice_checks.md`):

- `make -f Makefile.reffs license`
- `make -f Makefile.reffs style`
- `make check` under the local build dir
- reviewer agent if the slice triggers `.claude/CLAUDE.md`
  "Workflow rules" criteria (this slice does -- lock ordering,
  RCU lifecycle, cross-layer boundaries all touched).

1. **`ec_write_encoding_with_file` variant** in `lib/nfs4/ps/ec_pipeline.c`.
   Tests it unblocks: `test_write_encoding_with_file_smoke`,
   `test_write_encoding_with_file_propagates_creds`,
   `test_write_encoding_with_file_null_creds_default_auth`.
   - Refactor `ec_write_encoding` to factor the OPEN+LAYOUTGET prelude
     out into `ec_write_encoding_with_file(ms, mf, ...)`.
   - The current `ec_write_encoding(ms, path, ...)` becomes a thin
     wrapper that does mds_file_open then calls the new entry.
     This **matches the read-side pattern**: `ec_read_encoding` at
     `ec_pipeline.c:1140` is already a wrapper around
     `ec_read_encoding_with_file` at line 927.  Keeping the wrapper
     leaves ec_demo's existing call sites untouched.
   - Add `creds` parameter (last positional) and thread it to the
     LAYOUTGET / LAYOUTRETURN calls -- same pattern as slice b2.
   - Update the `ec_write_encoding` wrapper to pass NULL for creds
     (preserves existing test-call behaviour, identical to the
     read-side handling).
2. **`proxy_data_pipeline_write` helper** in `lib/backends/proxy_data.c`.
   No new tests at this step (helper is exercised via step 4's
   shim tests).
   - Internal entry called by the new shim from `ps_proxy_ops.c`
     when the proxy WRITE branch fires.
   - `proxy_data_db_write` (the data_block hook) **stays
     `-ENOSYS`**.  The pipeline path does not flow through the
     data_block hook; document inline.
3. **Write-buffer table + listener state machine** on
   `struct ps_listener_state`.  Tests it unblocks:
   `test_write_buffer_alloc_per_stateid`,
   `test_listener_draining_state_returns_delay`,
   `test_listener_id_collision_distinct_buffers`,
   `test_listener_shutdown_drains_buffers`,
   `test_listener_shutdown_waits_for_inflight_commit`.
   - `cds_lfht *pls_write_buffer_ht` (Rule 6 lifecycle in State
     section).
   - `_Atomic uint64_t pls_boot_gen` (snapshot into each new buffer).
   - `_Atomic enum ps_listener_state pls_state`
     (`{RUNNING, DRAINING, STOPPED}`).
   - `_Atomic uint64_t pls_active_buffer_refs` + `pls_drain_cv`
     for teardown quiesce.
   - Init in `ps_listener_start` (state = RUNNING, boot_gen
     incremented under release ordering); destroy in
     `ps_listener_stop` per the State machines pseudocode.
4. **`ps_proxy_pipeline_write` shim** in `lib/nfs4/ps/ps_proxy_ops.c`.
   Tests it unblocks: `test_write_buffer_appends_in_order`,
   `test_write_buffer_sparse_holes_zero_filled`,
   `test_write_buffer_overwrite_replaces`,
   `test_write_buffer_size_cap_returns_delay`,
   `test_write_larger_than_cap_returns_fbig`,
   `test_write_returns_unstable_with_verifier`,
   `test_concurrent_writes_same_stateid_serialize`,
   `test_pipeline_write_gss_refused`.
   - Mirror of `ps_proxy_pipeline_read`.
   - Lookup-or-allocate buffer per Rule 6 lifecycle.
   - Append bytes; cap checks (cap-overflow vs single-WRITE-FBIG
     are distinct); reply with UNSTABLE4 + listener verf.
5. **`ps_proxy_pipeline_commit` shim** in `lib/nfs4/ps/ps_proxy_ops.c`.
   Tests it unblocks: `test_commit_flushes_buffer_via_pipeline`,
   `test_commit_returns_same_verifier_as_writes`,
   `test_commit_drops_buffer_on_success`,
   `test_commit_keeps_buffer_on_failure`,
   `test_commit_layout_get_retry`,
   `test_commit_partial_ds_failure`,
   `test_commit_finalize_partial_failure`,
   `test_concurrent_writes_distinct_stateids_documented_loss`.
   - Lookup buffer; if none, return success with verifier (no-op COMMIT).
   - Snapshot listener gen; if mismatch, drop buffer + NFS4ERR_STALE.
   - Construct `mds_file` from buffer key (stateid + FH).
   - Flush the **entire** `[0, pwb_high_water)` buffer prefix
     regardless of `args.offset` / `args.count` (per RFC compliance
     bullet 3 -- spec permits the server to commit more than asked).
   - On success, drop buffer (cds_lfht_del + drop both refs); on
     failure, keep buffer + NFS4ERR_IO.
6. **`ps_proxy_pipeline_close` hook** in `lib/nfs4/ps/ps_proxy_ops.c`.
   Tests it unblocks: `test_close_implicit_flush`,
   `test_close_drops_buffer_after_implicit_flush`,
   `test_close_buffer_cleanup_on_error`.
   - Called from `nfs4_op_close` proxy branch BEFORE the existing
     forward-CLOSE path -- flush first, then proceed to forward
     so the upstream MDS sees the close.
   - Best-effort flush; unconditional buffer drop on the way out.
7. **Op-handler dispatch** in `lib/nfs4/server/file.c`.  No new
   tests at this step (the prior steps' tests now exercise the
   live dispatch path).
   - `nfs4_op_write` proxy branch: **replace**
     `ps_proxy_forward_write` with `ps_proxy_pipeline_write`.
     The forward WRITE call is removed for proxy SBs, not kept
     as a fallback (per RFC compliance bullet 1 -- avoids
     mixed-verifier-source bug).
   - `nfs4_op_commit` proxy branch: **replace** the call to
     `ps_proxy_forward_commit` with `ps_proxy_pipeline_commit`.
     The pipeline path covers both the buffered case and the
     no-buffered-bytes case (returning listener verifier for
     both), so the forward COMMIT call is removed from the
     proxy-SB dispatch path.  **The `ps_proxy_forward_commit`
     function itself is not deleted** -- it remains in
     `ps_proxy_ops.c` as a private helper, callable from
     non-proxy-SB contexts (today: none; reserved for future
     re-entry from PROXY_REGISTRATION fan-out or admin probe).
     Mark it `__attribute__((unused))` if no caller survives the
     slice; do not deadcode-remove, to keep symmetry with
     `ps_proxy_forward_write` (which the kick-tests still exercise)
     and to leave a re-entry point if Phase 4b reintroduces a
     forward fallback for a narrow class of COMMITs.
   - `nfs4_op_close` proxy branch: insert
     `ps_proxy_pipeline_close` BEFORE the existing forward CLOSE
     call.  The forward CLOSE remains -- the upstream MDS still
     needs to be told the file is closed, the pipeline-close hook
     just drains the buffer first.
8. **Probe op `ps-write-buffer-stats`** in `lib/probe1/probe1_server.c`
   and matching client wrapper.  Mirror of the `ps-listener-list`
   shape from the reconnect arc.  No new test in the unit-test
   table; covered by extending `scripts/test_sb_probe.py` style
   integration test once 4a is up on dreamer.
9. **`reffs.toml` parser** in `lib/config/` -- **NOT_NOW_BROWN_COW**.
   Deferred to a follow-on slice.  Today
   `REFFS_PS_WRITE_BUFFER_MAX` is a compile-time constant (1 GiB,
   defined in `ps_write_buffer_internal.h`); the operator-tunable
   `[[ps]] write_buffer_max_bytes` TOML field is its own slice's
   plumbing (parser + per-listener field + threading).  The pipeline
   shim is structured so the cap lookup can become a per-listener
   `pls_write_buffer_max` field without surface changes.
10. **Functional / CI test** -- **NOT_NOW_BROWN_COW**.  Deferred to
    the bench-integration arc.  Requires standing up the bench
    docker-compose (1 MDS + 10 DSes + 1 PS) plus a Linux NFSv4.2
    client mount, then running `cp /tmp/random_256m mount/file &&
    diff`.  The CI infrastructure for this is a separate concern
    from the pipeline's correctness, which is unit-test-covered.
    Until this lands, operators verify end-to-end via the
    `deploy/benchmark/` topology by hand.
11. **Update `proxy-server.md`**: status updated to DONE in the
    parent design doc; the Status section at the top of this doc
    is the canonical map of what shipped vs. deferred.

## Files to change

| File | Change |
|------|--------|
| `lib/nfs4/ps/ec_pipeline.c` | Factor `ec_write_encoding_with_file`; add `creds` parameter (mirror of read-side wrapper at `ec_pipeline.c:1140`) |
| `lib/nfs4/client/ec_client.h` | Declare `ec_write_encoding_with_file`; document creds positional |
| `lib/backends/proxy_data.c` | Add `proxy_data_pipeline_write` helper; document why `db_write` stays -ENOSYS |
| `lib/nfs4/ps/ps_state.h`, `ps_state.c` | Per-listener write buffer table, `pls_state` enum, `pls_boot_gen`, `pls_active_buffer_refs`, drain CV.  Plus `ps_listener_session_borrow`: gate on `pls_state == RUNNING` (returns NULL otherwise -- contract change visible to all forward callers, see "Listener-borrow contract" section) |
| `lib/nfs4/ps/ps_write_buffer.c` | NEW -- `struct ps_write_buffer` lifecycle (Rule 6 lifecycle) |
| `lib/nfs4/ps/ps_write_buffer_internal.h` | NEW -- whitebox surface for unit tests (mirror of `ps_renewal_internal.h`) |
| `lib/nfs4/ps/ps_proxy_ops.c`, `ps_proxy_ops.h` | NEW `ps_proxy_pipeline_write`, `ps_proxy_pipeline_commit`, `ps_proxy_pipeline_close` shims |
| `lib/nfs4/ps/Makefile.am` | Wire `ps_write_buffer.c` |
| `lib/nfs4/server/file.c` | `nfs4_op_write` / `nfs4_op_commit` / `nfs4_op_close` proxy branches: dispatch to pipeline path (forward path removed for WRITE/COMMIT) |
| `lib/config/` (`config_ps.c` or equivalent) | `write_buffer_max_bytes` field |
| `lib/xdr/probe1_xdr.x` | NEW op `ps-write-buffer-stats` |
| `lib/probe1/probe1_server.c`, `probe1_client.c` | Op handler + client wrapper |
| `lib/include/reffs/probe1.h` | Declare wrapper |
| `src/probe1_client.c` | `--op ps-write-buffer-stats` dispatch |
| `scripts/reffs/probe_client.py.in`, `scripts/reffs-probe.py.in` | Python wrapper + CLI |
| `lib/backends/tests/proxy_data_test.c` | Pipeline-write tests |
| `lib/nfs4/ps/tests/ps_proxy_pipeline_write_test.c` | NEW |
| `lib/nfs4/ps/tests/ps_write_buffer_test.c` | NEW -- Rule 6 lifecycle whitebox tests |
| `lib/nfs4/ps/tests/Makefile.am` | Wire new tests; LDADD for the new transitive deps |
| `scripts/ci_ps_phase4a_test.sh` | NEW functional test |
| `.claude/design/proxy-server.md` | Phase 4a status -> DONE; pointer to 4b |

## Reviewer checklist (planner self-review)

Per `.claude/standards.md`:

- Rule 1 (tests-first): full test surface listed before any
  implementation step; each implementation step in
  "Implementation steps" cites the tests it unblocks.
- Rule 2 (design compliance): this slice maps 1:1 to Phase 4a
  scope; Phase 4b is its own design doc.
- Rule 3 (atomics): `pls_boot_gen`, `pls_state`, and
  `pls_active_buffer_refs` use C11 `_Atomic` with the explicit
  memory orders called out in "State machines / Quiesce protocol":
  - `pls_state`: `release` on store (publish DRAINING / STOPPED),
    `acquire` on the post-`fetch_add` reload that closes the
    TOCTOU window.
  - `pls_active_buffer_refs`: `acq_rel` on every `fetch_add` /
    `fetch_sub` (couples the counter's modification order with
    the buffer's reachability so the teardown wait observes
    happens-before).
  - `pls_boot_gen`: `acquire` on every read (per-buffer
    staleness check); `release` on `ps_listener_start`'s
    increment.
  `pwb_ref` uses `urcu_ref` (liburcu's atomic refcount); no GCC
  builtins on new fields.
- Rule 4 (lock ordering): the buffer table follows
  patterns/ref-counting.md Rule 6 (lfht + urcu_ref); no rwlock
  is needed -- RCU is the read-side primitive.  Lock ordering:
  `pls_session_rwlock` (reconnect arc) > `rcu_read_lock` (buffer
  table lookup) > `pwb_mutex` (buffer mutation).  `pwb_mutex` is
  leaf-most: nothing else is acquired while it is held.
  Listener teardown takes `pls_session_rwlock` first (existing
  reconnect-arc invariant) then walks the buffer table under
  `rcu_read_lock`; the order is unchanged from the existing
  shutdown path.
- Rule 5 (forward declarations): no new public structs leak into
  cross-module headers; `struct ps_write_buffer` stays internal
  to a new `ps_write_buffer.c` with a `ps_write_buffer_internal.h`
  for the test whitebox surface (mirror of `ps_renewal_internal.h`).
- Rule 6 (error paths): every alloc has a matching free on the
  error path; tested under valgrind in
  `test_listener_shutdown_drains_buffers`.  RCU lifecycle covered
  by `test_listener_shutdown_waits_for_inflight_commit` (no UAF
  under TSAN).
- Rule 7 (logging): TRACE on buffer alloc/drop, COMMIT flush
  start/end with byte counts.  LOG (operator-actionable) only on
  listener-stop path that abandons buffered bytes.  No printf to
  stderr.
- Rule 8 (probe surface): `ps-write-buffer-stats` follows the
  established additive-append pattern for `probe_sb_info1`.
- Rule 9 (XDR): probe XDR is internal-only (additive);
  nfsv42_xdr.x unaffected; no protocol op renumbering.
- Rule 10 (UUID stability): no new long-lived objects.

All new files carry the standard SPDX header per
`make -f Makefile.reffs license`.

## Risks

1. **Whole-file buffer model is wrong for Track 2** (acknowledged
   up front).  Phase 4a does NOT close the chunk-collision-
   validation Track 2 gate.  Phase 4b does.  This slice's value
   is the wiring (db_write hook, pipeline shim, encoding_with_file
   variant, creds threading) which 4b reuses.

2. **Buffer cap interaction with large IOR runs.**  IOR `-F 1
   -b 256m` with 16 ranks against 16 PSes: each PS holds 256 MiB
   per file -- well under the 1 GiB cap.  But IOR `-b 1g` (the
   stretch case) would push past the default cap and the client
   would see NFS4ERR_DELAY storms.  Mitigation: bench config
   bumps the cap to 4 GiB; default stays 1 GiB for production
   safety.

3. **Verifier mismatch on listener restart loses buffered bytes.**
   If a PS listener restarts between WRITE and COMMIT, the
   client's COMMIT sees a verifier mismatch and rewrites.  This is
   correct per RFC, but it does mean uncommitted writes are lost
   on PS crash.  No mitigation in 4a -- the spec model assumes the
   client retries.  Documented in operator notes.

3a. **PS-listener verifier hides upstream MDS restart.**  The
   forward-COMMIT path returns an MDS-issued verifier, so an MDS
   restart between WRITE and COMMIT propagates as a verifier
   mismatch and the client rewrites the lost bytes.  The pipeline
   path returns a PS-listener verifier, which does NOT reflect the
   MDS state.  If a buffered file is partially flushed (Phase 4b
   territory; in 4a a flush is always whole-buffer), an MDS
   restart between flushes would silently lose the
   already-flushed prefix.  In 4a the model is whole-buffer-or-
   nothing, so the window is narrow but non-zero: an MDS restart
   between the pipeline's CHUNK_COMMIT-to-DSes and the WRITE reply
   is silently lost.  Phase 4b will incorporate the MDS verifier
   into the PS verifier so MDS restart propagates.  Tracked as
   NOT_NOW_BROWN_COW.

4. **Multi-writer-shared-file in 4a is broken.**  Two clients with
   distinct stateids writing the same file via the same PS: each
   gets its own buffer, COMMITs independently, the second COMMIT's
   `ec_write_encoding_with_file` overwrites the first's stripes.
   This is a known wrong outcome; the bench harness for 4a must
   not run shared-file workloads (use `-F 1`, not `-F 0`).
   Phase 4b is the real fix.

5. **`ec_write_encoding_with_file` refactor risk.**  Factoring the
   prelude out of `ec_write_encoding` could subtly change error-path
   ordering for ec_demo's existing callers.  Mitigation: the
   `ec_write_encoding` wrapper preserves bit-identical behaviour for
   NULL creds; ec_demo continues to use the wrapper, not the new
   entry.  Test `ec_demo write` smoke against the unmodified bench
   topology before declaring the refactor done.

6. **CLOSE-driven flush blocks CLOSE arbitrarily long.**  A 1 GiB
   buffered file's encode + 6×stripe CHUNK_WRITE+FINALIZE+COMMIT
   can take seconds.  If the client times out the CLOSE, the
   buffer is dropped (we have no way to retry).  Mitigation:
   document operator expectation that clients should issue COMMIT
   before CLOSE for large writes; CLOSE-flush is the safety net,
   not the intended path.  See Risk #7 for the upstream-stateid
   consequence and the timeout mitigation.

7. **Upstream-stateid leak if CLOSE-flush hangs past client timeout.**
   The CLOSE handler runs `best_effort_flush_with_timeout` BEFORE
   calling `ps_proxy_forward_close`.  Without a timeout, a wedged
   DS (or a long encode under memory pressure) holds the CLOSE
   indefinitely; if the client times out and tears down its
   stateid, the upstream MDS never sees the CLOSE and leaks the
   open until lease expiry.
   Mitigation: cap the flush at `REFFS_PS_FLUSH_TIMEOUT_NS`
   (default 30 s).  If exceeded, drop the buffer (bytes lost,
   TRACE+LOG line records the loss) and proceed to the forward
   CLOSE so the upstream stateid is released promptly.  The 30 s
   default is well above the encode time for a 1 GiB buffer over
   localhost loopback (single-digit seconds in bench).  Operator
   visibility: `ps-write-buffer-stats` reports
   `close_flush_timeouts_total` so the operator can detect a DS
   regression.

## Cross-references

- Parent: `.claude/design/proxy-server.md` Phase 4
- Predecessor wiring: `.claude/design/proxy-server-phase3.md`
  (READ-side pipeline path; same shim + creds threading shape)
- Slice b2 cred-forwarding pattern: commit `966d864bede6`
- Reconnect-state probe pattern (mirror for write-buffer-stats):
  commit `a083dd46f38c`
- Successor: `.claude/design/proxy-server-phase4b.md` (per-stripe
  RMW; the real Track 2 chunk-collision gate)
- Track 2 dependency: `.claude/design/chunk-collision-validation.md`
  Track 2 section
