# WI-CI-TOOLING-WORKFLOW-PILOT-SMOKE: prove the adopted queue lifecycle with a packet-only change

- ID: `WI-CI-TOOLING-WORKFLOW-PILOT-SMOKE`
- Status: `open`
- Kind: `tooling`
- Level of effort: `low`
- Area: `queue lifecycle in reffs`
- Created: `2026-09-21T21:01:10Z`
- Created human_owner: `David Flynn`
- Created host_user: `davidflynn2`
- Created hostname: `MacBook-Pro-10.local`
- Completed human_owner: `pending`
- Completed host_user: `pending`
- Completed hostname: `pending`
- Claimed At: `pending`
- Closed At: `pending`
- Implementation Duration: `pending`
- Tokens Used: `pending`
- Model Used: `pending`
- Estimated Cost: `pending`
- Estimated AWS Cost: `pending`
- Baseline: `origin/main 7513cd3e6c6a8e94432671cad1d420a60efad1d0`
- Task packet: `3`
- Accountable owner: `Tom Haynes`
- Architecture gate: `not-required`
- Architecture decision: `none`
- Architecture decision owner: `none`
- Documentation impact: `none`
- Documentation impact rationale: `The retrospective records the first lifecycle; no owner document changes.`
- Dependencies: `none`
- Previous attempt: `none`
- Next attempt: `none`
- Candidate base: `pending`
- Candidate commit: `pending`
- Submitted At: `pending`
- Submitted owner: `pending`
- Priority: `production`
- Decision owner: `none`

## Objective

One WQE travels the adopted lifecycle in reffs end to end: registered
through the gated control pull request, claimed, sealed on a candidate that
adds only its retrospective, submitted, and landed directly because no
queue cares about the retrospective tree; the gate's first result and the
first landing are on record.

## Reason

engineering-workflow v1.8.9 was adopted on 2026-09-21 and the REFFS queue
seeded. The adoption guide asks for one packet-only control change to
establish the gate's first result before ordinary work. The work is already
satisfied if a packet has landed through the gate; it is superseded if the
accountable human prefers the first real reffs WQE to serve as the pilot.

## Allowed source/build scope

- `docs/work-queue/items/WI-CI-TOOLING-WORKFLOW-PILOT-SMOKE.md`
- `docs/work-queue/retrospectives/2026-09-21-workflow-pilot-smoke.md`
- `docs/work-queue/retrospectives/README.md`
- `agent-work/WI-CI-TOOLING-WORKFLOW-PILOT-SMOKE/`

## Existing owner and required reuse

`tools/biq.py` owns the lifecycle; `docs/work-queue/TEMPLATE.md` and the
retrospective template own the shapes. Nothing under the reffs sources is
touched.

## Non-goals

No change to reffs sources, build files, workflows, or the queue's
definition. The REFFS run and its reffs-check are exercised by the first
real member, not by this packet.

## Required reading

- governing-authority: `AGENTS.md#start-every-task`
- governing-authority: `docs/LOCAL-AGENTS.md#verification-and-resources`
- verification: `tools/test_biq.py`
- creates-output: `docs/work-queue/retrospectives/2026-09-21-workflow-pilot-smoke.md`

## Affected contracts and invariants

Every control write is a gated pull request the tool opens and follows;
a candidate that no queue cares about lands directly through `land`.

## Risks and constraints

No Docker, cloud, or lab resources. Requires branch protection on main
with the verify-product-integration status so the gate's result is recorded.

## Implementation outline

1. Register this packet by joining the pilot workstream; the gate admits the control pull request.
2. Claim, commit the retrospective on a branch from the Baseline, seal, submit.
3. Land directly; observe the terminal claim release.

## Acceptance and completion evidence

- `python3 tools/biq.py lint` passes at every step.
- The registration and submission pull requests show verify-product-integration passing.
- The packet is completed on main with its retrospective indexed.

## Residual risks and unresolved decisions

None.

## Attempt history

None.

## Resolution

pending
