# WI-CI-INTEGRATION-REFFS-REPEAT-1: REFFS batch integration run 1

- ID: `WI-CI-INTEGRATION-REFFS-REPEAT-1`
- Status: `open`
- Kind: `integration`
- Level of effort: `medium`
- Area: `work-queue batch integration: REFFS run`
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
- Accountable owner: `David Flynn`
- Architecture gate: `not-required`
- Architecture decision: `none`
- Architecture decision owner: `none`
- Documentation impact: `none`
- Documentation impact rationale: `Generated batch integration packet; tools/biq.py help owns the run commands.`
- Queue: `REFFS`
- Queue kind: `leaf`
- Feeds: `none`
- Feeders: `none`
- Qualification: `none`
- Started At: `pending`
- Base: `pending`
- Predecessor: `none`
- Integration commit: `pending`
- Result commit: `pending`
- Tests: `pending`
- Cost: `$0`
- Budget: `$0`
- Dependencies: `none`
- Previous attempt: `none`
- Next attempt: `none`
- Candidate base: `pending`
- Candidate commit: `pending`
- Submitted At: `pending`
- Submitted owner: `pending`
- Decision owner: `none`

## Objective

Review every member below, then integrate the reviewed members as one batch on `main`, run reffs-check within the budget, and on green land the result on `main`. AWS campaign spend ceiling: `$0`.

The review is the integrator's own reading of each member's candidate against its merge base (`run review` prints the footprint, every deleted path, and anything the seal guard would refuse): the change must be the WQE's own work and nothing else. A member that drops or reverts earlier changes, deletes unrelated files or packets, touches other WQEs' agent-work, or is otherwise not sane is not integrated. When the integrator is not sure, `--hold <question>` returns the member to the open run and names the accountable human as its Decision owner; when it is wrong, `--reject <reason>` closes it failed-rejected and sheds its dependents from the run. Nothing destructive goes through on the strength of a green test.

## Reason

Every candidate REFFS cares about waits here until its dependencies are met; this is the next batch integration of the queue.

## Allowed source/build scope

- the integration branch of this run;
- this WQE packet and its retained evidence.

## Existing owner and required reuse

`tools/biq.py` owns runs; this run WQE's `Cares about`, `Queue tests`, and `Budget` define the queue, and `run start` copies them into the successor.

## Non-goals

- No change to any member candidate.
- No integration, test, or landing of a member the integrator has not reviewed.

## Required reading

- governing-authority: `AGENTS.md#start-every-task`
- verification: `tools/test_biq.py`

## Affected contracts and invariants

A member whose merge collides stays in the current run awaiting resolution, including after a rebuild. No member is tested or integrated unreviewed; a rejected member's dependents leave the run with it; a held member waits in the open run for the human named as its Decision owner.

## Risks and constraints

Queue budget `$0` per run.

## Implementation outline

1. `python3 tools/biq.py run start REFFS --owner <you> --apply`
2. `run review REFFS 1`, read each member's candidate, then per member `run review REFFS 1 <ID> --ok | --reject <reason> | --hold <question> --apply`
3. Fix any integration or landing-gate defect on a descendant commit with `run fix REFFS 1 --commit <sha> --owner <you> --apply`; this resets every test.
4. `run test REFFS 1 <test> --result green|red --evidence <path> --cost <usd>` per test
5. `run complete REFFS 1 [--reject <ID>] --apply`

## Acceptance and completion evidence

- Every member reviewed ok by the integrator, or rejected or held with its reason in Excluded.
- Every test recorded green and the result passed on or landed.

## Residual risks and unresolved decisions

None.

## Attempt history

None.

## Cares about

- file: Dockerfile
- file: Dockerfile.ci
- file: Makefile.am
- file: Makefile.ci
- file: Makefile.reffs
- file: check_license.sh
- file: configure.ac
- file: reffs.pc.in
- file: reffs.spec.in
- file: reffsd.service.in
- file: tree-build.sh
- prefix: src/
- prefix: lib/
- prefix: tools/
- prefix: support/
- prefix: third_party/
- prefix: scripts/
- prefix: examples/
- prefix: hooks/
- prefix: deploy/
- prefix: .github/

## Queue tests

- reffs-check: make -f Makefile.ci check (local, free)

## Members

none

## Excluded

none

## Resolution

pending
