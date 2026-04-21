# EC / CHUNK Fault-Injection Test Plan

## Motivation

Today's EC coverage is happy-path only: 4 codecs x 5 file sizes,
Tier 1-4 benchmarks (deploy/benchmark/, scripts/ec_benchmark*.sh).
Nothing deliberately exercises:

- **Write holes** -- mid-stripe failure where some DSes hold new
  data and others still hold old data.
- **Bad CRCs** -- server-rejected writes, client-detected reads,
  disk bit-rot between FINALIZE and READ.
- **CHUNK state-machine errors** -- PENDING stuck, owner_id
  mismatch, invalid transitions.
- **Degraded reads** -- exactly-k shards, k-1 shards, mixed
  missing/corrupt.
- **Client or MDS crash mid-stripe** -- what recovery is
  actually required, and what's the observable symptom?
- **Crash consistency of chunk_store persistence** -- torn
  writes to `<state_dir>/chunks/<ino>.meta` between fsync and
  rename.
- **Async I/O failure** -- io_uring submission / CQE -EIO on
  the non-RAM data path.

The goal is to put knobs on ec_demo and on reffsd so we can
inject each failure deterministically, observe the outcome, and
lock down expected behavior for the recovery primitives
(LAYOUTERROR, fencing, trust-stateid revoke, the eventual
CHUNK_ROLLBACK).

## Scope (in / out)

**In scope**:

1. ec_demo `--inject` flag family (client-side injection).
2. reffsd opt-in fault-injection build
   (`--enable-fault-injection`) with env-var control.
3. Decoder-gap + randomized round-trip unit tests in
   `lib/ec/tests/`.
4. Chunk-block state introspection probe op (`SB_CHUNK_STATE`)
   so fault-injection tests have a verifiable observable.
5. Chunk_store persistence crash-consistency test.
6. io_uring async-WRITE failure injection on the POSIX data
   path.
7. Functional tests under `scripts/ci_ec_fault_test.sh`.
8. A documented expected-outcome matrix per scenario.

**Deferred** (tracked under NOT_NOW_BROWN_COW):

- CHUNK_ROLLBACK, CHUNK_LOCK, CHUNK_UNLOCK, CHUNK_REPAIRED
  implementation.  The ops return NFS4ERR_NOTSUPP today; tests
  that depend on them are XFAIL with a pointer to the draft
  cref (§10.3 / §10.5 unresolved).
- Whole-file repair scenarios (draft §10.6 is a cref).
- MDS-to-DS trust-stateid flows -- tracked in
  `.claude/design/trust-stateid.md` Phase 1 Group E.
- Kernel-client fault tests -- ec_demo is the only controlled
  client.
- `SB_INJECT_FAULT` probe op for multi-phase scenarios (env var
  + one-reffsd-per-scenario is sufficient for fi-01..fi-14;
  runtime re-injection becomes load-bearing only if a test
  needs to switch fault modes between phases without restart).

## Test impact on existing tests

| File | Impact | Reason |
|------|--------|--------|
| Existing `make check` unit tests | PASS | All new code is additive, guarded by `--enable-fault-injection` |
| `scripts/ec_benchmark.sh` / `ec_benchmark_full.sh` | PASS | Injection is off by default |
| `scripts/ci_soak_test.sh` | PASS | Soak is build-loop stability, EC-independent |
| `ci_integration_test.sh` | PASS | Integration git-clone test uses no EC |
| `tools/ec_demo.c` | EXTEND | Add `--inject MODE[:ARG]`; existing flags unchanged |
| `lib/xdr/probe1_xdr.x` | EXTEND | New `SB_CHUNK_STATE` op (see §5) |

The existing `--skip-ds LIST` flag (ec_demo.c line 835-859)
already does read-side client injection.  It stays; new `--inject`
modes are orthogonal.

## Injection and observation surfaces

### A. Client side: ec_demo `--inject MODE[:ARG]`

One flag, enumerated modes.  Modes are mutually exclusive per
invocation; the harness runs each mode in a separate process.
Arg-parsing follows the `scripts/ec_benchmark.sh` `--only` idiom.

| Mode | Effect | Implementation touch |
|------|--------|---------------------|
| `drop-shard:IDX` | Skip CHUNK_WRITE to DS index IDX | ec_client.c write loop |
| `bad-crc:SHARD_IDX` | Compute correct CRC, XOR with 0xDEADBEEF before send | chunk_io.c encode path |
| `corrupt-payload:SHARD_IDX` | Flip one byte in shard data after CRC calc | chunk_io.c post-encode |
| `finalize-skip` | Send CHUNK_WRITE to all DSes, skip CHUNK_FINALIZE | ec_client.c stripe flow |
| `commit-skip` | WRITE + FINALIZE, skip CHUNK_COMMIT | ec_client.c stripe flow |
| `crash-after:STAGE` | `_exit(137)` after STAGE in {write,finalize,commit} | ec_client.c hooks |
| `stale-stateid` | Use all-zero stateid instead of issued layout stateid | chunk_io.c args fill |
| `bad-owner` | Second write with cg_client_id bumped | chunk_io.c owner construction |
| `double-write:OFFSET` | Two concurrent CHUNK_WRITEs (pthread) | ec_client.c |

Implementation in a new `tools/ec_demo_inject.c`:
`ec_inject_point_t ec_inject_get(void)` reads env var + CLI state
once, returns a tagged union.  Injection sites consult it with
`if (ec_inject_get().mode == EC_INJECT_DROP_SHARD && ...)`.

### B. Server side: reffsd `--enable-fault-injection` build

Opt-in at configure time (default off -- production builds have
zero injection code).  Three security guards, all required:

1. Configure option `--enable-fault-injection` gates the entire
   `HAVE_FAULT_INJECTION` preprocessor symbol.  No distro builds
   shall ship with this enabled.
2. The `REFFS_EC_INJECT` env var is read only if
   `HAVE_FAULT_INJECTION` is defined.
3. The probe dispatch table entry for any future
   `SB_INJECT_FAULT` op returns NFS4ERR_NOTSUPP when
   `HAVE_FAULT_INJECTION` is off (not absent, to preserve RPC
   wire compat).  See Deferred section.

These guards are named explicitly so that an injection-disabled
build exposes zero new attack surface.  Acceptance: a build
without `--enable-fault-injection` produces a reffsd binary with
no symbols containing `fault_inject`, `ec_inject`, or
`REFFS_EC_INJECT` (verifiable by `nm build/src/reffsd | grep -i
fault`).

**Tradeoff to name explicitly**: ASAN / UBSAN clean across
fault-injection runs does not prove the *production* binary
is clean.  Injection code is byte-for-byte absent from the
shipped binary, so a bug that only appears under injection
proves nothing about non-injected code.  This is the standard
outcome for compile-time test builds; the plan documents it so
no one mistakes green fault-injection CI for a shippable
guarantee.

Env var `REFFS_EC_INJECT=<MODE>[;<MODE>...]` is read at
`chunk.c` and `chunk_store.c` entry points.

| Mode | Effect |
|------|--------|
| `crc-rot-on-write` | On CHUNK_WRITE, after validation, XOR stored cb_crc32 with 0xFEEDFACE before persist -- a later CHUNK_READ fails recomputed-CRC check (simulates bit-rot) |
| `drop-pending-after:N` | After N CHUNK_WRITEs, delete the PENDING block without acking |
| `fail-finalize:N` | Return NFS4ERR_IO on the Nth CHUNK_FINALIZE |
| `fail-commit:N` | Return NFS4ERR_IO on the Nth CHUNK_COMMIT |
| `slow-write:MS` | Sleep MS ms inside CHUNK_WRITE before responding |
| `reject-all-writes` | All CHUNK_WRITE return NFS4ERR_DELAY |
| `kill-between-fsync-rename:N` | chunk_store persist: after fdatasync of `.meta.tmp`, before rename, abort(3) -- exercises torn-write recovery on restart |
| `iouring-cqe-eio:N` | On the Nth data_block_write completion, replace the CQE with -EIO |

Each mode also logs a single `LOG("fault-injection: MODE fired")`
line so the test harness can grep for it.

All injection code is under `#ifdef HAVE_FAULT_INJECTION`.

### C. Observation surface: `SB_CHUNK_STATE` probe op (new)

Without this, several fault scenarios have unverifiable
assertions -- e.g., fi-05 "blocks stuck PENDING" is
indistinguishable from "CHUNK_READ returned a hole because
nothing was ever written."

New op in `lib/xdr/probe1_xdr.x`:

```xdr
struct SB_CHUNK_STATE1args {
    unsigned hyper  scsa_sb_id;
    unsigned hyper  scsa_inode;        /* inode number */
    offset4         scsa_offset;
    count4          scsa_count;
};
struct probe_chunk_state_entry1 {
    offset4         pcse_offset;
    uint32_t        pcse_state;        /* EMPTY/PENDING/FINALIZED/COMMITTED */
    uint32_t        pcse_owner_client_id;
    uint32_t        pcse_owner_gen_id;
    uint32_t        pcse_crc32;
};
struct SB_CHUNK_STATE1resok {
    probe_chunk_state_entry1 scsr_blocks<>;
};
```

Read-only, always available (not gated by
`--enable-fault-injection`).  Exposes per-block state for a
given inode and range.  Ships with the Python CLI
(`reffs-probe.py sb-chunk-state`) and C client
(`reffs_probe1_clnt --op sb-chunk-state`) at the same time, per
roles.md rule 9.

Used in every functional test to assert the server's internal
state matches what the injection should have produced.  Also
useful outside fault testing: the RocksDB migration and
observability work will want it.

## State machines exercised

Chunk-block states (chunk_store.h line 31-36):

```
EMPTY --[CHUNK_WRITE]--> PENDING --[CHUNK_FINALIZE]--> FINALIZED
                             |                              |
                             |                      [CHUNK_COMMIT]
                             v                              v
                        (lost on                      COMMITTED
                         drop-pending)
```

Transitions the fault matrix forces the server to handle:

| From | To | Trigger | Expected | Status |
|------|-----|---------|----------|--------|
| EMPTY | PENDING | CHUNK_WRITE (bad CRC) | Rejected at entry, EMPTY preserved | Implemented |
| EMPTY | PENDING | CHUNK_WRITE (drop-shard) | PENDING on k-1 DSes, EMPTY on one | Implemented |
| PENDING | FINALIZED | CHUNK_FINALIZE (different owner_id) | -EINVAL, PENDING preserved for winning owner | Implemented |
| PENDING | FINALIZED | finalize-skip | Stays PENDING indefinitely | XFAIL until CHUNK_ROLLBACK / lease-expiry reaper |
| PENDING | EMPTY | CHUNK_ROLLBACK | NFS4ERR_NOTSUPP | XFAIL (op stubbed) |
| FINALIZED | COMMITTED | CHUNK_COMMIT (fail-commit) | NFS4ERR_IO, stays FINALIZED | Needs test (fi-10) |
| FINALIZED | EMPTY | CHUNK_ROLLBACK post-FINALIZE | NFS4ERR_NOTSUPP | XFAIL (op stubbed) |

## Test matrix

### C.1 Unit tests (no network; `make check`)

All new, synchronous, under 2 s each.  These are the hard
prerequisite for the functional matrix -- the functional tests
cannot prove what they claim until the decoder correctness tests
pass, because `drop-shard` in the harness is only meaningful if
the codec is known to reconstruct from any k-of-(k+m) subset.

**`lib/ec/tests/rs_degraded_test.c`** (NEW)

| Test | Intent |
|------|--------|
| `test_rs_decode_k_shards_ok` | Encode 4+2, erase 2 shards, decode recovers |
| `test_rs_decode_k_minus_1_fails` | Erase 3 of 6 shards (k=4) -> -EDOM |
| `test_rs_decode_mixed_erase` | 1 data + 1 parity erased, decode recovers |
| `test_rs_decode_all_present_systematic` | No erasures, systematic path unchanged |
| `test_rs_decode_randomized_roundtrip` | 200 iterations, random k-of-(k+m) subset (seed fixed for reproducibility), assert byte-identity |

**`lib/ec/tests/mojette_degraded_test.c`** (NEW)

| Test | Intent |
|------|--------|
| `test_mojette_sys_healthy_no_decode` | Systematic healthy-read returns data directly |
| `test_mojette_sys_missing_one_data` | One data shard missing, corner-peel recovers |
| `test_mojette_sys_over_degrade` | k+1 shards missing, decode fails |
| `test_mojette_nonsys_always_decodes` | Non-systematic decode always runs |
| `test_mojette_sys_randomized_roundtrip` | 200 iterations, fixed seed, random erasure pattern |
| `test_mojette_nonsys_variable_proj_size` | Shard sizes vary by direction, verify projection-size calculation (ceiling division per goals.md Known Issues) |

**`lib/nfs4/server/tests/chunk_state_machine_test.c`** (NEW)

| Test | Intent |
|------|--------|
| `test_chunk_state_finalize_wrong_owner` | PENDING(owner=A) + FINALIZE(owner=B) -> -EINVAL |
| `test_chunk_state_commit_before_finalize` | PENDING + CHUNK_COMMIT -> -EINVAL |
| `test_chunk_state_finalize_twice` | FINALIZED + CHUNK_FINALIZE -> -EINVAL |
| `test_chunk_state_rollback_notsupp` | Documents current NFS4ERR_NOTSUPP stance |

**`lib/nfs4/server/tests/chunk_crc_test.c`** (NEW)

| Test | Intent |
|------|--------|
| `test_chunk_write_bad_crc_rejected` | Mismatching CRC -> NFS4ERR_INVAL, no PENDING block |
| `test_chunk_write_empty_crc_skip` | cwa_crc32s_len=0 skips validation (documents current) |
| `test_chunk_read_recompute_ok` | Clean round-trip |
| `test_chunk_read_after_crc_rot` | Injected `crc-rot-on-write`, read detects |

**`lib/nfs4/server/tests/chunk_persist_crash_test.c`** (NEW)

| Test | Intent |
|------|--------|
| `test_chunk_persist_torn_fsync_rename` | Use `kill-between-fsync-rename:1` + in-process simulated restart; assert old `.meta` is intact, tmp file is removed, state is the pre-write committed view |
| `test_chunk_persist_no_tmp_leak` | After N injected failures, scan `<state_dir>/chunks/*.meta.tmp` -> zero stale files |

### C.2 Functional tests (`scripts/ci_ec_fault_test.sh`)

Single reffsd in combined mode + ec_demo.  **Tests run
sequentially; the harness is not parallel-safe** (server-side
injection is global per process).  Each scenario is a separate
bash function; the driver asserts expected exit code, log
pattern, and post-test `SB_CHUNK_STATE` output.

| ID | Injection | Scenario | Expected |
|----|-----------|----------|----------|
| fi-01 | none | Baseline put/get round-trip | Pass (sanity) |
| fi-02 | client `drop-shard:2` | RS 4+2 write with 1 shard dropped | Per-owner status array: 5 OK, 1 missing; client LAYOUTERROR |
| fi-03 | client `drop-shard:0,1` | 2 shards dropped (at m limit) | Parity covers; subsequent read succeeds |
| fi-04 | client `bad-crc:0` | Single bad CRC | Server NFS4ERR_INVAL for that shard; other 5 PENDING per SB_CHUNK_STATE |
| fi-05 | client `finalize-skip` | WRITE all, never FINALIZE | SB_CHUNK_STATE shows PENDING on all DSes; CHUNK_READ returns hole (draft §22.6) |
| fi-06 | client `commit-skip` | WRITE + FINALIZE, skip COMMIT | SB_CHUNK_STATE shows FINALIZED; restart server; post-restart state is whatever the design says (test captures the answer) |
| fi-07 | client `crash-after:write` | Process dies after CHUNK_WRITE loop | SB_CHUNK_STATE shows PENDING; second ec_demo instance gets owner_id mismatch on FINALIZE |
| fi-08 | server `crc-rot-on-write` | Write succeeds, stored CRC corrupted | CHUNK_READ fails client CRC -> -EIO; `ec_demo read --codec rs --skip-ds THAT_DS` reconstructs from parity |
| fi-09 | server `fail-finalize:1` | First FINALIZE returns NFS4ERR_IO | Client retries (assert) or LAYOUTERROR; SB_CHUNK_STATE shows PENDING |
| fi-10 | server `fail-commit:1` | First COMMIT returns NFS4ERR_IO | Client retries, eventual COMMITTED |
| fi-11 | client `stale-stateid` | Anonymous stateid with tight coupling | XFAIL -- trust-stateid Phase 1 Group E, pending |
| fi-12 | client `--skip-ds 0,1,2` on read | Over-degrade (k=4, m=2) | Decode fails with specific error; ec_demo surfaces it |
| fi-13 | client `double-write:OFFSET` | Two concurrent CHUNK_WRITEs | XFAIL until CHUNK_LOCK implemented; today one client's FINALIZE fails owner_id |
| fi-14 | server `slow-write:5000` | 5 s server delay | Client timeout behavior observable |
| fi-15 | server `kill-between-fsync-rename:1` | Torn persist during CHUNK_FINALIZE | Post-restart: `.meta.tmp` absent, `.meta` intact, SB_CHUNK_STATE matches pre-FINALIZE view |
| fi-16 | server `iouring-cqe-eio:1` on POSIX data | First data_block_write CQE is -EIO | CHUNK_WRITE returns NFS4ERR_IO; SB_CHUNK_STATE shows no PENDING block |

Harness usage (matches `ec_benchmark.sh` style):

```
scripts/ci_ec_fault_test.sh             # run all
scripts/ci_ec_fault_test.sh --only fi-05
scripts/ci_ec_fault_test.sh --list      # print scenario IDs + descriptions
```

Each scenario asserts:
1. ec_demo exit code matches expected.
2. `grep PATTERN reffsd.log` produces expected LOG lines
   (`fault-injection: <MODE> fired`).
3. `reffs-probe.py sb-chunk-state --id SB --inode INO
   --offset O --count N` output matches the expected per-block
   state array.
4. ASAN / LSAN / UBSAN clean.
5. No stale `.meta.tmp` files under `<state_dir>/chunks/`.

### C.3 CI integration

New `make -f Makefile.reffs fault-inject` target, gated on
`--enable-fault-injection`:

```
mkdir build-fi && cd build-fi
../configure --enable-asan --enable-ubsan --enable-fault-injection
make -j$(nproc)
make check                        # C.1 tests
cd .. && scripts/ci_ec_fault_test.sh    # C.2 tests
```

Optional GitHub workflow `ci-ec-fault.yml`, weekly (fault runs
take ~10 min and are noisy; not per-PR).  Local-first.

## Implementation order

Hard ordering -- each step proves the next is testing what we
think it is:

1. **lib/ec/tests** (C.1 decoder tests, including randomized
   round-trip).  These prove the codecs actually reconstruct
   from any k-of-(k+m) subset.  Without this, `drop-shard` in
   the harness can "pass" for the wrong reason.
2. **Server-side chunk_store unit tests**
   (`chunk_state_machine_test.c`, `chunk_crc_test.c`,
   `chunk_persist_crash_test.c`).  Pure C, direct API, no
   network.
3. **`SB_CHUNK_STATE` probe op** (§C).  Must exist before
   functional tests so they have a verifiable observable.
4. **ec_demo `--inject` flag + `tools/ec_demo_inject.c`.**
   Client-side injection.  Start with `drop-shard`, `bad-crc`,
   `finalize-skip`.
5. **reffsd `--enable-fault-injection` + `REFFS_EC_INJECT`**
   env var.  `#ifdef`-gated.  Start with `crc-rot-on-write`,
   `fail-finalize:N`.
6. **Extended server injection**
   (`kill-between-fsync-rename`, `iouring-cqe-eio`).
7. **`scripts/ci_ec_fault_test.sh` harness**, wire fi-01..fi-16
   (fi-11 and fi-13 XFAIL).
8. **Optional GitHub weekly workflow** `ci-ec-fault.yml`.

`SB_INJECT_FAULT` runtime re-injection probe op is NOT in this
list -- deferred until a scenario needs fault-mode switching
without reffsd restart.

## RFC and draft references

- draft-haynes-nfsv4-flexfiles-v2 §10 (Erasure Coding /
  Write holes) -- motivates the write-hole scenarios.
- draft-haynes-nfsv4-flexfiles-v2 §10.2 - §10.5 (repair
  sequences) -- fault tests document what the recovery
  primitives should do; surfaces the unresolved crefs.
- draft-haynes-nfsv4-flexfiles-v2 §22.6 (CHUNK_READ with hole)
  -- expected CHUNK_READ output shape for a missing stripe
  (the assertion for fi-05).
- draft-haynes-nfsv4-flexfiles-v2 §22.11 (CHUNK_WRITE
  vet-first vs first-error, unresolved cref) -- fi-04 will
  lock down whatever answer ships.
- RFC 8881 §18.42 (LAYOUTCOMMIT), §18.44 (LAYOUTRETURN) --
  client-to-MDS error reporting that fi-02 exercises.

## Deferred / NOT_NOW_BROWN_COW

- CHUNK_ROLLBACK-based recovery (fi-07 tail, scenarios beyond
  fi-07 that would retry after rollback).
- CHUNK_LOCK-based multi-writer contention (fi-13).
- MDS LAYOUTERROR -> fence -> REVOKE_STATEID -> re-issue loop:
  needs trust-stateid Phase 2.
- Whole-file repair from SPARE DSes (draft §10.6 unresolved).
- External bit-rot mutator that modifies `.dat` on a running
  reffsd (fi-08 is CRC-rot, not data-rot; a true data-rot
  simulator is larger scope).
- `SB_INJECT_FAULT` runtime op for mid-test fault switching.

## Key files

| File | Change |
|------|--------|
| `tools/ec_demo.c` | `--inject MODE[:ARG]` CLI |
| `tools/ec_demo_inject.c` / `.h` | NEW -- injection state |
| `lib/nfs4/client/ec_client.c` | Consult inject points in write loop |
| `lib/nfs4/client/chunk_io.c` | CRC tamper / payload corrupt hook |
| `configure.ac` | `--enable-fault-injection` option |
| `lib/nfs4/server/chunk.c` | `#ifdef HAVE_FAULT_INJECTION` hooks |
| `lib/nfs4/server/chunk_store.c` | `crc-rot`, `kill-between-fsync-rename` hooks |
| `lib/io/ring.c` (or equivalent) | `iouring-cqe-eio` hook on CQE path |
| `lib/ec/tests/rs_degraded_test.c` | NEW |
| `lib/ec/tests/mojette_degraded_test.c` | NEW |
| `lib/nfs4/server/tests/chunk_state_machine_test.c` | NEW |
| `lib/nfs4/server/tests/chunk_crc_test.c` | NEW |
| `lib/nfs4/server/tests/chunk_persist_crash_test.c` | NEW |
| `scripts/ci_ec_fault_test.sh` | NEW |
| `lib/xdr/probe1_xdr.x` | `SB_CHUNK_STATE` op (new) |
| `lib/probe1/probe1_server.c` | Handler |
| `lib/probe1/probe1_client.c` | Wrapper |
| `scripts/reffs/probe_client.py.in` | `sb_chunk_state()` method |
| `scripts/reffs-probe.py.in` | `sb-chunk-state` subcommand |

## Acceptance criteria

- All C.1 unit tests pass (added to `make check`; each < 2 s).
- `scripts/ci_ec_fault_test.sh` runs scenarios fi-01..fi-16,
  all pass or XFAIL with a documented reason pointing at a
  specific draft section or design doc.
- ASAN / UBSAN clean across all fault runs.
- No production-build regression: `nm build/src/reffsd | grep
  -iE '(fault_inject|ec_inject|REFFS_EC_INJECT)'` is empty in
  a default-configure build.
- Harness is not parallel-safe and says so in
  `scripts/ci_ec_fault_test.sh` header (server-side injection
  is global per reffsd process).
- A fresh contributor can reproduce each scenario by running
  `scripts/ci_ec_fault_test.sh --only fi-NN`.
- Post-test server state (verified by `SB_CHUNK_STATE`) matches
  the documented expectation for every scenario.

## Known limits of this test strategy

- ASAN / UBSAN green under injection does not prove the
  production binary (built without `--enable-fault-injection`)
  is clean; injection code is compiled out.
- Every fault test runs against combined-mode reffsd (MDS + DS
  in one process).  Cross-host failure modes (network partition
  between MDS and remote DS) are not covered here; they belong
  in a future integration-test plan.
- The decoder randomized round-trip uses fixed seeds for
  reproducibility.  A failure found by the seed we pick will be
  caught; a failure that needs a different seed won't.  The
  plan accepts this tradeoff -- property-based fuzzing with
  shrinking is out of scope.
