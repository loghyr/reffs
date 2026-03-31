<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Role Definitions

Three roles operate on this codebase. Claude may fill any or all
of them. The user is the final authority on all decisions.

## Planner / Designer

The planner creates the design and implementation plan before
any code is written.

### Responsibilities

1. **Tests first**: every plan must identify unit tests, functional
   tests, and CI tests BEFORE describing the implementation.
   - Unit tests: what behavior is being added or changed?
   - Functional tests: what end-to-end scenarios validate it?
   - CI tests: what integration coverage is needed?

2. **Test impact analysis**: identify which existing tests will be
   affected by the design. For each:
   - Will the test still compile?
   - Will the test still pass?
   - If not, why? Is the design correct or should it change?

3. **Test infrastructure**: consider whether the design requires
   changes to the test infrastructure (harnesses, fixtures, mocks).
   - Does the feature need a new test fixture (like mini_kdc)?
   - Does it need a new test mode (like bare vs. namespace-init)?
   - Document these as explicit plan items.

4. **RFC compliance**: cite the specific RFC sections that govern
   the design. The reviewer will check these.

5. **State machines**: any new lifecycle or state must have an
   explicit state diagram with:
   - All states
   - All transitions with preconditions
   - Error conditions for invalid transitions
   - What each state means for clients

6. **Persistence**: any new on-disk format must specify:
   - Version field for forward compatibility
   - Crash recovery behavior (what if power fails mid-write?)
   - Migration path from previous format (or "no deployed systems")

7. **Security model**: any new feature that touches authentication,
   authorization, or wire format must specify:
   - What happens when the security backend is unavailable?
   - NFS4ERR_DELAY for broken backends, not WRONGSEC (see standards)
   - What the client sees in each failure mode

8. **Deferred items**: explicitly list what is NOT being done and why.
   Use NOT_NOW_BROWN_COW markers in code for deferred work.

9. **Admin interface for non-POSIX operations**: when designing a
   feature, ask: "How does the admin perform this action?"  Normal
   filesystem operations (touch, mkdir, cat, etc.) cannot create
   superblocks, set security flavors, or perform other server-
   internal mutations.  Every feature that requires admin action
   must have a corresponding probe protocol op (C + Python,
   shipped simultaneously).  If the plan introduces server state
   that an admin needs to create or modify at runtime, the probe
   ops are part of the deliverable — not a future enhancement.

10. **Record plans in the project**: always write the implementation
    plan to a file in `.claude/design/` before starting work.
    Plans in the ephemeral plan mode file are lost on disconnect
    or SSH timeout.  A plan recorded in the project survives
    session loss and gives the next session (or another developer)
    full context to continue.  Reference the design file from
    `CLAUDE.md` if it represents a major feature.

## Programmer

The programmer implements the plan. May be the same entity as the
planner, but the role has distinct rules.

### Responsibilities

1. **Existing tests are sacred**: new code must not break existing
   passing tests. If a test breaks:
   - STOP. Do not modify the test.
   - Ask: is the design wrong, or is the test wrong?
   - If the test is wrong (testing incorrect behavior), explain
     the rationale and get explicit approval to change it.
   - If the design is wrong, fix the design.

2. **Understand before modifying**: before changing any existing
   code, read and understand its intent. Check:
   - Who calls this function? What are the callers' assumptions?
   - Are there tests for this code? What do they validate?
   - Are there comments explaining WHY, not just WHAT?

3. **TDD discipline**: write failing tests first, then implement.
   - The test defines the contract.
   - If the test is hard to write, the interface may be wrong.
   - If the test seems trivial, it still documents the behavior.

4. **Branch workflow**: never commit directly to `main` for feature
   work.  Work on a topic branch, commit freely, then clean up with
   `git rebase -i main` before merging.  See `.claude/standards.md`
   "Branch and Commit Methodology" for the full workflow.

5. **One concern per commit** (on `main`): each commit should do one
   thing.  Don't mix refactoring with new features.  On the topic
   branch, "WIP" and "debug" commits are fine — they get squashed.

6. **Style before commit**: always run `make -f Makefile.reffs fix-style`
   before committing. Style violations should never reach review.

7. **Build verification**: verify the build is clean before committing.
   - `make -j$(nproc)` — zero errors, zero warnings
   - `make check` — zero test failures
   - For RPC/wire changes: `ci-check` or `ci-test`

8. **Comment intent, not mechanism**: when adding code, explain
   WHY the code exists, not WHAT it does. The code shows WHAT;
   the comment explains WHY.
   - Bad: `/* increment the counter */`
   - Good: `/* Track active sessions for the reaper thread */`

9. **Logging discipline**:
   - LOG: fatal or actionable errors (operator must act)
   - TRACE: diagnostic events (developer debugging)
   - Use typed trace functions (trace_security_*, trace_fs_*, etc.)
   - Never commit temporary LOG→TRACE toggles; use the category system

10. **Error handling**: every function that can fail must have its
    error paths tested. Don't add error handling for scenarios that
    can't happen (see coding standards), but DO handle:
    - malloc failures
    - Invalid state transitions
    - Wire data from untrusted clients

11. **RCU and refcounting**: follow Rule 6 in patterns/ref-counting.md.
    Every hash-table entry lifecycle must follow the documented pattern.
    The reviewer will check this.

12. **UUID discipline for long-lived objects**: every object that is
    persisted to disk or referenced by external entities (NFS clients,
    admin tools) MUST have a stable UUID that is:
    - Assigned once at creation time
    - Persisted in the on-disk format
    - Restored on load (never regenerated)
    - Included in admin/probe responses for identification
    If `uuid_generate()` is called in an alloc function but the UUID
    is not persisted, it will change on restart — this is a bug.
    Objects that currently require stable UUIDs: server_state,
    super_block, and any future dstore or client identity.

## Reviewer

The reviewer validates the work of the programmer against the plan,
the standards, and the existing codebase.

### Responsibilities

1. **Test review** (highest priority):
   - Are there tests for the new code?
   - Do existing tests still pass?
   - If any test was modified: WHY? What design decision caused it?
   - If the reason is "the new code broke it": reject. Fix the code.
   - Are the tests testing the right things? (Intent, not just coverage)

2. **Design compliance**: does the code match the approved plan?
   - Are all planned items implemented?
   - Are there unplanned changes? If so, why?
   - Are deferred items properly marked with NOT_NOW_BROWN_COW?

3. **Standards compliance**: check against .claude/standards.md:
   - Style (tabs, 80-col, clang-format)
   - LOG vs TRACE usage
   - Error code conventions (negative errno for v3, nfsstat4 for v4)
   - RCU rules (patterns/rcu-violations.md)
   - Ref-counting rules (patterns/ref-counting.md, Rule 6)
   - Atomic operations (C11 stdatomic, not GCC builtins)
   - Clock usage (MONOTONIC for timers, REALTIME for wall clock)

4. **Security review**:
   - Buffer overflow potential (untrusted wire data)
   - Integer overflow in size calculations
   - Alignment-safe access (memcpy, not pointer casts, for wire data)
   - Credential validation at system boundaries
   - Graceful degradation (NFS4ERR_DELAY, not WRONGSEC)

5. **RFC compliance**: verify cited RFC sections match the implementation.
   Cross-check error codes, state transitions, and wire formats.

6. **Concurrency review**:
   - RCU read-side critical sections don't block
   - Hash table entries follow Rule 6 lifecycle
   - Reaper/timer threads are RCU-registered
   - Async paths don't touch freed memory (the EINPROGRESS pattern)

7. **Classify findings**: use BLOCKER / WARNING / NOTE.
   - BLOCKER: must fix before commit
   - WARNING: should fix, not blocking
   - NOTE: suggestion or observation

8. **On-disk format versioning**: if a change modifies an on-disk
   format (registry, chunk store, server state, client state),
   check whether there are existing deployments with data in the
   old format.  If YES: the change MUST include a version bump AND
   upgrade/migration code.  If NO (pre-deployment): the format
   version stays at 1 and no migration code is needed — just change
   the struct.  Flag as BLOCKER any version bump without migration
   code when deployments exist, or unnecessary migration code when
   no deployments exist.  The project will document in CLAUDE.md
   when persistent storage is first deployed.

9. **UUID stability review**: flag as BLOCKER any long-lived object
   that has a dynamically assigned UUID (`uuid_generate()` in alloc)
   without corresponding persistence (save to disk) and restoration
   (load from disk without regenerating).  A UUID that changes on
   restart is not a UUID — it's a random number.  Check:
   - Is the UUID in the on-disk format?
   - Is it restored on load, not regenerated?
   - Is it exposed in probe/admin responses?
   - Does any external entity (NFS client, admin tool) depend on it?

## Role Interactions

- The **planner** produces a plan with tests identified.
- The **reviewer** reviews the plan for completeness before coding starts.
- The **programmer** writes failing tests, then implements.
- The **reviewer** reviews the code against the plan and standards.
- The **user** approves at each stage.

When roles conflict (e.g., the programmer wants to modify a test
that the reviewer says is sacred), the user decides.
