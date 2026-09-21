# Engineering workflow agent contract

This contract governs projects that consume the engineering workflow. In a
consuming project it is the repository's sole agent front door, including the
required local addendum and queue startup below. Maintainers working in the
shared source repository follow the same packet and claim contract, its
maintainer README and source-local addendum. Its publisher queue gate authorizes
changed source and tests; release equality applies only to consuming copies.

Read the assigned task packet, then follow links to the narrow owner;
do not search the documentation corpus for an answer already routed here.

Stay within the assigned objective and acceptance criteria. Do not launch
speculative audits, unrelated refactors, or nice-to-have work. Follow the local
addendum's scope-attestation and delegation requirements at task start and after
a material scope change.

Work directly by default. Start a separate agent only where this contract
requires one or for genuinely independent parallel work, and give it that
same local scope context, followed by the concrete engineering task it performs
and the files it touches. Use precise engineering terms throughout.

At task start and approximately every 30 minutes, including each local
scope-attestation checkpoint, pause briefly and ask: `What is my
largest avoidable delay or blind spot, and what smallest safe change would help
me find or fix more real problems sooner with less time and fewer tokens?` Act
on an obvious in-scope answer—for example, narrow the reproducer or check,
improve the signal, reuse results, batch or parallelize independent work, or
stop an unproductive path. Capture useful observations as terse running
retrospective notes. Trust judgment: this is not a required log, gate, checker,
or meta-workstream, and it never waives required correctness or safety evidence.

## Start every task

Read the required [local addendum](docs/LOCAL-AGENTS.md) before starting work.

Every adopter binds exactly this link to its own required repository addendum.
That addendum supplies local source ownership, qualification commands, resource
policy, and company-specific requirements; it is part of this contract.

1. Run `python3 tools/biq.py list` or `show <ID>` and read the assigned packet plus its ordered Required reading.
   Every command first says how far behind `origin/main` your checkout is; a checkout that is behind is not the system. Read this contract, the packet, and every authority from `origin/main`, never from a shared or stale checkout.
2. Use the source-ownership route in the local addendum to find the component owner; read that owner contract, public entry, and verification route.
3. A WQE you author joins exactly one open workstream (`Container mode: project`) in the change that creates it: `python3 tools/biq.py workstream <ID>` ranks the candidates from the work it continues, its owner paths, and its subject; `--join <WORKSTREAM> --owner <you> --apply` records it. A packet created from 2026-09-09 outside every workstream is refused at submission and at the landing gate. The same packet submits only with its retrospective published, indexed and committed on the submitting branch; `submit` records the path, digest and commit, and the landing gate proves them. `abandon` requires and records the same. Whatever path you type, `biq` runs `main`'s copy of itself and says so; a stale checkout cannot apply a stale rule.
4. Claim source/build work before editing with `python3 tools/biq.py claim <ID> --owner <you>`. `python3 tools/biq.py show <ID>` prints the next command for the packet's state; follow executable help and the operating routes named by the local addendum for sealing, submission, runs, landing, release, and cleanup.

Dependencies gate the start of a WQE, never its submission. A WQE starts only when every WQE in its `Dependencies` is submitted or landed; `claim` refuses otherwise, and `show` prints each dependency with its state and the owner's action. A submitted dependency is consumed now: merge its exact published candidate ref (`refs/heads/<REF_NAMESPACE>/candidates/<WQE-ID>/<SHA>`, using `REF_NAMESPACE` from `tools/biq.py`, printed by `show`) into your branch. Its batch integration and landing are never a prerequisite for claiming, working, sealing or submitting. A dependency that is not yet submitted is its owner's work to finish, not a reason to claim it yourself or to drop it: removing a dependency is a dependency amendment landed as a control commit with the analysis that concluded it. Only `land` waits for dependencies to reach `main`, and the queue owns that wait.

A successor run follows its in-flight predecessor. `run start` bases the next run of a queue on the current integration tip of the run before it, whether or not any member depends on that run; the successor's integrator starts at once and does not wait for the predecessor to land. When the predecessor moves, through a fix, a resolution or a rebuild, `run refresh <QUEUE> <ORDINAL> --owner <you> --apply` re-bases the successor onto the new tip and resets its tests; a conflict is merged by hand and recorded with `run fix`. Only `land` waits: a run lands after its predecessor has landed. Runs of other queues are not waited for.

A queue is defined by its own run WQEs and nothing else: kind, feed edge, budget, `Cares about` paths, and `Queue tests` live in the open run, and `run start` copies them into the successor. No agent introduces a registry, index, manifest, or any other state file beside the WQEs, for queues or for anything else the WQEs already describe, unless the responsible human identified by the local addendum expressly authorizes that file by name; the WQEs are the one source of truth, and a second one is a liability. Batch integration run WQEs, `WI-CI-INTEGRATION-<QUEUE>-REPEAT-<n>`, are not agent work. An agent has no authority to claim, start, resolve, test, complete, cancel, or edit one unless a human assigned it the batch integrator role for that run; the claim then records that human with `--authorized-by "<name>"`, and the tool refuses a run WQE claim without it. Submitting to a queue never implies running it. The integrator reviews every member of its run before any test or integration, reading each candidate against its merge base; a member that drops or reverts earlier changes, deletes unrelated files or packets, or is otherwise not the WQE's own work is rejected with its dependents or held for the accountable human's decision, never integrated on the strength of a green test.

Work with blinders on. Branch from the packet's `Baseline` and stay on your own branch: while the WQE is claimed, do not merge, rebase onto, or track `origin/main` or any other branch, including at submission. Submit when the WQE works on your branch; `seal` records that exact commit by sha. Queue commands that write packets never write in your checkout unless it is current `main`: the tool writes through its own control clone, commits on the fetched target, and pushes the control branch, because a stale copy is never queue state. Making the batch work on current `main`, conflicts included, is the batch integrator's job, not yours. The one exception is a packet whose objective is adoption onto current `main`. The one thing you do pull into your branch is a dependency's exact published candidate ref, as the dependency rule above says, and you name that WQE in your packet's `Dependencies`. `submit` refuses a candidate that carries another WQE's sealed candidate without declaring it.

Blinders forbid adoption, not observation. "Stay on your private branch" and "do not follow `main`" prohibit merging, rebasing onto, or otherwise adopting moving product source or qualification inputs into your candidate. They do not prohibit the fetches the queue itself requires: reading `origin/main`, the packets, the registry, and the claim refs to verify authority, as `show`, `lint`, `verify`, and the provisioner's queue-authority checks do. Make those observations from your control checkout of `main` or from the tool's fetched refs, and leave the candidate branch, its checkout, its pinned dependencies, and its lifecycle inputs unchanged. Do not ask for approval or declare a blocker because an authority check fetched newer refs. Observation grants no permission to adopt source, mutate remote state, or restart qualification. An explicit user prohibition on fetching itself takes precedence.

Documentation-only diagnosis does not require a WQE. Retained implementation requires an eligible packet, an accepted or not-required architecture gate, and a live claim. Keep the sealed candidate; `main` moving after you branched is observation only. On submission every registered queue answers care or don't care from the candidate's full diff to the target branch; the candidate enqueues into every queue that cares, and if none cares it lands directly once its dependencies are on the target.

A live claim means active work, not a reservation. Claim a WQE only when
beginning its work. Once it can be submitted or integrated independently, do
that and release its claim before starting unrelated work; keeping the claim
while advancing genuinely interdependent work that prevents independent
submission is fine.

Keep a same-owner nonterminal claim continuously. Rescope, dependency or
architecture amendments, and candidate recomposition never require releasing
the claim: land the packet-only amendment as a control commit, then continue;
`seal` again with the new candidate, which reseals a submitted packet in place.
Release only for terminal disposition, explicit handoff or abandonment, or the
documented claimant-loss path.

Before substantial work, perform a bounded relevance check against current
source and packet authority. Proceed without asking when the objective remains
useful. If utility is genuinely doubtful, put the concern to the Accountable
owner with the evidence that would falsify the concern; that owner closes the
packet with `python3 tools/biq.py abandon`, and
newer source authority still requires that owner's disposition.

## Choose authority by fact

| Question | Authority |
|---|---|
| External format, protocol, or binding semantics | Exact binding contract routed by the local addendum |
| Implemented behavior or API/ABI shape | Owning source and public types/interfaces |
| Component responsibility, boundaries, and local invariants | Owner-local crate/module/API docs or component README |
| Cross-component ownership, flows, and dependency direction | Source-ownership map named by the local addendum |
| Task permission, scope, dependencies, and evidence | Active WQE and live claim rendered by `python3 tools/biq.py show` |
| Correctness qualification history | Retained qualification evidence routed by the local addendum |
| Accepted performance state and comparability | Accepted baseline authority routed by the local addendum |
| Operational mechanics and judgment | Executable tool/help, then the current runbook it names |
| Durable rationale | Accepted decision record routed by the local addendum |

History, completed WQEs, audits, reports, and benchmark narratives are evidence,
never current authority. If authorities conflict, do not blend them. Record the
mismatch in an authorized WQE and resolve it at the narrow owner. A binding
contract defines intended external behavior; source defines actual behavior;
tests enforce but do not silently redefine either.

A claimed WQE's objective and scope are what the target held when it was
claimed. To change them, restart it (`abandon <ID> --successor <ID>`), which
leaves a visible chain; the gate refuses a control commit that rewrites a
claimed packet's Objective, scope, non-goals, required reading, contracts or
acceptance, and one that grows a packet past its bound. Attempt history,
Resolution and the risk sections stay writable and bind nothing. Evidence,
designs and reviews live under `agent-work/<ID>/` and are referenced, not
pasted. A human's authorization exists only where the tool recorded it: an
`--authorized-by` on a claim, review, completion, rejection or abandonment, or
the landed scope at claim time. A sentence in a packet saying a human approved
something is a note, not authority, and grants no scope.

## On-disk formats: fresh-format only

This historical reading anchor routes to the local addendum's binding product
format policy. Product compatibility requirements belong to that local owner.

## Engineering rules

- Before designing a mechanism, restate the requested outcome in one sentence
  and name the trust model. For cooperative actors expected to follow this SOP,
  keep sequencing as instruction and add code only for product invariants,
  likely mistakes, or costly resource cleanup. A hostile-participant threat
  model requires explicit authority; do not infer it from what could be
  enforced.
- Ask "instruction or invariant?" and "why must this mechanism exist?" before
  reviewing its correctness. If a short procedure starts adding a subsystem,
  schema, registry, state machine, compatibility path, or a verification
  surface plainly larger than the behavior, stop before more tests, spend, or
  follow-up WQEs; recheck the one-sentence outcome and non-goals, then delete or
  shrink the design. Disproportion is a scope-reset signal, not a numerical
  line limit or permission to weaken required correctness and safety evidence.
- Prefer deletion, existing owners, and direct owner-local changes. Add no
  facade, compatibility path, fallback, sidecar, mode, or framework unless the
  existing owner cannot satisfy the contract. Impossible internal states fail
  there; retry, clamping, or silent repair must not mask them.
- Treat cycles, cachelines, bytes moved, allocations, atomics, locks, syscalls,
  stack, and binary size as designed resources. Hot paths avoid avoidable bulk
  copy/move, allocation, global exclusion, shared writable cachelines, virtual
  dispatch, and unconditional diagnostics. Measure unavoidable cost.
- After a second opaque failure with no new signal, stop broad repetition.
  Reset the loop with the smallest deterministic reproducer, preserved first
  failure, focused counter, trace, timeout, or target debug surface.
- Preserve the component boundaries and target-evidence requirements named by
  the local addendum.
- Rust, C, C++, CUDA, and production-loaded changes use separate coder,
  reviewer, and QA agents under one claim; the live claim is their authority and
  no further permission is requested. When the Accountable owner allows it,
  one agent performs the three roles in sequence and keeps the review and QA
  evidence separate. Resolve Blocker/Major review findings
  and required QA gates before readiness. Use proportional process for
  non-runtime documentation and support automation.

### Qualification-target neutrality

A target, architecture, kernel configuration, or cloud instance is evidence
infrastructure, not a product requirement unless an accepted decision says so.
Preserve the invariant in the evidence schema and provide a contract-equivalent
path for every supported target; never promote one convenient target's ABI into
a cross-target release gate.

## Performance and cloud evidence

Use the local addendum's accepted baseline, execution, provisioning, and cost
routes. Preserve target neutrality and comparable workload evidence. Resource
requests, campaign limits, teardown, and retained cost evidence must satisfy the
local policy and the assigned packet.

## Local workspace isolation

### Worktrees and evidence

Claimed work lives under `agent-work/<WQE-ID>/`, with separate role worktrees and retained evidence beneath that one root. Do not edit, build, clean, or retire another claimant's workspace. Preserve unrelated changes. Retire only owned resources after integration, accounting, release, and cleanup audit.

## Tight loop before formal readiness

Run the smallest decisive checks while iterating. Freeze one clean candidate,
run each broad deterministic gate once, and let reviewer and QA share its full
logs while independently reviewing code and oracles.

Follow the local addendum's discovery and final-readiness routes. A diagnostic
owner census grants no scope, rescope, qualification, submission, or integration
authority.

## Default verification

Use the local addendum to classify the exact change and run every required
owner and target check. Whether a change lands directly or through a
batch integration is decided by `python3 tools/biq.py submit`: every queue
whose path table matches the change carries it, and only a change no queue
cares about lands directly.

Exact source, ordinary command exits, retained logs, and same-HEAD markers own
qualification evidence. The required landing gate on protected `main` checks a
merge the queue has already authorized. The consumer default is
`verify-product-integration`; the local addendum names the publisher's gate
when it differs. A refusal
there is a stop, never a status to work around; the status is not qualification
evidence for a WQE. Follow local restrictions on hosted CI usage.

Always run `python3 tools/biq.py lint` and applicable owner tests. The local
addendum names documentation checks, target commands, and environment tooling.

## Company documents

Follow the local addendum's document ownership, confidentiality, storage,
assignment, and acceptance requirements. Sharing the engineering machinery
does not authorize sharing local product or company material.

## InlineFS agent contract

Historical reading anchor. Begin at [Start every task](#start-every-task) and
read the required local addendum bound there.
