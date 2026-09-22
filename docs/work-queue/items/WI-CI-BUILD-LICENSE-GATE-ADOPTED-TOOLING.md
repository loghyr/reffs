# WI-CI-BUILD-LICENSE-GATE-ADOPTED-TOOLING: main passes make license again and standards.md names the landing route main enforces

- ID: `WI-CI-BUILD-LICENSE-GATE-ADOPTED-TOOLING`
- Status: `completed`
- Kind: `build`
- Level of effort: `low`
- Area: `repository gates: check_license.sh and the landing workflow in .claude/standards.md`
- Created: `2026-09-22T01:19:25Z`
- Created human_owner: `Tom Haynes`
- Created host_user: `loghyr`
- Created hostname: `mana`
- Completed human_owner: `Tom Haynes`
- Completed host_user: `loghyr`
- Completed hostname: `mana`
- Claimed At: `2026-09-22T01:22:01Z`
- Closed At: `2026-09-22T02:51:39Z`
- Implementation Duration: `PT216S`
- Tokens Used: `pending`
- Model Used: `pending`
- Estimated Cost: `pending`
- Estimated AWS Cost: `pending`
- Baseline: `origin/main c43abc6898a9cdfd31d3c4d77e2608398b55563a`
- Task packet: `3`
- Accountable owner: `Tom Haynes`
- Architecture gate: `not-required`
- Architecture decision: `none`
- Architecture decision owner: `none`
- Documentation impact: `.claude/standards.md`
- Documentation impact rationale: `The Branch and Commit Methodology section describes fast-forward pushes to main, which branch protection now rejects; it is rewritten to name the queue route this repository actually enforces.`
- Queues: `REFFS`
- Implied dependencies: `none`
- Dependencies: `none`
- Previous attempt: `none`
- Next attempt: `none`
- Candidate base: `c43abc6898a9cdfd31d3c4d77e2608398b55563a`
- Candidate commit: `1be3f4f10110c8ee2d9c43d1500314b301bfef46`
- Submitted At: `2026-09-22T01:25:37Z`
- Submitted owner: `claude-reffs-loghyr`
- Priority: `production`
- Decision owner: `none`

## Objective

`make -f Makefile.reffs license` passes on `main`, so the `.git-hooks/pre-push`
hook and the local pre-commit gates work again for every contributor, and
`.claude/standards.md` describes the landing route that `main` actually
enforces. The product result is a repository whose own gates are green and
whose documented workflow is the one its branch protection admits.

This is the first REFFS batch integration run: the candidate touches
`check_license.sh`, which the queue cares about, so the run and its
`reffs-check` are exercised for the first time (the follow-up named by the
pilot smoke retrospective).

## Reason

The engineering-workflow v1.8.9 adoption (`75c2523606da`) installed six files
that the integration gate (`tools/biq.py adoption`) requires byte-equal to the
upstream release tag, and a `docs/work-queue/` tree the tool writes itself.
None of them can carry an SPDX header, and the upstream repository ships no
licence file to cite. Since that commit `check_license.sh` fails on `main`
(reffs issue #75), and the pre-push hook rejects every push from a checkout
that has it installed.

Separately, `.claude/standards.md` still says `main` receives fast-forward
pushes, which the branch protection set on 2026-09-21 rejects for everyone
including the admin.

The work is already satisfied if `make -f Makefile.reffs license` passes on
`origin/main` and standards.md no longer describes fast-forward landing. It is
superseded if the accountable owner reverts the branch protection and the
adoption, in which case only the license half remains useful. It is premised
on invalid facts if the upstream release gains a licence header the adopted
files can carry, in which case the exclusion should be narrowed.

## Allowed source/build scope

- `check_license.sh`
- `docs/LOCAL-AGENTS.md`
- `.claude/standards.md`
- `docs/work-queue/items/WI-CI-BUILD-LICENSE-GATE-ADOPTED-TOOLING.md`
- `docs/work-queue/retrospectives/2026-09-22-license-gate-adopted-tooling.md`
- `docs/work-queue/retrospectives/README.md`
- `agent-work/WI-CI-BUILD-LICENSE-GATE-ADOPTED-TOOLING/`

## Existing owner and required reuse

`check_license.sh` owns the SPDX and licence-compatibility check; its
existing exclusion list and the vendored-subtree provenance comment for
`lib/ec/snapraid-raid/` are the pattern reused. No second checker, no
allowlist file beside the script. `.claude/standards.md` owns the branch and
commit methodology; `AGENTS.md` and `docs/LOCAL-AGENTS.md` own the queue and
are referenced, not duplicated.

## Non-goals

- No change to the six adopted files, the `docs/work-queue/` tree the tool
  writes, or the adoption gate.
- No change to the branch protection or repository merge settings; their
  revert plan is a separate document outside this repository.
- No change to `.claude/agents/review.md`; the reviewer step written the
  same day lands under its own documentation WQE, which no queue cares about.
- No SPDX header on the adopted files; the upstream licence question is
  recorded in `check_license.sh` and in issue #75, not resolved here.

## Required reading

- governing-authority: `AGENTS.md#start-every-task`
- governing-authority: `docs/LOCAL-AGENTS.md#verification-and-resources`
- verification: `check_license.sh`
- verification: `Makefile.ci`
- historical-evidence: `docs/work-queue/retrospectives/2026-09-21-workflow-pilot-smoke.md`
- creates-output: `docs/work-queue/retrospectives/2026-09-22-license-gate-adopted-tooling.md`

## Affected contracts and invariants

- Every tracked text file outside the documented exclusions carries an SPDX
  header; the exclusion list grows by the adopted set and the tool-written
  tree and by nothing else.
- Licence compatibility (`INCOMPATIBLE_LICENSES`) is unchanged; excluded
  files are not scanned for either check, which is the existing behaviour
  for the vendored subtrees.
- The adoption gate's byte-equality invariant on the six installed files is
  preserved because none of them is touched.

## Risks and constraints

No cloud or lab resources. `reffs-check` (`make -f Makefile.ci check`) runs
in Docker on the integrator's machine; it is the first time the REFFS run
executes it, so a failure may be the run's plumbing rather than the
candidate. The candidate changes no C source, so the check is expected to
match `main`'s result.

## Implementation outline

1. Register this packet in the pilot workstream; the gate admits the control
   pull request.
2. Claim; on a branch from the Baseline, exclude the adopted set in
   `check_license.sh` with a provenance comment, add the header to
   `docs/LOCAL-AGENTS.md`, rewrite the landing steps in
   `.claude/standards.md` to name the queue route.
3. Run `make -f Makefile.reffs license` and `python3 tools/biq.py lint`;
   retain the output under `agent-work/`.
4. Commit the retrospective and its index line; seal; submit. REFFS cares
   about `check_license.sh`, so the candidate enqueues into
   `WI-CI-INTEGRATION-REFFS-REPEAT-1`.
5. The integrator the accountable owner assigns starts the run, reviews the
   member, runs `reffs-check`, completes the run, and lands it.

## Acceptance and completion evidence

- `make -f Makefile.reffs license` exits 0 on the candidate; output retained
  at `agent-work/WI-CI-BUILD-LICENSE-GATE-ADOPTED-TOOLING/evidence/license.log`.
- `python3 tools/biq.py lint` reports 0 problems; output retained beside it.
- `reffs-check` green on the run's integration commit; log retained under
  the run's evidence.
- The candidate lands on `main` through the REFFS run and issue #75 closes.

## Residual risks and unresolved decisions

The adopted files remain without a licence identifier in this repository;
whether the upstream release should carry one is the upstream project's
decision and is tracked in issue #75.

## Attempt history

None.

## Resolution

Landed on `main` by WI-CI-INTEGRATION-REFFS-REPEAT-1.

Work-queue substantive work provenance v1: {"claim_head":"c43abc6898a9cdfd31d3c4d77e2608398b55563a","human_owner":"Tom Haynes","item":"WI-CI-BUILD-LICENSE-GATE-ADOPTED-TOOLING","owner":"claude-reffs-loghyr","recorded_at":"2026-09-22T01:25:37Z","role":"implementation","schema":1}
Retrospective: `docs/work-queue/retrospectives/2026-09-22-license-gate-adopted-tooling.md` (sha256 `97a88479e98b4f580c183b1c2cc3bf9c757a546f0dce10c026d29dd9e86b4d48`) landed on protected origin/main at `fd847053245e158740e52ea13ae6aff01fdc0ccc`.
