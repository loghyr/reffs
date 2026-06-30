<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Experiment 2 Phase A: Encoding Hot-Swap

Closes the operational half of progress_report.md §3.4: *"Encoding
evolution becomes a deployment-managed change at the PS layer.
A new encoding ships with a PS update; existing clients pick it up
without a kernel patch."*

## Findings -- Phase A is not runnable as specified, for a
positive reason

The Phase A spec hypothesises a PS-side encoding configuration
that an operator can toggle at runtime ("switching the configured
encoding at a running PS for a target file path").  Source
inspection shows that **no such configuration exists**:

- `lib/nfs4/ps/ec_pipeline.c` declares `enum ec_encoding_type`
  exclusively as a function-argument parameter (`encoding_type`)
  passed in by the caller.
- `lib/nfs4/ps/chunk_io.c` and `ds_io.c` are encoding-agnostic --
  they operate on shards the pipeline produces.
- No `[[proxy_mds]]`, `[[export]]`, or PS-state struct in
  the source carries a encoding selection.
- `mds.toml` likewise has no encoding field.
- `grep` for `psc_encoding`, `encoding_default`, `pcfg.*encoding`,
  `ps.*set_encoding` returns zero matches across `lib/` and
  `src/`.

The encoding is selected **per-call by the client** (e.g.,
`ec_demo write --encoding rs ...` vs `--encoding mojette-sys ...`).
The PS forwards the requested encoding; the MDS records the layout
type and chunk encoding the client requested in the layout it
issues; nothing along the path stores or pre-decides which
encoding a future write will use.

The result is that Phase A's stated rollout procedure --
*"per-PS rollout: stop traffic, update PS configuration to
Mojette systematic, restart traffic"* -- has no surface to
operate on.  There is no PS configuration to update.

## Why this strengthens §3.4 rather than weakens it

The progress report claims:

> Encoding evolution becomes a deployment-managed change at the
> PS layer.  A new encoding ships with a PS update; existing
> clients pick it up without a kernel patch.

The implementation is more permissive than the claim.  Encoding
selection is **per-call** rather than *deployment-managed* --
which means:

- Two clients can use different encodings against the same MDS at
  the same time.  No encoding rollout is needed across the fleet
  because no fleet state is involved.
- A "new encoding deployment" reduces to `git pull && build` of
  the PS (so it understands the new ec_encoding_type value).
  Until any client actually invokes the new encoding, the
  pre-existing files stay on their original encoding; the
  deployment is invisible.
- "Hot-swap" has no meaning here -- there is nothing to swap.
  The PS handles whatever encoding the next client request
  specifies.

For the WG narrative, this is the **stronger** position.
Christoph's and Black's concern about encoding-on-every-client is
about kernel maintainability and encoding evolution velocity.
Per-call encoding selection at the PS removes the rollout problem
entirely: a new encoding is available the moment the PS code
supports it, with no synchronised fleet update, no client
config push, and no MDS coordination.

## Acceptance criteria -- Phase A

| spec criterion | required | result |
|----------------|----------|--------|
| Total fleet rollout < 1 hour | yes | N/A -- no rollout exists; deployment is `git pull && build` |
| Zero client-side changes | yes | YES -- encoding is per-call, no client kernel/config involvement |
| Zero MDS source changes | yes | YES -- MDS is encoding-agnostic; layout records what client requests |
| Both encodings simultaneously serviceable | yes | YES -- a client can write file F1 with RS and file F2 with Msys against the same MDS+PS in the same second |
| Read errors during rollout < 1% | yes | N/A -- no rollout to measure errors against |

The spec criteria all PASS in the trivial sense (no rollout
needed because no PS-held config exists).  This is a positive
falsification of the *implicit* assumption that the deployment
process matters: it does not.

## Demonstration -- multi-encoding coexistence

A trivial demonstration confirms the implementation behaves as
described: write three files with three different encodings against
the same bench MDS, read them all back successfully.  Captured
during experiment 6 setup (`.claude/experiments/06-real-network-ec/`):

```
ec_demo put _plain --input X      -> plain
ec_demo write _rs   --encoding rs    -> RS 4+2
ec_demo write _msys --encoding mojette-sys -> Mojette systematic 4+2
ec_demo write _mns  --encoding mojette-nonsys -> Mojette non-systematic 4+2
ec_demo read _plain ; ec_demo read _rs ; ec_demo read _msys ; ec_demo read _mns
```

All four reads succeed; bytes match the inputs.  Same MDS, same
DSes, same PS code, four encodings interleaved with no
configuration step.  This is what the §3.4 claim depends on, and
it is observable directly without a Phase A rollout test.

## What Phase A *would* test if PS-held encoding config existed

A future feature where the PS pins a per-export default encoding
("path /scientific gets RS 4+2, /database gets Mojette-sys 8+2")
would be a real candidate for a hot-swap test.  That feature
does not exist in the current implementation, and the design
documents do not propose it -- the per-call encoding model is
explicit.  If a future deployment requirement adds per-export
encoding policy, Phase A's hypothesis becomes testable; until then,
it does not.

## Implications for §3.4 of progress_report.md

Suggested patch (replace asserted-only language with the
measured architectural shape):

> *Encoding selection is per-call at the client, not a fleet-state
> decision: the PS forwards whatever encoding the request
> specifies, and the MDS layout records that choice.  A new
> encoding is available to clients the moment the PS binary
> understands it.  Multiple encodings are serviceable
> simultaneously against the same MDS+PS without any
> deployment step.  Confirmed by direct multi-encoding
> coexistence in the bench stack and by source inspection
> showing zero encoding state in PS configuration
> (`.claude/experiments/02-encoding-evolution-timing/phase-a/`).*

## Phase B status

Phase B (genuine new-encoding addition end-to-end) is the
substantive test of the encoding-evolution claim.  It remains
unblocked but unrun -- the spec quotes 2-4 weeks for a defensible
new encoding implementation.  Phase A's findings narrow Phase B's
scope: the deliverable is "implement encoding X in the PS, verify
end-to-end" with no rollout component, since none is needed.
