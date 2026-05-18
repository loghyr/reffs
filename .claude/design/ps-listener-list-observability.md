<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# `ps-listener-list` Observability Extension

## Context

`probe1_op_ps_listener_list` (op 30) shipped earlier to close the
"Probe visibility for reconnect state" deferral in
`.claude/design/ps-reconnect.md`.  The current per-listener record
surfaces:

- `ppli_listener_id`
- `ppli_upstream` (MDS address)
- `ppli_upstream_port`
- `ppli_session_present` (bool snapshot of pls_session != NULL)
- `ppli_reconnect_backoff_sec`
- `ppli_reconnect_next_attempt_ns`

This is enough to answer "is the upstream MDS session up and when
will the next reconnect attempt fire?" but leaves four other
admin-visible per-listener states unsurfaced:

1. **Short-circuit capability**: did `ps_shortcircuit_install` ever
   run for this listener?  When false, every "should-be-local"
   dispatch falls through to RPC, silently negating the Phase 5
   optimisation.  Today the only way to confirm SC is wired is to
   read source code.
2. **MDS root FH resolved**: `pls_mds_root_fh_len == 0` means the
   session is up but discovery hasn't run (or hit MDS-side
   ACCESS).  Distinguishes "fresh session, no traffic yet" from
   "session works, discovery succeeded".
3. **Discovered upstream exports count**: how many paths has the
   PS seen from the upstream MDS' MOUNT3 EXPORT reply?  Zero
   means discovery hasn't completed; non-zero means the PS knows
   about that many paths to proxy.
4. **Local-address table size**: `pls_nlocal_addrs` -- how many
   addresses seed the `ps_local_addr_match()` filter that gates
   `em_local` on the dispatch hook.  Zero means short-circuit
   will never fire even with capability installed.

All four are read-only snapshots of fields already on
`struct ps_listener_state`.  No new state, no new lifetime, no
new lock: each read is either a plain load of an immutable
register-time field (sc_installed, nlocal_addrs, mds_root_fh_len)
or an acquire-load on an existing `_Atomic` field (nexports).

## Scope

Add four fields to `probe_ps_listener_info1` (the XDR record
returned by `PS_LISTENER_LIST`).  Update the C handler to populate
them, the C client wrapper's `printf` row format, and the Python
CLI's column layout.  Add a unit test that exercises the handler
on a registered listener and asserts each field round-trips
through XDR with the right value.

## Tests first

### Existing tests affected

| File | Impact |
|------|--------|
| All `make check` tests | PASS -- the XDR record is wire-additive (probe1 is internal-only per reviewer rule 9; the design doc covers this).  No existing field is renumbered. |
| `scripts/test_sb_probe.py` integration test (if it touches PS_LISTENER_LIST) | PASS -- the script uses named-field access on `probe_ps_listener_info1`, additive new fields do not break the access pattern.  Will read once and verify. |

### New tests

**`lib/nfs4/ps/tests/ps_listener_list_observability_test.c`** (NEW):

| Test | Intent |
|------|--------|
| `test_observability_sc_installed_false_by_default` | Register a listener via ps_state_register WITHOUT calling ps_shortcircuit_install.  Handler reports `ppli_sc_installed = false`. |
| `test_observability_sc_installed_true_after_install` | After ps_shortcircuit_install on the listener, handler reports `ppli_sc_installed = true`. |
| `test_observability_root_fh_resolved_false_at_register` | Fresh registration: `pls_mds_root_fh_len == 0`, handler reports `ppli_root_fh_resolved = false`. |
| `test_observability_root_fh_resolved_true_after_seed` | Seed `pls_mds_root_fh_len = 1` via the existing test helper; handler reports `ppli_root_fh_resolved = true`. |
| `test_observability_nexports_zero_at_register` | Fresh registration: handler reports `ppli_nexports = 0`. |
| `test_observability_nexports_after_append` | `ps_state_listener_append_export` once; handler reports `ppli_nexports = 1`.  Append a second time; handler reports 2. |
| `test_observability_nlocal_addrs_matches_seed` | The fixture seeds N local addrs via `ps_state_register` config; handler reports `ppli_nlocal_addrs = N` (default 0 from a vanilla config, set the test config to advertise 2 addresses to verify the non-zero path). |

7 new tests in one TU.  Per-test wall-clock < 100 ms; well under
the standards.md 2-second budget.

## XDR change

`lib/xdr/probe1_xdr.x`, `struct probe_ps_listener_info1` (line
564-571).  Append:

```xdr
struct probe_ps_listener_info1 {
    unsigned int   ppli_listener_id;
    string         ppli_upstream<PROBE1_PS_UPSTREAM_MAX>;
    unsigned int   ppli_upstream_port;
    bool           ppli_session_present;
    unsigned int   ppli_reconnect_backoff_sec;
    unsigned hyper ppli_reconnect_next_attempt_ns;
    /* New fields (wire-additive, internal-only per reviewer rule 9): */
    bool           ppli_sc_installed;
    bool           ppli_root_fh_resolved;
    unsigned int   ppli_nexports;
    unsigned int   ppli_nlocal_addrs;
};
```

Probe1 is the internal protocol -- the C server and both clients
(C + Python) ship in the same source tree and rebuild together.
Rule 9 of `.claude/roles.md` explicitly exempts `probe1_xdr.x`
from the "XDR file change = BLOCKER" rule.  Persisted `feedback`
memory `feedback_xdr_proposed_vs_established.md` is the
corroborating context.

## C handler change

`lib/probe1/probe1_server.c`, around line 1427-1456 in the
per-listener fill block.  Add:

```c
info->ppli_sc_installed = (pls->pls_sc_write_fn != NULL);
info->ppli_root_fh_resolved = (pls->pls_mds_root_fh_len != 0);
info->ppli_nexports = atomic_load_explicit(&pls->pls_nexports,
                                           memory_order_acquire);
info->ppli_nlocal_addrs = pls->pls_nlocal_addrs;
```

- `pls_sc_write_fn`: plain read.  Set once at register time
  (after `ps_state_register`'s release-store on `ps_nlisteners`
  publishes the slot, per the ps_state.h:340-345 comment), never
  mutated after.  No fence needed; the borrow API the handler
  already uses synchronises with the publish edge.
- `pls_mds_root_fh_len`: plain read.  Same publish-once
  contract per ps_state.h:118-119 -- the field is mutated by a
  worker thread after discovery, so the read is a torn-by-design
  race-tolerant load (one byte's worth; the bool collapse hides
  any tearing).  An `atomic_load` here would change the field's
  storage class for every other reader; out of scope.
- `pls_nexports`: `_Atomic uint32_t`; acquire-load matches the
  publish-store pattern in `ps_state_listener_append_export`.
- `pls_nlocal_addrs`: plain read.  Seeded once at
  `ps_state_register` time and immutable thereafter
  (ps_state.h:316-324 comment); the release-store on
  `ps_nlisteners` fences every byte of the local-addr table.

## C client row format

`lib/probe1/probe1_client.c`, `ps_listener_list_cb` (line 744-).
Extend the printf row from the existing 4-column layout to add
two suffix columns:

- `SC` (bool): "Y" if `ppli_sc_installed`, "N" otherwise
- `EXPORTS` (uint): `ppli_nexports`

Skip `ppli_root_fh_resolved` and `ppli_nlocal_addrs` from the
C CLI; they're available via the underlying XDR for callers that
want them (the Python CLI surfaces all four).  Justification:
C CLI is for compact-row admin glance; "SC" + "EXPORTS" are the
two most-asked-for fields ("is the optimisation on?" + "is
discovery done?").

## Python CLI

`scripts/reffs-probe.py.in`, `ps_listener_list_cmd`.  Add four
columns to the row format:

```
LID  UPSTREAM        PORT   SESS RECONN_SEC SC  ROOT_FH NEXP NADDR
```

Width target: 80 columns.  Move `RECONN_SEC` to the right of
SESS so the cluster reads "is it up + when retry"; the new
columns hug the right edge so the existing left layout doesn't
shift for muscle-memory readers.

## Implementation order

1. Write the 7 unit tests in
   `lib/nfs4/ps/tests/ps_listener_list_observability_test.c`.
   They will fail to compile against the unmodified XDR struct.
2. Append the four fields to `probe_ps_listener_info1` in
   `lib/xdr/probe1_xdr.x`.  Run `xdr-parser` (auto-fires via
   the build) to regenerate C + Python XDR.
3. Update `lib/probe1/probe1_server.c`
   `probe1_op_ps_listener_list` per-listener fill block.
4. Update `lib/probe1/probe1_client.c` `ps_listener_list_cb`
   row format (C CLI).
5. Update `scripts/reffs-probe.py.in` `ps_listener_list_cmd`
   row format (Python CLI).
6. Wire the new test file into `lib/nfs4/ps/tests/Makefile.am`.
7. `make -j$(nproc)` -- zero warnings.
8. `make check` -- 22 + 7 = 29 PS tests, all pass.
9. `make -f Makefile.reffs style` + `license` -- clean.
10. Push branch, dreamer CI: `make -f Makefile.reffs ci-full`.

## Test impact analysis (planner rule 2)

| Existing test | Impact | Reason |
|---------------|--------|--------|
| Every existing `lib/nfs4/ps/tests/*` test | PASS | No production code path is changed.  The XDR encode/decode of the existing fields is unchanged; new fields are zero-initialised in legacy serialisations (probe1 is internal-only so legacy serialisations only exist for the duration of a single rebuild). |
| `lib/probe1/probe1_client.c` consumers (C CLI) | PASS | New `SC` and `EXPORTS` columns are append-only on the row.  Output diff is visible but non-breaking. |
| `scripts/reffs-probe.py.in` `ps-listener-list` formatter | PASS | Same append-only logic on the row. |

No existing test must be modified.  No production code path
outside the handler is touched.

## Risk

Very low.  All four fields are reads of existing per-listener
state; no new lifetime, no new lock, no new fan-out.  The XDR
change is internal-only (reviewer rule 9 explicit exemption).
The slice is ~80 LOC of substantive change (XDR + handler + CLI
+ tests) plus boilerplate -- under the 150-LOC reviewer-gating
threshold per the `.claude/CLAUDE.md` "Skip the reviewer agent
(review inline)" rule for test-and-CLI-only changes.

## Deferred

- A `--json` / `--verbose` flag on the Python CLI to dump the
  full per-listener record instead of the compact row.  Useful
  for scripting but not required for an admin glance.  Tracked
  as NOT_NOW_BROWN_COW.
- Per-listener short-circuit-hit ratio (dispatch hits / dispatch
  attempts).  Requires a second counter (dispatch attempts) that
  doesn't exist yet -- `pls_shortcircuit_total` is already
  surfaced via `ps-write-buffer-stats` op 37.  Adding the ratio
  is a follow-up slice and not in scope here.
