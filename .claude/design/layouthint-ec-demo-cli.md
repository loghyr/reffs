<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Slice 3: ec_demo CLI flags for ffv2_layouthint4

## Context

Slice 1 (`5336a0adc300`) shipped the XDR extension.  Slice 2
(`4b895b2fb79d`) made SETATTR(layout_hint) accept-and-validate
on the MDS, with a unit test driving the validator directly.

Slice 3 adds the missing client-side surface: ec_demo CLI flags
that populate an `ffv2_layouthint4` in the OPEN(CREATE)
createattrs of new files.  This closes the end-to-end loop --
operator runs ec_demo, the MDS TRACE log line confirms the
four hint fields round-tripped over the wire.  That TRACE line
is the slide evidence for the IETF 126 deck row 22h.

## Decisions

1. **Hint rides OPEN(CREATE).cva_attrs, not a separate SETATTR.**
   Per RFC 8881 sec 5.12.4 the hint is most useful at creation
   time when the MDS has no usage history.  Embedding in
   createattrs costs zero extra RPC round-trips.

2. **Optional `ffv2_layouthint4 *hint` parameter on
   `mds_file_open()`.**  NULL preserves the existing
   "createattrs zero-initialized" behaviour for all call sites
   that don't opt in.  ec_demo's four `mds_file_open` call sites
   pass NULL except for the file-creation paths that should
   carry the hint (decided per subcommand below).

3. **Hand-encode the FATTR4_LAYOUT_HINT blob inline in
   `mds_file_open`.**  Pattern matches the existing
   FATTR4_OWNER / FATTR4_OWNER_GROUP encoding in the same
   file (~line 850).  Avoids adding a new abstraction.

4. **CLI flags are file-creation scoped, not session scoped.**
   ec_demo subcommands that create files (put, ec_write,
   ec_write_codec_*) accept the flags.  Subcommands that only
   read or operate on existing files (get, check) ignore them
   even if passed.  The hint applies to the LAYOUTGET that
   *follows* the file create -- pre-existing files already
   have their layout choices baked in (per slice-2 design,
   the MDS SHOULD ignore the hint on grown files).

5. **No CLI default.**  Absent flags = no hint.  Zero is the
   wire-level "no hint" value, but I don't want to send a
   zero-valued hint by default since that consumes wire
   bytes for no benefit.  Caller opts in by passing
   `--stripe-unit-hint` and/or `--expected-size-hint`.

6. **Defer per-flavor (krb5) testing.**  AUTH_SYS is sufficient
   for the slide evidence.  Kerberos-flavored ec_demo lives
   in its own test path.

## Wire shape

```
OPEN args:
  ...
  createhow.mode = UNCHECKED4
  createhow.createattrs.attrmask = bitmap4 with bit 63 set
  createhow.createattrs.attr_vals = XDR-encoded fattr4_layout_hint:
    loh_type = LAYOUT4_FLEX_FILES_V2
    loh_body = XDR-encoded ffv2_layouthint4:
      ffv2lh_supported_types = { FFV2_ENCODING_RS_VANDERMONDE } /* hardcoded */
      ffv2lh_preferred_protection = { fdp_data=K, fdp_parity=M } /* from --K/--M */
      ffv2lh_stripe_unit = $stripe_unit_hint
      ffv2lh_expected_file_size = $expected_size_hint
```

`ffv2lh_supported_types` is hardcoded to RS_VANDERMONDE because
ec_demo always uses RS (the codec is fixed per
`--codec` and the slide cares about the size+stripe hint, not
the codec negotiation surface).  `ffv2lh_preferred_protection`
mirrors ec_demo's existing `--K` / `--M` geometry flags.  The
two new flags fill `ffv2lh_stripe_unit` and
`ffv2lh_expected_file_size`.

## API change

```c
/* lib/nfs4/include/nfs4/mds_file.h or wherever mds_file_open lives */

/*
 * @hint -- optional ffv2_layouthint4 payload to ride
 * OPEN(CREATE).createattrs as FATTR4_LAYOUT_HINT.  When NULL
 * the createattrs are zero-initialized (no hint, the prior
 * behaviour).  The MDS validates and TRACE's the hint per
 * slice-2.
 */
int mds_file_open(struct mds_session *ms, const char *path,
                  struct mds_file *mf,
                  const ffv2_layouthint4 *hint);
```

All four ec_demo call sites get a `NULL` parameter added
except the file-creation paths that thread the CLI hint
through.

## CLI surface

New `tools/ec_demo.c` long options:

| Flag | Type | Wire field |
|---|---|---|
| `--stripe-unit-hint=BYTES` | unsigned | ffv2lh_stripe_unit |
| `--expected-size-hint=BYTES` | uint64 | ffv2lh_expected_file_size |

Help text additions land in the usage block near the existing
codec / geometry flags.  Parsing accepts bare bytes and the
common KB/MB/GB suffixes (existing `parse_size()` helper if it
exists; else inline strtoull + suffix).

## Test plan

### Unit tests

`lib/nfs4/tests/setattr_layouthint_test.c` already covers the
MDS validation surface.  Slice 3 adds NO new unit tests --
the encoding helper inside `mds_file_open` produces the same
wire bytes the existing test feeds in, so the validator unit
test still covers the round-trip.

(NOT_NOW_BROWN_COW: a focused unit test for the createattrs
encoder would be nice for future-proofing.  Defer: slice 3's
production code is small and the validator test already
verifies the wire shape on the MDS side.)

### Functional test

`scripts/ci_layouthint_test.sh` (new, optional this slice):
- Start single-host bench MDS on dreamer
- Run `ec_demo put --stripe-unit-hint=1048576 --expected-size-hint=$((16*1024*1024*1024)) testfile`
- `grep "layouthint: stripe_unit=1048576 expected_size=17179869184" mds.log`
- Pass criteria: TRACE line present with the expected values

This is the slide evidence.  If skipped this slice, the
manual evidence-capture is:

```bash
ssh dreamer 'cd reffs-cc-t2 && bash deploy/benchmark/run-ps-bench-bringup.sh'
# ec_demo runs locally against the bench MDS
build/tools/ec_demo put \
    --stripe-unit-hint=1048576 \
    --expected-size-hint=$((16*1024*1024*1024)) \
    test.dat
ssh dreamer 'sudo docker logs reffs-bench-mds 2>&1 | grep layouthint:'
```

The TRACE line confirms slice 1+2+3 are wired end-to-end.

## Reviewer-agent gating

Per `.claude/CLAUDE.md` "Review before commit (gated)":
- No XDR change
- No on-disk format change
- No RCU / ref-counting lifecycle change
- No lock-ordering change
- Cross-layer touch: `mds_file_open()` signature change ripples
  to ec_demo (which lives in `tools/`, not `lib/`)
- Substantive change ~75 LOC

The cross-layer surface is `tools/` calling `lib/nfs4/client/`,
which is the documented architecture (ec_demo is the canonical
consumer of `lib/nfs4/client/`).  Not a new boundary.
Substantive LOC under 150.

Inline review fits.  Reviewer agent not required.

## Risks

- **API signature change** to `mds_file_open()` ripples to 4
  ec_demo call sites + the PS shim that may call it.  Need to
  grep for ALL callers and update them.  PS callers pass NULL.
- **fattr4 hand-encoding** is fiddly.  XDR encoding of a
  string-typed `loh_body` needs the 4-byte length prefix +
  4-byte alignment padding (same as OWNER encoding pattern
  at line 850).  Validator test will catch wire-format drift,
  but the encoder needs care.
- **End-to-end test on dreamer** requires reffs-cc-t2's bench
  to be standing.  If it's torn down, ~15 min to bring up
  again (per the Track 2 N=4/8 bringup earlier this session).

## Implementation order

1. Grep all callers of `mds_file_open` (lib + tools)
2. Add optional `const ffv2_layouthint4 *hint` param
3. Encode hint into createattrs when non-NULL (~25 LOC of XDR
   hand-encoding matching the OWNER pattern)
4. Update all call sites to pass NULL except the
   ec_demo creation paths
5. Add `--stripe-unit-hint` and `--expected-size-hint` to
   ec_demo getopt_long table + usage text
6. Thread CLI values into the hint and pass to mds_file_open
7. Build + run existing tests (no new tests needed, but
   confirm setattr_layouthint_test still PASS)
8. Manual end-to-end on dreamer (steps in functional-test
   section above)
9. Commit + push

## Deferred

- **CLI parsing for KB/MB/GB suffixes.**  Initial slice
  accepts bare bytes only.  Suffix parsing is a polish item.
- **Validator unit test for the encoder side.**  See test
  plan above.
- **Per-mirror hint shape.**  Slice 4 (LAYOUTGET consumer)
  is where per-mirror policy lives.
