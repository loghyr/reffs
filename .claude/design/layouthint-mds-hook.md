<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Slice 2: MDS validate-and-accept of `ffv2_layouthint4`

## Context

Slice 1 (`5336a0adc300`) landed the XDR extension: `ffv2_layouthint4`
gained `ffv2lh_stripe_unit` (uint32) and `ffv2lh_expected_file_size`
(uint64), plus the prefix rename `fflh_ -> ffv2lh_`.  Today the
SETATTR handler returns `NFS4ERR_ATTRNOTSUPP` for any compound that
sets `layout_hint`.

Slice 2 makes the MDS accept `SETATTR(layout_hint)` for the FFv2
layout type with validated `ffv2_layouthint4` contents.  The hint
is NOT stored on the inode and NOT used to influence LAYOUTGET in
this slice -- the actual striping policy that consumes the hint
remains NOT_NOW_BROWN_COW.  The deliverable is a wire-level
acceptance path so the bench / ec_demo can issue hints and the MDS
log line confirms the round-trip.

## Decisions

1. **Validate-only, no storage.**  The hint is decoded, range-
   checked, and TRACE'd.  The inode does not grow a field this
   slice; striping policy is the consumer and that's a separate
   slice.  Storage without a consumer is dead bookkeeping.

2. **Do not advertise via `supported_attributes` yet.**  Per
   CLAUDE.md "Attribute bitmap and supported_attributes" rule:
   advertising an attr that returns empty/zero data on GETATTR
   can cause the Linux NFS client to loop / hang during mount.
   `layout_hint` is write-only by RFC 8881 sec 5.12.4, so this
   risk is limited -- but advertising before there's a storage
   read-back path is premature.  Kept cleared.  Slice 3 (or
   slice 2b) will toggle the advertisement once storage lands.

   Well-behaved clients won't send `SETATTR(layout_hint)` while
   the attr is not advertised.  But ec_demo (which we control)
   and unit tests can drive the path to exercise validation.

3. **FFv2 only.**  `loh_type != LAYOUT4_FLEX_FILES_V2`
   returns `NFS4ERR_NOTSUPP`.  The slice doesn't address FFv1
   `ff_layouthint4` (`fflh_mirrors_hint`) -- that surface is
   pre-existing and unchanged.

4. **stripe_unit bounds.**  `[4096, 8 * 1024 * 1024]`.  Zero is
   "no hint" and accepted.  Out-of-range non-zero returns
   `NFS4ERR_INVAL`.  Rationale: 4 KiB floor matches the runway's
   minimum allocation grain; 8 MiB ceiling caps the
   per-mirror chunk size at a manageable per-RPC payload.
   Power-of-two is `SHOULD` in the draft prose but not
   enforced -- the server can round to alignment if needed
   when storage lands.

5. **expected_file_size: no range check.**  Any uint64 is
   acceptable.  Zero is "no hint".  Even ridiculous values are
   advisory and the server is free to ignore.

## Test plan

### Test impact analysis

Per `.claude/roles.md` planner rule 2: this slice touches
`nattr_is_settable()` (existing function, adds one case) and
`nattr_to_inode()` (existing function, adds one case in the
NFSv4-specific loop).  Files affected:

| Existing test file | Impact |
|---|---|
| All `make check` tests | PASS unchanged.  No advertised-attr change, no inode struct change, no on-disk format change. |

New test: `lib/nfs4/tests/setattr_layouthint_test.c`.

### Unit tests

| Test | Intent |
|---|---|
| `test_layouthint_accept_zero_hints` | All four fields zero -> NFS4_OK |
| `test_layouthint_accept_valid_full_hints` | RS 4+2, 1 MiB stripe, 16 GiB expected -> NFS4_OK |
| `test_layouthint_reject_stripe_below_floor` | stripe_unit=512 -> NFS4ERR_INVAL |
| `test_layouthint_reject_stripe_above_ceiling` | stripe_unit=64 MiB -> NFS4ERR_INVAL |
| `test_layouthint_accept_stripe_at_floor` | stripe_unit=4096 -> NFS4_OK |
| `test_layouthint_accept_stripe_at_ceiling` | stripe_unit=8 MiB -> NFS4_OK |
| `test_layouthint_reject_non_ffv2_type` | loh_type=LAYOUT4_FLEX_FILES (v1) -> NFS4ERR_NOTSUPP |
| `test_layouthint_reject_bad_xdr_body` | loh_body too short to decode -> NFS4ERR_BADXDR |

The validation logic is extracted as a helper
`nfs4_layouthint_validate(const fattr4_layout_hint *hint)` so
the unit tests don't need the full SETATTR compound plumbing.
The helper is the only new public-ish symbol; the SETATTR
case in `nattr_to_inode` just calls it.

### Functional tests

Deferred until slice 3 (ec_demo CLI flags) and slice 4 (storage +
LAYOUTGET consumer) -- without ec_demo emitting the hint, there's
no end-to-end functional path to exercise.

## Implementation

### Constants

`lib/nfs4/include/nfs4/layout.h` (or attr.h if layout.h is
inappropriate): add

```c
#define LAYOUTHINT_STRIPE_UNIT_MIN 4096u
#define LAYOUTHINT_STRIPE_UNIT_MAX (8u * 1024u * 1024u)
```

### Helper

`lib/nfs4/server/attr.c` near the existing helpers (after
`nattr_release` / before `nattr_from_fattr4`):

```c
/*
 * Validate an SETATTR-supplied layout_hint per
 * draft-haynes-nfsv4-flexfiles-v2 sec-ffv2-layouthint.
 *
 * Returns NFS4_OK if the hint is acceptable (including the zero
 * "no hint" form).  Returns NFS4ERR_NOTSUPP if the hint's
 * layout type isn't one we support, NFS4ERR_BADXDR if the
 * embedded body can't be decoded, NFS4ERR_INVAL if a range-
 * checked field is out of bounds.
 *
 * Slice 2: validate-only.  The decoded values are TRACE'd and
 * discarded -- storage is a separate slice once the striping
 * policy that would consume the hint exists.
 */
static nfsstat4
nfs4_layouthint_validate(const fattr4_layout_hint *hint)
{
    if (hint->loh_type != LAYOUT4_FLEX_FILES_V2)
        return NFS4ERR_NOTSUPP;

    ffv2_layouthint4 lh = { 0 };
    XDR xdrs;

    xdrmem_create(&xdrs,
                  hint->loh_body.loh_body_val,
                  hint->loh_body.loh_body_len,
                  XDR_DECODE);

    if (!xdr_ffv2_layouthint4(&xdrs, &lh))
        return NFS4ERR_BADXDR;

    if (lh.ffv2lh_stripe_unit != 0 &&
        (lh.ffv2lh_stripe_unit < LAYOUTHINT_STRIPE_UNIT_MIN ||
         lh.ffv2lh_stripe_unit > LAYOUTHINT_STRIPE_UNIT_MAX)) {
        xdr_free((xdrproc_t)xdr_ffv2_layouthint4, &lh);
        return NFS4ERR_INVAL;
    }

    TRC("layouthint: stripe_unit=%u expected_size=%llu "
        "supported_types_len=%u protection=%u/%u",
        lh.ffv2lh_stripe_unit,
        (unsigned long long)lh.ffv2lh_expected_file_size,
        lh.ffv2lh_supported_types.ffv2lh_supported_types_len,
        lh.ffv2lh_preferred_protection.fdp_data,
        lh.ffv2lh_preferred_protection.fdp_parity);

    xdr_free((xdrproc_t)xdr_ffv2_layouthint4, &lh);
    return NFS4_OK;
}
```

### nattr_is_settable

Add one case:

```c
case FATTR4_LAYOUT_HINT:
    return true;
```

### nattr_to_inode

Add one case in the NFSv4-specific switch:

```c
case FATTR4_LAYOUT_HINT: {
    nfsstat4 hs = nfs4_layouthint_validate(&nattr->layout_hint);
    if (hs != NFS4_OK) {
        pthread_mutex_unlock(&inode->i_attr_mutex);
        status = hs;
        goto out;
    }
    /* Validated, not stored -- striping policy is NOT_NOW_BROWN_COW. */
    break;
}
```

### Comment update

In `nattr_is_settable()` doc comment, remove `layout_hint`
from the "Unsupported write attrs" list.

In `nattr_to_inode()`, no comment update needed -- the
NOT_NOW_BROWN_COW about striping policy stays in
`lib/nfs4/server/layout.c:710`.

### What's NOT changed

- `supported_attributes` bitmap.  `layout_hint` stays cleared
  (line 2311) per decision 2 above.
- `struct inode`.  No new fields.
- On-disk format.  Unchanged.
- LAYOUTGET handler.  Unchanged.
- Slice 1's XDR.  Unchanged.

## Reviewer-agent gating

Per `.claude/CLAUDE.md` "Review before commit (gated)":
- No XDR change (slice 1 already landed that)
- No on-disk format change
- No RCU / ref-counting lifecycle change
- No lock-ordering change
- No cross-layer addition (server-side only, attr.c + new test file)
- Under 150 LOC substantive change (helper ~25 LOC + 2 case
  additions ~6 LOC + new test file ~150 LOC fixture-heavy)

Falls in the "inline review" category.  Reviewer agent is not
required.  Inline review against standards.md + reviewer
checklist suffices.

## Deferred / NOT_NOW_BROWN_COW

- **Slice 2b**: store validated hint on inode (`i_layouthint_*`
  fields), persist via the existing inode_sync path.  Lands
  when slice 3 (ec_demo CLI) wants round-trip evidence the
  hint survives.  Inode struct change triggers cross-layer
  scrutiny -- own slice.
- **Slice 2c**: advertise `FATTR4_LAYOUT_HINT` in
  `supported_attributes` (un-clear from the bitmap).  Lands
  with slice 2b so a GETATTR readback works.
- **Slice 4**: the actual striping policy at LAYOUTGET that
  reads the hint and picks single-mirror vs striped layout.
  Removes the NOT_NOW_BROWN_COW at `layout.c:710`.  Substantial
  work -- separate design.
- **Slice 5**: persistence of the hint across reboots.  Not
  meaningful until slice 4 exists.

## Cost

Helper + cases: 30 min.  Test file: 1-2 hours (libcheck
boilerplate + 8 cases).  Build + inline review + commit:
30 min.  Total: ~3 hours.
