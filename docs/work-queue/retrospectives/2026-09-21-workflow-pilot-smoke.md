# Workflow pilot smoke retrospective: 2026-09-21

> **Historical evidence and process rationale, not operating procedure.**

## Workstream and timeline

- WQE: `WI-CI-TOOLING-WORKFLOW-PILOT-SMOKE`
- Owner/workstream: `claude-reffs-pilot`; `WI-CI-TOOLING-WORKFLOW-PILOT`
- Dates: 2026-09-21
- Candidate/base/claim identities: baseline `7513cd3e6c6a8e94432671cad1d420a60efad1d0`; candidate recorded by seal.
- Final disposition: landed directly; no queue cares about the retrospective tree.

## What was worked on

The first WQE through the adopted engineering-workflow v1.8.9 lifecycle in
reffs: registered by a gated control pull request, claimed, sealed on a
candidate that adds only this retrospective and its index line, submitted,
and landed directly. The gate's first results and the first landing are on
record.

## What worked

- The six-file installation, the reffs addendum, and the REFFS queue seed
  landed as the bootstrap; `adoption` reported 0 mismatches and the
  installed suite passed with only the two publisher-only assertions skipped.

## What failed or cost more than expected

- The first bootstrap push over HTTPS was refused because the CLI token lacks
  the workflow scope for adding a workflow file; SSH on port 443 worked.

## Process improvements

1. Follow-up: the first real reffs WQE exercises the REFFS run and its reffs-check, which needs Docker on the integrator's machine.

## Follow-up ownership and success criteria

| Follow-up | Proposed owner | Success criterion |
|---|---|---|
| First REFFS run with reffs-check | Tom Haynes assigns the integrator | A run completes green and lands |

## Cleanup and retained evidence

- Evidence: `agent-work/WI-CI-TOOLING-WORKFLOW-PILOT-SMOKE/evidence/` (lint output).
- Claim released after the landing.
