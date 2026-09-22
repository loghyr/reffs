<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Local engineering addendum

## Ownership and scope

The accountable human is Tom Haynes; that human alone authorizes any
additional state file beside the WQEs and assigns the integrator for each
REFFS run. This repository owns the reffs sources under src/, lib/, tools/,
support/, scripts/, examples/, hooks/, deploy/, the autotools and CI build
files, and its local queue packets under docs/work-queue/. tools/biq.py and
tools/test_biq.py own the shared process; common changes go upstream to
engineering-workflow first and arrive here by adopting a tagged release.
Each assigned agent names its concrete task and files before editing.
CLAUDE.md and .claude/ remain the project's coding standards and roles; this
addendum and AGENTS.md govern the queue.

## Verification and resources

Run python3 -m unittest tools.test_biq for shared machinery changes. The REFFS
run WQE declares reffs-check, which is `make -f Makefile.ci check` (a Docker
build, unit and integration run); the hosted C/C++ CI workflow stays as an
advisory check. Retain exact source, commands, exit results, and logs under
the owning agent-work/<WQE-ID>/ root. Keep WQE retrospectives and their index
under docs/work-queue/retrospectives/. No cloud or lab allocation is
permitted through the queue. The GitHub landing status does not replace the
declared check. Source defines behavior; the active packet and claim define
scope. Historical reports are evidence, not current authority.

## Local documents

Keep credentials and organization-private material out of this public
history. Design documents live under .claude/design/ and docs/; the queue
adds no general README. The installed contract, templates, and tool help
route its process.
