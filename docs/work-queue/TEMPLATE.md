# WI-CI-CATEGORY-SUBJECT: concise outcome

- ID: `WI-CI-CATEGORY-SUBJECT`
- Status: `open`
- Kind: `architecture|audit|bug|build|documentation|feature|infrastructure|integration|performance|qualification|refactor|test|tooling`
- Level of effort: `unestimated`
- Area: `owning source or build area`
- Created: `YYYY-MM-DDTHH:MM:SSZ`
- Created human_owner: `Git user.name of creator`
- Created host_user: `OS account of creator`
- Created hostname: `host of creator`
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
- Baseline: `origin/main <commit>`
- Task packet: `3`
- Accountable owner: `concrete human name`
- Architecture gate: `not-required`
- Architecture decision: `none`
- Architecture decision owner: `none`
- Documentation impact: `none`
- Documentation impact rationale: `why no current owner changes, or how the named owners change`
- Dependencies: `none`
- Previous attempt: `none`
- Next attempt: `none`
- Candidate base: `pending`
- Candidate commit: `pending`
- Submitted At: `pending`
- Submitted owner: `pending`
- Priority: `production`
- Container mode: `operative|project`
- Container members: `WI-CI-CATEGORY-MEMBER`
- Decision owner: `none`

`Priority` is optional and defaults to `production`; use `on-hold` to park
development. Workstream holds also apply to members. Read the current packet
with `python3 tools/biq.py show <ID>` before resuming or claiming held work.

Before committing, replace every placeholder and choose exactly one value for
enumerated fields. For `pending` or `accepted`, set `Architecture decision` to
one anchored `docs/specs/...md#anchor` brief, add the same `task-design` under
Required reading, and follow the owner/status rules below and executable
`python3 tools/biq.py --help`.
Do not author the retired `blocked` status. Park discretionary work with
`Priority: on-hold`. For a submitted BIQ member needing a human decision, the
integrator uses `run review --hold <question>`; the item remains submitted and
names the run's accountable human as `Decision owner`. If this WQE owns the
missing design, make it a dependency-free `Kind: architecture` question, keep
`Status: open`, set `Architecture gate: pending`, name the concrete
`Architecture decision owner`, and leave `Decision owner: none`.
Scope overflow, another claimant, mutable same-path work, and a possible future
integration conflict are not blockers; use coordinated owner rescope or the
smallest separately claimed owner WQE instead.
Put implementation prerequisites only on the real-work WQEs. Author at least
one concrete non-architecture implementation, qualification, performance,
migration, or delivery WQE with every architecture question. Make the question
their `operative` container, make every child explicitly depend on it, and set
each child's architecture gate to `not-required`. An architecture WQE may not be
a child or remain empty. After human acceptance, the same architecture-question
claim seals and completes normally; do not release and reclaim a design phase.

For a non-container, delete both `Container mode` and `Container members`,
then list the new WQE in exactly one open workstream (`Container mode:
project`) in the same change: `python3 tools/biq.py workstream <ID>` ranks
the candidates and `--join <WORKSTREAM> --owner <you> --apply` records it. A
packet created from 2026-09-09 outside every workstream cannot be submitted
or landed.
For a newly authored container, keep both lines in this position, choose
`operative` or `project`, and list one or more distinct WQE IDs once each in
lexicographic order, separated by comma and one space. A WQE has at most one
active operative container and one project (workstream) parent. Operative
membership groups report rows only; every member keeps its own state, claim,
dependencies, scope, evidence, and accounting. A project is a dependency-free
parent that orchestrates normally claimed members and copies no member state
or cost; it is terminal when every member family is terminal. Listing one
ordinal of a repeat family, or one run of a batch integration queue, groups all
of its ordinals.

The queue writes `Queues` and `Implied dependencies` into the packet at
submission and `Replaces` names the rejected WQE a replacement supersedes; do
not author those by hand. Name in `Dependencies` every WQE whose work this
one needs, including every WQE whose published candidate you merge into your
branch. A WQE starts only after every dependency is submitted: `claim` refuses
before that, and `show` prints each dependency's candidate ref to merge; a
dependency's integration and landing are never a prerequisite, and removing one
is a deliberate dependency amendment. For discretionary work staged for later
human disposition, use `WI-<CI|PRODUCT>-PROPOSED-<CATEGORY>-<SUBJECT>`, set
`Decision owner` to the Accountable owner, and append exactly
`Work-queue proposal v1: {"producer":"<NON-PROPOSED-WQE-ID>","schema":1}` to
Resolution; nobody claims it until that owner accepts it by a control commit
that sets `Decision owner` to `none`. Run WQEs, `WI-CI-INTEGRATION-<QUEUE>-REPEAT-<n>`,
are generated by `python3 tools/biq.py`; do not copy this template into one.

Author the Objective and Reason so a later executor can perform one bounded
post-claim relevance check: state the observable product outcome, the current
authority or premise that makes it useful, and the concrete conditions that
would make the work already satisfied, duplicate, superseded, contraindicated,
or premised on invalid facts. A clean result proceeds without asking. A
material, evidence-backed concern is put to the Accountable owner; that owner
closes the packet with `python3 tools/biq.py abandon`.

New WQEs use the sortable identity
`WI-<CI|PRODUCT>-<CATEGORY>-<SUBJECT>[-REPEAT-N|-ATTEMPT-N]`.
Choose the realm first, then one primary category from `ARCHITECTURE`, `AUDIT`,
`BUG`, `BUILD`, `DOCUMENTATION`, `FEATURE`, `INFRASTRUCTURE`, `INTEGRATION`,
`PERFORMANCE`, `QUALIFICATION`, `REFACTOR`, `TEST`, or `TOOLING`, then the
owner/subject; do not restate the realm or chosen category in the subject.

Read the required local addendum bound by
[the shared contract](../../AGENTS.md#start-every-task) for source ownership,
verification, resource campaigns, and local packet-authoring requirements.
Declare applicable campaign limits and cleanup/cost evidence in this packet.

## Objective

State one observable source/build outcome and the product result it is intended
to create or preserve. Open with the one-sentence outcome that mechanism design
must not silently strengthen.

## Reason

Name the request, defect, requirement, or parent WQE that defines this work.

## Allowed source/build scope

List the exact source, build, test, and documentation paths this WQE may
change. Everything else is out of scope.

## Existing owner and required reuse

Name the component owner routed by the required local addendum whose contract
and entry points this work reuses, and what must not be duplicated.

## Non-goals

State what this WQE deliberately does not do, including any deferred work and
its WQE.

## Required reading

- task-design: `docs/specs/...md#anchor` (only with a pending or accepted gate)
- governing-authority: `AGENTS.md#anchor` or the owner README anchor
- verification: the exact owner check routed by the required local addendum
- historical-evidence: an existing report or packet that explains prior work
- creates-output: a repository path this WQE will create, not an input to read

Every nonempty line has the shape `- <role>:` followed by a backtick-quoted
`path[#heading]`. The accepted roles are
`governing-authority`, `task-design`, `verification`, `historical-evidence`,
and `creates-output`. Every role except `creates-output` names an existing
input. `governing-authority` and Markdown `task-design` references include a
heading. Keep at least one `governing-authority` and one `verification` line;
delete unused example lines rather than leaving placeholders.

## Affected contracts and invariants

Name the external formats, protocols, APIs, and invariants the change touches
and how each is preserved.

## Risks and constraints

Name the resources, targets, budgets, and safety limits that bound the work.

## Implementation outline

Number the steps; keep each verifiable.

## Acceptance and completion evidence

List the commands, checks, and retained evidence paths that prove completion.

## Residual risks and unresolved decisions

State what remains open after acceptance, or `None.`

## Attempt history

Record earlier attempts and what each established, or `None.`

## Resolution

Leave `pending` while open. The queue appends the landing or rejection note;
`abandon` records the reason. The sealed candidate commit and base are written
by `seal`; the caring queues by `submit`.
