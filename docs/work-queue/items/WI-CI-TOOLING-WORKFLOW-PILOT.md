# WI-CI-TOOLING-WORKFLOW-PILOT: orchestrate the shared-workflow pilot for reffs

- ID: `WI-CI-TOOLING-WORKFLOW-PILOT`
- Status: `open`
- Kind: `tooling`
- Level of effort: `low`
- Area: `reffs sources and queue orchestration`
- Created: `2026-09-21T21:00:07Z`
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
- Baseline: `origin/main 75c2523606dabaded95a473b76c3cda0b6e84e07`
- Task packet: `3`
- Accountable owner: `Tom Haynes`
- Architecture gate: `not-required`
- Architecture decision: `none`
- Architecture decision owner: `none`
- Documentation impact: `none`
- Documentation impact rationale: `Local orchestration only.`
- Dependencies: `none`
- Previous attempt: `none`
- Next attempt: `none`
- Candidate base: `pending`
- Candidate commit: `pending`
- Submitted At: `pending`
- Submitted owner: `pending`
- Priority: `production`
- Container mode: `project`
- Container members: `WI-CI-INTEGRATION-REFFS-REPEAT-1`
- Decision owner: `none`

## Objective

Orchestrate the REFFS queue pilot through separately claimed member WQEs, so reffs changes travel through batch integration with the declared reffs-check.

## Reason

Authorized local adoption of engineering-workflow v1.8.9; reconsider if the pilot is already satisfied or superseded.

## Allowed source/build scope

- `docs/work-queue/items/WI-CI-TOOLING-WORKFLOW-PILOT.md` and its membership.

## Existing owner and required reuse

`tools/biq.py` owns the queue and generated run packets; `Makefile.ci` owns reffs-check.

## Non-goals

No product implementation, resource allocation, or authority to claim member runs.

## Required reading

- governing-authority: `AGENTS.md#start-every-task`
- verification: `tools/test_biq.py`

## Affected contracts and invariants

Members keep their own claims, scope, dependencies, and evidence.

## Risks and constraints

reffs-check runs in Docker on the integrator's machine; no cloud or lab allocation.

## Implementation outline

1. Maintain membership and observe evidence.
2. Close only after every member family is terminal.

## Acceptance and completion evidence

`python3 tools/biq.py lint` passes; completed members retain their qualification and retrospective evidence.

## Residual risks and unresolved decisions

None.

## Attempt history

None.

## Resolution

pending
