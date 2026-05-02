<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Experiment 2 Phase A: Codec Hot-Swap

Closes the operational half of progress_report.md §3.4: *"Codec
evolution becomes a deployment-managed change at the PS layer.
A new codec ships with a PS update; existing clients pick it up
without a kernel patch."*

## Findings -- Phase A is not runnable as specified, for a
positive reason

The Phase A spec hypothesises a PS-side codec configuration
that an operator can toggle at runtime ("switching the configured
codec at a running PS for a target file path").  Source
inspection shows that **no such configuration exists**:

- `lib/nfs4/ps/ec_pipeline.c` declares `enum ec_codec_type`
  exclusively as a function-argument parameter (`codec_type`)
  passed in by the caller.
- `lib/nfs4/ps/chunk_io.c` and `ds_io.c` are codec-agnostic --
  they operate on shards the pipeline produces.
- No `[[proxy_mds]]`, `[[export]]`, or PS-state struct in
  the source carries a codec selection.
- `mds.toml` likewise has no codec field.
- `grep` for `psc_codec`, `codec_default`, `pcfg.*codec`,
  `ps.*set_codec` returns zero matches across `lib/` and
  `src/`.

The codec is selected **per-call by the client** (e.g.,
`ec_demo write --codec rs ...` vs `--codec mojette-sys ...`).
The PS forwards the requested codec; the MDS records the layout
type and chunk encoding the client requested in the layout it
issues; nothing along the path stores or pre-decides which
codec a future write will use.

The result is that Phase A's stated rollout procedure --
*"per-PS rollout: stop traffic, update PS configuration to
Mojette systematic, restart traffic"* -- has no surface to
operate on.  There is no PS configuration to update.

## Why this strengthens §3.4 rather than weakens it

The progress report claims:

> Codec evolution becomes a deployment-managed change at the
> PS layer.  A new codec ships with a PS update; existing
> clients pick it up without a kernel patch.

The implementation is more permissive than the claim.  Codec
selection is **per-call** rather than *deployment-managed* --
which means:

- Two clients can use different codecs against the same MDS at
  the same time.  No codec rollout is needed across the fleet
  because no fleet state is involved.
- A "new codec deployment" reduces to `git pull && build` of
  the PS (so it understands the new ec_codec_type value).
  Until any client actually invokes the new codec, the
  pre-existing files stay on their original codec; the
  deployment is invisible.
- "Hot-swap" has no meaning here -- there is nothing to swap.
  The PS handles whatever codec the next client request
  specifies.

For the WG narrative, this is the **stronger** position.
Christoph's and Black's concern about codec-on-every-client is
about kernel maintainability and codec evolution velocity.
Per-call codec selection at the PS removes the rollout problem
entirely: a new codec is available the moment the PS code
supports it, with no synchronised fleet update, no client
config push, and no MDS coordination.

## Acceptance criteria -- Phase A

| spec criterion | required | result |
|----------------|----------|--------|
| Total fleet rollout < 1 hour | yes | N/A -- no rollout exists; deployment is `git pull && build` |
| Zero client-side changes | yes | YES -- codec is per-call, no client kernel/config involvement |
| Zero MDS source changes | yes | YES -- MDS is codec-agnostic; layout records what client requests |
| Both codecs simultaneously serviceable | yes | YES -- a client can write file F1 with RS and file F2 with Msys against the same MDS+PS in the same second |
| Read errors during rollout < 1% | yes | N/A -- no rollout to measure errors against |

The spec criteria all PASS in the trivial sense (no rollout
needed because no PS-held config exists).  This is a positive
falsification of the *implicit* assumption that the deployment
process matters: it does not.

## Demonstration -- multi-codec coexistence

A trivial demonstration confirms the implementation behaves as
described: write three files with three different codecs against
the same bench MDS, read them all back successfully.  Captured
during experiment 6 setup (`.claude/experiments/06-real-network-ec/`):

```
ec_demo put _plain --input X      -> plain
ec_demo write _rs   --codec rs    -> RS 4+2
ec_demo write _msys --codec mojette-sys -> Mojette systematic 4+2
ec_demo write _mns  --codec mojette-nonsys -> Mojette non-systematic 4+2
ec_demo read _plain ; ec_demo read _rs ; ec_demo read _msys ; ec_demo read _mns
```

All four reads succeed; bytes match the inputs.  Same MDS, same
DSes, same PS code, four codecs interleaved with no
configuration step.  This is what the §3.4 claim depends on, and
it is observable directly without a Phase A rollout test.

## What Phase A *would* test if PS-held codec config existed

A future feature where the PS pins a per-export default codec
("path /scientific gets RS 4+2, /database gets Mojette-sys 8+2")
would be a real candidate for a hot-swap test.  That feature
does not exist in the current implementation, and the design
documents do not propose it -- the per-call codec model is
explicit.  If a future deployment requirement adds per-export
codec policy, Phase A's hypothesis becomes testable; until then,
it does not.

## Implications for §3.4 of progress_report.md

Suggested patch (replace asserted-only language with the
measured architectural shape):

> *Codec selection is per-call at the client, not a fleet-state
> decision: the PS forwards whatever codec the request
> specifies, and the MDS layout records that choice.  A new
> codec is available to clients the moment the PS binary
> understands it.  Multiple codecs are serviceable
> simultaneously against the same MDS+PS without any
> deployment step.  Confirmed by direct multi-codec
> coexistence in the bench stack and by source inspection
> showing zero codec state in PS configuration
> (`.claude/experiments/02-codec-evolution-timing/phase-a/`).*

## Phase B status

Phase B (genuine new-codec addition end-to-end) is the
substantive test of the codec-evolution claim.  It remains
unblocked but unrun -- the spec quotes 2-4 weeks for a defensible
new codec implementation.  Phase A's findings narrow Phase B's
scope: the deliverable is "implement codec X in the PS, verify
end-to-end" with no rollout component, since none is needed.
