<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Phase 5 Follow-up: `test_shortcircuit_partial_2_mirrors`

## Context

Phase 5 of the proxy server (proxy-server.md, proxy-server-phase5.md)
lists this test as the load-bearing integration check for the
dispatch hook in `lib/nfs4/ps/ec_pipeline.c`:

> Layout with 2 mirrors, 1 local + 1 remote; local takes shortcircuit,
> remote takes RPC; reconstruction succeeds.

Slice 5.5 shipped `pls_shortcircuit_total` plus
`test_shortcircuit_counter_increments` (slice 5.5 test in
`ps_shortcircuit_test.c:535`).  That test pins the counter primitive
in isolation by calling `ps_listener_record_shortcircuit()` directly.
It explicitly defers the dispatch-hook call site to this slice:

> The dispatch-hook call site is exercised by the higher-level
> integration test (test_shortcircuit_partial_2_mirrors, follow-up
> slice); this test pins the counter primitive in isolation so a
> future refactor that swaps the helper for a direct atomic call
> cannot silently drop the probe-surface plumbing.

This slice closes that gap: a focused test that drives
`ec_chunk_write` (and `ec_chunk_read`) directly with a 2-mirror
`ec_context` -- one mirror with `em_local=true`, one with
`em_local=false` -- and asserts:

1. The local-mirror call routes through `pls_sc_write_fn` /
   `pls_sc_read_fn` (the install stub records the call).
2. The non-local-mirror call does NOT route through the install
   stub (it falls through to the RPC path, which the test
   short-circuits with an error to keep the test pure-unit).
3. `pls_shortcircuit_total` advances by exactly 1 per pair of
   dispatch-hook entries (one for the local mirror only).
4. The dispatch hook honours `ctx_pls == NULL` and
   `pls_sc_write_fn == NULL` as the existing ec_demo fall-through
   path requires.

## Tests (TDD discipline -- written before any implementation)

### New file: `lib/nfs4/ps/tests/ec_pipeline_dispatch_test.c`

| Test | Intent |
|------|--------|
| `test_dispatch_local_mirror_writes_via_stub` | em_local=true mirror: stub called with the mirror's fh/uid/gid/offset; counter advances by 1; no RPC attempt |
| `test_dispatch_remote_mirror_skips_stub` | em_local=false mirror: stub NOT called; counter stays at 0; the RPC path is reached (verified via `ds_chunk_write` strong-override returning -EIO with attempt count = 1) |
| `test_dispatch_two_mirrors_partial_shortcircuit` | The load-bearing case: 2-mirror context, idx 0 local, idx 1 remote; one call per mirror; counter = 1; stub called once with mirror 0's fh; RPC attempted once for mirror 1 |
| `test_dispatch_null_pls_skips_stub` | ctx_pls = NULL: dispatch falls through to RPC even when em_local=true; counter not touched (no listener to count on); ec_demo path |
| `test_dispatch_null_sc_fn_skips_stub` | ctx_pls non-NULL but pls_sc_write_fn = NULL: dispatch falls through to RPC; counter not touched; defensive path for install ordering |
| `test_dispatch_read_path_mirrors_write` | Same shape as `test_dispatch_two_mirrors_partial_shortcircuit` but exercising `ec_chunk_read` -- the read-side hook at ec_pipeline.c:321 must behave identically to the write-side at :262 |

### Existing test impact

| File | Impact |
|------|--------|
| `lib/nfs4/ps/tests/ps_shortcircuit_test.c` | PASS -- the helper-direct tests remain unchanged.  The new test file complements them. |
| `lib/nfs4/ps/tests/ec_pipeline_stripe_test.c` | PASS -- stripe-math tests are orthogonal. |
| All other `make check` tests | PASS -- no production code path changes. |

## Implementation

The dispatch site (`lib/nfs4/ps/ec_pipeline.c:262-269` for write,
:321-329 for read) is a static function (`ec_chunk_write`,
`ec_chunk_read`).  To exercise it as a focused unit test, two
viable approaches:

| Approach | Pros | Cons |
|----------|------|------|
| (A) Add `lib/nfs4/ps/ec_pipeline_internal.h` declaring `ec_chunk_write` + `ec_chunk_read` and remove `static` from the definitions | Smallest test code; tests the exact dispatch site; no full-layout scaffolding | Adds a test-only internal header; widens the TU's symbol surface by 2 functions |
| (B) Drive the dispatch hook through `ec_write_stripe_with_file` with strong overrides of `mds_layout_get` and friends | No internal-header surface | Heavy mocking; the test grows past the slice's scope and starts looking like an integration test |

**Choice: (A)** -- the test-only internal header pattern.

The internal header is in the same directory as the source, marked
clearly as internal, and used only by the new test TU.  The
ec_pipeline.c functions stop being `static` but the public API of
the library is unchanged (the symbols are not added to any public
header).  This is the same pattern used by
`lib/nfs4/ps/ps_write_buffer_internal.h` (whitebox header for the
slice 4a buffer tests), so the precedent is established in the
neighbouring code.

### Files touched

| File | Change |
|------|--------|
| `lib/nfs4/ps/ec_pipeline_internal.h` | NEW -- declares `ec_chunk_write` + `ec_chunk_read`; comment marks as test-only |
| `lib/nfs4/ps/ec_pipeline.c` | Remove `static` from `ec_chunk_write` and `ec_chunk_read` definitions; include the new internal header for self-consistency |
| `lib/nfs4/ps/tests/ec_pipeline_dispatch_test.c` | NEW -- 6 tests above |
| `lib/nfs4/ps/tests/Makefile.am` | Wire new test into `check_PROGRAMS` |

### Strong-override pattern

Mirror 1's RPC path calls `ds_chunk_write` (`lib/nfs4/client/ec_io.c`
or wherever it lives).  The test TU provides a strong override that
records the attempt count and returns -EIO immediately, the same
pattern `ps_proxy_pipeline_write_test.c` uses for
`mds_compound_send_with_auth`.

Specifically:
- `int ds_chunk_write(...)` strong override -- increments
  `g_rpc_write_attempts` and returns -EIO.
- `int ds_chunk_read(...)` strong override -- same shape for the
  read-side test.

The local mirror never reaches the strong override because the
dispatch hook intercepts before the RPC fan-out.

### Stub install for the local mirror

The test installs a recording stub instead of the production
`ps_shortcircuit_write` / `ps_shortcircuit_read`:

```c
static int g_sc_write_calls;
static struct {
    uint8_t fh[64];
    uint32_t fh_len;
    uint64_t byte_off;
    uint32_t uid, gid;
} g_sc_write_record;

static int recording_sc_write(const uint8_t *fh, uint32_t fh_len,
                              uint64_t byte_off, const uint8_t *data,
                              size_t data_len, uint32_t uid,
                              uint32_t gid, const stateid4 *stid)
{
    g_sc_write_calls++;
    memcpy(g_sc_write_record.fh, fh, fh_len);
    g_sc_write_record.fh_len = fh_len;
    g_sc_write_record.byte_off = byte_off;
    g_sc_write_record.uid = uid;
    g_sc_write_record.gid = gid;
    return 0;
}
```

The stub is assigned to `pls->pls_sc_write_fn` directly (not via
`ps_shortcircuit_install`, which would install the production
helper).

### ec_context construction

The test builds a `struct ec_context` on the stack with:
- `ctx_pls` -> the registered listener
- `ctx_layout.el_mirrors` -> heap-allocated `struct ec_mirror[2]`
- `ctx_layout.el_nmirrors = 2`
- `ctx_ds_sess[]` zeroed (RPC path will fail on NULL dc_clnt; the
  strong override of `ds_chunk_write` intercepts before any clnt
  deref)

`em_local`, `em_fh`, `em_fh_len`, `em_uid`, `em_gid`, and
`em_tight_coupled` are set per-mirror.  `em_tight_coupled = false`
on both -- this slice does not exercise trust-stateid; the dispatch
hook passes NULL stateid when `em_tight_coupled` is false (see
ec_pipeline.c:244-246).

## Standards compliance

- **C11 atomics** -- the existing slice 5.5 counter uses
  `atomic_fetch_add_explicit(..., memory_order_relaxed)`; this test
  reads with `atomic_load_explicit(..., memory_order_relaxed)`,
  matching the standards.md "Statistics counters" rule.
- **Test budget** -- each test is a hash-table-free, in-memory
  dispatch-hook exercise; wall-clock comfortably under 100 ms each,
  satisfying the standards.md 2-second per-test budget.
- **License header + style** -- new files carry SPDX headers;
  `make -f Makefile.reffs style` + `license` will be run before
  commit per `feedback_reffs_preslice_checks.md` memory.
- **ASCII only** -- all source content is ASCII per standards.md.

## RFC compliance

This slice is a test against existing production code; no new
protocol surface.  The production path it exercises is the
proxy-server data-mover draft's short-circuit optimisation, which
is internal to the PS implementation and not specified in any RFC.

## State machines

No new state machine.  The dispatch hook is a stateless decision
on each `ec_chunk_*` entry.

## Persistence

No persistent state introduced.

## Security model

The cred-check and stateid-trust paths are already covered by the
slice 5.3 (`test_shortcircuit_reject_wrong_uid`,
`test_shortcircuit_accept_matching_uid`,
`test_shortcircuit_root_squash`) and slice 5.4
(`test_shortcircuit_null_stateid_skips_trust_check`,
`test_shortcircuit_trusted_stateid_accepted`,
`test_shortcircuit_rejects_unknown_stateid`) tests in
`ps_shortcircuit_test.c`.  This slice does NOT re-test those; it
tests the dispatch decision (em_local + ctx_pls + sc_fn) one layer
above.

The test fixture deliberately uses the recording stub (not the
production `ps_shortcircuit_write`) so that the dispatch-hook test
does not also become an end-to-end cred-check test.  Layering: the
slice 5.3/5.4 tests prove the helper enforces creds + trust;
this slice proves the dispatch hook calls the helper at the right
times.

## Admin interface

No admin interface implications -- pure test slice.

## Deferred items

None for this slice.  Optional follow-up (NOT_NOW_BROWN_COW): a
full integration test that walks LAYOUTGET -> ec_chunk_write ->
real DS RPC against a combined-MDS+DS reffsd via the ec_demo
harness.  That test exists in spirit at the BAT soak level
(`scripts/ci_ps_shortcircuit_test.sh` per proxy-server.md
"Co-residency correctness"); adding a CI-level variant is queued
for the deploy/benchmark side, not this slice.

## Implementation order

1. Write the 6 tests in `ec_pipeline_dispatch_test.c` -- compile
   fails because `ec_chunk_write` / `ec_chunk_read` are static.
2. Add `lib/nfs4/ps/ec_pipeline_internal.h` declaring both.
3. Remove `static` from `ec_chunk_write` and `ec_chunk_read` in
   `ec_pipeline.c`.
4. Add `ec_pipeline_dispatch_test` to `lib/nfs4/ps/tests/Makefile.am`.
5. `make -j$(nproc)` -- zero warnings.
6. `make check` -- all 6 new tests pass; existing tests unchanged.
7. `make -f Makefile.reffs style` + `license` -- clean.
8. Push branch, dreamer CI: `make -f Makefile.reffs ci-full` -- clean.

## Test count delta

Before: PS test suite has 12 `test_shortcircuit_*` cases (slice
5.3/5.4/5.5).  After: +6 cases in a new TU = `ec_pipeline_dispatch`,
total PS dispatch + helper coverage = 18.
