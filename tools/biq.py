#!/usr/bin/env python3
# engineering-workflow v1.8.9 0fd176b89167d3d9d508741ace982de18a5eeaeb
"""Batch integration queues.

The packets under docs/work-queue/items/ are the whole record. Each queue owns
one open run WQE, WI-CI-INTEGRATION-<QUEUE>-REPEAT-<n>, whose member list is
the queue. A submission is offered to every queue; each leaf queue answers by
the changed paths, and the candidate is appended to every caring queue's open
run WQE. Starting a run claims that WQE, freezes its members, merges them, and
opens the successor. A green run records its result on the run WQE and passes
it to the consumer queue's open run WQE, or lands it on the target branch. A
red run rejects the named members and merges again without them. Status is
written into packets when it changes. Live claims are compare-and-swap refs
under the configured REF_NAMESPACE's claims/ prefix.
"""
from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import getpass
import hashlib
import socket
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path
from pathlib import PurePosixPath
from collections import Counter
from graphlib import CycleError, TopologicalSorter

VERSION = "1.8.9"
REF_NAMESPACE = "reffs-work-queue"
CLAIM_REF_PREFIX = f"refs/heads/{REF_NAMESPACE}/claims/"
CANDIDATE_REF_PREFIX = f"refs/heads/{REF_NAMESPACE}/candidates/"

# InlineFS history: historical packets and frozen runs keep their original IDs.
# New runs use the current queue spelling and omit the redundant BIQ token.
QUEUE_RENAMES = {
    "FRONTEND-USERSPACE": "INLINEFS-FRONTEND-USERSPACE",
    "FRONTEND-KERNEL": "INLINEFS-FRONTEND-KERNEL",
    "FRONTEND-GPU": "INLINEFS-FRONTEND-GPU",
    "KERNEL-PATCH": "LINUX-KERNEL",
}

ITEMS_DIR = "docs/work-queue/items"
CONTROL_ROOTS = ("docs/work-queue/", "docs/specs/")
REMOTE = "origin"
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
ITEM_RE = re.compile(r"^WI-[A-Z0-9-]+$")
FIELD_RE = re.compile(r"^- ([A-Za-z][A-Za-z _]*): `([^`]*)`$")
MEMBER_RE = re.compile(r"^- (WI-[A-Z0-9-]+) @([0-9a-f]{40})(?: \((.*)\))?$")
TRAILER_RE = re.compile(r"^Work-Queue-Item: (WI-[A-Z0-9-]+)\s*$", re.M)
RUN_ITEM_RE = re.compile(r"^WI-CI-INTEGRATION-(?P<legacy>BIQ-)?(?P<queue>[A-Z0-9-]+?)-(?:REPEAT|RUN)-(?P<ordinal>[1-9][0-9]*)$")  # RUN is the retired spelling
CHANGE_STATUSES = frozenset({"A", "M", "D", "T"})
STATUSES = ("open", "submitted", "completed", "failed-rejected", "failed-abandoned")
TERMINAL = frozenset({"completed", "failed-rejected", "failed-abandoned"})


def run_identity(item_id: str, queue: str = "") -> tuple[str, int] | None:
    """Read current run IDs and historical BIQ IDs, excluding ordinary sync WIs."""
    match = RUN_ITEM_RE.fullmatch(item_id)
    if match is None:
        return None
    name = QUEUE_RENAMES.get(match["queue"], match["queue"])
    # The old BIQ prefix was unambiguous. Current IDs need the run's Queue
    # field: ordinary integration repeats (e.g. Hammerspace sync) are not runs.
    if not match["legacy"] and name != QUEUE_RENAMES.get(queue, queue):
        return None
    return name, int(match["ordinal"])


class BiqError(RuntimeError):
    pass


# --- git -------------------------------------------------------------------

def git(root: Path, *args: str, strip: bool = True) -> str:
    result = subprocess.run(["git", *args], cwd=root, text=True, capture_output=True)
    if result.returncode:
        raise BiqError(f"git {' '.join(args)}: {result.stderr.strip() or 'failed'}")
    return result.stdout.strip() if strip else result.stdout


def rev(root: Path, ref: str) -> str:
    value = git(root, "rev-parse", "--verify", f"{ref}^{{commit}}")
    if not COMMIT_RE.match(value):
        raise BiqError(f"{ref} is not a commit")
    return value


def is_ancestor(root: Path, ancestor: str, descendant: str) -> bool:
    ok = subprocess.run(["git", "merge-base", "--is-ancestor", ancestor, descendant], cwd=root, capture_output=True).returncode == 0
    if not ok and not (have_commit(root, ancestor) and have_commit(root, descendant)):
        fetch_candidates(root)
        ok = subprocess.run(["git", "merge-base", "--is-ancestor", ancestor, descendant], cwd=root, capture_output=True).returncode == 0
    return ok


_fetched_candidates = False


def have_commit(root: Path, sha: str) -> bool:
    return subprocess.run(["git", "cat-file", "-e", f"{sha}^{{commit}}"], cwd=root, capture_output=True).returncode == 0


def fetch_candidates(root: Path) -> None:
    """Every commit the packets name is published under the candidate refs; fetch them once when one is missing."""
    global _fetched_candidates
    if _fetched_candidates:
        return
    _fetched_candidates = True
    subprocess.run(["git", "fetch", "--quiet", REMOTE, f"+{CANDIDATE_REF_PREFIX}*:refs/remotes/{REMOTE}/{REF_NAMESPACE}/candidates/*"], cwd=root, capture_output=True)


def ensure_commit(root: Path, sha: str) -> None:
    if not have_commit(root, sha):
        fetch_candidates(root)
    if not have_commit(root, sha):
        raise BiqError(f"{sha[:12]} is not a commit here and is not published under {CANDIDATE_REF_PREFIX}")


def move_branch(root: Path, branch: str, commit: str) -> None:
    """Point a queue-owned branch at a commit, including when a worktree has it checked out.

    ``git branch -f`` refuses a branch that is checked out anywhere; an
    integrator who inspected or resolved on the run's branch in a worktree
    then saw the tool fail. A clean worktree on that branch is moved with it;
    one with local changes is named so the owner can commit or detach first.
    """
    moved = subprocess.run(["git", "branch", "-f", branch, commit], cwd=root, text=True, capture_output=True)
    if moved.returncode == 0:
        return
    worktree = None
    for block in git(root, "worktree", "list", "--porcelain", strip=False).split("\n\n"):
        lines = block.splitlines()
        if any(line == f"branch refs/heads/{branch}" for line in lines):
            worktree = Path(lines[0].split(" ", 1)[1])
            break
    if worktree is None:
        raise BiqError(f"git branch -f {branch}: {moved.stderr.strip() or 'failed'}")
    if git(worktree, "status", "--porcelain"):
        raise BiqError(f"{branch} is checked out at {worktree} with local changes; commit or detach there, then rerun")
    git(worktree, "reset", "--quiet", "--hard", commit)


def publish_candidate(root: Path, item_id: str, sha: str) -> None:
    """Publish a commit the packets name under the candidate refs of its WQE."""
    git(root, "push", "--quiet", REMOTE, f"{sha}:{CANDIDATE_REF_PREFIX}{item_id}/{sha[:12]}")


def publish_candidates_atomic(root: Path, candidates: list[tuple[str, str]]) -> None:
    """Publish a set of newly named commits together, or publish none of them."""
    if candidates:
        specs = [f"{sha}:{CANDIDATE_REF_PREFIX}{item_id}/{sha[:12]}" for item_id, sha in candidates]
        git(root, "push", "--quiet", "--atomic", REMOTE, *specs)


def changed_status(root: Path, base: str, head: str) -> dict[str, str]:
    """Path -> change letter (A, M, D, T) between the merge base and head."""
    out = git(root, "diff", "--name-status", "--no-renames", f"{base}...{head}", strip=False)
    status = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and parts[0][:1] in CHANGE_STATUSES:
            status[parts[1]] = parts[0][:1]
    return status


def describe_care(status: dict[str, str], hits: list[str], limit: int = 3) -> str:
    """'care (N paths: a added, m modified, d deleted; first, second, third)'."""
    names = {"A": "added", "M": "modified", "D": "deleted", "T": "retyped"}
    counts: dict[str, int] = {}
    for path in hits:
        letter = status.get(path, "M")
        counts[letter] = counts.get(letter, 0) + 1
    kinds = ", ".join(f"{n} {names.get(k, k)}" for k, n in sorted(counts.items(), key=lambda kv: -kv[1]))
    return f"care ({len(hits)} paths: {kinds}; " + ", ".join(hits[:limit]) + (", ..." if len(hits) > limit else "") + ")"


def changed_paths(root: Path, base: str, head: str) -> list[str]:
    """Paths added, modified, deleted, or retyped between the merge base and head."""
    out = git(root, "diff", "--name-status", "--no-renames", f"{base}...{head}", strip=False)
    paths = []
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and parts[0][:1] in CHANGE_STATUSES:
            paths.append(parts[1])
    return sorted(set(paths))


def now_utc() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


# --- shared release adoption -----------------------------------------------

# Upstream source -> installed path. README, ADOPTING and CHANGELOG stay upstream.
ADOPTION_COMMON_FILES = {
    "tools/biq.py": "tools/biq.py",
    "tools/test_biq.py": "tools/test_biq.py",
    "AGENTS.md": "AGENTS.md",
    "docs/work-queue/TEMPLATE.md": "docs/work-queue/TEMPLATE.md",
    "docs/work-queue/retrospectives/TEMPLATE.md": "docs/work-queue/retrospectives/TEMPLATE.md",
}
ADOPTION_GATE_FILES = {
    "github": ("templates/verify-product-integration.yml", ".github/workflows/verify-product-integration.yml"),
    "gitlab": ("templates/verify-product-integration.gitlab.yml", ".gitlab-ci.yml"),
}


def adoption_files(provider: str = "github") -> dict[str, str]:
    """Return the six installed files for one explicitly selected provider."""
    try:
        source, installed = ADOPTION_GATE_FILES[provider]
    except KeyError as error:
        raise BiqError(f"unknown adoption provider {provider!r}") from error
    return {**ADOPTION_COMMON_FILES, source: installed}


# Backward-compatible name for callers and fixtures using the default provider.
ADOPTION_FILES = adoption_files()
RELEASE_HEADER_RE = re.compile(rb"# engineering-workflow (\S+)(?: ([0-9a-f]{40}))?")


def adoption_file(root: Path, commit: str, path: str) -> tuple[str, bytes]:
    """Read only a regular file's mode and exact blob bytes, never a checkout."""
    entry = git(root, "ls-tree", "-z", commit, "--", path)
    if not entry:
        raise BiqError(f"{path}: missing installed file at {commit[:12]}")
    metadata, name = entry.rstrip("\0").split("\t", 1)
    mode, kind, sha = metadata.split()
    if name != path or kind != "blob" or mode not in {"100644", "100755"}:
        raise BiqError(f"{path}: expected a regular file, found {mode} {kind}")
    result = subprocess.run(["git", "cat-file", "blob", sha], cwd=root, capture_output=True)
    if result.returncode:
        raise BiqError(f"{path}: cannot read blob {sha}")
    return mode, result.stdout


def release_header(data: bytes, *, consumer: bool) -> tuple[str, str | None]:
    headers = [line for line in data.splitlines() if line.startswith(b"# engineering-workflow")]
    match = RELEASE_HEADER_RE.fullmatch(headers[0]) if len(headers) == 1 else None
    if match is None or match[1] == b"unreleased" or bool(match[2]) != consumer:
        raise BiqError("tools/biq.py: unreleased or unparsable release header (expected a tag" + (" and its 40-character commit SHA" if consumer else " only") + ")")
    try:
        return match[1].decode("ascii"), match[2].decode("ascii") if match[2] else None
    except UnicodeDecodeError as error:
        raise BiqError("tools/biq.py: unparsable release header") from error


def normalized_adoption_file(path: str, data: bytes) -> bytes:
    """Mask only the four explicitly permitted literal adoption bindings."""
    if path == "tools/biq.py":
        data = re.sub(rb"(?m)^(# engineering-workflow \S+) [0-9a-f]{40}$", rb"\1", data)
        pattern, replacement = rb'(?m)^REF_NAMESPACE = "[A-Za-z0-9][A-Za-z0-9._/-]*"$', b'REF_NAMESPACE = "<local>"'
    elif path in {"templates/verify-product-integration.yml", "templates/verify-product-integration.gitlab.yml"}:
        pattern, replacement = rb"(?m)^  BIQ_REF_NAMESPACE: [A-Za-z0-9][A-Za-z0-9._/-]*$", b"  BIQ_REF_NAMESPACE: <local>"
    elif path == "AGENTS.md":
        pattern = rb"(?m)^Read the required \[local addendum\]\([A-Za-z0-9_./-]+\.md(?:#[A-Za-z0-9_-]+)?\) before starting work\.$"
        replacement = b"Read the required [local addendum](<local>) before starting work."
    else:
        return data
    normalized, count = re.subn(pattern, replacement, data)
    if count != 1:
        raise BiqError(f"{path}: expected exactly one literal adoption binding, found {count}")
    return normalized


def adoption_errors(root: Path, upstream: Path, tree: str = "HEAD", provider: str = "github") -> list[str]:
    """Compare a committed installed copy with its locally available release tag.

    This inspection does not fetch, execute candidate code, relocate the checker,
    read queue definitions or release stale claims. The caller supplies the clone.
    """
    if not upstream.is_dir():
        return [f"{upstream}: upstream clone directory does not exist"]
    commit = rev(root, tree)
    try:
        consumer_tool = adoption_file(root, commit, "tools/biq.py")
        tag, recorded_sha = release_header(consumer_tool[1], consumer=True)
        released_sha = rev(upstream, f"refs/tags/{tag}")
    except BiqError as error:
        return [str(error)]
    if released_sha != recorded_sha:
        return [f"tools/biq.py: release tag {tag} resolves to {released_sha}, not recorded SHA {recorded_sha} (moved tag or incorrect SHA)"]
    errors = []
    try:
        files = adoption_files(provider)
    except BiqError as error:
        return [str(error)]
    for source, installed in files.items():
        try:
            expected_mode, expected = adoption_file(upstream, released_sha, source)
            actual_mode, actual = consumer_tool if installed == "tools/biq.py" else adoption_file(root, commit, installed)
            if source == "tools/biq.py" and release_header(expected, consumer=False)[0] != tag:
                errors.append(f"{installed}: upstream release header does not name {tag}")
            if actual_mode != expected_mode:
                errors.append(f"{installed}: file mode {actual_mode} differs from upstream {expected_mode}")
            if normalized_adoption_file(source, actual) != normalized_adoption_file(source, expected):
                errors.append(f"{installed}: bytes differ from {tag}:{source} outside the four permitted bindings")
        except BiqError as error:
            errors.append(str(error))
    return errors


# --- registry --------------------------------------------------------------

@dataclass(frozen=True)
class Queue:
    id: str
    kind: str
    feeds: str | None
    feeders: tuple[str, ...]
    budget_usd: float
    tests: tuple[str, ...]
    files: frozenset[str] = frozenset()
    prefixes: tuple[str, ...] = ()
    patterns: tuple[str, ...] = ()
    qualification: str | None = None

    def cares(self, paths: list[str]) -> list[str]:
        """The changed paths this leaf queue accepts; an aggregator accepts none by path."""
        if self.kind != "leaf":
            return []
        return [p for p in paths if p in self.files or p.startswith(self.prefixes) or any(fnmatch.fnmatch(p, g) for g in self.patterns)]


@dataclass(frozen=True)
class Registry:
    target: str
    queues: dict[str, Queue]
    workstream: str | None = None
    commands: dict[str, str] = field(default_factory=dict)
    tests: dict[str, dict] = field(default_factory=dict)  # name -> {"command", "target", "cost"}; what the previous registry carried

    def roots(self) -> list[Queue]:
        return [q for q in self.queues.values() if q.feeds is None]


def validate_queues(queues: dict[str, Queue]) -> None:
    for queue in queues.values():
        if queue.feeds is not None:
            consumer = queues.get(queue.feeds)
            if consumer is None or consumer.kind != "aggregator" or queue.id not in consumer.feeders:
                raise BiqError(f"queue {queue.id} feeds {queue.feeds}, which is not an aggregator listing it")
        for feeder in queue.feeders:
            if feeder not in queues or queues[feeder].feeds != queue.id:
                raise BiqError(f"aggregator {queue.id} names feeder {feeder}, which does not feed it")
        seen, current = {queue.id}, queue.feeds
        while current is not None:
            if current in seen:
                raise BiqError(f"feed cycle through {current}")
            seen.add(current)
            current = queues[current].feeds


DEFINITION_KINDS = {"leaf", "aggregator"}
CARE_LINE_RE = re.compile(r"^- (file|prefix|pattern): (\S.*)$")
QUEUE_TEST_RE = re.compile(r"^- ([A-Za-z0-9][A-Za-z0-9_.-]*): (.+?) \((local|kernel|gpu), (free|paid)\)$")


def none_or(value: str | None) -> str | None:
    return None if value in (None, "", "none", "pending") else value


def queue_from_run(packet: Packet) -> Queue | None:
    """The queue a run WQE defines: its header fields and its Cares about and Queue tests sections."""
    kind = packet.fields.get("Queue kind")
    if not packet.run or kind is None:
        return None
    queue_id = packet.run[0]
    if kind not in DEFINITION_KINDS:
        raise BiqError(f"{packet.item_id}: Queue kind must be leaf or aggregator")
    feeds = none_or(packet.fields.get("Feeds"))
    feeds = QUEUE_RENAMES.get(feeds, feeds) if feeds else None  # historical queue names keep reading
    feeders = tuple(QUEUE_RENAMES.get(f.strip(), f.strip()) for f in (none_or(packet.fields.get("Feeders")) or "").split(",") if f.strip())
    if kind == "leaf" and feeders:
        raise BiqError(f"{packet.item_id}: a leaf queue declares no Feeders")
    if kind == "aggregator" and not feeders:
        raise BiqError(f"{packet.item_id}: an aggregator declares its Feeders")
    qualification = none_or(packet.fields.get("Qualification"))
    if qualification is not None and not ITEM_RE.match(qualification):
        raise BiqError(f"{packet.item_id}: Qualification must be a WQE id")
    files, prefixes, patterns = set(), [], []
    for line in packet.sections.get("Cares about", "").splitlines():
        line = line.strip()
        if not line or line == "none":
            continue
        m = CARE_LINE_RE.match(line)
        if not m:
            raise BiqError(f"{packet.item_id}: Cares about lines are `- file: <path>`, `- prefix: <path/>`, or `- pattern: <glob>`, not {line!r}")
        {"file": files.add, "prefix": prefixes.append, "pattern": patterns.append}[m.group(1)](m.group(2).strip())
    tests = []
    for line in packet.sections.get("Queue tests", "").splitlines():
        line = line.strip()
        if not line or line == "none":
            continue
        m = QUEUE_TEST_RE.match(line)
        if not m:
            raise BiqError(f"{packet.item_id}: Queue tests lines are `- <name>: <command> (<local|kernel|gpu>, <free|paid>)`, not {line!r}")
        tests.append(m.group(1))
    return Queue(queue_id, kind, feeds, feeders, parse_usd(packet.fields.get("Budget", "$0")), tuple(tests), frozenset(files), tuple(prefixes), tuple(patterns), qualification)


def queue_tests_of(packet: Packet) -> dict[str, dict]:
    out = {}
    for line in packet.sections.get("Queue tests", "").splitlines():
        m = QUEUE_TEST_RE.match(line.strip())
        if m:
            out[m.group(1)] = {"command": m.group(2), "target": m.group(3), "cost": m.group(4)}
    return out


def registry_from_packets(packets: dict[str, Packet]) -> Registry | None:
    """The queues as their run WQEs define them: the open run of each queue, else its latest run.

    Returns None when no run WQE carries a definition.
    """
    defining: dict[str, Packet] = {}
    for packet in packets.values():
        if not packet.run or packet.fields.get("Queue kind") is None:
            continue
        queue_id, ordinal = packet.run
        current = defining.get(queue_id)
        if current is None or (run_state(packet) == "open") > (run_state(current) == "open") or (
                (run_state(packet) == "open") == (run_state(current) == "open") and ordinal > current.run[1]):
            defining[queue_id] = packet
    if not defining:
        return None
    queues, commands, tests = {}, {}, {}
    for queue_id, packet in sorted(defining.items()):
        queues[queue_id] = queue_from_run(packet)
        for name, meta in queue_tests_of(packet).items():
            if name in tests and tests[name] != meta:
                raise BiqError(f"queue test {name} is defined differently by {packet.item_id} and another run WQE")
            tests[name] = meta
            commands[name] = meta["command"]
    validate_queues(queues)
    for queue in queues.values():
        for test in queue.tests:
            if test not in commands:
                raise BiqError(f"queue {queue.id} test {test} has no command in its Queue tests")
    # The workstream is the project WQE that lists a run of these queues among its members;
    # that relationship lives in the workstream WQE and nowhere else.
    run_ids = {p.item_id for p in packets.values() if p.run}
    workstream = next((p.item_id for p in sorted(packets.values(), key=lambda p: p.item_id)
                       if p.fields.get("Container mode") == "project"
                       and any(m.strip() in run_ids for m in p.fields.get("Container members", "").split(","))), None)
    return Registry("main", queues, workstream, commands, tests)


def load_registry(root: Path, revision: str | None = None, packets: dict[str, Packet] | None = None) -> Registry:
    """The queues as their run WQEs define them; there is no other source."""
    packets = load_packets(root, revision) if packets is None else packets
    registry = registry_from_packets(packets)
    if registry is None:
        raise BiqError("no queue is defined: every queue's open run WQE carries its definition (Queue kind, Feeds, Feeders, Budget, Cares about, Queue tests)")
    return registry


def queue_state(root: Path, target: str = f"{REMOTE}/main", fresh: bool = True) -> tuple[Registry, dict[str, Packet], str | None]:
    """Queue state is the target branch's.

    The working tree is read only when this checkout's packets directory and
    registry are the target's, so that a control checkout of current main sees
    the edits earlier commands left there. Any other checkout, an owner's branch above
    all, reads the target's state: a stale copy is never queue state. A target
    that could not be fetched (`fresh` false) is such a copy, so the working tree
    is never taken for it. Returns the revision the state came from, or None for
    the working tree.
    """
    if fresh and queue_state_ids(root, "HEAD") == queue_state_ids(root, target) and (root / ITEMS_DIR).is_dir():
        packets = load_packets(root)
        return load_registry(root, packets=packets), packets, None
    packets = load_packets(root, target)
    return load_registry(root, target, packets=packets), packets, target


def queue_state_ids(root: Path, revision: str) -> tuple[str, str]:
    """The object id of what is queue state: the packets directory. Docs beside it are not."""
    try:
        return (git(root, "rev-parse", f"{revision}:{ITEMS_DIR}"), "")
    except BiqError:
        return ("", "")


def staleness_notice(root: Path, target: str) -> str | None:
    """How far this checkout sits behind the fetched target, so no agent mistakes an old tree for the system."""
    behind = subprocess.run(["git", "rev-list", "--count", f"HEAD..{target}"], cwd=root, text=True, capture_output=True)
    if behind.returncode or not behind.stdout.strip().isdigit() or int(behind.stdout) == 0:
        return None
    dates = subprocess.run(["git", "log", "-1", "--format=%ct", "HEAD"], cwd=root, text=True, capture_output=True).stdout.strip()
    tip = subprocess.run(["git", "log", "-1", "--format=%ct", target], cwd=root, text=True, capture_output=True).stdout.strip()
    days = f", {(int(tip) - int(dates)) // 86400} days" if dates.isdigit() and tip.isdigit() and int(tip) - int(dates) >= 86400 else ""
    return (f"this checkout is {int(behind.stdout)} commits{days} behind {target}: its docs, tools and source are not the current system; "
            f"queue state and this tool come from {target}, read authority there")


def fetch_target(root: Path, target: str) -> bool:
    """Refresh the remote target ref quietly; a failed fetch keeps the last fetched state, says so, and returns False.

    A clone taken from a partial (blobless) checkout fails here with unresolved
    deltas; reads go on from the last fetched state, and writes go through the
    control clone, which fetches for itself.
    """
    remote, _slash, branch = target.partition("/")
    if remote != REMOTE:
        return True
    result = subprocess.run(["git", "fetch", "--quiet", remote, branch], cwd=root, text=True, capture_output=True)
    if result.returncode:
        print(f"warning: could not fetch {target}; using the last fetched state ({result.stderr.strip().splitlines()[-1] if result.stderr.strip() else 'fetch failed'}); "
              "queue state is written only through the control clone", file=sys.stderr)
        return False
    return True


WRITES_PACKETS = {"seal", "submit", "abandon", "reject", "materialize", "land", "run", "workstream"}
TOOL_PATH = "tools/biq.py"
RELOCATED_ENV = "BIQ_TOOL_FROM_TARGET"


def relocate_to_target_tool(root: Path, target: str, argv: list[str], owner: str | None = None) -> None:
    """Run the target's copy of this tool, never a checkout's stale copy.

    The queue's rules are main's rules: if the file executing differs from
    ``target:tools/biq.py``, re-execute the same command from the control clone,
    which is checked out at the fetched target. ``verify`` runs the tool it is
    given (the gate's job), a pinned ``--target-ref`` keeps the local tool, and
    a target without the tool leaves the local one in place.
    """
    if os.environ.get(RELOCATED_ENV):
        return
    shown = subprocess.run(["git", "show", f"{target}:{TOOL_PATH}"], cwd=root, capture_output=True)
    if shown.returncode:
        return
    try:
        local = Path(__file__).read_bytes()
    except OSError:
        return
    if shown.stdout == local:
        return
    clone = control_clone(root, target, clone_owner(root, owner))
    tool = clone / TOOL_PATH
    if tool.read_bytes() != shown.stdout:
        raise BiqError(f"the control clone's {TOOL_PATH} differs from {target}; delete {clone} and run again")
    tip = rev(root, target)
    print(f"running {target}'s tool ({tip[:12]}) from {clone}; this checkout's copy is not it", file=sys.stderr)
    passthrough: list[str] = []
    skip = False
    for argument in argv:
        if skip:
            skip = False
            continue
        if argument == "--repository":
            skip = True
            continue
        if argument.startswith("--repository="):
            continue
        passthrough.append(argument)
    command = [sys.executable, str(tool), "--repository", str(root), *passthrough]
    os.execve(sys.executable, command, {**os.environ, RELOCATED_ENV: tip})
CONTROL_BRANCH_PREFIX = "control/"


def remote_ref_of(target: str) -> str:
    """origin/main for main or origin/main: the remote-tracking ref a fresh clone can check out."""
    branch = target.partition("/")[2] if target.startswith(REMOTE + "/") else target
    return f"{REMOTE}/{branch}"


def clone_owner(root: Path, owner: str | None) -> str:
    """The owner a control clone belongs to: the command's --owner, else the Git user."""
    return owner or git(root, "config", "user.name") or "agent"


def control_clone(root: Path, target: str, owner: str) -> Path:
    """The one control clone the tool keeps per owner and remote, an independent clone of the remote, detached at the fetched target.

    Queue state is written here and never in the agent's own checkout: an owner's
    clone stays on its branch, and a stale copy is never mistaken for queue state.
    The clone is the owner's alone: every command resets it to the fetched target
    before writing, so two owners sharing one clone would reset each other's
    staged writes. Set BIQ_CONTROL_DIR to place it; the default is under the
    user's cache directory.
    """
    url = git(root, "remote", "get-url", REMOTE)
    base = Path(os.environ.get("BIQ_CONTROL_DIR") or (Path.home() / ".cache" / "inlinefs-biq" / "control"))
    clone = base / (hashlib.sha256(url.encode("utf-8")).hexdigest()[:12] + "-" + re.sub(r"[^A-Za-z0-9._-]+", "-", owner))
    if not (clone / ".git").is_dir():
        clone.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(["git", "clone", "--quiet", url, str(clone)], text=True, capture_output=True)
        if result.returncode:
            raise BiqError(f"could not create the control clone at {clone}: {result.stderr.strip()}")
    ref = remote_ref_of(target)
    git(clone, "fetch", "--quiet", REMOTE, ref.partition("/")[2])
    git(clone, "checkout", "--quiet", "--detach", ref)
    git(clone, "reset", "--quiet", "--hard", ref)
    git(clone, "clean", "--quiet", "-fd", "--", ITEMS_DIR)
    return clone


def control_branch(owner: str, item_id: str) -> str:
    return f"{CONTROL_BRANCH_PREFIX}{re.sub(r'[^A-Za-z0-9._-]+', '-', owner)}/{item_id.lower()}"


def push_branch_with_lease(root: Path, branch: str, commit: str = "HEAD") -> None:
    """Push one observed branch tip with an explicit lease, including absence."""
    ref = f"refs/heads/{branch}"
    observed = git(root, "ls-remote", REMOTE, ref)
    expected = observed.split()[0] if observed else ""
    git(root, "push", "--quiet", f"--force-with-lease={ref}:{expected}", REMOTE, f"{commit}:{ref}")


OPERATION_TRAILER = "BIQ-Operation"
OPERATION_RE = re.compile(r"^BIQ-Operation: (sha256:[0-9a-f]{64}) (.+)$", re.M)


def operation_identity(args, packets: dict[str, "Packet"]) -> dict[str, str]:
    """Stable identity and readable label for one requested packet mutation.

    The identity prevents a rerun from executing the mutation twice.  It is a
    commit annotation, not queue state: packets and the target remain the only
    authority for what happened.
    """
    item_id = getattr(args, "item_id", None)
    if args.command == "run":
        if args.run_command == "start":
            item_id = open_run(packets, args.queue_id).item_id
        else:
            item_id = find_run(packets, args.queue_id, args.ordinal).item_id
    elif item_id is None:
        item_id = args.command
    ignored = {"repository", "target_ref", "apply"}
    values = {
        key: str(value) if isinstance(value, Path) else value
        for key, value in vars(args).items()
        if key not in ignored and value not in (None, False, [], "")
    }
    values["resolved_item"] = item_id
    encoded = json.dumps(values, sort_keys=True, separators=(",", ":"))
    command = args.command if args.command != "run" else f"run {args.run_command}"
    label = f"{command} {item_id}"
    return {"token": "sha256:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest(), "label": label, "item": item_id, "command": command}


def commit_operation(root: Path, commit: str) -> dict[str, str] | None:
    """The one operation trailer on commit, or None for a legacy commit."""
    match = OPERATION_RE.search(git(root, "show", "-s", "--format=%B", commit, strip=False))
    if match is None:
        return None
    return {"token": match.group(1), "label": match.group(2), "command": match.group(2).split(" ", 1)[0]}


def stage_control_clone(root: Path, clone: Path, target: str, owner: str, item_id: str, operation: dict[str, str],
                        extra_drafts: tuple[str, ...] = ()) -> tuple[str, dict[str, object] | None]:
    """Stage a new control write, or return the operation already pending for this WQE."""
    branch = control_branch(owner, item_id)
    ref = remote_ref_of(target)
    fetched = subprocess.run(["git", "fetch", "--quiet", REMOTE, f"+refs/heads/{branch}:refs/remotes/{REMOTE}/{branch}"], cwd=clone, text=True, capture_output=True)
    if fetched.returncode == 0 and not is_ancestor(clone, f"{REMOTE}/{branch}", ref):
        git(clone, "checkout", "--quiet", "-B", branch, f"{REMOTE}/{branch}")
        pending: dict[str, object] = commit_operation(clone, "HEAD") or {"token": "legacy", "label": f"legacy pending write for {item_id}", "command": "control"}
        pending["same"] = pending["token"] == operation["token"]
        return branch, pending
    else:
        git(clone, "checkout", "--quiet", "-B", branch, ref)
    for drafted in (item_id, *extra_drafts):  # a new packet drafted in the owner's clone, and a restart's successor
        local = root / ITEMS_DIR / f"{drafted}.md"
        if local.is_file() and not (clone / ITEMS_DIR / f"{drafted}.md").is_file():
            (clone / ITEMS_DIR / f"{drafted}.md").write_text(local.read_text(encoding="utf-8"), encoding="utf-8")
    return branch, None


def carry_retrospective(root: Path, clone: Path, item_id: str, path: str | None) -> str | None:
    """Copy the owner's committed retrospective and index into the control clone as its own commit, so the submission can name that commit."""
    try:
        rel = find_retrospective(root, item_id, path)
    except BiqError:
        return None
    retrospective_evidence(root, rel)  # committed and indexed in the owner's checkout
    if (clone / rel).is_file() and (clone / rel).read_bytes() == (root / rel).read_bytes() and not git(clone, "status", "--porcelain", "--", rel):
        return rel
    (clone / rel).parent.mkdir(parents=True, exist_ok=True)
    (clone / rel).write_bytes((root / rel).read_bytes())
    (clone / RETROSPECTIVE_INDEX).write_bytes((root / RETROSPECTIVE_INDEX).read_bytes())
    git(clone, "add", "--", rel, RETROSPECTIVE_INDEX)
    if git(clone, "status", "--porcelain", "--", rel, RETROSPECTIVE_INDEX):
        git(clone, "commit", "--quiet", "-m", f"queue: retrospective {item_id}\n\nWork-Queue-Item: {item_id}")
    return rel


def commit_control_clone(clone: Path, branch: str, command: str, item_id: str, owner: str, operation: dict[str, str],
                         target: str | None = None, candidate_refs: tuple[tuple[str, str], ...] = (),
                         branch_actions: tuple[tuple[str, str | None], ...] = ()) -> dict:
    """Commit what the command wrote, refuse it here if the gate would, then push the control branch.

    A control commit the target's gate would refuse is verified against the
    fetched target before anything is pushed: the commit is dropped, the clone
    is back at the branch's previous tip, and the command fails with the gate's
    reason, leaving no pending branch or pull request behind.
    """
    git(clone, "add", "--", ITEMS_DIR)
    if not git(clone, "status", "--porcelain", "--", ITEMS_DIR):
        return {"branch": branch, "commit": None, "pr": None}
    git(clone, "commit", "--quiet", "-m", f"queue: {command} {item_id}\n\nQueue state written by biq on the fetched target for {owner}.\n\nWork-Queue-Item: {item_id}\n{OPERATION_TRAILER}: {operation['token']} {operation['label']}")
    sha = git(clone, "rev-parse", "HEAD")
    if target is not None:
        try:
            verify_landing(clone, remote_ref_of(target), sha)
        except BiqError as error:
            git(clone, "reset", "--quiet", "--hard", f"{sha}^")
            raise BiqError(f"{command} {item_id} refused before push, nothing pending: {error}")
    if candidate_refs:
        ref = f"refs/heads/{branch}"
        observed = git(clone, "ls-remote", REMOTE, ref)
        expected = observed.split()[0] if observed else ""
        specs = [f"{candidate}:{CANDIDATE_REF_PREFIX}{candidate_item}/{candidate[:12]}"
                 for candidate_item, candidate in candidate_refs]
        try:
            git(clone, "push", "--quiet", "--atomic", f"--force-with-lease={ref}:{expected}", REMOTE, *specs, f"{sha}:{ref}")
        except BiqError as error:
            git(clone, "reset", "--quiet", "--hard", f"{sha}^")
            raise BiqError(f"{command} {item_id} publication failed, nothing pending: {error}") from error
    else:
        try:
            push_branch_with_lease(clone, branch)
        except BiqError as error:
            git(clone, "reset", "--quiet", "--hard", f"{sha}^")
            raise BiqError(f"{command} {item_id} publication failed, nothing pending: {error}") from error
    for run_branch, integration in branch_actions:
        if integration is None:
            subprocess.run(["git", "branch", "-D", run_branch], cwd=clone, capture_output=True)
        else:
            move_branch(clone, run_branch, integration)
    return {"branch": branch, "commit": sha, "pr": None}


LANDING_WAIT_SECONDS = int(os.environ.get("BIQ_LANDING_WAIT_SECONDS", "900"))
LANDING_POLL_SECONDS = float(os.environ.get("BIQ_LANDING_POLL_SECONDS", "10"))
PENDING_EXIT = 3  # a control or landing change request is not on the target yet: rerun the same command to follow it


def change_provider(cwd: Path) -> str:
    """GitHub or GitLab from origin, with one explicit override for an ambiguous host."""
    override = os.environ.get("BIQ_CHANGE_PROVIDER", "").lower()
    if override:
        if override not in {"github", "gitlab"}:
            raise BiqError("BIQ_CHANGE_PROVIDER must be github or gitlab")
        return override
    origin = git(cwd, "remote", "get-url", REMOTE).lower()
    matches = [provider for provider in ("github", "gitlab") if provider in origin]
    if len(matches) == 1:
        return matches[0]
    raise BiqError(f"cannot detect GitHub or GitLab from origin {origin!r}; set BIQ_CHANGE_PROVIDER=github or gitlab")


def change_cli(provider: str, cwd: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["gh" if provider == "github" else "glab", *args], cwd=cwd, text=True, capture_output=True)


def change_cli_available(provider: str) -> bool:
    return shutil.which("gh" if provider == "github" else "glab") is not None


def change_request_argument(request: str, provider: str) -> str:
    """Use GitLab's documented numeric MR argument even when we retain its URL for diagnostics."""
    if provider == "gitlab":
        match = re.search(r"/merge_requests/(\d+)(?:\D.*)?$", request)
        if match:
            return match.group(1)
    return request


def open_change_request(cwd: Path, branch: str, target: str, title: str, body: str) -> str | None:
    """The open PR/MR for a branch, created when absent; None when the provider CLI cannot do it."""
    provider = change_provider(cwd)
    if not change_cli_available(provider):
        return None
    target_branch = target.partition("/")[2] if target.startswith(REMOTE + "/") else target
    if provider == "github":
        existing = change_cli(provider, cwd, "pr", "list", "--state", "open", "--head", branch, "--base", target_branch, "--json", "url", "--jq", ".[0].url")
        if existing.stdout.strip():
            return existing.stdout.strip()
        created = change_cli(provider, cwd, "pr", "create", "--base", target_branch, "--head", branch, "--title", title, "--body", body)
        return created.stdout.strip() or None
    existing = change_cli(provider, cwd, "mr", "list", "--source-branch", branch, "--target-branch", target_branch, "--output", "json")
    if existing.returncode == 0:
        try:
            rows = json.loads(existing.stdout)
            if rows:
                return str(rows[0].get("web_url") or rows[0].get("webUrl") or rows[0].get("iid") or "") or None
        except (json.JSONDecodeError, AttributeError):
            pass
    created = change_cli(provider, cwd, "mr", "create", "--source-branch", branch, "--target-branch", target_branch,
                         "--title", title, "--description", body, "--remove-source-branch", "--yes")
    match = re.search(r"https?://\S+?/-/merge_requests/\d+", created.stdout + "\n" + created.stderr)
    return match.group(0) if match else None


def change_request_view(cwd: Path, request: str, provider: str | None = None) -> dict | None:
    """Normalized state, merge state and checks of a GitHub PR or GitLab MR."""
    provider = provider or change_provider(cwd)
    argument = change_request_argument(request, provider)
    if provider == "github":
        view = change_cli(provider, cwd, "pr", "view", argument, "--json", "state,mergeStateStatus,statusCheckRollup,mergeCommit,autoMergeRequest")
    else:
        view = change_cli(provider, cwd, "mr", "view", argument, "--output", "json")
    if view.returncode:
        return None
    try:
        data = json.loads(view.stdout)
    except json.JSONDecodeError:
        return None
    if provider == "gitlab":
        pipeline = data.get("head_pipeline") or data.get("headPipeline") or data.get("pipeline") or {}
        status = str(pipeline.get("status") or "").upper()
        conclusions = {"SUCCESS": "SUCCESS", "FAILED": "FAILURE", "FAILURE": "FAILURE", "CANCELED": "CANCELLED", "CANCELLED": "CANCELLED"}
        checks = [conclusions.get(status, status)] if status else []
        merge_status = str(data.get("detailed_merge_status") or data.get("detailedMergeStatus") or data.get("merge_status") or "").lower()
        dirty = bool(data.get("has_conflicts") or data.get("hasConflicts")) or merge_status in {"conflict", "cannot_be_merged"}
        state = str(data.get("state") or "").upper()
        if state == "OPENED":
            state = "OPEN"
        return {"state": state, "merge": "DIRTY" if dirty else "BLOCKED" if merge_status and merge_status not in {"mergeable", "can_be_merged"} else "CLEAN",
                "merge_status": merge_status,
                "checks": checks, "auto": bool(data.get("merge_when_pipeline_succeeds") or data.get("mergeWhenPipelineSucceeds") or data.get("auto_merge_enabled") or data.get("autoMergeEnabled")),
                "commit": data.get("merge_commit_sha") or data.get("mergeCommitSha")}
    checks = [str(c.get("conclusion") or c.get("state") or "") for c in (data.get("statusCheckRollup") or [])]
    return {"state": data.get("state"), "merge": data.get("mergeStateStatus"), "checks": checks,
            "auto": data.get("autoMergeRequest") is not None, "commit": (data.get("mergeCommit") or {}).get("oid")}


def arm_change_request(cwd: Path, request: str, tip: str, provider: str) -> bool:
    """Request a merge commit after required checks, with ordinary source removal on GitLab."""
    argument = change_request_argument(request, provider)
    if provider == "github":
        result = change_cli(provider, cwd, "pr", "merge", argument, "--auto", "--merge")
    else:
        result = change_cli(provider, cwd, "mr", "merge", argument, "--auto-merge", "--remove-source-branch", "--sha", tip, "--yes")
    return result.returncode == 0


def note_kind(part: str) -> str:
    """The one fact a member-note part carries: resolution, review, pending resolution, or something else."""
    for prefix, kind in (("resolved by ", "resolved"), (REVIEW_OK_MARK, "reviewed"), (RESOLUTION_PENDING_MARK, "pending")):
        if part.startswith(prefix):
            return kind
    return part


def merge_member_notes(older: str, newer: str) -> str:
    """Both notes' facts, the newer winning a shared kind; a resolution retires a pending resolution."""
    parts: dict[str, str] = {}
    for note in (older or "", newer or ""):
        for part in (p for p in note.split("; ") if p):
            parts[note_kind(part)] = part
    if "resolved" in parts:
        parts.pop("pending", None)
    order = {"resolved": 0, "pending": 0, "reviewed": 1}
    return "; ".join(part for _kind, part in sorted(parts.items(), key=lambda kv: (order.get(kv[0], 2), kv[1])))


def members_union(ours: str, theirs: str, item_id: str) -> str:
    """Two control commits each wrote the same run packet: keep both member lists, and both sides' notes of a shared member."""
    base = parse_packet(item_id, ours)
    added = parse_packet(item_id, theirs)
    merged: dict[str, tuple[str, str, str]] = {m[0]: m for m in base.members()}
    for item, sha, note in added.members():
        kept = merged.get(item)
        merged[item] = (item, sha, merge_member_notes(kept[2], note) if kept else note)
    excluded = added.sections.get("Excluded", base.sections.get("Excluded", "none"))
    text = base.source
    for section, body in (("Members", member_lines(list(merged.values()))), ("Excluded", excluded.strip() or "none")):
        pattern = re.compile(rf"(^## {section}\n\n)(.*?)(?=^## |\Z)", re.M | re.S)
        text = pattern.sub(lambda m: m.group(1) + body + "\n\n", text, count=1)
    return text


def reconcile_control_branch(clone: Path, branch: str, target: str) -> str:
    """Rebase the pending control branch onto the freshly fetched target, keep both sides of the queue's own conflicts, and push it."""
    ref = remote_ref_of(target)
    git(clone, "fetch", "--quiet", REMOTE, ref.partition("/")[2])
    git(clone, "checkout", "--quiet", branch)
    rebase_control_branch(clone, branch, ref)
    push_branch_with_lease(clone, branch)
    return git(clone, "rev-parse", "HEAD")


def rebase_control_branch(clone: Path, branch: str, ref: str) -> None:
    """Rebase the checked-out control branch onto ref, resolving the two conflicts a queue manufactures.

    Two control commits each append a member line to the same run packet, or a
    retrospective line to the index; both sides are kept. Any other conflict is
    the owner's to resolve and is reported as such.
    """
    rebase = subprocess.run(["git", "rebase", "--quiet", ref], cwd=clone, text=True, capture_output=True)
    while rebase.returncode:
        for path in git(clone, "diff", "--name-only", "--diff-filter=U", strip=False).split():
            ours = git(clone, "show", f":2:{path}", strip=False)    # the target's side
            theirs = git(clone, "show", f":3:{path}", strip=False)  # the replayed control commit
            if path == RETROSPECTIVE_INDEX:
                (clone / path).write_text(union_index(ours, theirs), encoding="utf-8")
            elif path.startswith(ITEMS_DIR + "/") and parse_packet(Path(path).stem, ours).run:
                (clone / path).write_text(members_union(ours, theirs, Path(path).stem), encoding="utf-8")
            else:
                subprocess.run(["git", "rebase", "--abort"], cwd=clone, capture_output=True)
                raise BiqError(f"the control branch {branch} conflicts with {ref} in {path}; resolve it by hand on that branch, push, and rerun the command")
            git(clone, "add", "--", path)
        rebase = subprocess.run(["git", "-c", "core.editor=true", "rebase", "--continue"], cwd=clone, text=True, capture_output=True)


def follow_change_request(root: Path, clone: Path, branch: str, request: str | None, target: str, *, reconcile: bool = True) -> dict:
    """Arm auto-merge and wait until the branch tip is on the target; say exactly where it stands otherwise.

    Returns {"state": landed|pending|conflict|gate-failed|remote-failed, "tip": sha, "pr": url, "detail": str}.
    Nothing is reported as landed until the fetched target contains the tip.
    """
    provider = change_provider(clone)
    ref = remote_ref_of(target)
    tip = git(clone, "rev-parse", branch)
    if request is None:
        cli = "gh" if provider == "github" else "glab"
        detail = f"{cli} is not available: open the gated {'pull request' if provider == 'github' else 'merge request'} from {branch}, then rerun this command to follow it" if not change_cli_available(provider) else f"{cli} could not find or create the change request from {branch}; check its output and rerun"
        return {"state": "pending" if not change_cli_available(provider) else "remote-failed", "tip": tip, "pr": None, "detail": detail}
    deadline = time.monotonic() + LANDING_WAIT_SECONDS
    armed = False
    while True:
        git(root, "fetch", "--quiet", REMOTE, ref.partition("/")[2])
        if is_ancestor(root, tip, ref):
            return {"state": "landed", "tip": tip, "pr": request, "detail": git(root, "rev-parse", ref)}
        view = change_request_view(clone, request, provider)
        if view is None:
            cli = "gh" if provider == "github" else "glab"
            return {"state": "remote-failed", "tip": tip, "pr": request, "detail": f"{cli} could not read the change request; check the network and rerun"}
        if view["state"] == "MERGED":
            continue  # the fetch above will see it on the next pass
        if view["state"] != "OPEN":
            return {"state": "gate-failed", "tip": tip, "pr": request, "detail": f"the change request is {view['state']}; reopen or recreate it from {branch}"}
        if view["merge"] == "DIRTY":
            if not reconcile:
                return {"state": "conflict", "tip": tip, "pr": request, "detail": f"the landing conflicts with {ref}; stage it again on the current target"}
            tip = reconcile_control_branch(clone, branch, target)
            armed = False
            continue
        if any(c in {"FAILURE", "ERROR", "CANCELLED", "TIMED_OUT", "ACTION_REQUIRED"} for c in view["checks"]):
            return {"state": "gate-failed", "tip": tip, "pr": request, "detail": f"the required check refused {tip[:12]}; read the check, fix the branch, and rerun"}
        if not armed or not view["auto"]:
            if not arm_change_request(clone, request, tip, provider):
                if provider == "gitlab" and view.get("merge_status") in {"checking", "unchecked", "preparing", "ci_still_running"} and time.monotonic() < deadline:
                    time.sleep(LANDING_POLL_SECONDS)
                    continue
                return {"state": "remote-failed", "tip": tip, "pr": request, "detail": "the provider CLI could not arm auto-merge; check its output and rerun"}
            armed = True
        if time.monotonic() >= deadline:
            return {"state": "pending", "tip": tip, "pr": request, "detail": f"auto-merge is armed and the check has not concluded within {LANDING_WAIT_SECONDS}s; rerun this command to keep following"}
        time.sleep(LANDING_POLL_SECONDS)


def follow_control_landing(clone: Path, branch: str, target: str, item_id: str, command: str, owner: str) -> dict:
    """Every control write ends here: the branch's change request is followed to the target and its state verified there."""
    request = open_change_request(clone, branch, target, f"queue: {command} {item_id}", f"Control commit written by `biq {command}` for {owner} on the fetched target. Gate: control commit.")
    outcome = follow_change_request(clone, clone, branch, request, target)
    if outcome["state"] != "landed":
        return outcome
    packets = load_packets(clone, remote_ref_of(target))
    packet = packets.get(item_id)
    expected = {"submit": "submitted", "abandon": "failed-abandoned"}.get(command)
    if expected and (packet is None or packet.status != expected):
        return {**outcome, "state": "gate-failed", "detail": f"{branch} is on {target} but {item_id} there is {packet.status if packet else 'absent'}, not {expected}"}
    if command == "submit" and packet is not None:
        missing = [q for q in packet.queues if not any(m[0] == item_id for r in runs_of(packets, q) for m in r.members())]
        if missing:
            return {**outcome, "state": "gate-failed", "detail": f"{item_id} is submitted on {target} but absent from the open run of " + ", ".join(missing)}
    return outcome


def pending_control_branches(root: Path, item_id: str, target: str) -> list[tuple[str, str]]:
    """Remote control branches for the item whose tip is not on the target: (branch, tip)."""
    ref = remote_ref_of(target)
    pending = []
    for line in git(root, "ls-remote", REMOTE, f"refs/heads/{CONTROL_BRANCH_PREFIX}*/{item_id.lower()}", strip=False).splitlines():
        parts = line.split()
        if len(parts) != 2:
            continue
        sha, branch = parts[0], parts[1][len("refs/heads/"):]
        if not have_commit(root, sha):
            subprocess.run(["git", "fetch", "--quiet", REMOTE, sha], cwd=root, capture_output=True)
        if not is_ancestor(root, sha, ref):
            pending.append((branch, sha))
    return pending


def pending_branch_tip(root: Path, branch: str, target: str) -> str | None:
    """Fetch branch and return its tip only while the target does not contain it."""
    tracking = f"refs/remotes/{REMOTE}/{branch}"
    fetched = subprocess.run(["git", "fetch", "--quiet", REMOTE, f"+refs/heads/{branch}:{tracking}"], cwd=root, text=True, capture_output=True)
    if fetched.returncode:
        return None
    tip = rev(root, tracking)
    if is_ancestor(root, tip, remote_ref_of(target)):
        return None
    git(root, "branch", "-f", branch, tip)
    return tip


def write_target_for(args, root: Path, revision: str | None, target: str, packets: dict[str, Packet]) -> tuple[Path, str | None, str, dict[str, str], dict[str, object] | None]:
    """Where a packet-writing command runs: always the agent's control clone, whose branch is followed onto the target.

    A checkout of the target itself gets no special path: writing there would
    leave packets that only a later hand-made commit could land, and the command
    would have to report success before the target held anything.
    """
    owner = getattr(args, "owner", None) or git(root, "config", "user.name") or "agent"
    operation = operation_identity(args, packets)
    item_id = getattr(args, "item_id", None)
    if item_id is None:  # run commands share one serialized control branch per queue
        item_id = f"run-{args.queue_id.lower()}" if args.command == "run" else args.command
    if args.command == "seal":  # the clone fetches the candidate the owner names
        sha = rev(root, args.candidate)
        publish_candidate(root, item_id, sha)
    elif args.command == "run" and args.run_command in {"fix", "resolve"}:
        sha = rev(root, args.commit)
        publish_candidate(root, operation["item"], sha)
    elif args.command == "submit":  # a candidate sealed by hand in this checkout is published the same way
        local = root / ITEMS_DIR / f"{item_id}.md"
        candidate = parse_packet(item_id, local.read_text(encoding="utf-8")).candidate if local.is_file() else None
        if candidate and have_commit(root, candidate):
            publish_candidate(root, item_id, candidate)
    clone = control_clone(root, target, owner)
    branch, pending = stage_control_clone(root, clone, target, owner, item_id, operation,
                                          tuple(filter(None, [getattr(args, "successor", None)])))
    if pending is not None:
        return clone, branch, item_id, operation, pending
    if args.command in {"submit", "abandon"}:
        carry_retrospective(root, clone, item_id, getattr(args, "retrospective", None))
    fetch_candidates(clone)
    return clone, branch, item_id, operation, None


# --- packets ---------------------------------------------------------------

@dataclass
class Packet:
    item_id: str
    source: str
    fields: dict[str, str] = field(default_factory=dict)
    sections: dict[str, str] = field(default_factory=dict)
    header_order: list[str] = field(default_factory=list)
    scalar_counts: dict[str, int] = field(default_factory=dict)
    section_order: list[str] = field(default_factory=list)
    misplaced_fields: set[str] = field(default_factory=set)

    @property
    def status(self) -> str:
        return self.fields.get("Status", "open")

    @property
    def candidate(self) -> str | None:
        value = self.fields.get("Candidate commit", "pending")
        return value if COMMIT_RE.match(value) else None

    def ids(self, name: str) -> tuple[str, ...]:
        value = self.fields.get(name, "none")
        return tuple(v.strip() for v in value.split(",") if ITEM_RE.match(v.strip()))

    @property
    def dependencies(self) -> tuple[str, ...]:
        return tuple(dict.fromkeys(self.ids("Dependencies") + self.ids("Implied dependencies")))

    @property
    def replaces(self) -> str | None:
        return (self.ids("Replaces") or (None,))[0]

    @property
    def queues(self) -> tuple[str, ...]:
        return tuple(QUEUE_RENAMES.get(v.strip(), v.strip()) for v in self.fields.get("Queues", "none").split(",") if v.strip() and v.strip() != "none")

    @property
    def run(self) -> tuple[str, int] | None:
        return run_identity(self.item_id, self.fields.get("Queue", ""))

    def members(self, section: str = "Members") -> list[tuple[str, str, str]]:
        """(item, commit, note) per line of the section."""
        out = []
        for line in self.sections.get(section, "").splitlines():
            match = MEMBER_RE.match(line.strip())
            if match:
                out.append((match.group(1), match.group(2), match.group(3) or ""))
        return out

    @property
    def tests(self) -> dict[str, str]:
        value = self.fields.get("Tests", "pending")
        out: dict[str, str] = {}
        for part in value.split(","):
            name, _sep, result = part.strip().partition("=")
            if name and name != "pending":
                out[name] = result or "pending"
        return out


def priority_hold_sources(item_id: str, packets: dict[str, Packet]) -> tuple[str, ...]:
    """Direct and workstream holds, shared by admission and the report projection.

    Only existing container relationships propagate priority; dependencies and
    integration-run membership never do. Item and Packet expose the same fields.
    """
    item = packets[item_id]
    if item.status in TERMINAL:
        return ()

    def family(packet: Packet) -> str:
        # The report's Item already resolves historical identity/family aliases.
        repeat = getattr(packet, "repeat_family_identity", None)
        if repeat is not None:
            return repeat[0]
        match = REPEAT_RE.fullmatch(packet.item_id)
        if match:
            return match["base"]
        seen = set()
        while packet.item_id not in seen:
            seen.add(packet.item_id)
            previous = packet.fields.get("Previous attempt", "none")
            if previous not in packets:
                break
            packet = packets[previous]
        return getattr(packet, "family_root", packet.item_id)

    def members(packet: Packet) -> tuple[str, ...]:
        resolved = getattr(packet, "container_members", None)
        return resolved if resolved is not None else tuple(
            value.strip() for value in packet.fields.get("Container members", "").split(",")
            if value.strip() and value.strip() not in {"none", "pending"}
        )

    item_family = family(item)
    sources = {item_id} if item.fields.get("Priority", "production") == "on-hold" else set()
    for parent in packets.values():
        if (parent.status in TERMINAL or parent.fields.get("Container mode") != "project"
                or parent.fields.get("Priority", "production") != "on-hold"):
            continue
        for member_id in members(parent):
            member = require_packet(packets, member_id)
            if family(member) == item_family or (
                member.fields.get("Container mode") == "operative"
                and any(family(require_packet(packets, child)) == item_family for child in members(member))
            ):
                sources.add(parent.item_id)
                break
    return tuple(sorted(sources))


def require_production_priority(packets: dict[str, Packet], item_id: str, root: Path) -> None:
    sources = set(priority_hold_sources(item_id, packets))
    # Pending local edits may add a hold, but cannot clear fetched main authority.
    durable = load_packets(root, f"{REMOTE}/main")
    if item_id in durable:
        sources.update(priority_hold_sources(item_id, durable))
    if sources:
        raise BiqError(f"{item_id} is On Hold via {', '.join(sorted(sources))}; move each hold to production before claiming or continuing development")


def markdown_lines(source: str):
    """Yield each line and whether it is outside a Markdown code fence."""
    fence = None
    for line in source.splitlines():
        stripped = line.lstrip(" ")
        indentation = len(line) - len(stripped)
        if fence:
            marker = stripped.rstrip(" \t")
            if indentation <= 3 and len(marker) >= fence[1] and marker == fence[0] * len(marker):
                fence = None
            yield line, False
        else:
            match = re.match(r"^(`{3,}|~{3,})(.*)$", stripped) if indentation <= 3 else None
            if match and not (match[1][0] == "`" and "`" in match[2]):
                fence = (match[1][0], len(match[1]))
                yield line, False
            else:
                yield line, True


def parse_packet(item_id: str, source: str) -> Packet:
    packet = Packet(item_id, source)
    current = None
    for line, outside in markdown_lines(source):
        heading = re.match(r"^ {0,3}##(?:[ \t]+(.*)|[ \t]*)$", line) if outside else None
        if heading:
            current = re.sub(r"[ \t]+#+[ \t]*$", "", (heading[1] or "").strip())
            packet.section_order.append(current)
            packet.sections.setdefault(current, "")
            continue
        raw = re.match(r"^- ([A-Za-z][A-Za-z _]*):", line) if outside else None
        if raw:
            packet.scalar_counts[raw[1]] = packet.scalar_counts.get(raw[1], 0) + 1
            if current is None:
                packet.header_order.append(raw[1])
            else:
                packet.misplaced_fields.add(raw[1])
        match = FIELD_RE.match(line) if outside else None
        if match and current is None and match.group(1) == "Priority":
            if "Priority" in packet.fields or match.group(2) not in {"production", "on-hold"}:
                raise BiqError(f"{item_id}: Priority must appear once and be production or on-hold")
        if match and current is None and match.group(1) not in packet.fields:
            packet.fields[match.group(1)] = match.group(2)
        elif current is not None:
            packet.sections[current] += line + "\n"
    return packet


def load_packets(root: Path, revision: str | None = None) -> dict[str, Packet]:
    packets: dict[str, Packet] = {}
    if revision is None:
        for path in sorted((root / ITEMS_DIR).glob("WI-*.md")):
            packets[path.stem] = parse_packet(path.stem, path.read_text(encoding="utf-8"))
        return packets
    for line in git(root, "ls-tree", "--name-only", revision, "--", f"{ITEMS_DIR}/", strip=False).splitlines():
        name = line.rsplit("/", 1)[-1]
        if name.startswith("WI-") and name.endswith(".md"):
            packets[name[:-3]] = parse_packet(name[:-3], git(root, "show", f"{revision}:{line}", strip=False))
    return packets


def packet_title(packet: Packet) -> str:
    first = packet.source.splitlines()[0] if packet.source else ""
    return first.split(": ", 1)[1].strip() if first.startswith("# ") and ": " in first else ""


def require_packet(packets: dict[str, Packet], item_id: str) -> Packet:
    if item_id not in packets:
        raise BiqError(f"{item_id} has no packet")
    return packets[item_id]


INSERT_BEFORE = {
    "Queues": ("Dependencies",), "Implied dependencies": ("Dependencies",),
    "Next attempt": ("Candidate base", "Candidate commit", "Submitted At", "Submitted owner", "Container mode", "Decision owner"),
    "Candidate base": ("Candidate commit", "Submitted At", "Submitted owner", "Container mode", "Decision owner"),
    "Candidate commit": ("Submitted At", "Submitted owner", "Container mode", "Decision owner"),
    "Submitted At": ("Submitted owner", "Container mode", "Decision owner"),
    "Submitted owner": ("Container mode", "Decision owner"),
    "Decision owner": (),  # last header field: appended when the packet lacks it
}


def set_field(source: str, name: str, value: str) -> str:
    """Set a header field; a field the packet lacks is inserted at its template position."""
    pattern = re.compile(rf"^- {re.escape(name)}: `[^`]*`$", re.M)
    if pattern.search(source):
        return pattern.sub(f"- {name}: `{value}`", source, count=1)
    if name not in INSERT_BEFORE:
        raise BiqError(f"packet has no field {name}")
    for anchor in INSERT_BEFORE[name]:
        match = re.search(rf"^- {re.escape(anchor)}: `[^`]*`$", source, re.M)
        if match is not None:
            return source[: match.start()] + f"- {name}: `{value}`\n" + source[match.start():]
    header = list(re.finditer(r"^- [^:\n]+: `[^`]*`$", source, re.M))
    if not header:
        raise BiqError(f"packet has no header to insert {name} into")
    end = header[-1].end()
    return source[:end] + f"\n- {name}: `{value}`" + source[end:]


def set_section(source: str, name: str, body: str) -> str:
    """Replace the body of `## name` (or append the section), keeping the rest byte for byte."""
    heading = f"## {name}\n"
    start = source.find(heading)
    body = body.rstrip("\n") + "\n"
    if start < 0:
        return source.rstrip("\n") + "\n\n" + heading + "\n" + body
    rest = source[start + len(heading):]
    next_heading = re.search(r"^## ", rest, re.M)
    end = start + len(heading) + (next_heading.start() if next_heading else len(rest))
    return source[:start] + heading + "\n" + body + ("\n" if next_heading else "") + source[end:]


def resolution_with_note(source: str, note: str) -> str:
    """The Resolution body with the note added as a paragraph ahead of any machine evidence lines."""
    body = parse_packet("", source).sections.get("Resolution", "").strip()
    lines = body.splitlines()
    first_evidence = next((i for i, line in enumerate(lines) if line.startswith("Work-queue ")), len(lines))
    prose = "\n".join(lines[:first_evidence]).strip()
    evidence = "\n".join(lines[first_evidence:]).strip()
    prose = note if prose in {"", "pending", "Open."} else prose.rstrip() + "\n\n" + note
    return prose + ("\n\n" + evidence if evidence else "")


def write_packet(root: Path, packets: dict[str, Packet], item_id: str, fields: list[tuple[str, str]] = (), *, sections: dict[str, str] | None = None, resolution: str | None = None) -> Packet:
    path = root / ITEMS_DIR / f"{item_id}.md"
    source = path.read_text(encoding="utf-8")
    for name, value in fields:
        source = set_field(source, name, value)
    for name, body in (sections or {}).items():
        source = set_section(source, name, body)
    if resolution is not None:
        source = set_section(source, "Resolution", resolution_with_note(source, resolution))
    path.write_text(source, encoding="utf-8")
    packets[item_id] = parse_packet(item_id, source)
    return packets[item_id]


# --- claims ----------------------------------------------------------------

def claim_ref(item_id: str) -> str:
    return CLAIM_REF_PREFIX + item_id


def read_claim(root: Path, item_id: str) -> tuple[str | None, dict | None]:
    """The remote claim ref's OID and its claim.json, or (None, None) when unclaimed."""
    out = git(root, "ls-remote", REMOTE, claim_ref(item_id))
    if not out:
        return None, None
    oid = out.split()[0]
    git(root, "fetch", "--quiet", REMOTE, claim_ref(item_id))
    try:
        data = json.loads(git(root, "show", f"{oid}:claim.json", strip=False))
    except (BiqError, json.JSONDecodeError):
        data = {}
    return oid, data if isinstance(data, dict) else {}


def push_claim(root: Path, item_id: str, data: dict | None, expected_oid: str | None) -> str | None:
    """Create, replace, or delete the claim ref with a compare-and-swap on the expected OID."""
    ref = claim_ref(item_id)
    lease = f"--force-with-lease={ref}:{expected_oid or ''}"
    if data is None:
        git(root, "push", "--quiet", lease, REMOTE, f":{ref}")
        return None
    payload = json.dumps(data, sort_keys=True, indent=2) + "\n"
    blob = subprocess.run(["git", "hash-object", "-w", "--stdin"], cwd=root, input=payload, text=True, capture_output=True, check=True).stdout.strip()
    tree = subprocess.run(["git", "mktree"], cwd=root, input=f"100644 blob {blob}\tclaim.json\n", text=True, capture_output=True, check=True).stdout.strip()
    parents = ["-p", expected_oid] if expected_oid else []
    commit = subprocess.run(["git", "commit-tree", tree, *parents, "-m", f"claim {item_id}"], cwd=root, text=True, capture_output=True, check=True).stdout.strip()
    git(root, "push", "--quiet", lease, REMOTE, f"{commit}:{ref}")
    return commit


def durable_packet(root: Path, item_id: str) -> tuple[str | None, Packet | None]:
    """The packet as main holds it, from origin/main or a local main: (ref, packet) or (None, None)."""
    for ref in ("origin/main", "main"):
        try:
            source = git(root, "show", f"{ref}:{ITEMS_DIR}/{item_id}.md", strip=False)
        except BiqError:
            continue
        return ref, parse_packet(item_id, source)
    return None, None


def claim_item(root: Path, packets: dict[str, Packet], item_id: str, owner: str, *, note: str = "", usage_snapshot: str | None = None, authorized_by: str | None = None, repair_candidate_base: bool = False) -> dict:
    packet = require_packet(packets, item_id)
    if not owner or "\n" in owner:
        raise BiqError("--owner must be one owner id")
    if repair_candidate_base and (note or usage_snapshot is not None or authorized_by is not None):
        raise BiqError("--repair-candidate-base cannot be combined with claim refresh options")
    require_production_priority(packets, item_id, root)
    durable_ref, durable = durable_packet(root, item_id)
    for source in (packet, durable):
        if source is not None and "Required reading" in source.sections:
            packet_reading(source)
    oid, existing = read_claim(root, item_id)
    authorized_by = authorized_by or (existing or {}).get("authorized_by")
    if packets[item_id].run and not authorized_by:
        raise BiqError(f"{item_id} is a batch integration run WQE: agents have no authority to claim or run it; a human assigns the integrator, and the claim records that human with --authorized-by \"<name>\"")
    if existing is not None and existing.get("owner") != owner:
        raise BiqError(f"{item_id} is claimed by {existing.get('owner')!r}")
    if existing is None and not packet.run:  # dependencies gate the start: a first claim waits for every dependency to be submitted
        unstarted = [f"{dep} is {state}" for dep, state, startable, _action in dependency_states(root, packets, item_id, rev(root, durable_ref) if durable_ref else None) if not startable]
        if unstarted:
            raise BiqError(f"{item_id} cannot start: " + "; ".join(unstarted) + "; a WQE starts only after every dependency is submitted (a submitted one is merged from its candidate ref, never waited on)")
    try:
        branch = git(root, "symbolic-ref", "--short", "-q", "HEAD") or "detached"
    except BiqError:
        branch = "detached"
    # The claim names the packet as main holds it: the owner's branch does not track
    # main, so its copy may lag a rename or edit landed as a control commit.
    identity_packet = durable or packet
    head = git(root, "rev-parse", "HEAD")
    if repair_candidate_base:
        if existing is None:
            raise BiqError(f"{item_id} has no existing claim to repair")
        if durable is None or durable.status != "open" or durable.run or packet.status != "open" or packet.run or existing.get("ready"):
            raise BiqError("candidate-base repair requires an open ordinary non-ready claim and durable packet")
        if (existing.get("head"), existing.get("branch"), existing.get("worktree")) != (head, branch, str(Path(root).resolve())):
            raise BiqError("candidate-base repair must run from the unchanged claim HEAD, branch, and worktree; refresh separately first")
        baseline = re.fullmatch(r"(?:origin/main )?([0-9a-f]{40})", durable.fields.get("Baseline", ""))
        if baseline is None:
            raise BiqError("candidate-base repair requires an exact SHA in the durable packet Baseline")
        base = baseline.group(1)
        ensure_commit(root, base)
        if not is_ancestor(root, base, head):
            raise BiqError("durable packet Baseline is not an ancestor of the unchanged claim HEAD")
        data = dict(existing)
        data.update(candidate_base=base, updated_at=now_utc())
        new_oid = push_claim(root, item_id, data, oid)
        return dict(data, claim_oid=new_oid)
    human, user, host = packet_identity(root)
    data = dict(existing or {})  # a refresh keeps every field it does not own (usage_baseline, ...)
    data.pop("claim_oid", None)
    data.setdefault("candidate_base", head)  # the base the owner branches from; kept across refreshes
    if authorized_by:
        data["authorized_by"] = authorized_by  # the human who assigned this claimant the batch integrator role
    if usage_snapshot is not None:  # the agent's usage counters at claim start; validated here, because the report reads every live claim
        data["usage_baseline"] = load_usage_snapshot_file(usage_snapshot, f"--usage-snapshot {usage_snapshot}")
    data.update({
        "schema": 1, "item": item_id, "title": packet_title(identity_packet) or item_id, "kind": identity_packet.fields.get("Kind", "integration"),
        "item_path": f"{ITEMS_DIR}/{item_id}.md", "item_identity": git(root, "ls-tree", durable_ref or "HEAD", "--", f"{ITEMS_DIR}/{item_id}.md") or "untracked",
        "owner": owner, "human_owner": human, "host_user": user, "hostname": host,
        "head": head, "branch": branch,
        "worktree": str(Path(root).resolve()), "started_at": (existing or {}).get("started_at") or now_utc(),
        "updated_at": now_utc(), "note": note or (existing or {}).get("note", ""),
    })
    data["claim_oid"] = push_claim(root, item_id, data, oid)
    successor = materialize_repeat_successor(root, packets, item_id)
    if successor:
        data["successor"] = successor
    return data


REPEAT_RE = re.compile(r"^(?P<base>WI-[A-Z0-9-]+?)-REPEAT-(?P<ordinal>[1-9][0-9]*)$")


def materialize_repeat_successor(root: Path, packets: dict[str, Packet], item_id: str) -> str | None:
    """The first claim of an open ordinary repeat tail writes -REPEAT-(N+1) into the checkout; the claimant lands it as a control commit."""
    packet = packets[item_id]
    if "Required reading" in packet.sections:
        packet_reading(packet)
    m = REPEAT_RE.match(item_id)
    if packet.run or m is None or packet.status != "open":
        return None
    successor = f"{m.group('base')}-REPEAT-{int(m.group('ordinal')) + 1}"
    if successor in packets or (root / ITEMS_DIR / f"{successor}.md").exists():
        return None
    human, user, host = packet_identity(root)
    text = packet.source.replace(item_id, successor)
    resets = [("Status", "open"), ("Created", now_utc()), ("Created human_owner", human), ("Created host_user", user), ("Created hostname", host),
              ("Completed human_owner", "pending"), ("Completed host_user", "pending"), ("Completed hostname", "pending"), ("Claimed At", "pending"), ("Closed At", "pending"),
              ("Implementation Duration", "pending"), ("Tokens Used", "pending"), ("Model Used", "pending"), ("Estimated Cost", "pending"), ("Estimated AWS Cost", "pending"),
              ("Candidate base", "pending"), ("Candidate commit", "pending"), ("Submitted At", "pending"), ("Submitted owner", "pending"), ("Queues", "none"), ("Implied dependencies", "none"),
              ("Previous attempt", "none"), ("Next attempt", "none")]
    for name, value in resets:
        if re.search(rf"^- {re.escape(name)}: `[^`]*`$", text, re.M):
            text = set_field(text, name, value)
    try:
        tip = rev(root, f"{REMOTE}/main")
        text = set_field(text, "Baseline", f"origin/main {tip}") if re.search(r"^- Baseline: `", text, re.M) else text
    except BiqError:
        pass
    text = set_section(text, "Attempt history", "None.")
    text = set_section(text, "Resolution", "pending")
    (root / ITEMS_DIR / f"{successor}.md").write_text(text, encoding="utf-8")
    packets[successor] = parse_packet(successor, text)
    return successor


def release_item(root: Path, item_id: str, owner: str) -> None:
    oid, existing = read_claim(root, item_id)
    if existing is None:
        raise BiqError(f"{item_id} is not claimed")
    if existing.get("owner") != owner:
        raise BiqError(f"{item_id} is claimed by {existing.get('owner')!r}, not {owner!r}")
    push_claim(root, item_id, None, oid)


def release_stale_claims(root: Path, packets: dict[str, Packet]) -> list[str]:
    """A claim on a packet the target shows as terminal is over: the next command anyone runs releases it.

    `land` completes the packet on the target, and the landing merge is observed
    later, by someone; an owner who forgets `release` after that would leave the
    report showing work in progress. Releasing here needs no discipline.
    """
    released = []
    for line in git(root, "ls-remote", REMOTE, CLAIM_REF_PREFIX + "*", strip=False).splitlines():
        parts = line.split()
        if len(parts) != 2:
            continue
        oid, item_id = parts[0], parts[1][len(CLAIM_REF_PREFIX):]
        packet = packets.get(item_id)
        if packet is None or packet.status not in TERMINAL:
            continue
        try:
            push_claim(root, item_id, None, oid)
        except BiqError:
            continue  # someone else released it first
        released.append(item_id)
    return released


def require_claim(root: Path, item_id: str, owner: str | None) -> dict:
    if not owner:
        raise BiqError("--owner is required: the claimant of " + item_id)
    _oid, existing = read_claim(root, item_id)
    if existing is None or existing.get("owner") != owner:
        raise BiqError(f"{item_id} must be claimed by {owner!r} first")
    packet = parse_packet(item_id, (root / ITEMS_DIR / f"{item_id}.md").read_text())
    if packet.run and not existing.get("authorized_by"):
        raise BiqError(f"{item_id} is a batch integration run WQE; its claim must name the human who assigned the integrator (claim --authorized-by)")
    return existing


def claim_snapshot(root: Path) -> tuple[str, str]:
    """Identity and digest of the whole claim namespace, for provenance records."""
    listing = git(root, "ls-remote", REMOTE, CLAIM_REF_PREFIX + "*", strip=False)
    rows = sorted(line for line in listing.splitlines() if line.strip())
    return f"{REMOTE} {CLAIM_REF_PREFIX}*", hashlib.sha256("\n".join(rows).encode()).hexdigest()


# --- run WQEs --------------------------------------------------------------

def run_item_id(queue_id: str, ordinal: int) -> str:
    return f"WI-CI-INTEGRATION-{QUEUE_RENAMES.get(queue_id, queue_id)}-REPEAT-{ordinal}"


def runs_of(packets: dict[str, Packet], queue_id: str) -> list[Packet]:
    return sorted((p for p in packets.values() if p.run and p.run[0] == queue_id), key=lambda p: p.run[1])


def run_state(packet: Packet) -> str:
    """open (collecting), started (frozen and under test), submitted, or terminal."""
    if packet.status == "open" and packet.fields.get("Started At", "pending") != "pending":
        return "started"
    return packet.status


def open_run(packets: dict[str, Packet], queue_id: str) -> Packet:
    candidates = [p for p in runs_of(packets, queue_id) if run_state(p) == "open"]
    if len(candidates) != 1:
        raise BiqError(f"{queue_id} has {len(candidates)} open run WQEs; run materialize --apply")
    return candidates[0]


def in_flight_runs(packets: dict[str, Packet], queue_id: str) -> list[Packet]:
    return [p for p in runs_of(packets, queue_id) if run_state(p) in {"started", "submitted"}]


def packet_identity(root: Path) -> tuple[str, str, str]:
    try:
        human = git(root, "config", "user.name")
    except BiqError:
        human = ""
    user = getpass.getuser()
    return human or user, user, socket.gethostname()


def run_packet_text(root: Path, registry: Registry, queue: Queue, ordinal: int, target_tip: str, members: list[tuple[str, str, str]] | None = None) -> str:
    """The run WQE of a queue, in the ordinary task packet shape; the queue fields sit before Dependencies."""
    item_id = run_item_id(queue.id, ordinal)
    human, user, host = packet_identity(root)
    outcome = f"pass the result to {queue.feeds}" if queue.feeds else f"land the result on `{registry.target}`"
    return "\n".join([
        f"# {item_id}: {queue.id} batch integration run {ordinal}", "",
        f"- ID: `{item_id}`", "- Status: `open`", "- Kind: `integration`", "- Level of effort: `medium`",
        f"- Area: `work-queue batch integration: {queue.id} run`", f"- Created: `{now_utc()}`",
        f"- Created human_owner: `{human}`", f"- Created host_user: `{user}`", f"- Created hostname: `{host}`",
        "- Completed human_owner: `pending`", "- Completed host_user: `pending`", "- Completed hostname: `pending`",
        "- Claimed At: `pending`", "- Closed At: `pending`", "- Implementation Duration: `pending`",
        "- Tokens Used: `pending`", "- Model Used: `pending`", "- Estimated Cost: `pending`", "- Estimated AWS Cost: `pending`",
        f"- Baseline: `origin/{registry.target} {target_tip}`", "- Task packet: `3`", f"- Accountable owner: `{human}`",
        "- Architecture gate: `not-required`", "- Architecture decision: `none`", "- Architecture decision owner: `none`",
        "- Documentation impact: `none`",
        "- Documentation impact rationale: `Generated batch integration packet; tools/biq.py help owns the run commands.`",
        f"- Queue: `{queue.id}`", f"- Queue kind: `{queue.kind}`", f"- Feeds: `{queue.feeds or 'none'}`", f"- Feeders: `{', '.join(queue.feeders) or 'none'}`",
        f"- Qualification: `{queue.qualification or 'none'}`",
        "- Started At: `pending`", "- Base: `pending`", "- Predecessor: `none`",
        "- Integration commit: `pending`", "- Result commit: `pending`", "- Tests: `pending`", "- Cost: `$0`", f"- Budget: `{usd(queue.budget_usd)}`",
        "- Dependencies: `none`", "- Previous attempt: `none`", "- Next attempt: `none`",
        "- Candidate base: `pending`", "- Candidate commit: `pending`", "- Submitted At: `pending`", "- Submitted owner: `pending`",
        "- Decision owner: `none`", "",
        "## Objective", "",
        f"Review every member below, then integrate the reviewed members as one batch on `{registry.target}`, run {', '.join(queue.tests) or 'no tests'}" + (f" as defined by `{queue.qualification}`" if queue.qualification else "") + f" within the budget, and on green {outcome}. AWS campaign spend ceiling: `{usd(queue.budget_usd)}`.", "",
        "The review is the integrator's own reading of each member's candidate against its merge base (`run review` prints the footprint, every deleted path, and anything the seal guard would refuse): the change must be the WQE's own work and nothing else. A member that drops or reverts earlier changes, deletes unrelated files or packets, touches other WQEs' agent-work, or is otherwise not sane is not integrated. When the integrator is not sure, `--hold <question>` returns the member to the open run and names the accountable human as its Decision owner; when it is wrong, `--reject <reason>` closes it failed-rejected and sheds its dependents from the run. Nothing destructive goes through on the strength of a green test.", "",
        "## Reason", "", f"Every candidate {queue.id} cares about waits here until its dependencies are met; this is the next batch integration of the queue.", "",
        "## Allowed source/build scope", "", "- the integration branch of this run;", "- this WQE packet and its retained evidence.", "",
        "## Existing owner and required reuse", "", "`tools/biq.py` owns runs; this run WQE's `Cares about`, `Queue tests`, and `Budget` define the queue, and `run start` copies them into the successor.", "",
        "## Non-goals", "", "- No change to any member candidate.", "- No integration, test, or landing of a member the integrator has not reviewed.", "",
        "## Required reading", "", "- governing-authority: `AGENTS.md#start-every-task`", "- verification: `tools/test_biq.py`", "",
        "## Affected contracts and invariants", "", "A member whose merge collides stays in the current run awaiting resolution, including after a rebuild. No member is tested or integrated unreviewed; a rejected member's dependents leave the run with it; a held member waits in the open run for the human named as its Decision owner.", "",
        "## Risks and constraints", "", f"Queue budget `{usd(queue.budget_usd)}` per run.", "",
        "## Implementation outline", "", f"1. `python3 tools/biq.py run start {queue.id} --owner <you> --apply`", f"2. `run review {queue.id} {ordinal}`, read each member's candidate, then per member `run review {queue.id} {ordinal} <ID> --ok | --reject <reason> | --hold <question> --apply`", f"3. Fix any integration or landing-gate defect on a descendant commit with `run fix {queue.id} {ordinal} --commit <sha> --owner <you> --apply`; this resets every test.", f"4. `run test {queue.id} {ordinal} <test> --result green|red --evidence <path> --cost <usd>` per test", f"5. `run complete {queue.id} {ordinal} [--reject <ID>] --apply`", "",
        "## Acceptance and completion evidence", "", "- Every member reviewed ok by the integrator, or rejected or held with its reason in Excluded.", "- Every test recorded green and the result passed on or landed.", "",
        "## Residual risks and unresolved decisions", "", "None.", "",
        "## Attempt history", "", "None.", "",
        "## Cares about", "", cares_lines(queue), "",
        "## Queue tests", "", queue_test_lines(registry, queue), "",
        "## Members", "", member_lines(members or []), "",
        "## Excluded", "", "none", "",
        "## Resolution", "", "pending", "",
    ])


def materialize(root: Path, registry: Registry, packets: dict[str, Packet], *, apply: bool, target_tip: str | None = None) -> list[str]:
    """Every queue has exactly one open run WQE, and every open run carries its members' dependencies."""
    created = []
    for queue in sorted(registry.queues.values(), key=lambda q: q.id):
        if any(run_state(p) == "open" for p in runs_of(packets, queue.id)):
            continue
        ordinal = 1 + max((p.run[1] for p in runs_of(packets, queue.id)), default=0)
        item_id = run_item_id(queue.id, ordinal)
        created.append(item_id)
        if apply:
            text = run_packet_text(root, registry, queue, ordinal, target_tip or "pending")
            (root / ITEMS_DIR / f"{item_id}.md").write_text(text, encoding="utf-8")
            packets[item_id] = parse_packet(item_id, text)
    if target_tip is not None:
        created.extend(complete_landed_leaves(root, registry, packets, target_tip, apply=apply))
    if apply and target_tip is not None:
        created.extend(propagate_dependencies(root, packets, target_tip))
    if apply:
        created.extend(ensure_workstream_membership(root, registry, packets))
    return created


def complete_landed_leaves(root: Path, registry: Registry, packets: dict[str, Packet], target_tip: str, *, apply: bool) -> list[str]:
    """A submitted leaf whose candidate is already on the target is completed; the previous queue landed it without saying so."""
    done = []
    human, user, host = packet_identity(root)
    for packet in sorted(packets.values(), key=lambda p: p.item_id):
        if packet.run or packet.status != "submitted" or packet.candidate is None or not is_ancestor(root, packet.candidate, target_tip):
            continue
        done.append(f"{packet.item_id}: completed (candidate {packet.candidate[:12]} is on {registry.target})")
        if apply:
            write_packet(root, packets, packet.item_id, [("Status", "completed"), ("Closed At", now_utc()), ("Completed human_owner", human), ("Completed host_user", user), ("Completed hostname", host), ("Queues", "none")],
                         resolution=f"Landed on `{registry.target}` before the batch integration queue reconciled it; completed by `materialize`.")
    return done


def ensure_workstream_membership(root: Path, registry: Registry, packets: dict[str, Packet]) -> list[str]:
    """The registry's workstream lists one run WQE per queue; every run of that queue is then its family."""
    if registry.workstream is None or registry.workstream not in packets:
        return []
    workstream = packets[registry.workstream]
    members = [m.strip() for m in workstream.fields.get("Container members", "none").split(",") if m.strip() and m.strip() != "none"]
    added = []
    for queue_id in sorted(registry.queues):
        if any(m in packets and packets[m].run and packets[m].run[0] == queue_id for m in members):
            continue
        runs = runs_of(packets, queue_id)
        if not runs:
            continue
        members.append(runs[0].item_id)
        added.append(f"{runs[0].item_id} -> {registry.workstream}")
    if added:
        write_packet(root, packets, registry.workstream, [("Container members", ", ".join(sorted(members)))])
    return added


# --- workstreams -----------------------------------------------------------------
# A workstream is an open packet with `Container mode: project`; membership is recorded only in its
# `Container members`. Every ordinary WQE created from WORKSTREAM_SINCE joins one at creation.

WORKSTREAM_SINCE = "2026-09-09T00:00:00Z"
GENERIC_TOKENS = frozenset({"WI", "CI", "PRODUCT", "PROPOSED", "ARCHITECTURE", "AUDIT", "BUG", "BUILD", "DOCUMENTATION", "FEATURE",
                            "INFRASTRUCTURE", "INTEGRATION", "PERFORMANCE", "QUALIFICATION", "REFACTOR", "REPORTING", "TEST", "TOOLING",
                            "TECHNICAL", "DEBT", "WORKSTREAM", "PROJECT", "REPEAT", "ATTEMPT"})


def container_members(packet: Packet) -> list[str]:
    return [m.strip() for m in packet.fields.get("Container members", "none").split(",") if m.strip() and m.strip() != "none"]


def is_workstream(packet: Packet) -> bool:
    return packet.fields.get("Container mode") == "project"


def family_root(item_id: str) -> str:
    """One listed ordinal associates the whole repeat family."""
    return re.sub(r"-REPEAT-\d+$", "", item_id)


def workstream_of(packets: dict[str, Packet], item_id: str) -> str | None:
    """The open workstream that lists the item or its repeat family directly.

    Container relationships are flat and carry no transitive meaning: an item held
    by an operative container that a workstream lists is not thereby a member of
    that workstream, which is also how the report projects membership.
    """
    root_id = family_root(item_id)
    for ws in sorted(packets.values(), key=lambda p: p.item_id):
        if is_workstream(ws) and ws.status not in TERMINAL and ws.item_id != item_id \
                and any(m == item_id or family_root(m) == root_id for m in container_members(ws)):
            return ws.item_id
    return None


def requires_workstream(packet: Packet) -> bool:
    """Ordinary packets created from WORKSTREAM_SINCE; runs, containers, and terminal packets are exempt."""
    return (not packet.run and not packet.fields.get("Container mode") and packet.status not in TERMINAL
            and packet.fields.get("Created", "") >= WORKSTREAM_SINCE)


def id_tokens(item_id: str) -> set[str]:
    return {t for t in item_id.split("-") if t and not t.isdigit() and t not in GENERIC_TOKENS}


def scope_paths(packet: Packet) -> set[str]:
    out = set()
    for line in packet.sections.get("Allowed source/build scope", "").splitlines():
        match = re.match(r"^- `([^`]+)`", line.strip())
        if match:
            out.add(match.group(1).rstrip("/"))
    return out


def suggest_workstreams(packets: dict[str, Packet], item_id: str) -> list[tuple[int, str, list[str]]]:
    """Open workstreams ranked for the item: related packets' workstreams, then shared scope paths and ID tokens."""
    packet = require_packet(packets, item_id)
    related = set(packet.dependencies) | {packet.replaces or ""} | {packet.fields.get("Previous attempt", "none")}
    related |= {re.sub(r"-ATTEMPT-\d+$", "", item_id)}
    match = re.search(r'"producer":"([^"]+)"', packet.sections.get("Resolution", ""))
    if match:
        related.add(match.group(1))
    related.discard(item_id); related.discard(""); related.discard("none")
    tokens, scope = id_tokens(item_id), scope_paths(packet)
    ranked = []
    for ws in sorted(packets.values(), key=lambda p: p.item_id):
        if not is_workstream(ws) or ws.status in TERMINAL:
            continue
        members = container_members(ws)
        score, reasons = 0, []
        kin = sorted(r for r in related if workstream_of(packets, r) == ws.item_id)
        if kin:
            score += 100 * len(kin); reasons.append("related: " + ", ".join(kin))
        member_scope = set().union(*(scope_paths(packets[m]) for m in members if m in packets))
        shared_paths = sorted(a for a in scope for b in member_scope if a == b or a.startswith(b + "/") or b.startswith(a + "/"))
        if shared_paths:
            score += 20 * len(set(shared_paths)); reasons.append("scope: " + ", ".join(sorted(set(shared_paths))[:3]))
        pool = id_tokens(ws.item_id) | set().union(*(id_tokens(m) for m in members))
        shared_tokens = sorted(tokens & id_tokens(ws.item_id))
        member_tokens = sorted((tokens & pool) - set(shared_tokens))
        score += 10 * len(shared_tokens) + 3 * len(member_tokens)
        if shared_tokens or member_tokens:
            reasons.append("tokens: " + ", ".join(shared_tokens + member_tokens))
        if score:
            ranked.append((score, ws.item_id, reasons))
    return sorted(ranked, key=lambda r: (-r[0], r[1]))


def join_workstream(root: Path, packets: dict[str, Packet], workstream_id: str, item_id: str) -> Packet:
    """List the item in the workstream's Container members; refused for a second workstream or a non-workstream."""
    packet = require_packet(packets, item_id)
    workstream = require_packet(packets, workstream_id)
    if not is_workstream(workstream) or workstream.status in TERMINAL:
        raise BiqError(f"{workstream_id} is not an open workstream (Container mode: project)")
    if packet.run or packet.fields.get("Container mode") == "project":
        raise BiqError(f"{item_id} is a run or workstream; runs join through their queue, workstreams do not nest")
    current = workstream_of(packets, item_id)
    if current == workstream_id:
        return workstream
    if current is not None:
        raise BiqError(f"{item_id} already belongs to {current}; a WQE belongs to at most one workstream")
    members = sorted(set(container_members(workstream)) | {item_id})
    return write_packet(root, packets, workstream_id, [("Container members", ", ".join(members))])


def workstream_lines(packets: dict[str, Packet], item_id: str) -> list[str]:
    current = workstream_of(packets, item_id)
    if current is not None:
        return [f"{item_id}: workstream {current}"]
    ranked = suggest_workstreams(packets, item_id)
    lines = [f"{item_id}: no workstream" + ("" if ranked else "; no open workstream relates to it: ask the Accountable owner, or propose a new `Container mode: project` WQE")]
    for score, ws, reasons in ranked[:5]:
        lines.append(f"  {ws} ({score}: " + "; ".join(reasons) + ")")
    if ranked:
        lines.append(f"join with: python3 tools/biq.py workstream {item_id} --join {ranked[0][1]} --owner <you> --apply")
    return lines


# --- retrospectives ------------------------------------------------------------------
# Every ordinary WQE created from WORKSTREAM_SINCE publishes its retrospective before submission:
# a dated file under docs/work-queue/retrospectives, listed in that directory's README, committed
# in the checkout that submits. The submission records its path, content digest, and commit; the
# landing gate proves that commit is an ancestor of the landing and still holds that content.

RETROSPECTIVES_DIR = "docs/work-queue/retrospectives"
RETROSPECTIVE_INDEX = f"{RETROSPECTIVES_DIR}/README.md"


def union_index(ours: str, theirs: str) -> str:
    """The retrospective index is an append-only list: a landing keeps every line of both sides, ours first."""
    lines = ours.splitlines()
    seen = set(lines)
    lines += [line for line in theirs.splitlines() if line not in seen and not seen.add(line)]
    return "\n".join(lines).rstrip("\n") + "\n"


def index_only_conflict(root: Path, base: str, subject: str) -> bool:
    """True when merging the subject onto the base conflicts in the retrospective index and nowhere else."""
    merged = subprocess.run(["git", "merge-tree", "--write-tree", "--name-only", base, subject], cwd=root, text=True, capture_output=True)
    if merged.returncode != 1:
        return False
    names = [line for line in merged.stdout.split("\n\n", 1)[0].splitlines()[1:] if line.strip()]
    return names == [RETROSPECTIVE_INDEX]
RETROSPECTIVE_NAME_RE = re.compile(r"^\d{4}-\d{2}-[a-z0-9][a-z0-9-]*\.md$")
RETROSPECTIVE_REFERENCE_RE = re.compile(
    r"^Retrospective: `(?P<path>docs/work-queue/retrospectives/\d{4}-\d{2}-[a-z0-9][a-z0-9-]*\.md)` "
    r"\(sha256 `(?P<digest>[0-9a-f]{64})`\) landed on protected origin/main at `(?P<commit>[0-9a-f]{40})`\.$", re.M)


def requires_retrospective(packet: Packet) -> bool:
    return requires_workstream(packet)


def retrospective_reference(packet: Packet) -> dict | None:
    match = RETROSPECTIVE_REFERENCE_RE.search(packet.sections.get("Resolution", ""))
    return match.groupdict() if match else None


def find_retrospective(root: Path, item_id: str, path: str | None = None) -> str:
    """The item's retrospective: the given path, or the one file under the directory whose `- WQE:` line names the item."""
    directory = root / RETROSPECTIVES_DIR
    if path is not None:
        rel = Path(path).as_posix()
        if not rel.startswith(RETROSPECTIVES_DIR + "/") or RETROSPECTIVE_NAME_RE.match(rel.rsplit("/", 1)[1]) is None:
            raise BiqError(f"{path} is not a dated retrospective under {RETROSPECTIVES_DIR}")
        if not (root / rel).is_file():
            raise BiqError(f"{rel} does not exist")
        return rel
    hits = []
    for candidate in sorted(directory.glob("*.md")) if directory.is_dir() else []:
        if RETROSPECTIVE_NAME_RE.match(candidate.name) is None:
            continue
        if re.search(rf"^- WQE:.*`{re.escape(item_id)}`", candidate.read_text(encoding="utf-8"), re.M):
            hits.append(f"{RETROSPECTIVES_DIR}/{candidate.name}")
    if len(hits) != 1:
        raise BiqError(f"{item_id} has {'no' if not hits else len(hits)} retrospective naming it on a `- WQE:` line under {RETROSPECTIVES_DIR}"
                       + ("; start one from TEMPLATE.md, list it in README.md, and commit both before submitting" if not hits else "; name one with --retrospective"))
    return hits[0]


def retrospective_evidence(root: Path, path: str) -> dict:
    """Digest, index membership, and the committed identity of a retrospective in this checkout."""
    text = (root / path).read_bytes()
    digest = hashlib.sha256(text).hexdigest()
    index = (root / RETROSPECTIVE_INDEX).read_text(encoding="utf-8") if (root / RETROSPECTIVE_INDEX).is_file() else ""
    if f"]({path.rsplit('/', 1)[1]})" not in index:
        raise BiqError(f"{path} is not listed in {RETROSPECTIVE_INDEX}")
    if git(root, "status", "--porcelain", "--", path, RETROSPECTIVE_INDEX):
        raise BiqError(f"{path} or its index line is not committed; commit them before submitting")
    commit = git(root, "log", "-n", "1", "--format=%H", "HEAD", "--", path)
    if not COMMIT_RE.match(commit):
        raise BiqError(f"{path} is not committed on this branch")
    if hashlib.sha256(git(root, "show", f"{commit}:{path}", strip=False).encode("utf-8")).hexdigest() != digest:
        raise BiqError(f"{path} differs from its committed content at {commit[:12]}")
    return {"path": path, "digest": digest, "commit": commit}


def retrospective_line(evidence: dict) -> str:
    return f"Retrospective: `{evidence['path']}` (sha256 `{evidence['digest']}`) landed on protected origin/main at `{evidence['commit']}`."


def verify_retrospective_at(root: Path, packet: Packet, head: str) -> None:
    """The gate: the recorded retrospective commit is on the landing and still carries the recorded content and index line."""
    reference = retrospective_reference(packet)
    if reference is None:
        raise BiqError(f"{packet.item_id}: submission records no retrospective")
    if not is_ancestor(root, reference["commit"], head):
        raise BiqError(f"{packet.item_id}: retrospective commit {reference['commit'][:12]} is not on the landing; it was rebased or squashed, submit again")
    for revision in (reference["commit"], head):
        shown = subprocess.run(["git", "show", f"{revision}:{reference['path']}"], cwd=root, capture_output=True)
        if shown.returncode or hashlib.sha256(shown.stdout).hexdigest() != reference["digest"]:
            raise BiqError(f"{packet.item_id}: {reference['path']} at {revision[:12]} differs from the recorded retrospective")
    index = subprocess.run(["git", "show", f"{head}:{RETROSPECTIVE_INDEX}"], cwd=root, capture_output=True, text=True)
    if index.returncode or f"]({reference['path'].rsplit('/', 1)[1]})" not in index.stdout:
        raise BiqError(f"{packet.item_id}: {reference['path']} is not listed in {RETROSPECTIVE_INDEX} at the landing")


def cares_lines(queue: Queue) -> str:
    lines = [f"- file: {p}" for p in sorted(queue.files)] + [f"- prefix: {p}" for p in queue.prefixes] + [f"- pattern: {p}" for p in queue.patterns]
    return "\n".join(lines) or "none"


def queue_test_lines(registry: Registry, queue: Queue) -> str:
    lines = []
    for name in queue.tests:
        meta = registry.tests.get(name) or {"command": registry.commands.get(name, "no command registered"), "target": "local", "cost": "free"}
        lines.append(f"- {name}: {meta['command']} ({meta['target']}, {meta['cost']})")
    return "\n".join(lines) or "none"


def member_lines(members: list[tuple[str, str, str]]) -> str:
    return "\n".join(f"- {item} @{sha}" + (f" ({note})" if note else "") for item, sha, note in members) or "none"


def seconds_between(start: str, end: str) -> int:
    parse = lambda s: dt.datetime.fromisoformat(str(s).replace("Z", "+00:00"))
    return max(0, int((parse(end) - parse(start)).total_seconds()))


def usd(amount: float) -> str:
    text = f"{float(amount):.6f}".rstrip("0").rstrip(".")
    return "$" + (text if text not in {"", "-0"} else "0")


def parse_usd(value: str) -> float:
    try:
        return float(value.strip().lstrip("$"))
    except ValueError as error:
        raise BiqError(f"malformed amount {value!r}") from error


# --- submission ------------------------------------------------------------

def seal_item(root: Path, packets: dict[str, Packet], item_id: str, candidate: str, target_tip: str, owner: str) -> Packet:
    """Record the candidate commit and its base in the packet.

    Resealing a submitted packet keeps it submitted: its entry in every open
    run WQE takes the new commit, and its caring queues are recomputed.
    """
    packet = require_packet(packets, item_id)
    require_production_priority(packets, item_id, root)
    require_claim(root, item_id, owner)
    candidate = rev(root, candidate)
    base = git(root, "merge-base", target_tip, candidate)
    check_candidate_history(root, item_id, base, candidate, packets)
    publish_candidate(root, item_id, candidate)
    packet = write_packet(root, packets, item_id, [("Candidate commit", candidate), ("Candidate base", base)])
    if packet.status == "submitted" and not packet.run:
        registry = load_registry(root)
        outcome = evaluate_submission(root, registry, packets, item_id, target_tip)
        write_packet(root, packets, item_id, [("Queues", ", ".join(outcome["queues"]) or "none"), ("Implied dependencies", ", ".join(outcome["implied"]) or "none")])
        for queue_id in registry.queues:
            run = open_run(packets, queue_id)
            held = any(m[0] == item_id for m in run.members())
            if queue_id in outcome["queues"]:
                add_member(root, packets, run, item_id, candidate)
            elif held:
                write_packet(root, packets, run.item_id, sections={"Members": member_lines([m for m in run.members() if m[0] != item_id])})
        propagate_dependencies(root, packets, target_tip)
        packet = packets[item_id]
    return packet


def candidate_footprint(root: Path, base: str, candidate: str) -> dict[str, list[str]]:
    """Paths the candidate adds, modifies, and deletes relative to its merge base."""
    out = {"added": [], "modified": [], "deleted": []}
    for line in git(root, "diff", "--name-status", "--no-renames", f"{base}...{candidate}", strip=False).splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        kind = {"A": "added", "D": "deleted"}.get(parts[0][:1], "modified")
        out[kind].append(parts[1])
    return out


def footprint_line(footprint: dict[str, list[str]]) -> str:
    total = sum(len(v) for v in footprint.values())
    return f"footprint: {total} paths ({len(footprint['added'])} added, {len(footprint['modified'])} modified, {len(footprint['deleted'])} deleted)"


def preserved_run_rename(root: Path, base: str, candidate: str, old_path: str, added: list[str]) -> bool:
    """The naming migration moves an unstarted run without deleting its record."""
    old_id = Path(old_path).stem
    if not old_id.startswith("WI-CI-INTEGRATION-BIQ-"):
        return False
    before = parse_packet(old_id, git(root, "show", f"{base}:{old_path}", strip=False))
    if before.run is None or run_state(before) != "open" or before.candidate:
        return False
    queue, ordinal = before.run
    new_id = f"WI-CI-INTEGRATION-{queue}-REPEAT-{ordinal}"
    new_path = f"{ITEMS_DIR}/{new_id}.md"
    if new_path not in added:
        return False
    after = parse_packet(new_id, git(root, "show", f"{candidate}:{new_path}", strip=False))
    if after.run != before.run or after.fields.get("ID") != new_id:
        return False
    # Names, descriptive area and the registry's campaign budget may change.
    # Every other header (including lifecycle, provenance, tests and cost) stays.
    labels = {"ID", "Queue", "Area", "Budget"}
    if ({k: v for k, v in before.fields.items() if k not in labels}
            != {k: v for k, v in after.fields.items() if k not in labels}):
        return False
    return all(before.sections.get(section, "") == after.sections.get(section, "")
               for section in ("Members", "Excluded", "Attempt history", "Resolution"))


def check_candidate_history(root: Path, item_id: str, base: str, candidate: str, packets: dict[str, Packet] | None = None) -> None:
    """A candidate carries its owner's work, the exact published candidates of its declared dependencies, and nothing else.

    A merge above the base whose tree is one parent's tree, when a true merge of
    its parents would differ, discarded the other side: with main as that side,
    landing the candidate would silently revert everything main gained. A
    two-parent conflict resolution may equal a parent only when Git reproduces
    that whole tree without dropping any non-conflicting merge content.
    A candidate also deletes no packet and touches no other item's agent-work,
    except the unchanged agent-work of a WQE named in Dependencies whose
    published candidate it carries (the one thing an owner merges).
    A preserved unstarted run naming migration is a move, not packet deletion.
    """
    for merge in git(root, "rev-list", "--merges", f"{base}..{candidate}").split():
        parents = git(root, "rev-list", "--parents", "-n", "1", merge).split()[1:]
        tree = git(root, "rev-parse", f"{merge}^{{tree}}")
        parent_trees = [git(root, "rev-parse", f"{p}^{{tree}}") for p in parents]
        if len(parents) < 2 or tree not in parent_trees:
            continue
        true = subprocess.run(["git", "merge-tree", "--write-tree", parents[0], parents[1]], cwd=root, text=True, capture_output=True)
        true_tree = true.stdout.split("\n", 1)[0].strip() if true.returncode in (0, 1) else ""
        if true.returncode == 1 and len(parents) == 2 and true_tree != tree:
            # Prefer only overlapping changes; independent edits still merge.
            preference = "ours" if tree == parent_trees[0] else "theirs"
            resolved = subprocess.run(["git", "merge-tree", "--write-tree", f"-X{preference}", *parents], cwd=root, text=True, capture_output=True)
            if resolved.returncode == 0:
                true_tree = resolved.stdout.split("\n", 1)[0].strip()
        if true_tree != tree:
            subject = git(root, "log", "-1", "--format=%s", merge)
            raise BiqError(f"candidate history contains a merge that discarded a parent: {merge[:12]} ({subject}); "
                           "retain both parents' non-conflicting changes and resolve overlapping changes normally")
    footprint = candidate_footprint(root, base, candidate)
    packets_deleted = [p for p in footprint["deleted"] if p.startswith(ITEMS_DIR + "/")
                       and not preserved_run_rename(root, base, candidate, p, footprint["added"])]
    if packets_deleted:
        raise BiqError(f"candidate deletes {len(packets_deleted)} packet(s), e.g. {packets_deleted[0]}; a packet is closed by `abandon`, never deleted")
    foreign = sorted({p.split("/", 2)[1] for v in footprint.values() for p in v
                      if p.startswith("agent-work/") and not p.startswith(f"agent-work/{item_id}/")
                      and ITEM_RE.fullmatch(p.split("/", 2)[1])})
    refused = [f"{other} ({why})" for other in foreign if (why := inherited_work_problem(root, packets or {}, item_id, candidate, other))]
    if refused:
        raise BiqError(f"candidate touches other items' agent-work: {', '.join(refused[:4])}{', ...' if len(refused) > 4 else ''}; "
                       "a candidate carries only its own item's work and the unchanged agent-work of a declared dependency's exact published candidate")


def carrying_dependency(root: Path, packets: dict[str, Packet], packet: Packet | None, candidate: str, other: str) -> tuple[str, str] | None:
    """The declared dependency whose merged published candidate holds agent-work/<other>/ exactly as the candidate does, if one does."""
    for declared in (packet.dependencies if packet is not None else ()):
        dependency = packets.get(resolve_replacement(packets, declared))
        if dependency is None or dependency.candidate is None:
            continue
        ensure_commit(root, dependency.candidate)
        if not is_ancestor(root, dependency.candidate, candidate):
            continue
        if git(root, "ls-tree", "-r", "--name-only", dependency.candidate, "--", f"agent-work/{other}/") \
                and not git(root, "diff", "--name-only", "--no-renames", dependency.candidate, candidate, "--", f"agent-work/{other}/"):
            return dependency.item_id, dependency.candidate
    return None


def inherited_work_problem(root: Path, packets: dict[str, Packet], item_id: str, candidate: str, other: str) -> str:
    """Why agent-work/<other>/ in the candidate is not the unchanged content of a declared dependency's published candidate; empty when it is."""
    packet = packets.get(item_id)
    declared = packet is not None and any(dep == other or resolve_replacement(packets, dep) == other for dep in packet.dependencies)
    if not declared:
        carrier = carrying_dependency(root, packets, packet, candidate, other)
        return "not in Dependencies" + (f"; carried unchanged inside {carrier[0]}'s published candidate {carrier[1][:12]}, so name {other} in Dependencies as well" if carrier else "")
    dependency = packets.get(other)
    if dependency is None or dependency.candidate is None:
        return "no published candidate"
    ensure_commit(root, dependency.candidate)
    if not is_ancestor(root, dependency.candidate, candidate):
        return f"published candidate {dependency.candidate[:12]} is not merged into the candidate"
    if git(root, "diff", "--name-only", "--no-renames", dependency.candidate, candidate, "--", f"agent-work/{other}/"):
        return f"edited beyond published candidate {dependency.candidate[:12]}"
    return ""


def landed(root: Path, packets: dict[str, Packet], item_id: str, target_tip: str) -> bool:
    packet = packets.get(item_id)
    if packet is None:
        return False
    if packet.status == "completed":
        return True
    return packet.candidate is not None and is_ancestor(root, packet.candidate, target_tip)


def resolve_replacement(packets: dict[str, Packet], item_id: str) -> str:
    """A dependency on a rejected WQE, or on one restarted on a fresh baseline, is met by its replacement."""
    seen = set()
    while item_id in packets and packets[item_id].status in {"failed-rejected", "failed-abandoned"} and item_id not in seen:
        seen.add(item_id)
        replacement = next((p.item_id for p in packets.values() if p.replaces == item_id), None)
        if replacement is None:  # a rejected packet of the previous queue names its successor in Next attempt
            successor = packets[item_id].fields.get("Next attempt", "none")
            replacement = successor if successor in packets else None
        if replacement is None:
            break
        item_id = replacement
    return item_id


def unsubmitted_history(root: Path, packets: dict[str, Packet], item_id: str, candidate: str, target_tip: str) -> list[str]:
    """Commits in the candidate that belong to another WQE which has not been submitted."""
    log = git(root, "log", "--format=%H%n%B%x00", f"{target_tip}..{candidate}", strip=False)
    foreign = []
    for block in log.split("\x00"):
        if not block.strip():
            continue
        sha, _nl, body = block.strip().partition("\n")
        for other in TRAILER_RE.findall(body):
            if other != item_id and (other not in packets or packets[other].status == "open"):
                foreign.append(sha)
    return foreign


def evaluate_submission(root: Path, registry: Registry, packets: dict[str, Packet], item_id: str, target_tip: str) -> dict:
    packet = require_packet(packets, item_id)
    if packet.candidate is None:
        raise BiqError(f"{item_id} has no sealed candidate; run seal first")
    if packet.run:
        raise BiqError(f"{item_id} is a run WQE; its result is recorded by run complete")
    paths = changed_paths(root, target_tip, packet.candidate)
    answers = {q.id: q.cares(paths) for q in registry.queues.values()}
    caring = sorted(q for q, hits in answers.items() if hits)
    implied = sorted(
        p.item_id for p in packets.values()
        if p.item_id != item_id and p.candidate and not p.run and p.status == "submitted"
        and not is_ancestor(root, p.candidate, target_tip) and is_ancestor(root, p.candidate, packet.candidate)
    )
    foreign = unsubmitted_history(root, packets, item_id, packet.candidate, target_tip)
    if foreign:
        raise BiqError(f"{item_id} carries commits of unsubmitted work: " + ", ".join(c[:12] for c in foreign))
    # A declared dependency stands for its replacement: the candidate of the restart that replaced a
    # rejected or abandoned dependency is declared by the original's name, so resolve the declared side.
    declared = set(packet.dependencies) | {resolve_replacement(packets, dep) for dep in packet.dependencies}
    undeclared = [dep for dep in implied if dep not in declared]
    if undeclared:
        raise BiqError(f"{item_id} carries the sealed candidate of " + ", ".join(undeclared) + "; an owner that pulls another WQE's candidate into its branch names that WQE in Dependencies")
    return {"item": item_id, "candidate": packet.candidate, "paths": paths, "answers": answers, "queues": caring, "implied": implied}


def submit_item(root: Path, registry: Registry, packets: dict[str, Packet], item_id: str, target_tip: str, owner: str, *, apply: bool,
                usage_snapshot: str | None = None, aws_cost: str | None = None, charged_to: str | None = None, human_only: bool = False,
                retrospective: str | None = None) -> dict:
    require_production_priority(packets, item_id, root)
    claim = require_claim(root, item_id, owner)
    packet = require_packet(packets, item_id)
    if packet.status != "open":
        raise BiqError(f"{item_id} is {packet.status}; only an open packet is submitted")
    outcome = evaluate_submission(root, registry, packets, item_id, target_tip)
    if requires_workstream(packet) and workstream_of(packets, item_id) is None:
        raise BiqError(f"{item_id} belongs to no workstream; run `python3 tools/biq.py workstream {item_id}` and join one before submitting")
    evidence = retrospective_evidence(root, find_retrospective(root, item_id, retrospective)) if requires_retrospective(packet) or retrospective else None
    accounting = usage_evidence(root, claim, item_id, usage_snapshot=usage_snapshot, charged_to=charged_to, human_only=human_only, packets=packets)
    if aws_cost is not None:
        usd(parse_usd(aws_cost))  # canonical form or refusal
    if apply:
        submitted = now_utc()
        claimed = str((claim or {}).get("started_at") or packet.fields.get("Claimed At") or submitted)
        claimed = claimed if not claimed.endswith("+00:00") else claimed[:-6] + "Z"
        fields = [("Status", "submitted"), ("Submitted At", submitted), ("Submitted owner", owner),
                  ("Queues", ", ".join(outcome["queues"]) or "none"), ("Implied dependencies", ", ".join(outcome["implied"]) or "none")]
        if packet.fields.get("Claimed At") == "pending":
            fields.append(("Claimed At", claimed))
        if packet.fields.get("Implementation Duration") == "pending":
            fields.append(("Implementation Duration", f"PT{seconds_between(claimed, submitted)}S"))
        if accounting is not None:
            _evidence, tokens, models, cost = accounting
            for name, value in (("Tokens Used", tokens), ("Model Used", models), ("Estimated Cost", cost)):
                if name in packet.fields:
                    fields.append((name, value))
        if aws_cost is not None and "Estimated AWS Cost" in packet.fields:
            fields.append(("Estimated AWS Cost", usd(parse_usd(aws_cost))))
        write_packet(root, packets, item_id, fields)
        lines = []
        if accounting is not None:
            lines.append(USAGE_EVIDENCE_PREFIX + json.dumps(accounting[0], sort_keys=True, separators=(",", ":")))
        lines.append(provenance_line(root, item_id, claim, "implementation", submitted))
        if evidence is not None:
            lines.append(retrospective_line(evidence))
        append_resolution_lines(root, packets, item_id, lines)
        for queue_id in outcome["queues"]:
            add_member(root, packets, open_run(packets, queue_id), item_id, packet.candidate)
        outcome["implied_members"] = propagate_dependencies(root, packets, target_tip)
    return outcome


def add_member(root: Path, packets: dict[str, Packet], run: Packet, item_id: str, sha: str, note: str = "") -> None:
    members = [m for m in run.members() if m[0] != item_id] + [(item_id, sha, note)]
    write_packet(root, packets, run.item_id, sections={"Members": member_lines(members)})


def propagate_dependencies(root: Path, packets: dict[str, Packet], target_tip: str) -> list[str]:
    """A submitted, unlanded dependency of a member joins that member's open run as an implied member."""
    added = []
    for run in [p for p in list(packets.values()) if p.run and p.status == "open"]:
        carried = {m[0] for r in in_flight_runs(packets, run.run[0]) for m in r.members()}
        while True:
            members = run.members()
            present = {m[0] for m in members} | carried
            grown = False
            for item, _sha, _note in members:
                packet = packets.get(item)
                if packet is None or packet.run:
                    continue
                for dep in packet.dependencies:
                    dep = resolve_replacement(packets, dep)
                    dep_packet = packets.get(dep)
                    if dep in present or dep_packet is None or dep_packet.run or dep_packet.status != "submitted" or dep_packet.candidate is None:
                        continue
                    if landed(root, packets, dep, target_tip):
                        continue
                    add_member(root, packets, run, dep, dep_packet.candidate, f"implied by {item}")
                    run = packets[run.item_id]
                    present.add(dep)
                    added.append(f"{dep} -> {run.item_id}")
                    grown = True
            if not grown:
                break
    return added


def require_successor(packets: dict[str, Packet], item_id: str, successor: str) -> Packet:
    """A restart's successor is an open packet that names the old attempt as both Previous attempt and Replaces."""
    if successor == item_id or successor not in packets:
        raise BiqError(f"successor {successor} has no packet; draft it beside {item_id} before abandoning")
    packet = packets[successor]
    if packet.status != "open" or packet.run:
        raise BiqError(f"successor {successor} is {packet.status}; a restart's successor is a fresh open packet")
    if packet.replaces != item_id or packet.fields.get("Previous attempt") != item_id:
        raise BiqError(f"successor {successor} must name {item_id} in both Previous attempt and Replaces")
    return packet


def replace_workstream_member(root: Path, packets: dict[str, Packet], item_id: str, successor: str) -> None:
    """Keep a reciprocal restart in the predecessor's existing project."""
    for workstream in [p for p in packets.values() if is_workstream(p) and item_id in container_members(p)]:
        members = sorted((set(container_members(workstream)) - {item_id}) | {successor})
        write_packet(root, packets, workstream.item_id, [("Container members", ", ".join(members))])


def rejection_fields(packet: Packet) -> list[tuple[str, str]]:
    """The closing every rejection writes: a held submission's Decision owner is released with its status."""
    fields = [("Status", "failed-rejected"), ("Closed At", now_utc())]
    if packet.fields.get("Decision owner", "none") != "none":
        fields.append(("Decision owner", "none"))
    return fields


def reject_item(root: Path, packets: dict[str, Packet], item_id: str, reason: str, authorized_by: str | None, *, apply: bool,
                successor: str | None = None, registry: Registry | None = None, owner: str | None = None,
                deferred_publications: list[tuple[str, str]] | None = None,
                deferred_branch_moves: list[tuple[str, str | None]] | None = None) -> Packet:
    """Close a submitted leaf as failed-rejected, on the accountable human's word.

    Rejection inside a run stays the integrator's (`run review --reject`). This is
    for a candidate a closed run left behind, or one whose objective was met
    elsewhere: submitted work is never abandoned, it is rejected. Open work that
    depends on it must be told what meets the dependency now: the successor is
    a fresh open restart exactly as for `abandon`, or the completed packet whose
    landing met the objective. A fresh restart may atomically remove the old
    candidate from every holding run; it remains an ordinary submission.
    """
    packet = require_packet(packets, item_id)
    if packet.status != "submitted" or packet.run:
        raise BiqError(f"{item_id} is {packet.status}; only a submitted leaf is rejected outside a run")
    if not (authorized_by or "").strip():
        raise BiqError("rejecting submitted work needs the Accountable owner's word: --authorized-by \"<human>\"")
    if not reason.strip():
        raise BiqError("--reason is required")
    dependents = sorted(p.item_id for p in packets.values() if not p.run and p.status not in TERMINAL and item_id in p.dependencies)
    if dependents and not successor:
        raise BiqError(f"{item_id} is a dependency of {', '.join(dependents)}; name what meets it with --successor <ID>: a fresh open restart, or the completed packet whose landing met the objective")
    met_by = bool(successor) and successor != item_id and successor in packets and packets[successor].status == "completed" and not packets[successor].run
    if successor and not met_by:
        require_successor(packets, item_id, successor)
    holdings = [(run, any(m[0] == item_id for m in run.members()), item_id in excluded_members(run))
                for run in packets.values() if run.run and run.status not in TERMINAL
                if any(m[0] == item_id for m in run.members()) or item_id in excluded_members(run)]
    if holdings and (not successor or met_by):
        names = ", ".join(run.item_id for run, _member, _excluded in holdings)
        raise BiqError(f"{item_id} is a member of {names}; reject it there with `run review <QUEUE> <n> {item_id} --reject <reason> --authorized-by <human> --apply`")
    if holdings:
        if registry is None:
            raise BiqError("rejecting a held member with a successor requires the queue registry")
        submitted = [run.item_id for run, _member, _excluded in holdings if run_state(run) == "submitted"]
        if submitted:
            raise BiqError(f"{item_id} is held by submitted run(s) {', '.join(submitted)}; resolve or reopen those results before replacement")
        for run, _member, _excluded in holdings:
            if run_state(run) == "started":
                require_claim(root, run.item_id, owner)
                open_run(packets, run.run[0])
    if not apply:
        return packet
    human, user, host = packet_identity(root)
    fields = rejection_fields(packet) + [("Completed human_owner", human), ("Completed host_user", user), ("Completed hostname", host)] + ([("Next attempt", successor)] if successor and not met_by else [])
    if holdings:
        rejection = f"rejected outside the run on the word of {authorized_by.strip()}: {reason.strip()}"
        plans = {held.item_id: plan_shed_members(root, packets, held, {item_id: rejection})
                 for held, member, _excluded in holdings if run_state(held) == "started" and member}
        item_dir = root / ITEMS_DIR
        saved_packets = {path: path.read_bytes() for path in item_dir.glob("*.md")}
        saved_branches = {f"biq/{held.run[0].lower()}/{held.run[1]}": held.fields["Integration commit"]
                          for held, member, _excluded in holdings if run_state(held) == "started" and member}
        try:
            write_packet(root, packets, item_id, fields,
                         resolution=f"Rejected by {authorized_by.strip()} outside a run: {reason.strip()} Restarted as {successor}.")
            replace_workstream_member(root, packets, item_id, successor)
            rebuilt = []
            for held, member, excluded in holdings:
                run = packets[held.item_id]
                if run_state(run) == "started" and member:
                    outcome = shed_members(root, registry, packets, run, {item_id: rejection}, reject_reason=rejection,
                                           publish=False, move=deferred_branch_moves is None, plan=plans[held.item_id])
                    if outcome.get("integration"):
                        rebuilt.append((run.item_id, outcome["integration"]))
                        if deferred_branch_moves is not None:
                            deferred_branch_moves.append((f"biq/{run.run[0].lower()}/{run.run[1]}", outcome["integration"]))
                    elif outcome.get("closed") and deferred_branch_moves is not None:
                        deferred_branch_moves.append((f"biq/{run.run[0].lower()}/{run.run[1]}", None))
                    run = packets[held.item_id]
                elif run_state(run) == "open" and member:
                    write_packet(root, packets, run.item_id,
                                 sections={"Members": member_lines([m for m in run.members() if m[0] != item_id])})
                    run = packets[held.item_id]
                if run.status not in TERMINAL and (excluded or item_id in excluded_members(run)):
                    remaining = {key: value for key, value in excluded_members(run).items() if key != item_id}
                    write_packet(root, packets, run.item_id,
                                 sections={"Excluded": "\n".join(f"- {key}: {value}" for key, value in remaining.items()) or "none"})
            if deferred_publications is None:
                publish_candidates_atomic(root, rebuilt)
            else:
                deferred_publications.extend(rebuilt)
        except Exception:
            for path in item_dir.glob("*.md"):
                if path not in saved_packets:
                    path.unlink()
            for path, content in saved_packets.items():
                path.write_bytes(content)
            for branch, commit in saved_branches.items():
                move_branch(root, branch, commit)
            packets.clear()
            packets.update(load_packets(root))
            raise
        return packets[item_id]
    if met_by:  # the dependents' declared dependency moves to the landed work that met it
        for dependent in dependents:
            declared = [d for d in packets[dependent].ids("Dependencies") if d != item_id]
            write_packet(root, packets, dependent, [("Dependencies", ", ".join(declared + [successor] * (successor not in declared)) or "none")])
    packet = write_packet(root, packets, item_id, fields, resolution=f"Rejected by {authorized_by.strip()} outside a run: {reason.strip()}"
                          + (f" Objective met by {successor}" + (f"; {', '.join(dependents)} now depend on it." if dependents else ".") if met_by else f" Restarted as {successor}." if successor else ""))
    if successor and not met_by:
        replace_workstream_member(root, packets, item_id, successor)
    return packet


def abandon_item(root: Path, packets: dict[str, Packet], item_id: str, owner: str, reason: str, *, apply: bool, retrospective: str | None = None,
                 successor: str | None = None, release: bool = True) -> Packet:
    """Close an open packet as failed-abandoned; the claim, if any, must be the owner's or absent.

    A packet the retrospective rule covers is abandoned only with its retrospective
    published, indexed and committed, and the closing records the reference.
    With a successor, the closing is a restart: the old packet names the successor
    in Next attempt, and a dependency on the old packet follows to the successor,
    which meets it only by landing. Without one, the abandonment is terminal.
    """
    packet = require_packet(packets, item_id)
    if packet.status != "open":
        raise BiqError(f"{item_id} is {packet.status}; only an open packet is abandoned")
    if packet.run:
        raise BiqError(f"{item_id} is a run WQE; it closes through its run")
    _oid, claim = read_claim(root, item_id)
    if claim is not None and claim.get("owner") != owner:
        raise BiqError(f"{item_id} is claimed by {claim.get('owner')!r}")
    if not reason.strip():
        raise BiqError("--reason is required")
    evidence = retrospective_evidence(root, find_retrospective(root, item_id, retrospective)) if requires_retrospective(packet) or retrospective else None
    if successor:
        require_successor(packets, item_id, successor)
    if not apply:
        return packet
    human, user, host = packet_identity(root)
    fields = [
        ("Status", "failed-abandoned"), ("Closed At", now_utc()),
        ("Completed human_owner", human), ("Completed host_user", user), ("Completed hostname", host),
        ("Candidate base", "none"), ("Candidate commit", "none"), ("Submitted At", "none"), ("Submitted owner", "none"),
    ] + ([("Next attempt", successor)] if successor else [])
    resolution = f"Abandoned by {owner}: {reason.strip()}" + (f" Restarted as {successor}." if successor else "")
    packet = write_packet(root, packets, item_id, fields, resolution=resolution)
    if evidence is not None:
        append_resolution_lines(root, packets, item_id, [retrospective_line(evidence)])
        packet = packets[item_id]
    if successor:
        replace_workstream_member(root, packets, item_id, successor)
    if claim is not None and release:  # the command releases after its control commit lands
        release_item(root, item_id, owner)
    return packet


# --- dependencies and readiness --------------------------------------------

def leaves_of(packets: dict[str, Packet], item_id: str, seen: set[str] | None = None) -> set[str]:
    """The leaf WQEs inside a member: itself, or every leaf of a run's members."""
    seen = seen if seen is not None else set()
    packet = packets.get(item_id)
    if packet is None or not packet.run or item_id in seen:
        return {item_id}
    seen.add(item_id)
    out: set[str] = set()
    for member, _sha, _note in packet.members():
        out |= leaves_of(packets, member, seen)
    return out


def dependency_states(root: Path, packets: dict[str, Packet], item_id: str, target_tip: str | None) -> list[tuple[str, str, bool, str]]:
    """Each declared dependency of a leaf after replacement resolution: (id, state, startable, the owner's action).

    A dependency gates the start of its dependent, never its submission: a
    submitted one is consumed by merging its published candidate ref, and
    only `land` waits for it to reach the target.
    """
    packet = packets.get(item_id)
    if packet is None or packet.run:
        return []
    states = []
    for declared in packet.dependencies:
        dep = resolve_replacement(packets, declared)
        via = f" (replacing {declared})" if dep != declared else ""
        other = packets.get(dep)
        if other is None:
            states.append((dep + via, "not a packet", False, "not startable; fix Dependencies as a control commit"))
        elif other.status == "completed" or (target_tip is not None and landed(root, packets, dep, target_tip)):
            ref = f"{CANDIDATE_REF_PREFIX}{dep}/{other.candidate[:12]}" if other.candidate else None
            states.append((dep + via, "landed", True, f"merge {ref} only if your Baseline predates it" if ref else "nothing to merge"))
        elif other.status == "submitted":
            states.append((dep + via, "submitted", True, f"merge {CANDIDATE_REF_PREFIX}{dep}/{other.candidate[:12]} into your branch; its integration and landing are not prerequisites"))
        elif other.status in TERMINAL:
            states.append((dep + via, other.status + " with no successor", False, "not startable; the accountable human names what meets it"))
        else:
            states.append((dep + via, "sealed, not submitted" if other.candidate else "open, not submitted", False, "not startable until it is submitted"))
    return states


def unmet_dependencies(root: Path, packets: dict[str, Packet], item_id: str, target_tip: str, inside: set[str]) -> list[str]:
    """Dependencies of a leaf not on the target, not inside the run, and not met by a replacement."""
    packet = packets.get(item_id)
    if packet is None or packet.run:
        return []
    unmet = []
    for dep in packet.dependencies:
        dep = resolve_replacement(packets, dep)
        if dep in inside or landed(root, packets, dep, target_tip):
            continue
        unmet.append(dep)
    return sorted(set(unmet))


def uncovered_leaves(registry: Registry, packets: dict[str, Packet], queue: Queue, result_id: str, inside: set[str]) -> list[str]:
    """For an aggregator member: leaves that another feeder queue cares about but has not delivered here."""
    if queue.kind != "aggregator":
        return []
    producer = packets[result_id].run[0] if result_id in packets and packets[result_id].run else None
    delivered: dict[str, set[str]] = {}
    for member in inside:
        member_packet = packets.get(member)
        if member_packet and member_packet.run:
            for leaf in leaves_of(packets, member):
                delivered.setdefault(leaf, set()).add(member_packet.run[0])
    missing = []
    for leaf in sorted(leaves_of(packets, result_id)):
        leaf_packet = packets.get(leaf)
        if leaf_packet is None:
            continue
        for other in leaf_packet.queues:
            if other in queue.feeders and other != producer and other not in delivered.get(leaf, set()):
                missing.append(f"{other} result containing {leaf}")
    return missing


def readiness(root: Path, registry: Registry, packets: dict[str, Packet], queue: Queue, members: list[tuple[str, str, str]], target_tip: str, predecessor: Packet | None) -> tuple[list[tuple[str, str, str]], list[tuple[str, str, str, str]], bool]:
    """Members whose dependencies are met (fixed point), the waiting ones with reasons, and whether the predecessor is needed."""
    chosen: dict[str, tuple[str, str, str]] = {}
    predecessor_items = {m[0] for m in predecessor.members()} if predecessor else set()
    present = {m[0] for m in members}
    needs_predecessor = False
    while True:
        inside = set(chosen)
        progressed = False
        for member in members:
            item = member[0]
            if item in chosen:
                continue
            unmet = unmet_dependencies(root, packets, item, target_tip, inside | predecessor_items)
            unmet += uncovered_leaves(registry, packets, queue, item, present)
            if unmet:
                continue
            if set(unmet_dependencies(root, packets, item, target_tip, inside)) & predecessor_items:
                needs_predecessor = True
            chosen[item] = member
            progressed = True
        if not progressed:
            break
    # a result counted on another result that turned out not ready leaves too
    while True:
        dropped = [item for item in chosen if uncovered_leaves(registry, packets, queue, item, set(chosen))]
        if not dropped:
            break
        for item in dropped:
            del chosen[item]
    waiting = []
    for member in members:
        if member[0] not in chosen:
            reasons = unmet_dependencies(root, packets, member[0], target_tip, set(chosen) | predecessor_items) + uncovered_leaves(registry, packets, queue, member[0], set(chosen))
            waiting.append((*member, "waiting: " + ", ".join(reasons)))
    return [chosen[m[0]] for m in members if m[0] in chosen], waiting, needs_predecessor


# --- merging ----------------------------------------------------------------

def merge_members(root: Path, base: str, members: list[tuple[str, str, str]], *, message: str) -> tuple[str, list[tuple[str, str]]]:
    """Merge every member onto base in a temporary worktree; a textual conflict excludes that member."""
    worktree = Path(tempfile.mkdtemp(prefix="biq-"))
    collisions: list[tuple[str, str]] = []
    try:
        git(root, "worktree", "add", "--detach", "--quiet", str(worktree), base)
        for item, sha, _note in members:
            ensure_commit(root, sha)
            result = subprocess.run(["git", "merge", "--no-ff", "--no-edit", "-m", f"{message}: {item}", sha], cwd=worktree, text=True, capture_output=True)
            if result.returncode:
                subprocess.run(["git", "merge", "--abort"], cwd=worktree, capture_output=True)
                collisions.append((item, "textual conflict with the batch"))
        return git(worktree, "rev-parse", "HEAD"), collisions
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", str(worktree)], cwd=root, capture_output=True)
        shutil.rmtree(worktree, ignore_errors=True)


# --- runs ------------------------------------------------------------------

def start_run(root: Path, registry: Registry, packets: dict[str, Packet], queue_id: str, target_tip: str, *, owner: str | None, apply: bool) -> dict:
    """Freeze the open run WQE's ready members, merge them, and open the successor."""
    queue = registry.queues.get(queue_id)
    if queue is None:
        raise BiqError(f"unknown queue {queue_id}")
    run = open_run(packets, queue_id)
    if apply:
        require_claim(root, run.item_id, owner)
    in_flight = in_flight_runs(packets, queue_id)
    predecessor = in_flight[-1] if in_flight else None
    ready, waiting, chain = readiness(root, registry, packets, queue, run.members(), target_tip, predecessor)
    ready = [m for m in ready if not landed(root, packets, m[0], target_tip)]
    if not ready:
        raise BiqError(f"{run.item_id} has no member with met dependencies")
    # A successor integrates on its in-flight predecessor's current tip from the start, whether or not a
    # member depends on it; it waits for the predecessor only at its own landing.
    if predecessor is not None and predecessor_tip(predecessor) is not None:
        base, chain = predecessor_tip(predecessor), True
    else:
        base, predecessor, chain = target_tip, None, False
    integration, collisions = merge_members(root, base, ready, message=f"{queue_id} run {run.run[1]}")
    collided = dict(collisions)
    # A collided member stays in the run awaiting the integrator's hand merge: a merge conflict is
    # integration work, never grounds to drop or defer a member. Only members whose dependencies
    # are not met move to the successor.
    members = [(item, sha, f"{RESOLUTION_PENDING_MARK}{collided[item]}" if item in collided else note) for item, sha, note in ready]
    outcome = {"item": run.item_id, "base": base, "predecessor": predecessor.item_id if predecessor else None, "integration": integration,
               "members": [m for m in members if m[0] not in collided], "ready": ready, "collisions": collisions, "waiting": waiting, "tests": list(queue.tests)}
    if apply:
        write_packet(root, packets, run.item_id, [
            ("Started At", now_utc()), ("Claimed At", now_utc()), ("Base", base), ("Predecessor", predecessor.item_id if predecessor else "none"),
            ("Integration commit", integration), ("Tests", ", ".join(f"{t}=pending" for t in queue.tests) or "pending"),
        ], sections={"Members": member_lines(members), "Excluded": "none"})
        move_branch(root, f"biq/{queue_id.lower()}/{run.run[1]}", integration)
        publish_candidate(root, run.item_id, integration)
        successor = run_item_id(queue_id, run.run[1] + 1)
        carried = [(item, sha, "") for item, sha, _note, _reason in waiting]
        text = run_packet_text(root, registry, queue, run.run[1] + 1, target_tip, carried)
        (root / ITEMS_DIR / f"{successor}.md").write_text(text, encoding="utf-8")
        packets[successor] = parse_packet(successor, text)
        outcome["successor"] = successor
    return outcome


def predecessor_tip(predecessor: Packet) -> str | None:
    """What a successor bases on: the predecessor's result once it has one, else its integration commit."""
    for field in ("Result commit", "Integration commit"):
        value = predecessor.fields.get(field, "pending")
        if COMMIT_RE.match(value):
            return value
    return None


def refresh_run(root: Path, registry: Registry, packets: dict[str, Packet], run: Packet, *, owner: str | None, apply: bool) -> dict:
    """Re-base a started or submitted run onto its predecessor's current tip; the run's tests reset.

    The predecessor moved (a fix, a resolution, a rebuild) after this run started
    on its earlier tip. The run's own integration commit is merged onto the new tip
    with the same machinery a start uses; a conflict is the integrator's to resolve
    by hand and record with `run fix`.
    """
    if run_state(run) not in {"started", "submitted"}:
        raise BiqError(f"{run.item_id} is {run_state(run)}; a started or submitted run is refreshed")
    require_claim(root, run.item_id, owner)
    predecessor_id = run.fields.get("Predecessor", "none")
    predecessor = packets.get(predecessor_id) if predecessor_id != "none" else None
    if predecessor is None:
        raise BiqError(f"{run.item_id} has no predecessor to refresh from")
    tip = predecessor_tip(predecessor)
    previous = run.fields.get("Integration commit", "pending")
    if tip is None or not COMMIT_RE.match(previous):
        raise BiqError(f"{run.item_id} or {predecessor_id} has no integration commit")
    queue_id, ordinal = run.run
    queue = registry.queues[queue_id]
    if tip == run.fields.get("Base") or is_ancestor(root, tip, previous):
        return {"item": run.item_id, "base": tip, "previous": previous, "integration": previous, "moved": False, "tests": list(queue.tests)}
    ensure_commit(root, tip)
    integration, collisions = merge_members(root, tip, [(run.item_id, previous, "")], message=f"{queue_id} run {ordinal} refreshed onto {predecessor_id}")
    if collisions:
        raise BiqError(f"{run.item_id} conflicts with {predecessor_id}'s tip {tip[:12]}: git checkout biq/{queue_id.lower()}/{ordinal} && git merge {tip[:12]} (resolve, commit), then run fix {queue_id} {ordinal} --commit HEAD --owner <you> --apply")
    outcome = {"item": run.item_id, "base": tip, "previous": previous, "integration": integration, "moved": True, "tests": list(queue.tests)}
    if apply:
        write_packet(root, packets, run.item_id, [
            ("Status", "open"), ("Base", tip), ("Integration commit", integration), ("Result commit", "pending"),
            ("Tests", ", ".join(f"{name}=pending" for name in queue.tests) or "pending"),
            ("Candidate base", "pending"), ("Candidate commit", "pending"),
            ("Submitted At", "pending"), ("Submitted owner", "pending"),
        ], sections={"Evidence": "none"}, resolution=f"Refreshed onto {predecessor_id} at {tip[:12]}; tests reset.")
        move_branch(root, f"biq/{queue_id.lower()}/{ordinal}", integration)
        publish_candidate(root, run.item_id, integration)
    return outcome


def already_started_by(root: Path, packets: dict[str, Packet], queue_id: str, owner: str | None) -> Packet | None:
    """The nonterminal run an explicit owner already holds, for safe start reruns."""
    if not owner:
        return None
    for run in reversed(in_flight_runs(packets, queue_id)):
        _oid, claim = read_claim(root, run.item_id)
        if claim and claim.get("owner") == owner:
            return run
    return None


def fix_run(root: Path, registry: Registry, packets: dict[str, Packet], run: Packet, commit: str, *, owner: str | None, apply: bool) -> dict:
    """Replace a run result with a descendant integration-fix commit and reset its tests."""
    if run_state(run) not in {"started", "submitted"}:
        raise BiqError(f"{run.item_id} is {run_state(run)}; integration fixes apply to a started or submitted run")
    require_claim(root, run.item_id, owner)
    previous = run.fields.get("Integration commit", "pending")
    if not COMMIT_RE.match(previous):
        raise BiqError(f"{run.item_id} has no integration commit to fix")
    commit = rev(root, commit)
    if commit == previous or not is_ancestor(root, previous, commit):
        raise BiqError(f"{commit[:12]} must descend from {run.item_id}'s integration commit {previous[:12]}")
    queue = registry.queues[run.run[0]]
    paths = changed_paths(root, previous, commit)
    retained = f"agent-work/{run.item_id}/"
    refused = [path for path in paths if not queue.cares([path]) and not path.startswith(retained)]
    if refused:
        raise BiqError(f"integration fix changes paths outside {queue.id}'s care: {', '.join(refused[:10])}")
    outcome = {"item": run.item_id, "previous": previous, "integration": commit, "paths": paths, "tests": list(queue.tests)}
    if apply:
        write_packet(root, packets, run.item_id, [
            ("Status", "open"), ("Integration commit", commit), ("Result commit", "pending"),
            ("Tests", ", ".join(f"{name}=pending" for name in queue.tests) or "pending"),
            ("Candidate base", "pending"), ("Candidate commit", "pending"),
            ("Submitted At", "pending"), ("Submitted owner", "pending"),
        ], sections={"Evidence": "none"}, resolution="Integration fix applied; tests reset.")
        move_branch(root, f"biq/{run.run[0].lower()}/{run.run[1]}", commit)
        publish_candidate(root, run.item_id, commit)
    return outcome


# --- accounting (ported from the previous tool: same snapshot shape, rate card, arithmetic, and receipt lines) ---

USAGE_RATE_CARD_PATH = "docs/work-queue/model-token-rates.json"
USAGE_EVIDENCE_PREFIX = "Work-queue usage v1: "
SUBSTANTIVE_WORK_PROVENANCE_PREFIX = "Work-queue substantive work provenance v1: "
USAGE_COUNTER_FIELDS = ("input_tokens", "cached_input_tokens", "cache_write_input_tokens", "output_tokens", "reasoning_output_tokens", "total_tokens")
USAGE_LONG_COUNTER_FIELDS = ("long_input_tokens", "long_cached_input_tokens", "long_cache_write_input_tokens", "long_output_tokens")
USAGE_MODEL_COUNTER_FIELDS = USAGE_COUNTER_FIELDS + USAGE_LONG_COUNTER_FIELDS


def zero_usage_counters(*, model: bool = False) -> dict[str, int]:
    return {field: 0 for field in (USAGE_MODEL_COUNTER_FIELDS if model else USAGE_COUNTER_FIELDS)}


def usage_counter_map(value: object, fields: tuple[str, ...], source: str) -> dict[str, int]:
    if not isinstance(value, dict) or set(value) != set(fields):
        raise BiqError(f"{source}: usage counters must contain exactly {', '.join(fields)}")
    result = {}
    for field in fields:
        count = value[field]
        if type(count) is not int or count < 0:
            raise BiqError(f"{source}: usage {field} must be a nonnegative integer")
        result[field] = count
    if result["cached_input_tokens"] + result["cache_write_input_tokens"] > result["input_tokens"]:
        raise BiqError(f"{source}: cached and cache-write input overlap total input")
    if result["reasoning_output_tokens"] > result["output_tokens"]:
        raise BiqError(f"{source}: reasoning output exceeds total output")
    if result["total_tokens"] != result["input_tokens"] + result["output_tokens"]:
        raise BiqError(f"{source}: total tokens must equal input plus output")
    if fields == USAGE_MODEL_COUNTER_FIELDS:
        for long_field, total_field in (("long_input_tokens", "input_tokens"), ("long_cached_input_tokens", "cached_input_tokens"), ("long_cache_write_input_tokens", "cache_write_input_tokens"), ("long_output_tokens", "output_tokens")):
            if result[long_field] > result[total_field]:
                raise BiqError(f"{source}: {long_field} exceeds {total_field}")
    return result


def validate_usage_snapshot(value: object, source: str) -> dict:
    required = {"schema", "provider", "source_id", "observed_at", "counters", "models"}
    if not isinstance(value, dict) or set(value) != required or value.get("schema") != 1:
        raise BiqError(f"{source}: malformed work-queue usage snapshot (schema 1 with provider, source_id, observed_at, counters, models)")
    for field in ("provider", "source_id", "observed_at"):
        if not isinstance(value[field], str) or not value[field].strip():
            raise BiqError(f"{source}: usage snapshot {field} must be non-empty")
    validation_time(value["observed_at"])
    counters = usage_counter_map(value["counters"], USAGE_COUNTER_FIELDS, source)
    if not isinstance(value["models"], dict) or not value["models"]:
        raise BiqError(f"{source}: usage snapshot models must be non-empty")
    for model, raw in value["models"].items():
        if isinstance(raw, dict) and set(raw) != set(USAGE_MODEL_COUNTER_FIELDS):
            raise BiqError(f"{source}: usage snapshot model {model} must carry exactly these counters: {', '.join(USAGE_MODEL_COUNTER_FIELDS)}")
    models = {}
    for model, raw in value["models"].items():
        if not isinstance(model, str) or not model.strip() or "," in model:
            raise BiqError(f"{source}: usage snapshot has invalid model ID")
        models[model] = usage_counter_map(raw, USAGE_MODEL_COUNTER_FIELDS, f"{source} model {model}")
    for field in USAGE_COUNTER_FIELDS:
        if sum(m[field] for m in models.values()) != counters[field]:
            raise BiqError(f"{source}: per-model {field} does not reconcile with total")
    return {"schema": 1, "provider": value["provider"], "source_id": value["source_id"], "observed_at": value["observed_at"], "counters": counters, "models": models}


def load_usage_snapshot_file(path: str, source: str) -> dict:
    try:
        return validate_usage_snapshot(json.loads(Path(path).read_text(encoding="utf-8")), source)
    except (OSError, json.JSONDecodeError) as error:
        raise BiqError(f"{source}: cannot read usage snapshot {path}: {error}")


def usage_delta(baseline: dict, cutoff: dict, *, allow_zero: bool = False) -> dict:
    for field in ("provider", "source_id"):
        if baseline[field] != cutoff[field]:
            raise BiqError(f"usage cutoff {field} does not match claim baseline")
    if validation_time(cutoff["observed_at"]) < validation_time(baseline["observed_at"]):
        raise BiqError("usage cutoff precedes claim baseline")
    counters = {f: int(cutoff["counters"][f]) - int(baseline["counters"][f]) for f in USAGE_COUNTER_FIELDS}
    if any(v < 0 for v in counters.values()):
        raise BiqError("usage cutoff counters precede claim baseline")
    models = {}
    for model in sorted(set(baseline["models"]) | set(cutoff["models"])):
        before = baseline["models"].get(model, zero_usage_counters(model=True)); after = cutoff["models"].get(model, zero_usage_counters(model=True))
        delta = {f: int(after[f]) - int(before[f]) for f in USAGE_MODEL_COUNTER_FIELDS}
        if any(v < 0 for v in delta.values()):
            raise BiqError(f"usage cutoff counters for {model} precede baseline")
        if delta["total_tokens"]:
            usage_counter_map(delta, USAGE_MODEL_COUNTER_FIELDS, f"usage delta {model}"); models[model] = delta
    if counters["total_tokens"] == 0 and not allow_zero:
        raise BiqError("measured usage interval is zero; use --human-only or --charged-to only when that attribution is true")
    for field in USAGE_COUNTER_FIELDS:
        if sum(m[field] for m in models.values()) != counters[field]:
            raise BiqError(f"per-model usage delta does not reconcile for {field}")
    return {"counters": counters, "models": models}


def load_usage_rate_card(root: Path) -> dict:
    path = root / USAGE_RATE_CARD_PATH
    try:
        raw = path.read_bytes(); value = json.loads(raw)
    except (OSError, json.JSONDecodeError) as error:
        raise BiqError(f"cannot read usage rate card {path}: {error}")
    if not isinstance(value, dict) or value.get("schema") != 1 or not isinstance(value.get("basis"), str) or not isinstance(value.get("models"), dict):
        raise BiqError(f"malformed usage rate card: {path}")
    return {**value, "path": USAGE_RATE_CARD_PATH, "sha256": hashlib.sha256(raw).hexdigest()}


def usage_rates_for_models(root: Path, models: set[str]) -> dict:
    card = load_usage_rate_card(root)
    missing = sorted(models - set(card["models"]))
    if missing:
        raise BiqError("usage rate card has no entry for: " + ", ".join(missing) + f"; update {USAGE_RATE_CARD_PATH} from an authoritative price source")
    return {"basis": card["basis"], "path": card["path"], "sha256": card["sha256"], "models": {m: card["models"][m] for m in sorted(models)}}


def estimate_usage_cost(usage: dict, rates: dict, source: str) -> str:
    from decimal import Decimal, ROUND_HALF_UP
    def rate(value, field):
        try:
            result = Decimal(str(value))
        except Exception as error:
            raise BiqError(f"{source}: invalid {field} rate") from error
        if result < 0:
            raise BiqError(f"{source}: {field} rate must be nonnegative")
        return result
    total = Decimal(0); million = Decimal(1_000_000)
    required = {"input_per_million_usd", "cached_input_per_million_usd", "output_per_million_usd", "cache_write_multiplier", "long_context_threshold_input_tokens", "long_context_input_multiplier", "long_context_output_multiplier", "source"}
    for model, raw in usage["models"].items():
        counts = usage_counter_map(raw, USAGE_MODEL_COUNTER_FIELDS, f"{source} model {model}")
        r = rates["models"].get(model)
        if not isinstance(r, dict) or set(r) != required or type(r["long_context_threshold_input_tokens"]) is not int:
            raise BiqError(f"{source}: malformed rates for {model}")
        input_rate, cached_rate, output_rate = rate(r["input_per_million_usd"], "input"), rate(r["cached_input_per_million_usd"], "cached input"), rate(r["output_per_million_usd"], "output")
        cache_write, long_in, long_out = rate(r["cache_write_multiplier"], "cache write multiplier"), rate(r["long_context_input_multiplier"], "long input multiplier"), rate(r["long_context_output_multiplier"], "long output multiplier")
        uncached = counts["input_tokens"] - counts["cached_input_tokens"] - counts["cache_write_input_tokens"]
        long_uncached = counts["long_input_tokens"] - counts["long_cached_input_tokens"] - counts["long_cache_write_input_tokens"]
        standard_uncached = uncached - long_uncached
        standard_cached = counts["cached_input_tokens"] - counts["long_cached_input_tokens"]
        standard_cache_write = counts["cache_write_input_tokens"] - counts["long_cache_write_input_tokens"]
        standard_output = counts["output_tokens"] - counts["long_output_tokens"]
        if min(standard_uncached, standard_cached, standard_cache_write, standard_output, long_uncached) < 0:
            raise BiqError(f"{source}: invalid long-context split for {model}")
        total += (
            (Decimal(standard_uncached) + Decimal(long_uncached) * long_in) * input_rate
            + (Decimal(standard_cached) + Decimal(counts["long_cached_input_tokens"]) * long_in) * cached_rate
            + (Decimal(standard_cache_write) + Decimal(counts["long_cache_write_input_tokens"]) * long_in) * input_rate * cache_write
            + (Decimal(standard_output) + Decimal(counts["long_output_tokens"]) * long_out) * output_rate
        ) / million
    return f"${total.quantize(Decimal('0.000001'), rounding=ROUND_HALF_UP):f}"


def usage_evidence(root: Path, claim: dict, item_id: str, *, usage_snapshot: str | None, charged_to: str | None, human_only: bool, packets: dict[str, Packet]) -> tuple[dict, str, str, str] | None:
    """(evidence, Tokens Used, Model Used, Estimated Cost) for this submission, or None when nothing was declared."""
    if charged_to is not None:
        if usage_snapshot or human_only:
            raise BiqError("--charged-to cannot be combined with --usage-snapshot or --human-only")
        if charged_to == item_id or charged_to not in packets:
            raise BiqError("--charged-to must name a different durable WQE")
        return {"schema": 1, "mode": "charged-to", "item": charged_to}, "0", "none", "$0"
    if human_only:
        if usage_snapshot:
            raise BiqError("a human-only submission cannot accept a model usage cutoff")
        return {"schema": 1, "mode": "human-only"}, "0", "none", "$0"
    if usage_snapshot is None:
        return None
    baseline = claim.get("usage_baseline")
    if baseline is None:
        raise BiqError(f"{item_id}: the claim has no usage baseline; claim --usage-snapshot records it at claim start")
    baseline = validate_usage_snapshot(baseline, f"claim baseline for {item_id}")
    cutoff = load_usage_snapshot_file(usage_snapshot, f"usage cutoff for {item_id}")
    usage = usage_delta(baseline, cutoff)
    rates = usage_rates_for_models(root, set(usage["models"]))
    evidence = {"schema": 1, "mode": "measured", "baseline": baseline, "cutoff": cutoff, "delegates": [], "delegate_segments": [], "usage": usage, "rates": rates,
                "estimated_cost": estimate_usage_cost(usage, rates, "usage closeout")}
    return evidence, str(usage["counters"]["total_tokens"]), ", ".join(sorted(usage["models"])), evidence["estimated_cost"]


def provenance_line(root: Path, item_id: str, claim: dict | None, role: str, recorded_at: str) -> str:
    human, _user, _host = packet_identity(root)
    data = {"schema": 1, "item": item_id, "role": role, "human_owner": (claim or {}).get("human_owner") or human, "owner": (claim or {}).get("owner"),
            "claim_head": (claim or {}).get("head"), "recorded_at": recorded_at}
    return SUBSTANTIVE_WORK_PROVENANCE_PREFIX + json.dumps(data, sort_keys=True, separators=(",", ":"))


# Receipt readers validate durable evidence; they do not revive the old claim or
# recovery commands. Historical formats retain their local acceptance rules.
RECEIPT_BATCH_RE = re.compile(
    r"(?:WI-BATCH-INTEGRATION|WI-CI-INTEGRATION-BATCH)-[0-9]{8}-[0-9]{6}Z-ATTEMPT-[1-9][0-9]*|"
    r"(?:WI-BATCH-INTEGRATION|WI-CI-INTEGRATION-BATCH(?:-[A-Z0-9]+(?:-[A-Z0-9]+)*)?)-REPEAT-[1-9][0-9]*")
RECEIPT_ITEM_RE = re.compile(r"(?:WI|TD)-[A-Z0-9]+(?:-[A-Z0-9]+)*")
RECEIPT_RUN_PREFIX = "Work-queue proposal run authorization v1: "
RECEIPT_REJECTION_PREFIX = "Work-queue proposal rejection v1: "
RECEIPT_OWNER_LOSS_PREFIX = "Work-queue post-landing batch claimant-loss authorization v1: "
RECEIPT_AUTH_FIELDS = set("schema item authorizer authorized_at reason item_identity".split())
RECEIPT_OWNER_LOSS_FIELDS = set((
    "schema authorizer authorized_at aws_cost aws_cost_evidence batch claim_branch claim_head claim_host_user "
    "claim_hostname claim_human_owner claim_oid claim_owner claim_provider claim_source_id claim_started_at "
    "claim_worktree contact_attempted_at contact_evidence integration_commit outcome packet_set_sha256 recovery_item recovery_owner").split())
RECEIPT_CLAIM_FIELDS = set("item oid owner head started_at updated_at human_owner host_user hostname worktree branch provider source_id".split())
RECEIPT_UNAVAILABLE = ("cutoff", "delegates", "usage", "rates", "estimated_cost")


def receipt_require(condition: bool, message: str) -> None:
    if not condition:
        raise BiqError(message)


def receipt_text(value: object, *, concrete: bool = False, single_line: bool = False) -> bool:
    return (isinstance(value, str) and bool(value.strip())
            and (not concrete or value.strip().casefold() not in {
                "-", "none", "tbd", "unknown", "unassigned", "pending", "unrecorded", "unauthorized_preview_owner",
                "project_owner_assignment_required", "project_owner_architecture_decision_owner_assignment_required"})
            and (not single_line or value == value.strip() and len(value.splitlines()) == 1))


def receipt_time(value: object, *, canonical: bool = False) -> dt.datetime:
    suffix = "Z" if canonical else r"(?:Z|\+00:00)"
    receipt_require(isinstance(value, str) and re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}" + suffix, value) is not None,
                    "receipt timestamp must use whole-second UTC")
    return validation_time(value)


def receipt_objects(packet: Packet, prefix: str, *, fields: set[str] | None = None, canonical: bool = False, multiple: bool = False) -> list[dict]:
    lines = [line[len(prefix):] for line in packet.sections.get("Resolution", "").splitlines() if line.startswith(prefix)]
    receipt_require(multiple or len(lines) <= 1, f"duplicate {prefix.strip()}")
    values = []
    for line in lines:
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise BiqError(f"malformed {prefix.strip()} JSON") from error
        receipt_require(isinstance(value, dict) and value.get("schema") == 1, f"malformed {prefix.strip()} schema")
        if fields is not None:
            receipt_require(set(value) == fields and type(value["schema"]) is int, f"malformed {prefix.strip()} fields")
        if canonical:
            receipt_require(line == json.dumps(value, sort_keys=True, separators=(",", ":")), f"{prefix.strip()} must use canonical compact JSON")
        values.append(value)
    return values


def receipt_proposed_id(item_id: str) -> bool:
    if re.fullmatch(r"WI-PROPOSED-(?!PROPOSED(?:-|$))(?!ATTEMPT-[1-9][0-9]*$)(?!REPEAT-[1-9][0-9]*$)[A-Z0-9]+(?:-[A-Z0-9]+)*", item_id):
        return True
    return bool(re.fullmatch(r"WI-(?:CI|PRODUCT)-PROPOSED-(?:ARCHITECTURE|AUDIT|BUG|BUILD|DOCUMENTATION|FEATURE|INFRASTRUCTURE|INTEGRATION|PERFORMANCE|QUALIFICATION|REFACTOR|REPORTING|TEST|TOOLING)-[A-Z0-9]+(?:-[A-Z0-9]+)*", item_id))


def receipt_authorization(value: object, item_id: str, *, rejection: bool = False) -> dict:
    fields = (RECEIPT_AUTH_FIELDS - {"authorized_at"}) | {"outcome", "rejected_at"} if rejection else RECEIPT_AUTH_FIELDS
    receipt_require(isinstance(value, dict) and set(value) == fields and type(value.get("schema")) is int and value["schema"] == 1,
                    "malformed proposal authorization fields")
    receipt_require(value["item"] == item_id and concrete_human_name(value["authorizer"]) and receipt_text(value["reason"], single_line=True),
                    "malformed proposal authorization identity or authorizer")
    identity = value["item_identity"]
    receipt_require(isinstance(identity, str) and re.fullmatch(r"100644 blob (?:[0-9a-f]{40}|[0-9a-f]{64})\t" + re.escape(f"{ITEMS_DIR}/{item_id}.md"), identity) is not None,
                    "malformed proposal packet identity")
    receipt_time(value["rejected_at" if rejection else "authorized_at"])
    if rejection:
        receipt_require(value["outcome"] == "failed-rejected", "malformed proposal rejection outcome")
    return value


def receipt_add_usage(total: dict, addition: dict) -> None:
    for field in USAGE_COUNTER_FIELDS:
        total["counters"][field] += addition["counters"][field]
    for model, counts in addition["models"].items():
        target = total["models"].setdefault(model, zero_usage_counters(model=True))
        for field in USAGE_MODEL_COUNTER_FIELDS:
            target[field] += counts[field]


def receipt_delegate_segment(value: object) -> dict:
    receipt_require(isinstance(value, dict) and set(value) == {"schema", "claim_started_at", "accounting_cutoff_at", "baseline", "cutoff", "usage"} and value["schema"] == 1,
                    "malformed delegate usage segment")
    start, end = validation_time(value["claim_started_at"]), validation_time(value["accounting_cutoff_at"])
    baseline = validate_usage_snapshot(value["baseline"], "delegate baseline")
    cutoff = validate_usage_snapshot(value["cutoff"], "delegate cutoff")
    receipt_require(end >= start and validation_time(baseline["observed_at"]) <= start <= validation_time(cutoff["observed_at"]) <= end,
                    "delegate usage is outside its claim window")
    receipt_require(value["usage"] == usage_delta(baseline, cutoff, allow_zero=True), "stored delegate usage does not reconcile")
    return value


def receipt_measured_usage(value: dict, *, claim_segment: bool = False) -> dict:
    required = {"schema", "mode", "baseline", "cutoff", "delegates", "usage", "rates", "estimated_cost"}
    shapes = [required | {"delegate_segments"}] if claim_segment else [required, required - {"delegates"}, required | {"delegate_segments"}]
    receipt_require(set(value) in shapes and (not claim_segment or type(value["schema"]) is int), "malformed measured usage fields")
    baseline = validate_usage_snapshot(value["baseline"], "usage baseline")
    cutoff = validate_usage_snapshot(value["cutoff"], "usage cutoff")
    delegates, segments = value.get("delegates", []), value.get("delegate_segments", [])
    receipt_require(isinstance(delegates, list) and isinstance(segments, list), "usage delegates and segments must be lists")
    total = usage_delta(baseline, cutoff, allow_zero=True)
    sources = {cutoff["source_id"]}
    for delegate in delegates:
        snap = validate_usage_snapshot(delegate, "delegate usage")
        receipt_require(snap["source_id"] not in sources, "duplicate measured usage source")
        sources.add(snap["source_id"])
        receipt_add_usage(total, {"counters": snap["counters"], "models": {m: c for m, c in snap["models"].items() if any(c.values())}})
    for raw in segments:
        segment = receipt_delegate_segment(raw)
        receipt_require(validation_time(segment["accounting_cutoff_at"]) == validation_time(cutoff["observed_at"]), "delegate cutoff differs from root cutoff")
        source_id = segment["cutoff"]["source_id"]
        receipt_require(source_id not in sources, "duplicate measured usage source")
        sources.add(source_id)
        receipt_add_usage(total, segment["usage"])
    receipt_require(claim_segment or total["counters"]["total_tokens"] > 0, "measured usage interval is zero")
    receipt_require(value["usage"] == total, "stored usage delta does not reconcile")
    rates = value["rates"]
    receipt_require(isinstance(rates, dict) and isinstance(rates.get("models"), dict) and set(rates["models"]) == set(total["models"]),
                    "embedded usage and rate models differ")
    if claim_segment:
        receipt_require(set(rates) == {"basis", "path", "sha256", "models"} and receipt_text(rates["basis"])
                        and rates["path"] == USAGE_RATE_CARD_PATH and isinstance(rates["sha256"], str)
                        and re.fullmatch(r"[0-9a-f]{64}", rates["sha256"]) is not None, "malformed proposal usage rates")
    receipt_require(value["estimated_cost"] == estimate_usage_cost(total, rates, "embedded receipt"), "stored usage cost does not reconcile")
    return value


def receipt_claim_segments(value: dict) -> dict:
    from decimal import Decimal, ROUND_HALF_UP
    receipt_require(set(value) == {"schema", "mode", "item", "segments", "usage", "estimated_cost"} and type(value["schema"]) is int
                    and isinstance(value["item"], str) and receipt_proposed_id(value["item"]), "malformed claim-segmented usage")
    segments = value["segments"]
    receipt_require(isinstance(segments, list) and bool(segments), "claim-segmented usage needs segments")
    total, cost = {"counters": zero_usage_counters(), "models": {}}, Decimal(0)
    prior_end, claim_oids, source_cutoffs = None, set(), {}
    segment_fields = set("schema state claim_oid owner head started_at ended_at note actor run_authorization item_identity usage".split())
    for index, segment in enumerate(segments):
        receipt_require(isinstance(segment, dict) and set(segment) == segment_fields and type(segment["schema"]) is int and segment["schema"] == 1,
                        "malformed proposal claim segment")
        receipt_require(segment["state"] in ("released", "sealed") and (index == len(segments) - 1 or segment["state"] == "released")
                        and receipt_text(segment["owner"]) and receipt_text(segment["note"], single_line=True)
                        and all(isinstance(segment[f], str) and COMMIT_RE.fullmatch(segment[f]) for f in ("claim_oid", "head")), "invalid proposal claim segment fields")
        start, end = receipt_time(segment["started_at"]), receipt_time(segment["ended_at"])
        receipt_require(end >= start and (prior_end is None or start >= prior_end) and segment["claim_oid"] not in claim_oids,
                        "proposal claim segments overlap, repeat or are reordered")
        prior_end = end; claim_oids.add(segment["claim_oid"])
        actor = segment["actor"]
        receipt_require(isinstance(actor, dict) and set(actor) == {"human_owner", "host_user", "hostname"}
                        and all(receipt_text(v) for v in actor.values()) and concrete_human_name(actor["human_owner"]), "malformed proposal segment actor")
        authorization = receipt_authorization(segment["run_authorization"], value["item"])
        receipt_require(segment["item_identity"] == authorization["item_identity"], "segment identity differs from authorization")
        usage = segment["usage"]
        receipt_require(isinstance(usage, dict) and type(usage.get("schema")) is int and usage["schema"] == 1, "malformed proposal segment usage")
        if usage.get("mode") == "human-only":
            receipt_require(set(usage) == {"schema", "mode"}, "malformed human-only segment")
            continue
        receipt_require(usage.get("mode") == "measured", "invalid proposal segment usage mode")
        receipt_measured_usage(usage, claim_segment=True)
        receipt_require(validation_time(usage["baseline"]["observed_at"]) <= start <= validation_time(usage["cutoff"]["observed_at"]) <= end,
                        "proposal usage is outside its active segment")
        receipt_require(all(validation_time(d["claim_started_at"]) == start for d in usage["delegate_segments"]), "delegate claim bound differs from active segment start")
        sources = [(usage["baseline"], usage["cutoff"]), *[(None, d) for d in usage["delegates"]], *[(d["baseline"], d["cutoff"]) for d in usage["delegate_segments"]]]
        for baseline, cutoff in sources:
            source_id = cutoff["source_id"]
            if source_id in source_cutoffs:
                receipt_require(baseline is not None, "repeated cumulative proposal usage source")
                usage_delta(source_cutoffs[source_id], baseline, allow_zero=True)
            source_cutoffs[source_id] = cutoff
        receipt_add_usage(total, usage["usage"])
        receipt_require(isinstance(usage["estimated_cost"], str) and re.fullmatch(r"\$(?:0|[1-9][0-9]*)(?:\.[0-9]{1,6})?", usage["estimated_cost"]) is not None,
                        "malformed segment estimated cost")
        cost += Decimal(usage["estimated_cost"][1:])
    rounded = cost.quantize(Decimal("0.000001"), rounding=ROUND_HALF_UP)
    receipt_require(value["usage"] == total and value["estimated_cost"] == ("$0" if rounded == 0 else f"${rounded:f}"),
                    "claim-segmented usage total or cost does not reconcile")
    return value


def receipt_owner_loss_authorization(value: object) -> dict:
    receipt_require(isinstance(value, dict) and set(value) == RECEIPT_OWNER_LOSS_FIELDS and type(value["schema"]) is int and value["schema"] == 1,
                    "malformed owner-loss authorization fields")
    receipt_require(all(receipt_text(value[f]) for f in RECEIPT_OWNER_LOSS_FIELDS - {"schema"}) and RECEIPT_BATCH_RE.fullmatch(value["batch"])
                    and RECEIPT_ITEM_RE.fullmatch(value["recovery_item"]) and not RECEIPT_BATCH_RE.fullmatch(value["recovery_item"])
                    and all(COMMIT_RE.fullmatch(value[f]) for f in ("claim_oid", "claim_head", "integration_commit"))
                    and re.fullmatch(r"[0-9a-f]{64}", value["packet_set_sha256"]) and value["outcome"] in ("completed", "failed-rejected")
                    and re.fullmatch(r"\$(?:0|[1-9][0-9]*)(?:\.[0-9]{1,6})?", value["aws_cost"]) and Path(value["claim_worktree"]).is_absolute()
                    and concrete_human_name(value["authorizer"]) and all(receipt_text(value[f], concrete=True) for f in ("aws_cost_evidence", "contact_evidence")),
                    "malformed owner-loss authorization values")
    receipt_time(value["authorized_at"], canonical=True); receipt_time(value["contact_attempted_at"], canonical=True)
    receipt_time(value["claim_started_at"])
    return value


def receipt_usage(value: dict) -> dict:
    mode = value.get("mode")
    if mode == "measured":
        return receipt_measured_usage(value)
    if mode == "claim-segmented":
        return receipt_claim_segments(value)
    if mode == "human-only":
        receipt_require(set(value) == {"schema", "mode"}, "malformed human-only usage")
    elif mode == "charged-to":
        receipt_require(set(value) == {"schema", "mode", "item"} and isinstance(value["item"], str) and RECEIPT_ITEM_RE.fullmatch(value["item"]), "malformed charged-to usage")
    elif mode == "owner-loss-unavailable":
        receipt_require(set(value) == {"schema", "mode", "baseline", "claim", "authorization", "unavailable_reason", *RECEIPT_UNAVAILABLE}
                        and type(value["schema"]) is int, "malformed owner-loss usage")
        baseline = validate_usage_snapshot(value["baseline"], "owner-loss baseline")
        claim = value["claim"]
        receipt_require(isinstance(claim, dict) and set(claim) == RECEIPT_CLAIM_FIELDS and all(receipt_text(v) for v in claim.values()), "malformed owner-loss claim")
        receipt_require(RECEIPT_BATCH_RE.fullmatch(claim["item"]) and all(COMMIT_RE.fullmatch(claim[f]) for f in ("oid", "head"))
                        and Path(claim["worktree"]).is_absolute() and all(claim[f] == baseline[f] for f in ("provider", "source_id")), "malformed owner-loss claim values")
        receipt_time(claim["started_at"]); receipt_time(claim["updated_at"])
        authorization = receipt_owner_loss_authorization(value["authorization"])
        receipt_require(all(value[f] == "unavailable" for f in RECEIPT_UNAVAILABLE)
                        and value["unavailable_reason"] == "claimant-and-usage-source-unavailable", "invalid owner-loss unavailable fields")
        bindings = {"item": "batch", "oid": "claim_oid", **{f: "claim_" + f for f in RECEIPT_CLAIM_FIELDS - {"item", "oid", "updated_at"}}}
        receipt_require(all(claim[f] == authorization[target] for f, target in bindings.items()), "owner-loss claim differs from authorization")
        return {**value, "baseline": baseline}
    else:
        raise BiqError(f"unsupported usage evidence mode {mode!r}")
    return value


def receipt_quarantine_disposition(packet: Packet) -> dict | None:
    """Passive authorizer exception needed by historical proposal accounting."""
    fields = set("schema container decision authorizer decided_at group_sha256 prepared_commit packet_set_sha256".split())
    values = receipt_objects(packet, "Work-queue quarantine disposition v1: ", fields=fields, canonical=True)
    if not values:
        return None
    value = values[0]
    receipt_require(value["container"] == packet.item_id and value["decision"] in ("accepted", "rejected") and concrete_human_name(value["authorizer"])
                    and all(isinstance(value[f], str) and re.fullmatch(r"[0-9a-f]{64}", value[f]) for f in ("group_sha256", "packet_set_sha256"))
                    and isinstance(value["prepared_commit"], str)
                    and (bool(COMMIT_RE.fullmatch(value["prepared_commit"])) if value["decision"] == "accepted" else value["prepared_commit"] == "none"),
                    "malformed quarantine accounting disposition")
    receipt_time(value["decided_at"])
    return value


def receipt_original_candidate(packet: Packet) -> str | None:
    """Read a passive historical reseal annotation, without its recovery protocol."""
    values = receipt_objects(packet, "Work-queue submitted candidate reseal v1: ", canonical=True) if packet.fields.get("Task packet") == "3" else []
    if not values:
        return packet.fields.get("Candidate commit")
    value = values[0]
    receipt_require(set(value) == set("schema item recovery_item recovery_owner original_base original_candidate replacement_base replacement_candidate recovered_at".split())
                    and all(isinstance(value[f], str) and RECEIPT_ITEM_RE.fullmatch(value[f]) for f in ("item", "recovery_item"))
                    and receipt_text(value["recovery_owner"]) and value["recovery_owner"] not in {"pending", "none"}
                    and "`" not in value["recovery_owner"] and "\n" not in value["recovery_owner"]
                    and all(isinstance(value[f], str) and COMMIT_RE.fullmatch(value[f]) for f in ("original_base", "original_candidate", "replacement_base", "replacement_candidate")),
                    "malformed submitted-candidate reseal evidence")
    receipt_time(value["recovered_at"], canonical=True)
    return value["original_candidate"]


def receipt_proposal_bindings(packet: Packet, usage: dict | None) -> None:
    runs = [receipt_authorization(v, packet.item_id) for v in receipt_objects(packet, RECEIPT_RUN_PREFIX, fields=RECEIPT_AUTH_FIELDS, canonical=True, multiple=True)]
    rejection_fields = (RECEIPT_AUTH_FIELDS - {"authorized_at"}) | {"outcome", "rejected_at"}
    rejections = [receipt_authorization(v, packet.item_id, rejection=True) for v in receipt_objects(packet, RECEIPT_REJECTION_PREFIX, fields=rejection_fields, canonical=True)]
    proposed = receipt_proposed_id(packet.item_id)
    receipt_require(proposed or not (runs or rejections), "proposal accounting receipts require a proposed item")
    if not proposed or not (usage or runs or rejections):
        return
    fields = packet.fields
    segments = usage["segments"] if usage and usage["mode"] == "claim-segmented" else []
    disposition = receipt_quarantine_disposition(packet)
    def authorized(authorizer, decision):
        return authorizer == fields.get("Accountable owner") or (disposition is not None and disposition["decision"] == decision and disposition["authorizer"] == authorizer)
    if segments:
        starts = [receipt_time(s["started_at"]) for s in segments]
        duration = sum(int((receipt_time(s["ended_at"]) - start).total_seconds()) for s, start in zip(segments, starts))
        lifecycle = fields.get("Claimed At") == min(starts).strftime("%Y-%m-%dT%H:%M:%SZ") and fields.get("Implementation Duration") == f"PT{duration}S"
    else:
        lifecycle = False
    if rejections:
        rejection = rejections[0]
        preclaim = (not runs and fields.get("Claimed At") == fields.get("Implementation Duration") == "none"
                    and usage == {"schema": 1, "mode": "human-only"})
        released = bool(segments) and lifecycle and runs == [s["run_authorization"] for s in segments] and all(s["state"] == "released" for s in segments)
        receipt_require(packet.status == "failed-rejected" and authorized(rejection["authorizer"], "rejected")
                        and receipt_time(rejection["rejected_at"]).strftime("%Y-%m-%dT%H:%M:%SZ") == fields.get("Closed At")
                        and fields.get("Completed human_owner") == rejection["authorizer"] and fields.get("Estimated AWS Cost") == "$0"
                        and all(fields.get(f) == "none" for f in ("Candidate base", "Candidate commit", "Submitted At", "Submitted owner", "Decision owner"))
                        and (preclaim or released), "proposal rejection does not match released history and packet fields")
        created = fields.get("Created")
        if created:
            created_at = dt.datetime.combine(dt.date.fromisoformat(created), dt.time(), dt.timezone.utc) if re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", created) else validation_time(created)
            receipt_require(created_at <= receipt_time(fields["Closed At"]), "Created follows proposal rejection")
    elif packet.status in {"submitted", *TERMINAL}:
        endpoint = fields.get("Submitted At") if fields.get("Submitted At") not in {None, "pending", "none"} else fields.get("Closed At")
        receipt_require(bool(runs) and bool(segments) and runs == [s["run_authorization"] for s in segments] and lifecycle
                        and receipt_time(segments[-1]["ended_at"]).strftime("%Y-%m-%dT%H:%M:%SZ") == endpoint,
                        "proposal usage does not match ordered runs and lifecycle")
    else:
        receipt_require(not runs, "unsealed proposal carries run receipts")
    if runs:
        receipt_require(authorized(runs[-1]["authorizer"], "accepted"), "latest proposal run authorizer differs from Accountable owner")


def receipt_validation_errors(packet: Packet) -> list[str]:
    """Validate present supported receipts without claims, files or transitions."""
    try:
        values = receipt_objects(packet, USAGE_EVIDENCE_PREFIX)
        usage = receipt_usage(values[0]) if values else None
        if usage and usage["mode"] == "owner-loss-unavailable":
            receipt_objects(packet, USAGE_EVIDENCE_PREFIX, canonical=True)
            receipt_require(json.dumps(values[0], sort_keys=True, separators=(",", ":")) == json.dumps(usage, sort_keys=True, separators=(",", ":")),
                            "owner-loss usage must use canonical normalized JSON")
        if packet.fields.get("Task packet") not in {"2", "3"}:
            return []
        fields = packet.fields
        batch = RECEIPT_BATCH_RE.fullmatch(packet.item_id) is not None
        if usage:
            receipt_require(packet.status in {"submitted", *TERMINAL}, "only a sealed item may record usage evidence")
            mode = usage["mode"]
            if mode in {"measured", "claim-segmented"}:
                expected = (str(usage["usage"]["counters"]["total_tokens"]), ", ".join(sorted(usage["usage"]["models"])) or "none", usage["estimated_cost"])
                if mode == "claim-segmented":
                    receipt_require(receipt_proposed_id(packet.item_id) and usage["item"] == packet.item_id, "claim-segmented usage names another item")
                elif usage.get("delegate_segments"):
                    start = receipt_time(fields.get("Claimed At"))
                    receipt_require(all(validation_time(s["claim_started_at"]) == start for s in usage["delegate_segments"]), "delegate claim bound differs from Claimed At")
            elif mode == "owner-loss-unavailable":
                receipt_require(batch and packet.status in TERMINAL and fields.get("Estimated AWS Cost") == usage["authorization"]["aws_cost"], "owner-loss usage requires terminal batch and authorized AWS summary")
                expected = ("unavailable",) * 3
            else:
                expected = ("0", "none", "$0")
                receipt_require(mode != "charged-to" or usage["item"] != packet.item_id, "usage cannot be charged to the same WQE")
            receipt_require(tuple(fields.get(f) for f in ("Tokens Used", "Model Used", "Estimated Cost")) == expected, "accounting summary does not match usage receipt")
        elif all(fields.get(f) == "unavailable" for f in ("Tokens Used", "Model Used", "Estimated Cost")):
            raise BiqError("unavailable accounting requires owner-loss evidence")
        provenance = receipt_objects(packet, SUBSTANTIVE_WORK_PROVENANCE_PREFIX, canonical=True)
        if provenance:
            value = provenance[0]
            receipt_require(set(value) == {"schema", "item", "role", "human_owner", "owner", "claim_head", "recorded_at"}
                            and concrete_human_name(value["human_owner"]) and receipt_text(value["owner"], concrete=True, single_line=True)
                            and "`" not in value["owner"] and isinstance(value["claim_head"], str) and COMMIT_RE.fullmatch(value["claim_head"]), "malformed substantive-work provenance")
            receipt_time(value["recorded_at"], canonical=True)
            submitted = fields.get("Submitted At") not in {None, "pending", "none"}
            receipt_require(packet.status in {"submitted", *TERMINAL} and fields.get("Claimed At") != "none" and value["item"] == packet.item_id
                            and value["role"] == ("integration" if batch else "implementation")
                            and value["recorded_at"] == fields.get("Submitted At" if submitted else "Closed At")
                            and (not submitted or value["owner"] == fields.get("Submitted owner")), "substantive-work provenance does not match packet identity or lifecycle")
            if submitted:
                expected_head = receipt_original_candidate(packet)
                receipt_require(packet_has_biq_admission(packet) or value["claim_head"] == expected_head,
                                "substantive-work provenance does not match submitted claim head")
        authorizations = receipt_objects(packet, RECEIPT_OWNER_LOSS_PREFIX, canonical=True)
        if authorizations:
            value = receipt_owner_loss_authorization(authorizations[0])
            receipt_require(not batch and packet.status in {"open", *TERMINAL} and fields.get("Architecture gate") == "accepted"
                            and value["recovery_item"] == packet.item_id and value["authorizer"] == fields.get("Accountable owner"), "owner-loss authorization does not match recovery WQE")
        receipt_proposal_bindings(packet, usage)
        return []
    except (BiqError, ValueError, TypeError, KeyError, ArithmeticError) as error:
        return [f"{packet.item_id}: {error}"]


def append_resolution_lines(root: Path, packets: dict[str, Packet], item_id: str, lines: list[str]) -> None:
    packet = packets[item_id]
    body = packet.sections.get("Resolution", "").strip()
    # A resubmission replaces its earlier receipt of the same kind (the text up to
    # the first colon) instead of stacking a duplicate the report's validator refuses.
    kinds = {line.split(":", 1)[0] + ":" for line in lines if ":" in line}
    kept = [line for line in body.splitlines() if not any(line.startswith(kind) for kind in kinds)]
    body = "\n".join(kept).strip()
    while "\n\n\n" in body:
        body = body.replace("\n\n\n", "\n\n")
    body = ("" if body in {"", "pending"} else body + "\n\n") + "\n".join(lines)
    write_packet(root, packets, item_id, sections={"Resolution": body})


def excluded_members(run: Packet) -> dict[str, str]:
    """Item -> reason from the run's Excluded section."""
    out = {}
    for line in run.sections.get("Excluded", "").splitlines():
        m = re.match(r"^- (\S+): (.*)$", line.strip())
        if m and m.group(1) != "none":
            out[m.group(1)] = m.group(2)
    return out


def resolve_instructions(queue_id: str, ordinal: int, item: str, candidate: str, owner: str | None) -> str:
    return (f"the integrator resolves it: git checkout biq/{queue_id.lower()}/{ordinal} && git merge {candidate[:12]} (resolve, commit), "
            f"then python3 tools/biq.py run resolve {queue_id} {ordinal} {item} --commit HEAD --owner {owner or '<you>'} --apply")


def resolve_member(root: Path, registry: Registry, packets: dict[str, Packet], run: Packet, item_id: str, commit: str, *, owner: str | None, apply: bool) -> dict:
    """Record the integrator's hand-resolved merge of a collided member onto the run; the run's tests reset."""
    if run_state(run) != "started":
        raise BiqError(f"{run.item_id} is {run_state(run)}; a member is resolved on a started run")
    excluded = excluded_members(run)
    pending = pending_resolutions(run)
    if item_id not in pending and item_id not in excluded:
        raise BiqError(f"{item_id} awaits no resolution in {run.item_id}: " + (", ".join(sorted(pending) + sorted(excluded)) or "nothing is pending"))
    packet = require_packet(packets, item_id)
    candidate = packet.candidate
    if candidate is None:
        raise BiqError(f"{item_id} has no sealed candidate")
    commit = rev(root, commit)
    parents = git(root, "rev-list", "--parents", "-n", "1", commit).split()[1:]
    integration = run.fields.get("Integration commit", "pending")
    if parents != [integration, candidate]:
        raise BiqError(f"{commit[:12]} must be a merge of {item_id}'s candidate {candidate[:12]} onto {run.item_id}'s integration commit {integration[:12]} (parents: " + ", ".join(p[:12] for p in parents) + ")")
    queue_id, ordinal = run.run
    queue = registry.queues[queue_id]
    outcome = {"item": run.item_id, "member": item_id, "integration": commit, "tests": list(queue.tests)}
    if apply:
        require_claim(root, run.item_id, owner)
        resolved_note = f"resolved by {owner} as {commit[:12]}"
        if item_id in pending:
            members = [(i, sha, merge_member_notes(note, resolved_note) if i == item_id else note) for i, sha, note in run.members()]
        else:
            members = run.members() + [(item_id, candidate, resolved_note)]
        remaining = {k: v for k, v in excluded.items() if k != item_id}
        write_packet(root, packets, run.item_id, [("Integration commit", commit), ("Tests", ", ".join(f"{t}=pending" for t in queue.tests) or "pending")],
                     sections={"Members": member_lines(members), "Excluded": "\n".join(f"- {k}: {v}" for k, v in remaining.items()) or "none", "Evidence": "none"})
        move_branch(root, f"biq/{queue_id.lower()}/{ordinal}", commit)
        publish_candidate(root, run.item_id, commit)
        successor = open_run(packets, queue_id)
        if any(m[0] == item_id for m in successor.members()):
            write_packet(root, packets, successor.item_id, sections={"Members": member_lines([m for m in successor.members() if m[0] != item_id])})
    return outcome


def cancel_run(root: Path, registry: Registry, packets: dict[str, Packet], run: Packet, *, owner: str | None, apply: bool) -> dict:
    """Undo a start: the run returns to open with its members and the successor's, and the successor is removed."""
    if run_state(run) != "started":
        raise BiqError(f"{run.item_id} is {run_state(run)}; only a started run is cancelled")
    queue_id, ordinal = run.run
    queue = registry.queues[queue_id]
    successor = open_run(packets, queue_id)
    if successor.run[1] != ordinal + 1:
        raise BiqError(f"{run.item_id}: the open run is {successor.item_id}, not its successor")
    seen = {m[0] for m in run.members()}
    members = [(i, s, "") for i, s, _n in run.members()] + [(i, s, "") for i, s, _n in successor.members() if i not in seen]
    outcome = {"item": run.item_id, "members": [m[0] for m in members], "removed": successor.item_id}
    if apply:
        require_claim(root, run.item_id, owner)
        write_packet(root, packets, run.item_id, [
            ("Started At", "pending"), ("Claimed At", "pending"), ("Base", "pending"), ("Predecessor", "pending"), ("Integration commit", "pending"),
            ("Result commit", "pending"), ("Tests", ", ".join(f"{t}=pending" for t in queue.tests) or "pending"), ("Cost", "$0"),
        ], sections={"Members": member_lines(members), "Excluded": "none", "Evidence": "none"})
        (root / ITEMS_DIR / f"{successor.item_id}.md").unlink()
        packets.pop(successor.item_id, None)
        subprocess.run(["git", "branch", "-D", f"biq/{queue_id.lower()}/{ordinal}"], cwd=root, capture_output=True)
    return outcome


def find_run(packets: dict[str, Packet], queue_id: str, ordinal: int) -> Packet:
    matches = [p for p in runs_of(packets, queue_id) if p.run[1] == ordinal]
    if len(matches) != 1:
        raise BiqError(f"{queue_id} run {ordinal}: expected one run WQE, found {len(matches)}")
    return matches[0]


def record_test(root: Path, packets: dict[str, Packet], run: Packet, name: str, result: str, evidence: str | None, cost_usd: float, *, owner: str | None) -> Packet:
    """A test result is the integrator's: recorded only under the run's claim."""
    if run_state(run) != "started":
        raise BiqError(f"{run.item_id} is {run_state(run)}; tests are recorded on a started run")
    require_claim(root, run.item_id, owner)
    require_resolved(run)
    require_reviewed(run)
    tests = run.tests
    if name not in tests:
        raise BiqError(f"{run.item_id} does not run {name}")
    if result not in {"green", "red"}:
        raise BiqError("result must be green or red")
    total = parse_usd(run.fields.get("Cost", "$0")) + cost_usd
    budget = parse_usd(run.fields.get("Budget", "$0"))
    if total > budget + 1e-9:
        raise BiqError(f"{run.item_id} cost {usd(total)} would exceed its budget {usd(budget)}")
    tests[name] = result
    evidence_lines = run.sections.get("Evidence", "").strip()
    line = f"- {name}: {result}" + (f", {usd(cost_usd)}" if cost_usd else "") + (f", `{evidence}`" if evidence else "")
    return write_packet(root, packets, run.item_id, [("Tests", ", ".join(f"{n}={r}" for n, r in tests.items())), ("Cost", usd(total))],
                        sections={"Evidence": (evidence_lines + "\n" if evidence_lines and evidence_lines != "none" else "") + line})


REVIEW_OK_MARK = "reviewed ok by "
RESOLUTION_PENDING_MARK = "resolution pending: "


def pending_resolutions(run: Packet) -> dict[str, str]:
    """Members that collided with the batch and await the integrator's hand merge: item -> reason."""
    return {item: note[len(RESOLUTION_PENDING_MARK):] for item, _sha, note in run.members() if (note or "").startswith(RESOLUTION_PENDING_MARK)}


def require_resolved(run: Packet) -> None:
    """Merge conflicts are the integrator's to resolve; nothing is tested or completed around a member."""
    pending = pending_resolutions(run)
    if pending:
        queue_id, ordinal = run.run
        raise BiqError(f"{run.item_id} has members awaiting resolution: " + ", ".join(sorted(pending))
                       + f"; resolve each (git checkout biq/{queue_id.lower()}/{ordinal} && git merge <candidate>, resolve, commit, then run resolve {queue_id} {ordinal} <ID> --commit HEAD --owner <you> --apply)")


def reviewed_ok(note: str) -> bool:
    return REVIEW_OK_MARK in (note or "")


def unreviewed_members(run: Packet) -> list[str]:
    return [item for item, _sha, note in run.members() if not reviewed_ok(note)]


def require_reviewed(run: Packet) -> None:
    """No member is tested or integrated before the integrator has looked at it."""
    pending = unreviewed_members(run)
    if pending:
        queue_id, ordinal = run.run
        raise BiqError(f"{run.item_id} has members not yet reviewed: {', '.join(pending)}; look at each with "
                       f"`run review {queue_id} {ordinal}` and record `run review {queue_id} {ordinal} <ID> --ok|--reject <reason>|--hold <question>`")


def member_diagnostics(root: Path, packets: dict[str, Packet], item: str, sha: str, target_tip: str) -> dict:
    """What the integrator looks at: the member's footprint from its merge base, and anything the seal guard would refuse."""
    ensure_commit(root, sha)
    base = git(root, "merge-base", sha, target_tip)
    footprint = candidate_footprint(root, base, sha)
    problem = None
    try:
        check_candidate_history(root, item, base, sha, packets)
    except BiqError as error:
        problem = str(error)
    return {"item": item, "sha": sha, "base": base, "footprint": footprint, "problem": problem}


def review_lines(root: Path, packets: dict[str, Packet], run: Packet, target_tip: str) -> list[str]:
    lines = []
    for item, sha, note in run.members():
        d = member_diagnostics(root, packets, item, sha, target_tip)
        fp = d["footprint"]
        verdict = "reviewed ok" if reviewed_ok(note) else "NOT REVIEWED"
        lines.append(f"{item} @{sha[:12]}: {verdict}; {footprint_line(fp)}; merge base {d['base'][:12]}")
        for path in fp["deleted"][:20]:
            lines.append(f"    deletes {path}")
        if len(fp["deleted"]) > 20:
            lines.append(f"    ... {len(fp['deleted']) - 20} more deletions")
        if d["problem"]:
            lines.append(f"    PROBLEM: {d['problem']}")
    for item, reason in excluded_members(run).items():
        lines.append(f"{item}: excluded ({reason})")
    return lines


def plan_shed_members(root: Path, packets: dict[str, Packet], run: Packet, reasons: dict[str, str]) -> dict:
    """Compute every dependency and merge result before a shed transition writes state."""
    queue_id, ordinal = run.run
    members = run.members()
    shed = set(reasons)
    while True:
        grown = False
        for item, _sha, _note in members:
            if item in shed:
                continue
            packet = packets.get(item)
            deps = set(packet.dependencies) if packet and not packet.run else leaves_of(packets, item) - {item}
            if deps & shed:
                shed.add(item)
                grown = True
        if not grown:
            break
    dependents = sorted(shed - set(reasons))
    kept = [(item, sha, "; ".join(part for part in note.split("; ") if not part.startswith(RESOLUTION_PENDING_MARK)))
            for item, sha, note in members if item not in shed]
    integration, collisions = (merge_members(root, run.fields["Base"], kept, message=f"{queue_id} run {ordinal}")
                               if kept else (None, []))
    return {"shed": shed, "dependents": dependents, "kept": kept, "integration": integration, "collisions": collisions}


def shed_members(root: Path, registry: Registry, packets: dict[str, Packet], run: Packet, reasons: dict[str, str], *,
                 reject_reason: str | None, publish: bool = True, move: bool = True, plan: dict | None = None) -> dict:
    """Drop the named members and everything in the run that depends on them, then merge again without them.

    A rejected member is closed failed-rejected with the reason; a held member returns
    to the open run. Dependents always return to the open run.
    """
    queue_id, ordinal = run.run
    queue = registry.queues[queue_id]
    members = run.members()
    plan = plan or plan_shed_members(root, packets, run, reasons)
    shed = plan["shed"]
    dependents = plan["dependents"]
    if reject_reason is not None:
        for item in sorted(reasons):
            if packets[item].status != "failed-rejected":
                write_packet(root, packets, item, rejection_fields(packets[item]), resolution=f"Rejected by {run.item_id} at member review: {reject_reason}")
    successor = open_run(packets, queue_id)
    excluded = excluded_members(run)
    excluded.update(reasons)
    for item in dependents:
        excluded[item] = f"depends on {', '.join(sorted(shed & set(reasons)))}; returned to {successor.item_id}"
    for item, sha, _note in members:
        if item in dependents or (item in reasons and reject_reason is None):
            add_member(root, packets, packets[successor.item_id], item, sha)
    # A previous conflict may disappear when its competing member is removed.
    kept = plan["kept"]
    outcome = {"item": run.item_id, "shed": sorted(reasons), "dependents": dependents, "kept": [m[0] for m in kept]}
    if not kept:
        write_packet(root, packets, run.item_id, [("Status", "failed-abandoned"), ("Closed At", now_utc())],
                     sections={"Members": "none", "Excluded": "\n".join(f"- {k}: {v}" for k, v in excluded.items()) or "none"},
                     resolution="Every member was shed at review or returned to the open run with its dependency; nothing left to integrate.")
        if move:
            subprocess.run(["git", "branch", "-D", f"biq/{queue_id.lower()}/{ordinal}"], cwd=root, capture_output=True)
        outcome["closed"] = True
        return outcome
    integration, collisions = plan["integration"], plan["collisions"]
    collided = dict(collisions)
    kept = [(item, sha, f"{RESOLUTION_PENDING_MARK}{collided[item]}" if item in collided else note) for item, sha, note in kept]
    write_packet(root, packets, run.item_id, [("Integration commit", integration), ("Tests", ", ".join(f"{t}=pending" for t in queue.tests) or "pending")],
                 sections={"Members": member_lines(kept), "Excluded": "\n".join(f"- {k}: {v}" for k, v in excluded.items()) or "none", "Evidence": "none"})
    if move:
        move_branch(root, f"biq/{queue_id.lower()}/{ordinal}", integration)
    if publish:
        publish_candidate(root, run.item_id, integration)
    outcome["integration"] = integration
    return outcome


def review_member(root: Path, registry: Registry, packets: dict[str, Packet], run: Packet, item_id: str, *, owner: str | None, ok: bool, reject: str | None, hold: str | None, apply: bool, authorized_by: str | None = None) -> dict:
    """The integrator's verdict on one member: ok, rejected with its dependents, or held for the human."""
    if run_state(run) != "started":
        raise BiqError(f"{run.item_id} is {run_state(run)}; members are reviewed on a started run")
    if sum(bool(x) for x in (ok, reject, hold)) != 1:
        raise BiqError("give exactly one of --ok, --reject <reason>, --hold <question>")
    members = run.members()
    if not any(m[0] == item_id for m in members):
        raise BiqError(f"{item_id} is not a member of {run.item_id}")
    if not apply:
        return {"item": run.item_id, "member": item_id, "verdict": "ok" if ok else "reject" if reject else "hold"}
    claim = require_claim(root, run.item_id, owner)
    reviewer = str(claim.get("owner") or owner)
    if ok:
        stamped = []
        for item, sha, note in members:
            if item == item_id:
                kept_note = "; ".join(part for part in (note or "").split("; ") if part and not part.startswith(REVIEW_OK_MARK))
                note = (kept_note + "; " if kept_note else "") + f"{REVIEW_OK_MARK}{reviewer} {now_utc()}"
            stamped.append((item, sha, note))
        write_packet(root, packets, run.item_id, sections={"Members": member_lines(stamped)})
        return {"item": run.item_id, "member": item_id, "verdict": "ok"}
    if reject:
        if not (authorized_by or "").strip():
            raise BiqError(f"rejecting {item_id} removes it from the batch; a merge conflict or a failing test is the integrator's to fix, and removal needs the Accountable owner's word: --authorized-by \"<human>\" (or --hold to ask)")
        outcome = shed_members(root, registry, packets, run, {item_id: f"rejected at review by {reviewer} on the word of {authorized_by.strip()}: {reject}"}, reject_reason=f"{reject} (on the word of {authorized_by.strip()})")
        outcome["verdict"] = "reject"
        return outcome
    human = str(run.fields.get("Accountable owner", "pending"))
    write_packet(root, packets, item_id, [("Decision owner", human)])
    outcome = shed_members(root, registry, packets, run, {item_id: f"held for {human} by {reviewer}: {hold}"}, reject_reason=None)
    outcome["verdict"] = "hold"
    outcome["decision_owner"] = human
    return outcome


def reject_members(root: Path, packets: dict[str, Packet], run: Packet, reject: list[str]) -> tuple[list[tuple[str, str, str]], list[str]]:
    """Mark the named members failed-rejected; shed them and everything that depends on them."""
    members = run.members()
    ids = {m[0] for m in members}
    unknown = sorted(set(reject) - ids)
    if unknown:
        raise BiqError(f"{run.item_id} does not contain {', '.join(unknown)}")
    shed = set(reject)
    while True:
        grown = False
        for item, _sha, _note in members:
            if item in shed:
                continue
            packet = packets.get(item)
            deps = set(packet.dependencies) if packet and not packet.run else leaves_of(packets, item) - {item}
            if deps & shed:
                shed.add(item)
                grown = True
        if not grown:
            break
    for item in sorted(reject):
        if packets[item].status != "failed-rejected":
            write_packet(root, packets, item, rejection_fields(packets[item]), resolution=f"Rejected by {run.item_id}: red test.")
    kept = [m for m in members if m[0] not in shed]
    returned = sorted(shed - set(reject))
    return kept, returned


def complete_run(root: Path, registry: Registry, packets: dict[str, Packet], run: Packet, target_tip: str, *, reject: list[str], apply: bool, authorized_by: str | None = None, owner: str | None) -> dict:
    """Green: record the result and pass it on. Red: reject the named members and merge again without them. Only the run's claimant completes it."""
    if run_state(run) != "started":
        raise BiqError(f"{run.item_id} is {run_state(run)}")
    require_claim(root, run.item_id, owner)
    require_resolved(run)
    require_reviewed(run)
    queue_id, ordinal = run.run
    queue = registry.queues[queue_id]
    tests = run.tests
    pending = sorted(n for n, r in tests.items() if r == "pending")
    red = sorted(n for n, r in tests.items() if r == "red")
    if pending:
        raise BiqError(f"{run.item_id} has unrecorded tests: {', '.join(pending)}")
    rejected_elsewhere = sorted(m[0] for m in run.members() if packets.get(m[0]) is not None and packets[m[0]].status == "failed-rejected")
    if red or reject:
        if red and not reject:
            raise BiqError(f"{run.item_id} has red tests ({', '.join(red)}); a failing test is the integrator's to fix on the run; to remove a member instead, name it with --reject on the Accountable owner's word (--authorized-by)")
        if reject and not (authorized_by or "").strip() and not set(reject) <= set(rejected_elsewhere):
            raise BiqError(f"removing {', '.join(sorted(reject))} from {run.item_id} needs the Accountable owner's word: --authorized-by \"<human>\"")
        if not red and not set(reject) <= set(rejected_elsewhere):
            raise BiqError(f"{run.item_id} has no red test; only members already rejected elsewhere may be shed: " + (", ".join(rejected_elsewhere) or "none"))
        if not apply:
            return {"rejected": sorted(reject), "rebuild": True}
        kept, returned = reject_members(root, packets, run, reject)
        successor = open_run(packets, queue_id)
        for item in returned:
            sha = next(m[1] for m in run.members() if m[0] == item)
            add_member(root, packets, packets[successor.item_id], item, sha)
        if not kept:
            write_packet(root, packets, run.item_id, [("Status", "failed-abandoned"), ("Closed At", now_utc())], sections={"Members": "none"},
                         resolution="Every member was rejected or returned to the open run with its dependency; nothing left to integrate.")
            subprocess.run(["git", "branch", "-D", f"biq/{queue_id.lower()}/{ordinal}"], cwd=root, capture_output=True)
            return {"rejected": sorted(reject), "returned": returned, "closed": True, "rebuild": True}
        integration, collisions = merge_members(root, run.fields["Base"], kept, message=f"{queue_id} run {ordinal}")
        collided = dict(collisions)
        kept = [(item, sha, f"{RESOLUTION_PENDING_MARK}{collided[item]}" if item in collided else note) for item, sha, note in kept]
        write_packet(root, packets, run.item_id, [
            ("Integration commit", integration), ("Tests", ", ".join(f"{t}=pending" for t in queue.tests) or "pending"),
        ], sections={"Members": member_lines(kept), "Evidence": "none"})
        move_branch(root, f"biq/{queue_id.lower()}/{ordinal}", integration)
        publish_candidate(root, run.item_id, integration)
        return {"rejected": sorted(reject), "returned": returned, "integration": integration, "rebuild": True}
    if rejected_elsewhere:
        raise BiqError(f"{run.item_id} carries members rejected elsewhere: " + ", ".join(rejected_elsewhere) + "; shed them with --reject")
    result = run.fields["Integration commit"]
    if not run.members():
        raise BiqError(f"{run.item_id} has no member; resolve its excluded members with `run resolve` first")
    outcome = {"result": result, "passed_to": queue.feeds}
    if apply:
        _oid, claim = read_claim(root, run.item_id)
        finished = now_utc()
        started = str(run.fields.get("Started At", finished))
        write_packet(root, packets, run.item_id, [
            ("Status", "submitted"), ("Result commit", result), ("Candidate commit", result), ("Candidate base", str(run.fields.get("Base", "pending"))), ("Submitted At", finished),
            ("Submitted owner", str((claim or {}).get("owner") or "pending")),
            ("Implementation Duration", f"PT{seconds_between(started, finished)}S"),
        ],
                     resolution=f"Tested green on `{result}`; " + (f"passed to {queue.feeds}." if queue.feeds else "ready to land."))
        if queue.feeds:
            add_member(root, packets, open_run(packets, queue.feeds), run.item_id, result)
    return outcome


# --- landing ---------------------------------------------------------------

AUTHORIZED_LANDING_RE = re.compile(r"on the word of (?P<human>[^`]+?) without its queues' runs \((?P<queues>[^)]*)\)")


def landing_subject(root: Path, registry: Registry, packets: dict[str, Packet], item_id: str, target_tip: str, *, authorized_by: str | None = None) -> tuple[str, list[str], str]:
    """The commit a landing carries, the WQEs it completes, and why it may land."""
    packet = require_packet(packets, item_id)
    if packet.status != "submitted":
        raise BiqError(f"{item_id} is {packet.status}; only a submitted WQE lands")
    if packet.run:
        queue = registry.queues[packet.run[0]]
        if queue.feeds is not None:
            raise BiqError(f"{item_id} results pass to {queue.feeds}; they land inside its result")
        result = packet.fields.get("Result commit", "pending")
        if not COMMIT_RE.match(result):
            raise BiqError(f"{item_id} has no result commit")
        predecessor_id = packet.fields.get("Predecessor", "none")
        if predecessor_id != "none" and predecessor_id in packets and packets[predecessor_id].status != "completed":
            raise BiqError(f"{item_id} waits for its predecessor {predecessor_id} to land; refresh onto its tip if it moved (run refresh {queue.id} {packet.run[1]})")
        completes = [item_id]
        pending = [m[0] for m in packet.members()]
        while pending:
            member = pending.pop()
            if member in completes:
                continue
            completes.append(member)
            if member in packets and packets[member].run:
                pending.extend(m[0] for m in packets[member].members())
        return result, sorted(completes), f"result of {queue.id} run {packet.run[1]}"
    outcome = evaluate_submission(root, registry, packets, item_id, target_tip)
    if outcome["queues"] and not authorized_by:
        raise BiqError(f"{item_id} is cared about by {', '.join(outcome['queues'])}; it lands inside a run, or directly on the accountable human's word with --authorized-by \"<name>\"")
    unmet = unmet_dependencies(root, packets, item_id, target_tip, set())
    if unmet:
        raise BiqError(f"{item_id} waits for {', '.join(unmet)}")
    if outcome["queues"]:
        return packet.candidate or "", [item_id], f"leaf {item_id} landed on the word of {authorized_by} without its queues' runs ({', '.join(outcome['queues'])})"
    return packet.candidate or "", [item_id], f"leaf {item_id} that no queue cares about"


def prepare_landing(root: Path, registry: Registry, packets: dict[str, Packet], item_id: str, target_tip: str, *, apply: bool, authorized_by: str | None = None) -> dict:
    """Stage one merge commit onto the target tip, carrying the packet completions, on branch biq/land/<item>.

    With --authorized-by, a leaf that queues care about lands on the accountable
    human's word without their runs: the packet records the word and every open
    run drops the member. The queues' tests are left to their next run.
    """
    subject, completes, reason = landing_subject(root, registry, packets, item_id, target_tip, authorized_by=authorized_by)
    ensure_commit(root, subject)
    worktree = Path(tempfile.mkdtemp(prefix="biq-land-"))
    try:
        git(root, "worktree", "add", "--detach", "--quiet", str(worktree), target_tip)
        merge = subprocess.run(["git", "merge", "--no-ff", "--no-edit", "-m", f"queue: land {item_id}", subject], cwd=worktree, text=True, capture_output=True)
        if merge.returncode and index_only_conflict(root, target_tip, subject):
            # Two landings each appended a retrospective line: keep both, in order; the conflict carries no information.
            (worktree / RETROSPECTIVE_INDEX).write_text(union_index(git(worktree, "show", f":2:{RETROSPECTIVE_INDEX}", strip=False),
                                                                    git(worktree, "show", f":3:{RETROSPECTIVE_INDEX}", strip=False)), encoding="utf-8")
            git(worktree, "add", "--", RETROSPECTIVE_INDEX)
            git(worktree, "commit", "--quiet", "--no-edit")
        elif merge.returncode:
            raise BiqError(f"{item_id} no longer merges cleanly onto {registry.target}; the target moved")
        landed_packets = load_packets(worktree)
        missing = [done for done in completes if done not in landed_packets]
        if missing:
            raise BiqError(f"packets of {', '.join(missing)} are not on {registry.target}; land the control commit first")
        landed = landed_packets[item_id]
        if landed.status != "submitted" or landed.candidate != subject:
            raise BiqError(f"{item_id} is not submitted with candidate {subject[:12]} on {registry.target}; land the control commit first")
        closed = now_utc()
        human, user, host = packet_identity(root)
        for done in completes:
            if True:
                write_packet(worktree, landed_packets, done, [("Status", "completed"), ("Closed At", closed), ("Completed human_owner", human), ("Completed host_user", user), ("Completed hostname", host)],
                             resolution=f"Landed on `{registry.target}` by {item_id}." if done != item_id or "on the word of" not in reason
                             else f"Landed on `{registry.target}` directly, {reason.split(' landed ', 1)[1]}.")
        if "on the word of" in reason:
            for run in landed_packets.values():
                if run.run and run_state(run) == "open" and any(m[0] == item_id for m in run.members()):
                    excluded = excluded_members(run)
                    excluded[item_id] = f"landed directly on the word of {authorized_by}"
                    write_packet(worktree, landed_packets, run.item_id, sections={"Members": member_lines([m for m in run.members() if m[0] != item_id]),
                                                                                    "Excluded": "\n".join(f"- {k}: {v}" for k, v in excluded.items()) or "none"})
        git(worktree, "add", "--", ITEMS_DIR)
        git(worktree, "commit", "--quiet", "--amend", "--no-edit")  # the merge commit carries the completions
        head = git(worktree, "rev-parse", "HEAD")
        if apply:
            move_branch(root, f"biq/land/{item_id.lower()}", head)
        return {"subject": subject, "head": head, "completes": completes, "reason": reason, "branch": f"biq/land/{item_id.lower()}"}
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", str(worktree)], cwd=root, capture_output=True)
        shutil.rmtree(worktree, ignore_errors=True)


def validation_packets(root: Path, revision: str | None) -> dict[str, Packet]:
    """Read historical WI/TD fields for validation without executing their reader.

    Historical metadata allowed unquoted scalar values. Preserve that reading
    here; the operational packet writer continues to emit the current format.
    """
    if revision is None:
        sources = {p.stem: p.read_text(encoding="utf-8") for p in (root / ITEMS_DIR).glob("*.md") if re.fullmatch(r"(?:WI|TD)-[A-Z0-9-]+", p.stem)}
    else:
        paths = git(root, "ls-tree", "--name-only", revision, "--", ITEMS_DIR + "/", strip=False).splitlines()
        sources = {Path(p).stem: git(root, "show", f"{revision}:{p}", strip=False) for p in paths if re.fullmatch(r"(?:WI|TD)-[A-Z0-9-]+\.md", Path(p).name)}
    packets = {}
    for item_id, source in sources.items():
        packet = parse_packet(item_id, source)
        packet.fields.update(validation_metadata(source)[0])
        packets[item_id] = packet
    return packets


FROZEN_SECTIONS = ("Objective", "Allowed source/build scope", "Existing owner and required reuse", "Non-goals",
                   "Required reading", "Affected contracts and invariants", "Acceptance and completion evidence")
FROZEN_FIELDS = ("Kind", "Area")
PACKET_LINE_BOUND = 600
PACKET_BYTE_BOUND = 65536


def is_leaf_work(packet: Packet) -> bool:
    return not packet.run and not packet.fields.get("Container mode")


def packet_size(packet: Packet) -> tuple[int, int]:
    return packet.source.count("\n"), len(packet.source.encode("utf-8"))


def over_bound(packet: Packet) -> bool:
    lines, size = packet_size(packet)
    return lines > PACKET_LINE_BOUND or size > PACKET_BYTE_BOUND


def packet_bound_errors(packets: dict[str, Packet]) -> list[str]:
    """A non-terminal leaf packet is at most PACKET_LINE_BOUND lines and PACKET_BYTE_BOUND bytes: evidence lives beside it, under agent-work/<ID>/."""
    return [f"{item_id}: packet is over the bound of {PACKET_LINE_BOUND} lines and {PACKET_BYTE_BOUND} bytes; move evidence under agent-work/{item_id}/ and reference it, or restart it"
            for item_id, packet in sorted(packets.items()) if is_leaf_work(packet) and packet.status not in TERMINAL and over_bound(packet)]


def packet_change_errors(root: Path, base_packets: dict[str, Packet], head_packets: dict[str, Packet]) -> list[str]:
    """What a change to an existing packet may not do: a claimed leaf keeps its objective and scope, and an over-bound leaf may shrink but not grow.

    A packet's objective and scope are what the target held when it was
    claimed; changing the plan is a restart (`abandon --successor`), which
    leaves a visible chain. Text in Attempt history, Resolution and the risk
    sections binds nothing and stays writable. An unclaimed draft is edited
    freely. Authorization exists only where the tool recorded it.
    """
    out = []
    for item_id, head in sorted(head_packets.items()):
        base = base_packets.get(item_id)
        if base is None or not is_leaf_work(base) or base.status in TERMINAL:
            continue
        changed = [s for s in FROZEN_SECTIONS if base.sections.get(s, "").strip() != head.sections.get(s, "").strip()]
        changed += [f for f in FROZEN_FIELDS if base.fields.get(f) != head.fields.get(f)]
        if changed and (base.fields.get("Claimed At", "pending") != "pending" or read_claim(root, item_id)[0] is not None):
            out.append(f"{item_id}: {', '.join(changed)} changed after claim; a claimed packet's objective and scope are fixed, restart it to change them: "
                       f"abandon {item_id} --successor <ID> --owner <you> --reason <why> --apply")
        if head.status not in TERMINAL and over_bound(head) and packet_size(head) > packet_size(base):
            out.append(f"{item_id}: packet is over the bound of {PACKET_LINE_BOUND} lines and {PACKET_BYTE_BOUND} bytes and grew; move evidence under agent-work/{item_id}/ and reference it, or restart it")
    return out


def stranded_submissions(packets: dict[str, Packet]) -> list[str]:
    """Submitted leaves that no run of a caring queue short of landing holds: the queue cannot reach them, so they are not ready for anything.

    A run holds its members while it collects, tests, and waits to land. A
    completed run disposes of every member: integrated, rejected, or returned
    to the open successor. A packet left behind by a run that closed around it
    shows here until it is resealed into the open run or rejected on the
    accountable human's word.
    """
    out = []
    for item_id, packet in sorted(packets.items()):
        if packet.status != "submitted" or packet.run or not packet.queues:
            continue
        holding = {r.run[0] for q in packet.queues for r in runs_of(packets, q)
                   if r.status not in TERMINAL and any(m[0] == item_id for m in r.members())}
        missing = [q for q in packet.queues if q not in holding]
        if missing:
            out.append(f"{item_id}: submitted but no open, started or unlanded run of {', '.join(missing)} holds it; "
                       f"reseal it to enqueue it again, or `reject {item_id} --reason <why> --authorized-by <human> --apply`")
    return out


def validation_errors(root: Path, revision: str | None) -> list[str]:
    packets = validation_packets(root, revision)
    errors = [error for packet in packets.values() for error in packet_contract_errors(packet)]
    errors += [error for packet in packets.values() for error in receipt_validation_errors(packet)]
    errors += packet_graph_errors(packets) + packet_reference_errors(root, packets, revision)
    errors += stranded_submissions(packets) + packet_bound_errors(packets)
    return sorted(set(errors))


def validation_regressions(root: Path, base: str, head: str) -> list[str]:
    """Additional checks reject new errors; unchanged historical errors stay local."""
    introduced = validation_errors(root, head)
    if not introduced:
        return []
    existing = set(validation_errors(root, base))
    return [error for error in introduced if error not in existing]


def new_packet_baseline_errors(root: Path, registry: Registry, base_packets: dict[str, Packet], head_packets: dict[str, Packet], target: str) -> list[str]:
    """A newly admitted packet starts at one real commit already reachable from the target."""
    errors = []
    expected = f"origin/{registry.target}"
    for item_id in sorted(set(head_packets) - set(base_packets)):
        packet = head_packets[item_id]
        if packet.fields.get("Task packet") not in {"2", "3"}:
            continue
        value = packet.fields.get("Baseline", "")
        match = re.fullmatch(rf"{re.escape(expected)} ([0-9a-f]{{40}})", value)
        if match is None:
            errors.append(f"{item_id}: Baseline must be `{expected} <40-hex commit>` from the configured target")
            continue
        commit = match.group(1)
        if not have_commit(root, commit):
            errors.append(f"{item_id}: Baseline commit {commit} does not resolve; use the full result of `git rev-parse {expected}`")
        elif not is_ancestor(root, commit, target):
            errors.append(f"{item_id}: Baseline commit {commit} is not reachable from {expected}; use a commit from the configured target")
    return errors


def verify_landing(root: Path, base: str, head: str) -> str:
    """The gate: a control commit, or exactly one merge carrying a landable WQE plus its completions."""
    registry = load_registry(root, head)  # the landing may introduce or change the registry
    packets = load_packets(root, base)
    head_packets = load_packets(root, head)
    errors = lint(registry, head_packets) + reference_errors(root, head_packets, head)
    if errors:
        raise BiqError("packets at the landing head: " + "; ".join(errors[:5]))
    changes = changed_paths(root, base, head)
    if any(p.startswith(ITEMS_DIR + "/") for p in changes):
        additional = validation_regressions(root, base, head) + new_packet_baseline_errors(root, registry, packets, head_packets, base)
        if additional:
            raise BiqError("packet validation at the landing head: " + "; ".join(additional[:5]))
        changed = packet_change_errors(root, packets, head_packets)
        if changed:
            raise BiqError("packet changes at the landing head: " + "; ".join(changed[:5]))
    if all(p.startswith(CONTROL_ROOTS) for p in changes):
        return "control commit"
    merges = [line.split() for line in git(root, "rev-list", "--first-parent", "--parents", f"{base}..{head}", strip=False).splitlines() if len(line.split()) > 2]
    if len(merges) != 1:
        raise BiqError("a landing carries exactly one merge onto the target")
    subject = merges[0][2]
    product = [p for p in changes if not p.startswith(CONTROL_ROOTS)]
    if product != [p for p in changed_paths(root, base, subject) if not p.startswith(CONTROL_ROOTS)]:
        raise BiqError("the landing changes product paths that its subject does not")
    # The landing is the clean merge of the subject onto the base and nothing more: main may have
    # moved on the same paths since the subject was sealed, so compare against that merge, not the subject.
    merged = subprocess.run(["git", "merge-tree", "--write-tree", base, subject], cwd=root, text=True, capture_output=True)
    if merged.returncode != 0 and not index_only_conflict(root, base, subject):
        raise BiqError(f"the subject {subject[:12]} does not merge cleanly onto {base[:12]}")
    expected_tree = merged.stdout.splitlines()[0].strip()
    if merged.returncode != 0:  # the retrospective index alone conflicted: the landing carries the union of both sides
        union = union_index(git(root, "show", f"{base}:{RETROSPECTIVE_INDEX}", strip=False), git(root, "show", f"{subject}:{RETROSPECTIVE_INDEX}", strip=False))
        if git(root, "show", f"{head}:{RETROSPECTIVE_INDEX}", strip=False) != union:
            raise BiqError(f"the landing resolves {RETROSPECTIVE_INDEX} other than by keeping both sides' lines")
    for path in product:
        if git(root, "ls-tree", head, "--", path) != git(root, "ls-tree", expected_tree, "--", path):
            raise BiqError(f"the landing changes {path} beyond its subject")
    for packet in packets.values():
        if packet.status != "submitted":
            continue
        if packet.run and packet.fields.get("Result commit") == subject:
            queue = registry.queues[packet.run[0]]
            if queue.feeds is not None:
                raise BiqError(f"{packet.item_id} is a feeder result; it lands inside a result of {queue.feeds}")
            return f"result of {queue.id} run {packet.run[1]}"
        if not packet.run and packet.candidate == subject:
            if requires_retrospective(packet) or retrospective_reference(packet) is not None:
                verify_retrospective_at(root, packet, head)
            landed = load_packets(root, head).get(packet.item_id)
            word = AUTHORIZED_LANDING_RE.search((landed.sections.get("Resolution", "") if landed else ""))
            _subject, _completes, reason = landing_subject(root, registry, packets, packet.item_id, base, authorized_by=word.group("human") if word else None)
            return reason
    raise BiqError("landing is neither a control commit, a result of a consumer-less queue, nor a submitted leaf that no queue cares about")


# --- lint --------------------------------------------------------------------

def validation_time(value: str) -> dt.datetime:
    """Parse a recorded UTC instant, including the historical offset spelling."""
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (ValueError, AttributeError) as error:
        raise BiqError(f"invalid UTC timestamp {value!r}") from error
    if parsed.tzinfo is None or parsed.utcoffset() != dt.timedelta(0):
        raise BiqError(f"timestamp must be UTC: {value!r}")
    return parsed


def concrete_human_name(value: str) -> bool:
    """Check authored display-name syntax; this does not establish identity."""
    if not isinstance(value, str) or not value or any(unicodedata.category(c) in {"Cc", "Cf"} for c in value):
        return False
    normalized = unicodedata.normalize("NFKC", value)
    key = " ".join("".join(c if c.isalnum() else " " for c in normalized.casefold()).split())
    if key in {
        "accountable owner", "approver", "architect", "architecture approver",
        "architecture decision maker", "architecture decision owner", "blocker owner",
        "code reviewer", "concrete human", "concrete human name", "decision owner",
        "design owner", "durable memory device format and daemon flush owner",
        "git user name of creator", "human owner", "human name required",
        "independent reviewer", "inlinefs project owner", "lead", "maintainer",
        "maker", "manager", "none", "owner", "pending", "project owner",
        "project owner architecture decision owner assignment required",
        "project owner assignment required", "qa lead", "queue owner",
        "release manager", "repository maintainer", "reviewer", "some other placeholder",
        "source owner", "system architect", "t b d", "tbd", "technical lead",
        "to be determined", "unassigned", "unauthorized preview owner", "unknown", "unrecorded",
    }:
        return False
    for token in normalized.strip().split(" "):
        if not token:
            return False
        previous_letter = False
        saw_letter = False
        for index, char in enumerate(token):
            category = unicodedata.category(char)
            if category.startswith("L"):
                previous_letter = saw_letter = True
            elif category.startswith("M"):
                if not previous_letter:
                    return False
            elif char in {"'", "’", "-", "."} and previous_letter:
                if index + 1 == len(token):
                    if char != ".":
                        return False
                elif not unicodedata.category(token[index + 1]).startswith("L"):
                    return False
                previous_letter = index + 1 == len(token)
            else:
                return False
        if not saw_letter or not previous_letter:
            return False
    return True


def packet_has_biq_admission(packet: Packet) -> bool:
    """Preserve the old reader's exemption markers without reading another file."""
    return bool(re.search(r"^- Queues?: |^Abandoned by ", packet.source, re.M))


PACKET_SECTIONS = (
    "Objective", "Reason", "Allowed source/build scope", "Existing owner and required reuse",
    "Non-goals", "Required reading", "Affected contracts and invariants", "Risks and constraints",
    "Implementation outline", "Acceptance and completion evidence",
    "Residual risks and unresolved decisions", "Attempt history", "Resolution",
)
PACKET_REQUIRED = {
    "Task packet", "Accountable owner", "Architecture gate", "Architecture decision",
    "Architecture decision owner", "Documentation impact", "Documentation impact rationale", "Decision owner",
}
PACKET_STATE = ("Previous attempt", "Next attempt", "Candidate base", "Candidate commit", "Submitted At", "Submitted owner")
PACKET_PROVENANCE = tuple(f"{stage} {name}" for stage in ("Created", "Completed") for name in ("human_owner", "host_user", "hostname"))
PACKET_USAGE = ("Tokens Used", "Model Used", "Estimated Cost")
PACKET_LIFECYCLE = ("Claimed At", "Closed At", "Implementation Duration")
READING_ROLES = ("governing-authority", "task-design", "verification", "historical-evidence", "creates-output")
READING_LINE = re.compile(r"^- (" + "|".join(READING_ROLES) + r"): `([^`]+)`$")
PACKET_OPTIONAL = ("Container mode", "Container members", "Batch members", "Integration filter", "Terminal target")


def validation_metadata(source: str) -> tuple[dict[str, str], list[str], Counter, Counter]:
    """Additional-validation metadata uses the historical scalar grammar only."""
    values, order, raw, parsed = {}, [], Counter(), Counter()
    for number, (line, outside) in enumerate(markdown_lines(source)):
        if outside and re.match(r"^ {0,3}##(?:\s|$)", line):
            break
        if not outside or number == 0:
            continue
        header = re.match(r"^- ([A-Za-z][A-Za-z_ ]*):", line)
        if header:
            raw[header[1]] += 1
        match = re.match(r"^- ([A-Za-z][A-Za-z_ ]*):\s*`?([^`\n]+)`?\s*$", line)
        if match:
            values[match[1]] = match[2].strip()
            order.append(match[1]); parsed[match[1]] += 1
    return values, order, raw, parsed


def reference_target(value: str) -> tuple[str, str | None]:
    path, separator, anchor = value.partition("#")
    parsed = PurePosixPath(path)
    if (not path or path.startswith(("/", "./")) or path.endswith("/") or "\\" in path
            or any(c.isspace() for c in value) or ".." in parsed.parts or parsed.as_posix() != path
            or (separator and (not anchor or "#" in anchor))):
        raise BiqError(f"invalid repository file reference {value!r}")
    return path, anchor if separator else None


def packet_reading(packet: Packet) -> list[tuple[str, str, str | None]]:
    references = []
    for line in packet.sections.get("Required reading", "").splitlines():
        if not line.strip():
            continue
        match = READING_LINE.fullmatch(line)
        if not match:
            if references and line.startswith(("  ", "\t")):
                continue
            raise BiqError("Required reading bullets must be `- <role>: `path[#heading]`` where role is " + ", ".join(READING_ROLES))
        role, target = match.groups()
        path, anchor = reference_target(target)
        if role != "historical-evidence" and path.startswith(ITEMS_DIR + "/"):
            raise BiqError(f"{role} cannot name a work-item packet; use historical-evidence: {target}")
        if role in {"governing-authority", "task-design"} and path.endswith(".md") and not anchor:
            raise BiqError(f"{role} Markdown reading needs an anchor: {target}")
        if role == "task-design" and not (path.startswith("docs/specs/") and path.endswith(".md")):
            raise BiqError("task-design must name Markdown under docs/specs/")
        references.append((role, path, anchor))
    for role in ("governing-authority", "verification"):
        if not any(r[0] == role for r in references):
            raise BiqError(f"Required reading needs a {role} reference")
    return references


def validate_immediate_receipt_routes(routes: object) -> None:
    """Validate passive historical routing evidence, without executing filters."""
    receipt_require(isinstance(routes, list) and bool(routes), "immediate routes must be a nonempty list")
    families, endpoints, main_actions = [], set(), []
    for route in routes:
        receipt_require(isinstance(route, dict) and set(route) == {"family", "tail", "filter", "result"}
                        and all(isinstance(route[k], str) for k in ("family", "tail", "filter"))
                        and RECEIPT_BATCH_RE.fullmatch(route["tail"]), "malformed immediate route")
        tail = route["tail"]
        family = (tail.rsplit("-REPEAT-", 1)[0] if tail.startswith("WI-CI-INTEGRATION-BATCH-") and re.search(r"-REPEAT-[1-9][0-9]*$", tail)
                  else "WI-CI-INTEGRATION-BATCH")
        receipt_require(route["family"] == family, "immediate route family differs from its tail")
        path = route["filter"]
        receipt_require(bool(path) and not path.startswith(("/", "./")) and not path.endswith("/") and "\\" not in path
                        and not any(c.isspace() for c in path) and not any(p in {"", ".", ".."} for p in PurePosixPath(path).parts)
                        and PurePosixPath(path).as_posix() == path, "immediate filter must name a normalized repository file")
        result = route["result"]
        receipt_require(isinstance(result, dict) and set(result) == {"schema", "policy_commit", "base", "head", "action", "reason", "checks"}
                        and type(result["schema"]) is int and result["schema"] == 1
                        and result["action"] in ("immediate", "unaffected", "pass-through")
                        and all(isinstance(result[k], str) and COMMIT_RE.fullmatch(result[k]) for k in ("policy_commit", "base", "head")),
                        "malformed immediate route result or forbidden custody action")
        receipt_require(receipt_text(result["reason"], single_line=True) and len(result["reason"]) <= 1024,
                        "immediate route reason must be one bounded nonempty line")
        checks = result["checks"]
        receipt_require(isinstance(checks, list) and len(checks) <= 64
                        and all(receipt_text(check, single_line=True) and len(check) <= 1024 for check in checks)
                        and checks == sorted(set(checks)), "immediate route checks must be bounded sorted unique lines")
        families.append(family)
        endpoints.add(tuple(result[k] for k in ("policy_commit", "base", "head")))
        if family == "WI-CI-INTEGRATION-BATCH":
            main_actions.append(result["action"])
    receipt_require(families == sorted(set(families)), "immediate routes must name sorted distinct families")
    receipt_require(len(endpoints) == 1, "immediate routes changed exact endpoints")
    receipt_require(main_actions == ["immediate"], "immediate routes require exactly one Main Immediate")


def validate_accounting_scalars(packet: Packet, f: dict[str, str]) -> None:
    """Check declared summaries; no receipt or missing measurement is invented."""
    sealed = f.get("Status") in {"submitted", *TERMINAL}
    if sealed and packet_has_biq_admission(packet):
        return
    if all(k in f for k in PACKET_USAGE):
        tokens, models, cost = (f[k] for k in PACKET_USAGE)
        pending = (tokens, models, cost) == ("pending",) * 3
        if not sealed:
            receipt_require(pending, "unsealed accounting must be pending")
        elif not pending:
            unavailable = (tokens, models, cost) == ("unavailable",) * 3
            if unavailable:
                receipt_require(RECEIPT_BATCH_RE.fullmatch(packet.item_id) and f.get("Status") in TERMINAL,
                                "unavailable accounting requires a terminal legacy batch")
            else:
                receipt_require(re.fullmatch(r"0|[1-9][0-9]*", tokens), "Tokens Used must be a nonnegative integer")
                parts = models.split(", ")
                receipt_require(all(p and p == p.strip() and "," not in p and p.casefold() not in {"-", "n/a", "pending", "tbd", "unknown", "unrecorded"} for p in parts)
                                and len(parts) == len(set(parts)), "Model Used must name unique concrete model IDs")
                receipt_require(re.fullmatch(r"\$(?:0|[1-9][0-9]*)(?:\.[0-9]{1,6})?", cost), "Estimated Cost must be a nonnegative dollar decimal")
                if tokens == "0":
                    receipt_require(models == "none" and not any(c in "123456789" for c in cost), "zero Tokens Used requires Model Used none and zero Estimated Cost")
                else:
                    receipt_require(all(p.casefold() != "none" for p in parts), "positive Tokens Used requires a concrete model")
    if "Estimated AWS Cost" in f:
        aws = f["Estimated AWS Cost"]
        if not sealed:
            receipt_require(aws == "pending", "unsealed Estimated AWS Cost must be pending")
        else:
            receipt_require(aws == "pending" or re.fullmatch(r"\$(?:0|[1-9][0-9]*)(?:\.[0-9]{1,6})?", aws), "Estimated AWS Cost must be a nonnegative dollar decimal or unmeasured")


def validate_packet_state(packet: Packet, f: dict[str, str]) -> None:
    """Candidate syntax is universal; admitted packets keep owner lifecycle data."""
    status = f.get("Status", "open")
    batch = RECEIPT_BATCH_RE.fullmatch(packet.item_id) is not None
    if f.get("Task packet") == "3" and not packet.run:
        base, candidate, submitted, owner = (f.get(k, "") for k in PACKET_STATE[2:])
        quartet = (base, candidate, submitted, owner)
        concrete_candidate = bool(COMMIT_RE.fullmatch(base) and COMMIT_RE.fullmatch(candidate))
        named_batch = re.fullmatch(r"WI-CI-INTEGRATION-BATCH-[A-Z0-9]+(?:-[A-Z0-9]+)*-REPEAT-[1-9][0-9]*", packet.item_id) is not None
        receipt_require(not batch or status != "blocked", "legacy batch cannot be blocked")
        receipt_require(not (batch and not named_batch and status == "submitted"), "legacy Main batch cannot be submitted")
        if status in {"open", "blocked", "abandonment-proposed"}:
            # Current seal records the two SHAs before submit fills its fields.
            prepared = status == "open" and concrete_candidate and (submitted, owner) == ("pending", "pending")
            receipt_require(quartet == ("pending",) * 4 or prepared,
                            "unsubmitted candidate fields must be pending or a sealed SHA pair with pending submission")
        elif status == "submitted" or status in TERMINAL:
            absent = quartet == ("none",) * 4
            if status in TERMINAL and (batch or status == "failed-abandoned"):
                receipt_require(absent, "terminal batch or abandoned Candidate and Submitted fields must all be none")
            elif status == "submitted" or not absent:
                receipt_require(concrete_candidate, "Candidate base and Candidate commit must be 40-lowercase-hex")
                try:
                    receipt_time(submitted, canonical=True)
                except BiqError as error:
                    raise BiqError(f"Submitted At: {error}") from error
                receipt_require(bool(owner.strip()) and owner not in {"pending", "none"}, "Submitted owner must be concrete")
    if packet.run or packet_has_biq_admission(packet) or not all(k in f for k in PACKET_LIFECYCLE):
        return
    if status not in {"submitted", *TERMINAL}:
        receipt_require(all(f[k] == "pending" for k in PACKET_LIFECYCLE), "unsealed lifecycle fields must all be pending")
        return
    created_text = f.get("Created", "")
    created = (dt.datetime.combine(dt.date.fromisoformat(created_text), dt.time(), dt.timezone.utc)
               if re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", created_text) else receipt_time(created_text, canonical=True))
    rejection_fields = (RECEIPT_AUTH_FIELDS - {"authorized_at"}) | {"outcome", "rejected_at"}
    proposal_rejections = receipt_objects(packet, RECEIPT_REJECTION_PREFIX, fields=rejection_fields, canonical=True)
    ordinary_rejection = packet_ordinary_rejection(packet)
    group_fields = set("schema item container decision authorizer decided_at group_sha256 prestate".split())
    groups = receipt_objects(packet, "Work-queue quarantine member disposition v1: ", fields=group_fields, canonical=True)
    if groups:
        group = groups[0]
        receipt_require(group["item"] == packet.item_id and isinstance(group["container"], str) and receipt_proposed_id(group["container"])
                        and group["decision"] in ("accepted", "rejected") and group["prestate"] in ("durable", "prospective")
                        and (group["decision"] != "rejected" or group["prestate"] == "prospective")
                        and concrete_human_name(group["authorizer"]) and isinstance(group["group_sha256"], str)
                        and re.fullmatch(r"[0-9a-f]{64}", group["group_sha256"]), "malformed claimless group disposition")
        receipt_time(group["decided_at"])
        receipt_require(status == ("failed-abandoned" if group["decision"] == "accepted" else "failed-rejected")
                        and f.get("Completed human_owner") == group["authorizer"] and f["Closed At"] == group["decided_at"]
                        and tuple(f.get(k) for k in PACKET_USAGE) == ("0", "none", "$0") and f.get("Estimated AWS Cost") == "$0"
                        and all(f.get(k) == "none" for k in (*PACKET_STATE[2:], "Decision owner"))
                        and receipt_objects(packet, USAGE_EVIDENCE_PREFIX) == [{"schema": 1, "mode": "human-only"}]
                        and not receipt_objects(packet, SUBSTANTIVE_WORK_PROVENANCE_PREFIX), "claimless group disposition does not match packet state")
    claimless = (status == "failed-rejected" and (ordinary_rejection is not None or bool(proposal_rejections))
                 or status in {"failed-rejected", "failed-abandoned"} and bool(groups))
    if claimless:
        usages = receipt_objects(packet, USAGE_EVIDENCE_PREFIX)
        usage = receipt_usage(usages[0]) if usages else None
        released = proposal_rejections and usage is not None and usage["mode"] == "claim-segmented"
        if released:
            # This existing receipt owner checks exact segment start/duration,
            # ordered authorizations and the later claimless rejection endpoint.
            receipt_proposal_bindings(packet, usage)
        else:
            receipt_require(f["Claimed At"] == f["Implementation Duration"] == "none",
                            "claimless disposition needs Claimed At and Implementation Duration none")
        closed = receipt_time(f["Closed At"], canonical=True)
        receipt_require(created <= closed, "Created must not follow Closed At")
        return
    claimed = receipt_time(f["Claimed At"], canonical=True)
    duration = re.fullmatch(r"PT(0|[1-9][0-9]*)S", f["Implementation Duration"])
    receipt_require(duration is not None, "Implementation Duration must use canonical PT<seconds>S")
    receipt_require(created <= claimed, "Created must not follow Claimed At")
    submitted = f.get("Submitted At")
    submitted_shape = submitted not in {None, "pending", "none"}
    if status == "submitted":
        receipt_require(f["Closed At"] == "pending" and submitted_shape, "submitted lifecycle needs Closed At pending and concrete Submitted At")
        endpoint = receipt_time(submitted, canonical=True)
    else:
        closed = receipt_time(f["Closed At"], canonical=True)
        immediate = receipt_objects(packet, "Work-queue Main immediate disposition v1: ", canonical=True)
        # Historical immediate completion includes work through closure. Its
        # source/route authorization remains with the existing transition owner.
        if immediate:
            value = immediate[0]
            receipt_require(set(value) == {"schema", "item", "landed_main", "submitted_packet_sha256", "routes"}
                            and value["item"] == packet.item_id and isinstance(value["landed_main"], str) and COMMIT_RE.fullmatch(value["landed_main"])
                            and isinstance(value["submitted_packet_sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", value["submitted_packet_sha256"])
                            and isinstance(value["routes"], list) and bool(value["routes"]), "malformed historical immediate completion")
            validate_immediate_receipt_routes(value["routes"])
        if submitted_shape and not immediate:
            endpoint = receipt_time(submitted, canonical=True)
            receipt_require(endpoint <= closed, "Submitted At must not follow Closed At")
        else:
            endpoint = closed
    receipt_require(claimed <= endpoint, "Claimed At must not follow its lifecycle endpoint")
    elapsed = int((endpoint - claimed).total_seconds())
    seconds = int(duration[1])
    receipt_require(seconds <= elapsed if receipt_proposed_id(packet.item_id) else seconds == elapsed,
                    "Implementation Duration does not match its lifecycle interval")


def packet_contract_errors(packet: Packet) -> list[str]:
    """Validate authored structure without making historical projection a gate."""
    errors = []
    f, header_order, raw_counts, parsed_counts = validation_metadata(packet.source)
    version = f.get("Task packet")
    status = f.get("Status", "open")
    admitted = packet_has_biq_admission(packet)
    batch = RECEIPT_BATCH_RE.fullmatch(packet.item_id) is not None
    def fail(message):
        errors.append(f"{packet.item_id}: {message}")
    basic = {"ID", "Status", "Kind", "Level of effort", "Area", "Created", "Baseline", "Architecture gate", "Dependencies"}
    missing = sorted(basic - f.keys())
    if missing:
        fail("missing fields: " + ", ".join(missing))
    if not RECEIPT_ITEM_RE.fullmatch(packet.item_id) or f.get("ID") != packet.item_id:
        fail("ID must match the filename and local WQE grammar")
    title = packet.source.splitlines()[0] if packet.source.splitlines() else ""
    if not title.startswith("# ") or not title[2:].strip().startswith(f"{packet.item_id}: "):
        fail("first line must be the item's level-one title")
    if status not in {*STATUSES, "blocked", "abandonment-proposed"}:
        fail("Status is not supported")
    if f.get("Kind") not in {"architecture", "audit", "bug", "build", "documentation", "feature", "infrastructure", "integration", "performance", "qualification", "refactor", "reporting", "technical-debt", "test", "tooling"}:
        fail("Kind is not supported")
    if f.get("Level of effort") not in {"unestimated", "low", "medium", "high"}:
        fail("Level of effort must be one of: unestimated, low, medium, high")
    if batch and not re.fullmatch(r"origin/main [0-9a-f]{40}", f.get("Baseline", "")):
        fail("legacy batch Baseline must name origin/main and its exact commit")
    for key in ("Level of effort", "Created", *PACKET_LIFECYCLE, *PACKET_USAGE, "Estimated AWS Cost"):
        if raw_counts[key] > 1:
            fail(f"duplicate {key} field")
    for key in ("Priority", *PACKET_PROVENANCE):
        if parsed_counts[key] > 1:
            fail(f"duplicate {key} field")
    try:
        created = f.get("Created", "")
        if re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", created):
            dt.date.fromisoformat(created)
        elif re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z", created):
            validation_time(created)
        else:
            raise BiqError("invalid Created shape")
    except (ValueError, BiqError):
        fail("Created must be a valid date or whole-second UTC timestamp")
    partial = (PACKET_REQUIRED - {"Architecture gate", "Decision owner"}) | set(PACKET_STATE) | set(PACKET_OPTIONAL)
    if version is None and any(packet.scalar_counts.get(k, 0) for k in partial):
        fail("partial versioned packet block without Task packet")
    for group in (PACKET_PROVENANCE, PACKET_USAGE, PACKET_LIFECYCLE):
        counts = parsed_counts if group == PACKET_PROVENANCE else raw_counts
        present = {k for k in group if counts[k]}
        if present and (present != set(group) or any(not f.get(k) for k in group)):
            fail("fields are all-or-none with nonempty values: " + ", ".join(group))
    if raw_counts["Estimated AWS Cost"] and (not f.get("Estimated AWS Cost") or any(not f.get(k) for k in PACKET_USAGE)):
        fail("Estimated AWS Cost requires a complete accounting block")
    if all(k in f for k in PACKET_PROVENANCE):
        if any(not f[k] or f[k].casefold() == "pending" for k in PACKET_PROVENANCE[:3]):
            fail("creator provenance must be concrete")
        completed = tuple(f[k] for k in PACKET_PROVENANCE[3:])
        if (status in TERMINAL and any(not v or v.casefold() == "pending" for v in completed)) or (status not in TERMINAL and any(v != "pending" for v in completed)):
            fail("completion provenance must match terminal status")
    decision_owner = f.get("Decision owner")
    if status == "blocked":
        if not concrete_human_name(decision_owner):
            fail("blocked work needs a concrete Decision owner")
        if packet.sections.get("Resolution", "").strip() == "Open.":
            fail("blocked Resolution must explain its decision")
    elif status == "abandonment-proposed":
        if version != "3" or decision_owner != f.get("Accountable owner"):
            fail("abandonment-proposed needs its Accountable owner as Decision owner")
    elif status == "submitted" and admitted and decision_owner not in {None, "none"}:
        if not concrete_human_name(decision_owner):
            fail("held BIQ submission needs a concrete Decision owner")
    elif decision_owner not in {None, "none"} and (version is not None or status != "completed"):
        fail("non-blocked work needs Decision owner none")
    if version is None and raw_counts["Decision owner"] > 1:
        fail("Decision owner must appear at most once")
    try:
        validate_accounting_scalars(packet, f)
    except BiqError as error:
        fail(str(error))
    try:
        validate_packet_state(packet, f)
    except (BiqError, ValueError) as error:
        fail(str(error))
    if version not in {"2", "3"}:
        if version is not None or status not in TERMINAL:
            fail("active work needs Task packet 2 or 3")
        return errors
    required = PACKET_REQUIRED | (set(PACKET_STATE) if version == "3" else set())
    for group in (PACKET_PROVENANCE, PACKET_USAGE, PACKET_LIFECYCLE):
        if version == "3" or any(k in f for k in group):
            required |= set(group)
    if version == "3":
        required.add("Estimated AWS Cost")
    if any(k in f for k in ("Container mode", "Container members")):
        required.update(("Container mode", "Container members"))
    reserved = PACKET_REQUIRED | set(PACKET_STATE) | set(PACKET_OPTIONAL)
    if version == "2" and not admitted and any(packet.scalar_counts.get(k, 0) for k in (*PACKET_STATE, *PACKET_OPTIONAL)):
        fail("task packet 2 must not carry task packet 3 fields")
    if version == "3":
        if batch:
            composite = any(k in f for k in ("Container mode", "Container members"))
            if composite == bool(packet.scalar_counts.get("Batch members", 0)):
                fail("legacy batch needs one Batch members scalar or one composite container pair")
            if not composite:
                required.add("Batch members")
            named = re.fullmatch(r"WI-CI-INTEGRATION-BATCH-[A-Z0-9]+(?:-[A-Z0-9]+)*-REPEAT-[1-9][0-9]*", packet.item_id) is not None
            if named or any(packet.scalar_counts.get(k, 0) for k in ("Integration filter", "Terminal target")):
                required.update(("Integration filter", "Terminal target"))
        elif any(packet.scalar_counts.get(k, 0) for k in ("Batch members", "Integration filter", "Terminal target")):
            fail("ordinary task packet forbids legacy batch fields")
    for key in sorted(required):
        count = packet.scalar_counts.get(key, 0) if key in reserved else parsed_counts[key]
        if not f.get(key) or count != 1 or (key in reserved and key in packet.misplaced_fields):
            fail(f"{key} must appear exactly once in the header with a scalar value")
    if not header_order or header_order[-1] != "Decision owner":
        fail("Decision owner must be the final header field")
    if version == "3" and "Dependencies" in header_order:
        tail = ["Dependencies", *PACKET_STATE]
        if "Priority" in f:
            tail.append("Priority")
        tail += [k for k in ("Container mode", "Container members", "Batch members", "Integration filter", "Terminal target") if k in f]
        tail.append("Decision owner")
        if header_order[header_order.index("Dependencies"):] != tail:
            fail("fields after Dependencies differ from the task packet 3 order")
    headings = [h for h in packet.section_order if not (packet_has_biq_admission(packet) and h in {"Cares about", "Queue tests", "Members", "Excluded", "Evidence"})]
    if tuple(headings) != PACKET_SECTIONS:
        fail("task sections must appear once in the template order")
    for section in PACKET_SECTIONS:
        if not packet.sections.get(section, "").strip():
            fail(f"{section} must be nonempty")
    if not concrete_human_name(f.get("Accountable owner", "")):
        fail("Accountable owner must name a concrete human")
    for key in PACKET_STATE[:2]:
        if key in f and f[key] != "none" and (not RECEIPT_ITEM_RE.fullmatch(f[key]) or f[key] == packet.item_id):
            fail(f"{key} must name another local WQE or none")
    gate, decision, owner = (f.get(k, "") for k in ("Architecture gate", "Architecture decision", "Architecture decision owner"))
    try:
        reading = packet_reading(packet)
        designs = [(p, a) for role, p, a in reading if role == "task-design"]
        if gate == "not-required":
            if decision != "none" or owner != "none" or designs:
                fail("not-required architecture needs no decision, owner or task-design")
        elif gate in {"pending", "accepted"}:
            path, anchor = reference_target(decision)
            if not path.startswith("docs/specs/") or not path.endswith(".md") or not anchor:
                fail("Architecture decision must name anchored Markdown under docs/specs/")
            sealed = status in {"submitted", *TERMINAL}
            if not sealed and designs != [(path, anchor)]:
                fail("active architecture requires one matching task-design")
            if sealed and designs and not admitted:
                fail("sealed work must remove or reclassify task-design")
            if gate == "pending" and not concrete_human_name(owner):
                fail("pending Architecture decision owner must name a concrete human")
            if gate == "accepted" and owner != "none":
                fail("accepted Architecture decision owner must be none")
            if version == "3" and gate == "pending" and packet.status not in TERMINAL and (f.get("Kind") != "architecture" or packet.dependencies):
                fail("pending architecture must be a dependency-free architecture question")
            if gate == "pending" and sealed and status != "failed-abandoned":
                rejection_prefix = RECEIPT_REJECTION_PREFIX if receipt_proposed_id(packet.item_id) else "Work-queue ordinary rejection v1: "
                rejection_fields = (RECEIPT_AUTH_FIELDS - {"authorized_at"}) | {"outcome", "rejected_at"}
                rejected = status == "failed-rejected" and any(receipt_objects(packet, prefix, fields=rejection_fields, canonical=True)
                    for prefix in (rejection_prefix, "Work-queue architecture rejection v1: "))
                if not rejected:
                    fail("submitted or completed work cannot retain pending architecture")
        else:
            fail("Architecture gate is not supported")
    except BiqError as error:
        fail(str(error))
    impact = f.get("Documentation impact", "none")
    if impact != "none":
        paths = impact.split(", ")
        if len(paths) != len(set(paths)):
            fail("Documentation impact must name distinct files")
        for path in paths:
            try:
                reference_target(path)
            except BiqError as error:
                fail(str(error))
    if f.get("Documentation impact rationale", "").strip().casefold() in {"", "none", "pending", "tbd", "unknown"}:
        fail("Documentation impact rationale must be concrete")
    return errors


def packet_ordinary_rejection(packet: Packet) -> dict | None:
    """A passive claimless rejection may close a project without its children."""
    fields = (RECEIPT_AUTH_FIELDS - {"authorized_at"}) | {"outcome", "rejected_at"}
    values = receipt_objects(packet, "Work-queue ordinary rejection v1: ", fields=fields, canonical=True)
    if not values:
        return None
    value = receipt_authorization(values[0], packet.item_id, rejection=True)
    f = packet.fields
    usage = receipt_objects(packet, USAGE_EVIDENCE_PREFIX)
    receipt_require(not receipt_proposed_id(packet.item_id) and f.get("Task packet") == "3" and packet.status == "failed-rejected"
                    and value["authorizer"] == f.get("Accountable owner") == f.get("Completed human_owner")
                    and receipt_time(value["rejected_at"]).strftime("%Y-%m-%dT%H:%M:%SZ") == f.get("Closed At")
                    and all(f.get(k) == "none" for k in ("Claimed At", "Implementation Duration", "Candidate base", "Candidate commit", "Submitted At", "Submitted owner", "Decision owner"))
                    and tuple(f.get(k) for k in PACKET_USAGE) == ("0", "none", "$0") and f.get("Estimated AWS Cost") == "$0"
                    and usage == [{"schema": 1, "mode": "human-only"}], "ordinary rejection does not match claimless packet accounting")
    return value


def packet_graph_errors(packets: dict[str, Packet]) -> list[str]:
    """Validate declared relationships using current explicit restart semantics."""
    errors = []
    def fail(item_id, message):
        errors.append(f"{item_id}: {message}")
    def declared(packet, field):
        value = packet.fields.get(field, "none")
        if value == "none":
            return ()
        values = tuple(value.split(", "))
        if not values or any(not RECEIPT_ITEM_RE.fullmatch(v) for v in values):
            fail(packet.item_id, f"{field} must contain local WQE IDs separated by comma and space")
            return ()
        if len(values) != len(set(values)):
            fail(packet.item_id, f"{field} contains duplicate IDs")
        return values
    dependencies = {p.item_id: declared(p, "Dependencies") for p in packets.values()}
    previous, following, replacements = {}, {}, {}
    for packet in packets.values():
        for field, table in (("Previous attempt", previous), ("Next attempt", following), ("Replaces", replacements)):
            target = packet.fields.get(field, "none")
            if target in {"none", ""}:
                continue
            if not RECEIPT_ITEM_RE.fullmatch(target) or target not in packets or target == packet.item_id:
                fail(packet.item_id, f"{field} must name another existing local WQE")
                continue
            table[packet.item_id] = target
    def pending_restart(old, new):
        # require_successor needs a durable open draft before abandon writes the
        # predecessor's closing/Next fields. Its creation must remain admissible.
        return (old.status == new.status == "open" and not old.run and not new.run
                and previous.get(new.item_id) == old.item_id and replacements.get(new.item_id) == old.item_id
                and old.item_id not in following)
    roots = {}
    def attempt(item_id):
        chain = []
        current = item_id
        while current in packets and current not in roots and current not in chain:
            chain.append(current)
            parent = previous.get(current) or replacements.get(current)
            if parent is None:
                root = re.sub(r"-ATTEMPT-[1-9][0-9]*$", "", current)
                break
            current = parent
        else:
            if current in chain:
                fail(item_id, "cyclic attempt links: " + " -> ".join(chain + [current]))
                root = min(chain)
            else:
                root = roots.get(current, current)
        roots.update((member, root) for member in chain)
        return root
    def work_family(item_id):
        packet = packets[item_id]
        return ("run", packet.run[0]) if packet.run else ("item", family_root(attempt(item_id)))
    families = {}
    graph = {}
    successors = {}
    for packet in packets.values():
        families.setdefault(attempt(packet.item_id), []).append(packet)
        pred_id = previous.get(packet.item_id)
        replacement_id = replacements.get(packet.item_id)
        if pred_id and replacement_id and pred_id != replacement_id:
            fail(packet.item_id, "Previous attempt and Replaces disagree")
        parent_id = pred_id or replacement_id
        if parent_id:
            predecessor = packets[parent_id]
            successors.setdefault(parent_id, set()).add(packet.item_id)
            draft = pending_restart(predecessor, packet)
            if predecessor.status not in {"failed-rejected", "failed-abandoned"} and not draft:
                fail(packet.item_id, "only rejected or explicitly restarted work may have a successor")
            if pred_id and following.get(pred_id) != packet.item_id and not draft:
                fail(packet.item_id, "attempt links must be reciprocal")
            if predecessor.status == "failed-abandoned" and (pred_id != parent_id or replacement_id != parent_id):
                fail(packet.item_id, "restart must name its predecessor in Previous attempt and Replaces")
        next_id = following.get(packet.item_id)
        if next_id:
            successors.setdefault(packet.item_id, set()).add(next_id)
            if previous.get(next_id) != packet.item_id:
                fail(packet.item_id, "attempt links must be reciprocal")
            if packet.status not in {"failed-rejected", "failed-abandoned"}:
                fail(packet.item_id, "only rejected or explicitly restarted work may have a successor")
            if packet.status == "failed-abandoned" and replacements.get(next_id) != packet.item_id:
                fail(packet.item_id, "restart successor must name the abandoned predecessor in Replaces")
    for item_id, children in successors.items():
        if len(children) > 1:
            fail(item_id, "attempt has multiple successors")
    for root_id, members in families.items():
        completed = [p for p in members if p.status == "completed"]
        active = [p for p in members if p.status not in TERMINAL]
        if len(completed) > 1:
            fail(root_id, "attempt family has multiple completed attempts")
        if len(active) > 1 and not (len(active) == 2 and (pending_restart(*active) or pending_restart(*reversed(active)))):
            fail(root_id, "attempt family has multiple nonterminal tails")
        modern = any(p.item_id in replacements for p in members)
        numbered = [(int(m[1]) if (m := re.search(r"-ATTEMPT-([1-9][0-9]*)$", p.item_id)) else 1, p) for p in members]
        conventional = all(re.sub(r"-ATTEMPT-[1-9][0-9]*$", "", p.item_id) == root_id for p in members)
        if conventional and not modern and (len(members) > 1 or numbered[0][0] != 1):
            ordinals = sorted(n for n, _p in numbered)
            if ordinals != list(range(1, max(ordinals) + 1)):
                fail(root_id, "attempt ordinals must begin at 1 and be contiguous")
            ordered = [p for _n, p in sorted(numbered, key=lambda pair: (pair[0], pair[1].item_id))]
            for index, packet in enumerate(ordered):
                expected_previous = ordered[index - 1].item_id if index else None
                expected_next = ordered[index + 1].item_id if index + 1 < len(ordered) else None
                if previous.get(packet.item_id) != expected_previous or following.get(packet.item_id) != expected_next:
                    fail(packet.item_id, "attempt links must be reciprocal and contiguous")
                if packet.status not in TERMINAL and index != len(ordered) - 1:
                    fail(packet.item_id, "nonterminal attempt must be the tail")
        if len(members) > 1:
            for packet in members:
                if packet.fields.get("Task packet") != "3" and not packet_has_biq_admission(packet):
                    fail(packet.item_id, "linked attempts require Task packet 3")
        current = completed or active
        graph[root_id] = tuple(sorted({attempt(d) for p in current for d in dependencies[p.item_id] if d in packets}))
        for packet in members:
            targets = [attempt(d) for d in dependencies[packet.item_id] if d in packets]
            if root_id in targets:
                fail(packet.item_id, "dependency names its own attempt family")
            if len(targets) != len(set(targets)):
                fail(packet.item_id, "dependencies name one attempt family more than once")
            for dep in dependencies[packet.item_id]:
                if dep not in packets:
                    fail(packet.item_id, f"dependency names missing {dep}")
                    continue
                resolved = resolve_replacement(packets, dep)
                target = packets[resolved]
                boundary = RECEIPT_BATCH_RE.fullmatch(packet.item_id) and RECEIPT_BATCH_RE.fullmatch(dep) and packets[dep].status == "failed-abandoned"
                if packet.status not in TERMINAL and target.status in TERMINAL - {"completed"} and not boundary:
                    fail(packet.item_id, f"dependency {dep} has no completed or active replacement")
                if packet.status == "completed" and not boundary and not any(p.status == "completed" for p in families[attempt(dep)]):
                    fail(packet.item_id, f"completed work has incomplete dependency {dep}")
    try:
        tuple(TopologicalSorter(graph).static_order())
    except CycleError as error:
        errors.append("cyclic dependency families: " + " -> ".join(error.args[1]))
    repeats = {}
    for packet in packets.values():
        match = re.search(r"-REPEAT-([1-9][0-9]*)$", packet.item_id)
        identity = packet.run or ((packet.item_id[:match.start()], int(match[1])) if match else None)
        if identity is None:
            continue
        family, ordinal = identity
        if re.search(r"-REPEAT-[1-9][0-9]*$", family):
            fail(packet.item_id, "repeat identities cannot be nested")
        if packet.fields.get("Task packet") != "3":
            fail(packet.item_id, "repeating work requires Task packet 3")
        repeats.setdefault((bool(packet.run), family), []).append(ordinal)
    for (_run, family), ordinals in repeats.items():
        if sorted(ordinals) != list(range(1, max(ordinals) + 1)):
            fail(family, "repeat ordinals must begin at 1 and be contiguous")
    projects, operative = {}, {}
    for container in packets.values():
        mode = container.fields.get("Container mode")
        if mode is None:
            continue
        if mode not in {"project", "operative", "quarantine"}:
            fail(container.item_id, "Container mode is not supported")
            continue
        members = declared(container, "Container members")
        if not members:
            fail(container.item_id, "container must name at least one member")
        if mode == "project":
            if container.dependencies or container.fields.get("Architecture gate", "not-required") != "not-required":
                fail(container.item_id, "workstream must be dependency-free with no architecture gate")
            if container.status not in {"open", *TERMINAL} or container.run or RECEIPT_BATCH_RE.fullmatch(container.item_id) or receipt_proposed_id(container.item_id):
                fail(container.item_id, "workstream must be an ordinary open or terminal packet")
        historical = container.status in TERMINAL and (mode == "quarantine" or RECEIPT_BATCH_RE.fullmatch(container.item_id))
        rejected_project = False
        if mode == "project":
            try:
                rejected_project = packet_ordinary_rejection(container) is not None
            except BiqError as error:
                fail(container.item_id, str(error))
        question = container.fields.get("Kind") == "architecture" and container.status not in TERMINAL
        if question and (mode != "operative" or container.dependencies or container.fields.get("Architecture gate") not in {"pending", "accepted"}):
            fail(container.item_id, "architecture question must be an operative, dependency-free pending/accepted decision")
        seen_families = {}
        for member_id in members:
            if member_id not in packets:
                fail(container.item_id, f"Container members names missing {member_id}")
                continue
            member = packets[member_id]
            if member_id == container.item_id:
                fail(container.item_id, "container cannot contain itself")
                continue
            if historical:
                continue
            if (mode == "project" and member.fields.get("Container mode") == "project") or (mode != "project" and member.fields.get("Container mode")):
                fail(container.item_id, f"unsupported nested container {member_id}")
            if mode == "project" and container.item_id in member.dependencies:
                fail(container.item_id, f"member {member_id} cannot depend on its workstream")
            if question and (container.item_id not in member.dependencies or member.fields.get("Architecture gate") != "not-required" or member.fields.get("Kind") == "architecture"):
                fail(container.item_id, f"architecture child {member_id} must be real work depending on this question with no separate gate")
            if mode == "project":
                key = work_family(member_id)
                prior = seen_families.get(key)
                if prior is not None and not (pending_restart(prior, member) or pending_restart(member, prior)):
                    fail(container.item_id, "workstream names one work family more than once")
                # A restart draft must join its predecessor's workstream before
                # abandon replaces the predecessor's direct membership.
                seen_families.setdefault(key, member)
                if not rejected_project and key in projects and projects[key] != container.item_id:
                    fail(member_id, f"multiple workstream parents: {projects[key]}, {container.item_id}")
                if not rejected_project:
                    projects[key] = container.item_id
                if container.status in TERMINAL and not rejected_project and any(p.status not in TERMINAL for p in packets.values() if work_family(p.item_id) == key):
                    fail(container.item_id, f"closed workstream has active member family {member_id}")
            elif not container.run and not RECEIPT_BATCH_RE.fullmatch(container.item_id):
                if member_id in operative and operative[member_id] != container.item_id:
                    fail(member_id, f"multiple operative parents: {operative[member_id]}, {container.item_id}")
                operative[member_id] = container.item_id
    for packet in packets.values():
        if packet.fields.get("Kind") == "architecture" and packet.status not in TERMINAL and packet.fields.get("Container mode") != "operative":
            fail(packet.item_id, "active architecture question must contain its real-work children")
        if not receipt_proposed_id(packet.item_id):
            continue
        try:
            records = receipt_objects(packet, "Work-queue proposal v1: ", fields={"schema", "producer"}, canonical=True)
            if len(records) != 1:
                raise ValueError("one proposal record is required")
            producer = records[0]["producer"]
            if not isinstance(producer, str) or producer not in packets or receipt_proposed_id(producer):
                raise ValueError("proposal producer must name an existing non-proposed WQE")
        except (BiqError, ValueError, TypeError) as error:
            fail(packet.item_id, str(error))
    return errors


def packet_reference_errors(root: Path, packets: dict[str, Packet], revision: str | None) -> list[str]:
    """Resolve complete typed reading and architecture metadata using source blobs."""
    errors, contents = [], {}
    def content(path, *, text_needed=True):
        if path not in contents or text_needed and contents[path] is None:
            if revision is None:
                try:
                    if not (root / path).is_file():
                        raise OSError("not a file")
                    contents[path] = (root / path).read_text(encoding="utf-8") if text_needed else None
                except (OSError, UnicodeError) as error:
                    raise BiqError(f"referenced file is missing or unreadable: {path}") from error
            else:
                entry = subprocess.run(["git", "ls-tree", "-z", revision, "--", path], cwd=root, capture_output=True)
                if entry.returncode or b" blob " not in entry.stdout.split(b"\t", 1)[0]:
                    raise BiqError(f"referenced file is missing or unreadable: {path}")
                contents[path] = None
                if text_needed:
                    shown = subprocess.run(["git", "show", f"{revision}:{path}"], cwd=root, capture_output=True)
                    try:
                        if shown.returncode:
                            raise UnicodeError("unreadable blob")
                        contents[path] = shown.stdout.decode("utf-8")
                    except UnicodeError as error:
                        raise BiqError(f"referenced file is missing or unreadable: {path}") from error
        return contents[path]
    def anchors(text):
        return markdown_anchors("\n".join(line for line, outside in markdown_lines(text) if outside))
    for packet in packets.values():
        if packet.fields.get("Task packet") not in {"2", "3"}:
            continue
        try:
            for role, path, anchor in packet_reading(packet):
                if role == "creates-output":
                    continue
                try:
                    text = content(path, text_needed=anchor is not None)
                except BiqError as error:
                    raise BiqError(f"{role} must name an existing input ({path}); use creates-output for a path this WQE will create") from error
                if anchor and anchor not in anchors(text):
                    raise BiqError(f"{role} heading is missing: {path}#{anchor}")
            impact = packet.fields.get("Documentation impact", "none")
            if impact != "none":
                for path in impact.split(", "):
                    reference_target(path)
                    content(path, text_needed=False)
            gate = packet.fields.get("Architecture gate")
            if gate not in {"pending", "accepted"}:
                continue
            path, anchor = reference_target(packet.fields.get("Architecture decision", ""))
            text = content(path)
            if anchor not in anchors(text):
                raise BiqError("Architecture decision heading is missing")
            lines = [(n, line) for n, (line, outside) in enumerate(markdown_lines(text)) if outside]
            raw_status = [n for n, line in lines if line.startswith("- **Decision status:**")]
            raw_owner = [n for n, line in lines if line.startswith("- **Decision owner:**")]
            statuses = [(n, m[1].strip()) for n, line in lines if (m := re.fullmatch(r"- \*\*Decision status:\*\* `([^`]+)`", line))]
            owners = [(n, m[1].strip()) for n, line in lines if (m := re.fullmatch(r"- \*\*Decision owner:\*\* `([^`]+)`", line))]
            if len(raw_status) != 1 or len(raw_owner) != 1 or len(statuses) != 1 or len(owners) != 1 or not concrete_human_name(owners[0][1]):
                raise BiqError("Architecture decision needs one status and concrete owner")
            first_section = next((n for n, line in lines if re.match(r"^ {0,3}##(?:\s|$)", line)), len(text.splitlines()))
            if not statuses[0][0] < owners[0][0] < first_section:
                raise BiqError("Architecture status must precede owner in top metadata")
            if statuses[0][1] != ("proposed" if gate == "pending" else "accepted"):
                raise BiqError(f"Architecture decision status does not match {gate} gate")
            if gate == "pending" and owners[0][1] != packet.fields.get("Architecture decision owner"):
                raise BiqError("Architecture decision owner differs from its brief")
        except BiqError as error:
            errors.append(f"{packet.item_id}: {error}")
    return errors

REFERENCE_RE = re.compile(r"`([^`\s#]+\.md)#([A-Za-z0-9_.-]+)`")


def markdown_anchors(text: str) -> set[str]:
    """GitHub heading anchors outside fenced blocks, as the report's validator computes them."""
    anchors: set[str] = set()
    counts: dict[str, int] = {}
    fenced = False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        stripped = line.lstrip(" ")
        if fenced or not stripped.startswith("#") or len(line) - len(stripped) > 3:
            continue
        match = re.match(r"#{1,6}\s+(.*?)\s*#*\s*$", stripped)
        if match is None:
            continue
        base = re.sub(r"[\s-]+", "-", re.sub(r"[^\w\- ]", "", match.group(1).lower())).strip("-")
        ordinal = counts.get(base, 0)
        counts[base] = ordinal + 1
        anchors.add(base if ordinal == 0 else f"{base}-{ordinal}")
    return anchors


def reference_errors(root: Path, packets: dict[str, Packet], revision: str | None) -> list[str]:
    """An active packet's Required reading names files and headings that exist; the HTML report refuses one that does not."""
    errors = []
    texts: dict[str, str | None] = {}
    for item_id, packet in sorted(packets.items()):
        if packet.status not in {"open", "submitted"} or packet.run:  # run WQEs carry the tool's own reading
            continue
        references = []
        for line in packet.sections.get("Required reading", "").splitlines():
            typed = READING_LINE.fullmatch(line)
            if typed:
                role, target = typed.groups()
                path, anchor = reference_target(target)
                references.append((role, path, anchor))
            else:  # grammar admission belongs to additional validation; this reader preserves historical lines
                references.extend(("existing-input", path, anchor) for path, anchor in REFERENCE_RE.findall(line))
        for role, path, anchor in references:
            if role == "creates-output" or anchor is None:
                continue
            if path.startswith(ITEMS_DIR + "/"):  # a packet is resolved by the queue, not the tree
                continue
            if path not in texts:
                if revision is None:
                    texts[path] = (root / path).read_text(encoding="utf-8") if (root / path).is_file() else None
                else:
                    shown = subprocess.run(["git", "show", f"{revision}:{path}"], cwd=root, text=True, capture_output=True)
                    texts[path] = shown.stdout if shown.returncode == 0 else None
            if texts[path] is None:
                errors.append(f"{item_id}: {role} must name an existing input ({path}); use creates-output for a path this WQE will create")
            elif anchor not in markdown_anchors(texts[path]):
                errors.append(f"{item_id}: {role} {path}#{anchor} names no heading there")
    return errors


def lint(registry: Registry, packets: dict[str, Packet]) -> list[str]:
    errors = []
    open_runs: dict[str, int] = {}
    for item_id, packet in sorted(packets.items()):
        if not packet.source.startswith(f"# {item_id}"):
            errors.append(f"{item_id}: first line must be the title `# {item_id}: ...`")
        if packet.fields.get("ID") != item_id:
            errors.append(f"{item_id}: ID field differs from the file name")
        if packet.fields.get("Priority", "production") not in {"production", "on-hold"}:
            errors.append(f"{item_id}: Priority must be production or on-hold")
        if packet.status not in STATUSES:
            errors.append(f"{item_id}: unknown Status {packet.status!r}")
        if ":" in packet.fields.get("Dependencies", ""):
            errors.append(f"{item_id}: Dependencies must name local WQEs; cross-repository needs are met by adopting a release")
        for dep in packet.dependencies:
            if dep not in packets:
                errors.append(f"{item_id}: dependency {dep} has no packet")
        if packet.replaces and packet.replaces not in packets:
            errors.append(f"{item_id}: Replaces {packet.replaces} has no packet")
        commit, base = packet.fields.get("Candidate commit", "pending"), packet.fields.get("Candidate base", "pending")
        absent = {"pending", "none"}
        if (commit in absent) != (base in absent) or (commit not in absent and not COMMIT_RE.match(commit)):
            errors.append(f"{item_id}: candidate commit and base must both be absent or both be commits")
        if packet.status == "submitted" and not packet.run and packet.candidate is None:
            errors.append(f"{item_id}: submitted without a candidate")
        if "Resolution" not in packet.sections:
            errors.append(f"{item_id}: no Resolution section")
        if packet.run:
            queue_id, _ordinal = packet.run
            if queue_id not in registry.queues:
                errors.append(f"{item_id}: unknown queue {queue_id}")
            if run_state(packet) == "open":
                open_runs[queue_id] = open_runs.get(queue_id, 0) + 1
            for member, _sha, _note in packet.members():
                if member not in packets:
                    errors.append(f"{item_id}: member {member} has no packet")
            for queue in packet.queues:
                if queue not in registry.queues:
                    errors.append(f"{item_id}: unknown queue {queue}")
        else:
            for queue in packet.queues:
                if queue not in registry.queues:
                    errors.append(f"{item_id}: unknown queue {queue}")
            if requires_workstream(packet) and workstream_of(packets, item_id) is None:
                errors.append(f"{item_id}: no workstream; every WQE created from {WORKSTREAM_SINCE[:10]} joins one (biq workstream {item_id})")
            if packet.status in {"submitted", "failed-abandoned"} and not packet.fields.get("Container mode") \
                    and packet.fields.get("Created", "") >= WORKSTREAM_SINCE and retrospective_reference(packet) is None:
                errors.append(f"{item_id}: {packet.status} without a retrospective reference; every WQE created from {WORKSTREAM_SINCE[:10]} publishes one before submit or abandon")
    for queue_id in registry.queues:
        if open_runs.get(queue_id, 0) != 1:
            errors.append(f"{queue_id}: {open_runs.get(queue_id, 0)} open run WQEs (expected 1; run materialize --apply)")
    return errors


# --- views -------------------------------------------------------------------

def render_report(root: Path, registry: Registry, packets: dict[str, Packet], target_tip: str) -> str:
    lines = [f"target {registry.target} @ {target_tip[:12]}"]

    def render(queue: Queue, depth: int) -> None:
        pad = "  " * depth
        lines.append(f"{pad}{queue.id} ({queue.kind}, {usd(queue.budget_usd)}/run)")
        for run in runs_of(packets, queue.id):
            if run.status in TERMINAL:
                continue
            members = run.members()
            if run_state(run) == "open":
                in_flight = in_flight_runs(packets, queue.id)
                ready, waiting, _chain = readiness(root, registry, packets, queue, members, target_tip, in_flight[-1] if in_flight else None)
                lines.append(f"{pad}  {run.item_id} (open): {len(members)} members, {len(ready)} ready")
                for item, sha, _note in ready:
                    lines.append(f"{pad}    - {item} @{sha[:12]} ready")
                for item, sha, _note, reason in waiting:
                    lines.append(f"{pad}    - {item} @{sha[:12]} {reason}")
            else:
                tests = ", ".join(f"{n}={r}" for n, r in run.tests.items()) or "none"
                lines.append(f"{pad}  {run.item_id} ({run_state(run)}): {len(members)} members, {run.fields.get('Cost', '$0')} of {run.fields.get('Budget', '$0')}, tests {tests}")
                for item, sha, note in members:
                    lines.append(f"{pad}    - {item} @{sha[:12]}" + (f" ({note})" if note else ""))
        for feeder in sorted(queue.feeders):
            render(registry.queues[feeder], depth + 1)

    for queue in sorted(registry.roots(), key=lambda q: q.id):
        render(queue, 0)
    return "\n".join(lines)


def render_show(root: Path, registry: Registry, packets: dict[str, Packet], item_id: str, target_tip: str) -> str:
    packet = require_packet(packets, item_id)
    lines = [f"{item_id}: {run_state(packet) if packet.run else packet.status}"]
    holds = priority_hold_sources(item_id, packets)
    if holds:
        lines.append("priority: On Hold via " + ", ".join(holds))
    _oid, claim = read_claim(root, item_id)
    lines.append("claim: " + (f"{claim.get('owner')} since {claim.get('started_at')}" if claim else "none"))
    if packet.run:
        for name in ("Queue", "Base", "Predecessor", "Integration commit", "Result commit", "Tests", "Cost", "Budget"):
            lines.append(f"{name.lower()}: {packet.fields.get(name, 'pending')}")
        queue = registry.queues[packet.run[0]]
        if queue.qualification:
            lines.append(f"qualification: {queue.qualification}")
        for test in queue.tests:
            meta = registry.tests.get(test)
            lines.append(f"  {test}: {registry.commands.get(test, 'no command registered')}" + (f" ({meta['target']}, {meta['cost']})" if meta else ""))
        lines.append("members: " + (", ".join(m[0] + (f" ({m[2]})" if m[2] else "") for m in packet.members()) or "none"))
        pending = pending_resolutions(packet)
        if pending:
            lines.append("awaiting resolution: " + ", ".join(f"{item} ({reason})" for item, reason in sorted(pending.items())))
        lines.append("excluded: " + (packet.sections.get("Excluded", "").strip() or "none").replace("\n", "; "))
        return "\n".join(lines)
    lines.append("candidate: " + (packet.candidate or "pending"))
    lines.append("dependencies: " + (", ".join(packet.dependencies) or "none"))
    if packet.status not in TERMINAL:
        for dep, state, _startable, action in dependency_states(root, packets, item_id, target_tip):
            lines.append(f"  {dep}: {state}; {action}")
    for branch, sha in pending_control_branches(root, item_id, registry.target):
        lines.append(f"control: PENDING on {branch} at {sha[:12]}; {registry.target} still shows {packet.status}; rerun the command that wrote it to follow its change request")
    unmet = unmet_dependencies(root, packets, item_id, target_tip, set())
    if packet.status == "submitted":
        lines.append("queues: " + (", ".join(packet.queues) or "none; lands directly"))
        lines.append("landing waits for: " + (", ".join(unmet) or "nothing"))
        holding = []
        for q in packet.queues:
            for r in runs_of(packets, q):
                for member, _sha, note in r.members():
                    if member == item_id:
                        holding.append(r.item_id + f" ({run_state(r)}" + (f"; {note}" if note else "") + ")")
        lines.append("runs: " + (", ".join(holding) or "none"))
        if claim is not None and not landed(root, packets, item_id, target_tip):
            lines.append(f"handoff: claim still held by {claim.get('owner')}; the submission is complete once its control commit is on {registry.target}, and integration uses the run's claim even for the same agent")
        elif claim is None:
            lines.append("handoff: complete; no claim held")
    if packet.replaces:
        lines.append(f"replaces: {packet.replaces}")
    for step in next_steps(root, registry, packets, packet, claim, target_tip, unmet):
        lines.append(f"next: {step}")
    return "\n".join(lines)


def next_steps(root: Path, registry: Registry, packets: dict[str, Packet], packet: Packet, claim: dict | None, target_tip: str, unmet: list[str]) -> list[str]:
    """What the owner runs next, from the packet's state; empty when the queue has it or it is closed."""
    item_id = packet.item_id
    owner = (claim or {}).get("owner") or "<you>"
    tool = "python3 tools/biq.py"
    holds = priority_hold_sources(item_id, packets)
    notices = ["OVER BOUND: " + line.split(": ", 1)[1] for line in packet_bound_errors({item_id: packet})]
    if packet.status not in TERMINAL and is_leaf_work(packet):  # this checkout's packet against the target's: what a control write of it would be refused for
        shown = subprocess.run(["git", "show", f"{target_tip}:{ITEMS_DIR}/{item_id}.md"], cwd=root, text=True, capture_output=True)
        if shown.returncode == 0 and shown.stdout != packet.source:
            notices += [("FIXED AT CLAIM: " if "changed after claim" in line else "OVER BOUND: ") + line.split(": ", 1)[1]
                        for line in packet_change_errors(root, {item_id: parse_packet(item_id, shown.stdout)}, {item_id: packet})]
    if notices:
        return notices + (next_steps_after(root, registry, packets, packet, claim, target_tip, unmet) or [])
    return next_steps_after(root, registry, packets, packet, claim, target_tip, unmet)


def next_steps_after(root: Path, registry: Registry, packets: dict[str, Packet], packet: Packet, claim: dict | None, target_tip: str, unmet: list[str]) -> list[str]:
    item_id = packet.item_id
    owner = (claim or {}).get("owner") or "<you>"
    tool = "python3 tools/biq.py"
    holds = priority_hold_sources(item_id, packets)
    if packet.status == "open" and holds:
        return [f"On Hold via {', '.join(holds)}; pause development and move each hold to production before continuing"]
    if packet.status == "open":
        if requires_workstream(packet) and workstream_of(packets, item_id) is None:
            return [f"{tool} workstream {item_id}, then join the workstream it names (--join <WORKSTREAM> --owner <you> --apply) before claiming"]
        if claim is None:
            unstarted = [f"{dep} is {state}" for dep, state, startable, _action in dependency_states(root, packets, item_id, target_tip) if not startable]
            if unstarted:
                return ["not startable: " + "; ".join(unstarted) + "; a WQE starts only after every dependency is submitted"]
            return [f"{tool} claim {item_id} --owner <you>"]
        if requires_retrospective(packet) and packet.candidate is not None:
            try:
                find_retrospective(root, item_id)
            except BiqError:
                return [f"publish the retrospective: copy {RETROSPECTIVES_DIR}/TEMPLATE.md to {RETROSPECTIVES_DIR}/<yyyy>-<mm>-<slug>.md naming `{item_id}` on its `- WQE:` line, list it in {RETROSPECTIVE_INDEX}, commit both, then {tool} submit {item_id} --owner {owner} --apply"]
        if packet.candidate is None:
            return [f"commit the work on your own branch from the packet's Baseline (do not merge, rebase onto, or track {registry.target}) with the trailer `Work-Queue-Item: {item_id}`, then {tool} seal {item_id} --owner {owner} --candidate <sha>"]
        return [f"{tool} submit {item_id} --owner {owner} --apply, then commit docs/work-queue as a control commit and open the gated change request; integrating with current {registry.target} is the run's job"]
    if packet.status == "submitted":
        if landed(root, packets, item_id, target_tip):
            return []
        if unmet:
            return ["landing waits for " + ", ".join(unmet) + " to reach " + registry.target + " or a run; the queue owns that wait and there is nothing for the owner to do"]
        if not packet.queues:
            return [f"once the control commit is on {registry.target}: {tool} land {item_id} --apply, then open the gated change request from the staged branch"]
        if claim is not None:
            holder = str(claim.get("owner"))
            integrating = any(
                (read_claim(root, r.item_id)[1] or {}).get("owner") == holder
                for q in packet.queues for r in runs_of(packets, q) if any(m[0] == item_id for m in r.members())
            )
            if integrating:
                return [f"you hold the run's claim too: integrate it there; the member claim is released once its submission control commit is on {registry.target}: {tool} release {item_id} --owner {holder}"]
            return [f"the submission is handed off; release the member claim: {tool} release {item_id} --owner {holder} (the queue integrates it under the run's own claim)"]
        stranded = [line for line in stranded_submissions(packets) if line.startswith(item_id + ":")]
        if stranded:
            return ["STRANDED: " + stranded[0].split(": ", 1)[1]]
        return ["nothing; the queue runs it"]
    return []


def render_list(packets: dict[str, Packet], status_filter: str | None, priority: str = "production") -> str:
    rows = []
    for item_id, packet in sorted(packets.items()):
        holds = priority_hold_sources(item_id, packets)
        if status_filter and packet.status != status_filter:
            continue
        if priority != "all" and bool(holds) != (priority == "on-hold"):
            continue
        detail = f"\tOn Hold via {', '.join(holds)}" if holds else ""
        rows.append(f"{packet.status}\t{item_id}{detail}")
    return "\n".join(rows)


# --- CLI -------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0] + " One WQE: claim; commit the work on the target branch; seal; submit --apply, which lands its own control commit. `show <ID>` prints the next step.")
    parser.add_argument("--repository", type=Path, default=None)
    parser.add_argument("--target-ref", default=None, help="ref of the target branch tip (default origin/<target>)")
    sub = parser.add_subparsers(dest="command", required=True)
    lst = sub.add_parser("list", help="packets by status"); lst.add_argument("--status"); lst.add_argument("--priority", choices=("production", "on-hold", "all"), default="production", help="production intake (default), On Hold work, or both")
    show = sub.add_parser("show", help="one packet: state, claim, candidate, queues, and the next step"); show.add_argument("item_id")
    sub.add_parser("report", help="the queues and their members")
    sub.add_parser("lint", help="check every packet and the registry")
    mat = sub.add_parser("materialize", help="restore a missing open run WQE"); mat.add_argument("--apply", action="store_true")
    claim = sub.add_parser("claim", help="take the live claim on a packet"); claim.add_argument("item_id"); claim.add_argument("--owner", required=True); claim.add_argument("--note", default=""); claim.add_argument("--usage-snapshot", help="JSON file with the agent's usage counters at claim start, stored as usage_baseline"); claim.add_argument("--authorized-by", help="the human who assigned the batch integrator role; required to claim a run WQE")
    claim.add_argument("--repair-candidate-base", action="store_true", help="repair only the existing open ordinary claim base from its durable packet Baseline; run at the unchanged claim HEAD, branch, and worktree")
    release = sub.add_parser("release", help="release the live claim"); release.add_argument("item_id"); release.add_argument("--owner", required=True)
    seal = sub.add_parser("seal", help="record the exact candidate commit; target conflicts belong to the integrator"); seal.add_argument("item_id"); seal.add_argument("--owner", required=True); seal.add_argument("--candidate", default="HEAD", help="full SHA from git rev-parse HEAD in the implementation checkout; pass explicitly")
    submit = sub.add_parser("submit", help="offer the sealed candidate to every queue; --apply records it"); submit.add_argument("item_id"); submit.add_argument("--owner", required=True); submit.add_argument("--apply", action="store_true")
    submit.add_argument("--usage-snapshot", help="JSON usage cutoff; priced against the claim baseline and the rate card"); submit.add_argument("--aws-cost", help="Estimated AWS Cost, e.g. $12.50")
    submit.add_argument("--charged-to", help="WQE that owns this claim's model usage"); submit.add_argument("--human-only", action="store_true", help="model-free human work")
    submit.add_argument("--retrospective", metavar="PATH", help="the packet's retrospective (default: the one file under docs/work-queue/retrospectives naming it)")
    land = sub.add_parser("land", help="stage the landing merge on biq/land/<id>"); land.add_argument("item_id"); land.add_argument("--apply", action="store_true"); land.add_argument("--authorized-by", metavar="HUMAN", help="land a leaf that queues care about on the accountable human's word, without their runs")
    abandon = sub.add_parser("abandon", help="close an open packet as failed-abandoned"); abandon.add_argument("item_id"); abandon.add_argument("--owner", required=True); abandon.add_argument("--reason", required=True); abandon.add_argument("--retrospective", metavar="PATH"); abandon.add_argument("--successor", metavar="ID", help="restart: the fresh open packet that names this one in Previous attempt and Replaces"); abandon.add_argument("--apply", action="store_true")
    reject = sub.add_parser("reject", help="close a submitted leaf as failed-rejected, on the accountable human's word"); reject.add_argument("item_id"); reject.add_argument("--reason", required=True); reject.add_argument("--authorized-by", required=True, metavar="HUMAN"); reject.add_argument("--successor", metavar="ID", help="what meets a dependency on this packet now: a fresh open restart naming it in Previous attempt and Replaces, or the completed packet whose landing met the objective"); reject.add_argument("--owner", help="owner of every affected started run"); reject.add_argument("--apply", action="store_true")
    ws = sub.add_parser("workstream", help="the packet's workstream, or ranked suggestions; --join lists it in one"); ws.add_argument("item_id")
    ws.add_argument("--join", metavar="WORKSTREAM", help="the open `Container mode: project` WQE to list the packet in"); ws.add_argument("--owner"); ws.add_argument("--apply", action="store_true")
    verify = sub.add_parser("verify", help="the landing gate for one base..head edge"); verify.add_argument("--base", required=True); verify.add_argument("--head", required=True)
    adoption = sub.add_parser("adoption", help="compare installed files at a commit with their tagged release; read-only, no fetch or queue setup")
    adoption.add_argument("--upstream", type=Path, required=True, help="local clone holding the release tag and objects")
    adoption.add_argument("--tree", default="HEAD", help="installed commit to inspect (default HEAD)")
    adoption.add_argument("--provider", choices=tuple(ADOPTION_GATE_FILES), default="github", help="installed landing-gate provider (default github)")
    run = sub.add_parser("run", help="start, fix, resolve, test, complete, or cancel a queue's run (integrator only, human-authorized)"); run_sub = run.add_subparsers(dest="run_command", required=True)
    rs = run_sub.add_parser("start"); rs.add_argument("queue_id"); rs.add_argument("--owner"); rs.add_argument("--apply", action="store_true")
    rf = run_sub.add_parser("fix", help="replace a started or submitted result with a descendant integration-fix commit and reset tests"); rf.add_argument("queue_id"); rf.add_argument("ordinal", type=int); rf.add_argument("--commit", required=True); rf.add_argument("--owner"); rf.add_argument("--apply", action="store_true")
    rr = run_sub.add_parser("resolve"); rr.add_argument("queue_id"); rr.add_argument("ordinal", type=int); rr.add_argument("item_id"); rr.add_argument("--commit", required=True); rr.add_argument("--owner"); rr.add_argument("--apply", action="store_true")
    rv = run_sub.add_parser("review", help="print each member's footprint, or record the integrator's verdict on one member"); rv.add_argument("queue_id"); rv.add_argument("ordinal", type=int); rv.add_argument("item_id", nargs="?"); rv.add_argument("--ok", action="store_true"); rv.add_argument("--reject", metavar="REASON"); rv.add_argument("--hold", metavar="QUESTION"); rv.add_argument("--owner"); rv.add_argument("--apply", action="store_true"); rv.add_argument("--authorized-by", metavar="HUMAN", help="the Accountable owner whose word a --reject carries")
    rt = run_sub.add_parser("test"); rt.add_argument("queue_id"); rt.add_argument("ordinal", type=int); rt.add_argument("name"); rt.add_argument("--result", required=True, choices=["green", "red"]); rt.add_argument("--owner"); rt.add_argument("--evidence"); rt.add_argument("--cost", type=float, default=0.0)
    rq = run_sub.add_parser("refresh", help="re-base a started or submitted run onto its predecessor's current tip and reset its tests"); rq.add_argument("queue_id"); rq.add_argument("ordinal", type=int); rq.add_argument("--owner"); rq.add_argument("--apply", action="store_true")
    rx = run_sub.add_parser("cancel"); rx.add_argument("queue_id"); rx.add_argument("ordinal", type=int); rx.add_argument("--owner"); rx.add_argument("--apply", action="store_true")
    rc = run_sub.add_parser("complete"); rc.add_argument("queue_id"); rc.add_argument("ordinal", type=int); rc.add_argument("--reject", action="append", default=[]); rc.add_argument("--owner"); rc.add_argument("--authorized-by", metavar="HUMAN", help="the Accountable owner whose word removes the rejected members"); rc.add_argument("--apply", action="store_true")
    args = parser.parse_args(argv)
    root = Path(git((args.repository or Path.cwd()).resolve(), "rev-parse", "--show-toplevel"))
    try:
        if args.command == "adoption":
            errors = adoption_errors(root, args.upstream.resolve(), args.tree, args.provider)
            for error in errors:
                print(error)
            print(f"adoption: {len(errors)} mismatches")
            return 2 if errors else 0
        if args.command == "verify":
            print(f"landing admitted: {verify_landing(root, rev(root, args.base), rev(root, args.head))}")
            return 0
        target = args.target_ref or f"{REMOTE}/main"
        fresh = True
        if args.target_ref is None:
            fresh = fetch_target(root, target)
            notice = staleness_notice(root, target)
            if notice and RELOCATED_ENV not in os.environ:  # once per command, not again after relocation
                print(notice, file=sys.stderr)
            relocate_to_target_tool(root, target, list(sys.argv[1:] if argv is None else argv), getattr(args, "owner", None))
        registry, packets, revision = queue_state(root, target, fresh)
        if revision is not None:
            print(f"queue state read from {revision}; this checkout's packets are not its", file=sys.stderr)
        if fresh and args.target_ref is None:
            try:
                for item_id in release_stale_claims(root, packets):
                    print(f"released the claim of {item_id}: {target} shows it {packets[item_id].status}", file=sys.stderr)
            except BiqError:
                pass  # the claim namespace is unreachable; the command itself decides what that means
        writes = args.command in WRITES_PACKETS and (getattr(args, "apply", False) or args.command == "seal" or (args.command == "run" and args.run_command == "test"))
        control = None
        control_operation = None
        deferred_publications: list[tuple[str, str]] = []
        deferred_branch_moves: list[tuple[str, str | None]] = []
        if writes:
            root, control, control_item, control_operation, pending_operation = write_target_for(args, root, revision, target, packets)
            if control is not None:
                target = remote_ref_of(target)
                if pending_operation is not None:
                    owner = getattr(args, "owner", None) or "agent"
                    followed = follow_control_landing(root, control, target, control_item, pending_operation["command"], owner)
                    code = report_landing(followed, control, target)
                    if pending_operation["same"]:
                        return code
                    print(f"{control} carries {pending_operation['label']}; rerun {control_operation['label']} after it lands", file=sys.stderr)
                    return code if code == 2 else PENDING_EXIT
                packets = load_packets(root)  # the pending control branch, rebased onto the target, is the state to continue from
                registry, revision = load_registry(root, packets=packets), None
                print(f"writing queue state in the control clone {root} on {control}", file=sys.stderr)
        if args.command == "lint":
            errors = lint(registry, packets) + reference_errors(root, packets, revision)
            additional = set(validation_errors(root, revision))
            existing = set(validation_errors(root, target))
            errors += sorted(additional - existing) + packet_change_errors(root, load_packets(root, target), packets)
            for error in errors:
                print(error)
            print(f"{len(packets)} packets, {len(registry.queues)} queues, {len(errors)} problems")
            if additional & existing:
                print(f"{len(additional & existing)} unchanged historical validation problems remain with their owners")
            return 2 if errors else 0
        if args.command == "list":
            print(render_list(packets, args.status, args.priority))
            return 0
        target_tip = rev(root, target)
        if args.command == "show":
            print(render_show(root, registry, packets, args.item_id, target_tip))
        elif args.command == "report":
            print(render_report(root, registry, packets, target_tip))
        elif args.command == "materialize":
            created = materialize(root, registry, packets, apply=args.apply, target_tip=target_tip)
            print(("created " if args.apply else "would create ") + (", ".join(created) or "nothing; every queue has its open run WQE and every member's dependencies"))
        elif args.command == "claim":
            data = claim_item(root, packets, args.item_id, args.owner, note=args.note, usage_snapshot=args.usage_snapshot, authorized_by=args.authorized_by, repair_candidate_base=args.repair_candidate_base)
            if args.repair_candidate_base:
                print(f"repaired {args.item_id} candidate base to {data['candidate_base']} at unchanged claim HEAD {data['head']}")
            else:
                print(f"claimed {args.item_id} for {args.owner} at {data['head'][:12]}")
            if data.get("successor"):
                print(f"wrote {data['successor']}; it lands with this command's control commit")
        elif args.command == "release":
            pending = pending_control_branches(root, args.item_id, target)
            if pending:
                raise BiqError(f"{args.item_id} has a control branch not yet on {target}: " + ", ".join(b for b, _ in pending)
                               + "; rerun the command that wrote it to follow its change request, then release")
            release_item(root, args.item_id, args.owner)
            print(f"released {args.item_id}")
        elif args.command == "seal":
            packet = seal_item(root, packets, args.item_id, args.candidate, target_tip, args.owner)
            print(f"sealed {args.item_id}: candidate {packet.candidate}")
            print(footprint_line(candidate_footprint(root, packet.fields["Candidate base"], packet.candidate)))
            if packet.status == "open":
                outcome = evaluate_submission(root, registry, packets, args.item_id, target_tip)
                status = changed_status(root, target_tip, packet.candidate)
                for qid, hits in sorted(outcome["answers"].items()):
                    print(f"{qid}: {describe_care(status, hits) if hits else 'no'}")
                print(f"next: python3 tools/biq.py submit {args.item_id} --owner {args.owner} --apply")
        elif args.command == "submit" and control is not None and args.apply and packets.get(args.item_id) is not None and packets[args.item_id].status == "submitted":
            print(f"{args.item_id} is already submitted on the pending {control}; following its change request")
        elif args.command == "submit":
            outcome = submit_item(root, registry, packets, args.item_id, target_tip, args.owner, apply=args.apply,
                                  usage_snapshot=args.usage_snapshot, aws_cost=args.aws_cost, charged_to=args.charged_to, human_only=args.human_only,
                                  retrospective=args.retrospective)
            status = changed_status(root, target_tip, outcome["candidate"])
            for qid, hits in sorted(outcome["answers"].items()):
                print(f"{qid}: {describe_care(status, hits) if hits else 'no'}")
            if outcome["implied"]:
                print("implied dependencies: " + ", ".join(outcome["implied"]))
            for added in outcome.get("implied_members", []):
                print("implied member: " + added)
            if outcome["queues"]:
                print("enqueued into " + ", ".join(open_run(packets, q).item_id for q in outcome["queues"]) if args.apply else "would enqueue into " + ", ".join(outcome["queues"]))
            else:
                print("no queue cares: land with `biq land` once its dependencies are on " + registry.target)
            if not args.apply:
                print("pass --apply to record the submission")
        elif args.command == "workstream":
            if args.join is None:
                for line in workstream_lines(packets, args.item_id):
                    print(line)
            elif args.apply:
                join_workstream(root, packets, args.join, args.item_id)
                print(f"{args.item_id} listed in {args.join}")
            else:
                if workstream_of(packets, args.item_id) not in {None, args.join}:
                    raise BiqError(f"{args.item_id} already belongs to {workstream_of(packets, args.item_id)}")
                require_packet(packets, args.join)
                print(f"would list {args.item_id} in {args.join}; pass --apply with --owner")
        elif args.command == "reject" and control is not None and args.apply and packets.get(args.item_id) is not None and packets[args.item_id].status == "failed-rejected":
            print(f"{args.item_id} is already rejected on the pending {control}; following its change request")
        elif args.command == "reject":
            reject_item(root, packets, args.item_id, args.reason, args.authorized_by, apply=args.apply,
                        successor=args.successor, registry=registry, owner=args.owner,
                        deferred_publications=deferred_publications if control is not None else None,
                        deferred_branch_moves=deferred_branch_moves if control is not None else None)
            print(f"{args.item_id}: failed-rejected on the word of {args.authorized_by}" if args.apply else f"{args.item_id}: would be rejected; pass --apply")
        elif args.command == "abandon" and control is not None and args.apply and packets.get(args.item_id) is not None and packets[args.item_id].status == "failed-abandoned":
            print(f"{args.item_id} is already abandoned on the pending {control}; following its change request")
        elif args.command == "abandon":
            abandon_item(root, packets, args.item_id, args.owner, args.reason, apply=args.apply, retrospective=args.retrospective, successor=args.successor, release=False)
            restarted = f" and restarted as {args.successor}" if args.successor else ""
            print(f"{args.item_id}: failed-abandoned{restarted}; the claim is released once the control commit lands" if args.apply else f"{args.item_id}: would be abandoned{restarted}; pass --apply")
        elif args.command == "land":
            if require_packet(packets, args.item_id).status == "completed":
                print(f"{args.item_id} is already completed on {target}: nothing pending")
                return 0
            landing_branch = f"biq/land/{args.item_id.lower()}"
            staged = pending_branch_tip(root, landing_branch, target)
            if staged is not None:
                # A staging is followed only while it still carries what the queue would land now; after
                # `run fix` the result moved, and following the old merge repeats the old gate failure.
                subject = landing_subject(root, registry, packets, args.item_id, target_tip, authorized_by=args.authorized_by)[0]
                merged = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{staged}^2"], cwd=root, text=True, capture_output=True).stdout.strip()
                if merged == subject:
                    request = open_change_request(root, landing_branch, target, f"queue: land {args.item_id}", f"Landing already staged by `biq land` for {args.item_id}.")
                    print(f"{args.item_id} already has a pending staged landing on {landing_branch}; following it")
                    return report_landing(follow_change_request(root, root, landing_branch, request, target, reconcile=False), landing_branch, target)
                print(f"{args.item_id}'s staged landing on {landing_branch} carries {merged[:12] or 'no merge'}, not the current result {subject[:12]}; restaging")
            outcome = prepare_landing(root, registry, packets, args.item_id, target_tip, apply=args.apply, authorized_by=args.authorized_by)
            print(f"{outcome['reason']}: merge {outcome['subject'][:12]} onto {target_tip[:12]}, completes " + ", ".join(outcome["completes"]))
            if args.apply:
                push_branch_with_lease(root, outcome["branch"], outcome["head"])
                request = open_change_request(root, outcome["branch"], target, f"queue: land {args.item_id}", f"Landing staged by `biq land` for {args.item_id}: {outcome['reason']}.")
                print(f"staged and pushed {outcome['branch']}")
                followed = follow_change_request(root, root, outcome["branch"], request, target, reconcile=False)
                return report_landing(followed, outcome["branch"], target)
            print("pass --apply to stage, push and follow the landing")
        elif args.command == "run" and args.run_command == "start":
            existing = already_started_by(root, packets, args.queue_id, args.owner) if args.apply else None
            current = open_run(packets, args.queue_id)
            _open_oid, open_claim = read_claim(root, current.item_id)
            if existing is not None and not (open_claim and open_claim.get("owner") == args.owner):
                print(f"{existing.item_id} is already {run_state(existing)} for {args.owner}: nothing pending")
                return 0
            outcome = start_run(root, registry, packets, args.queue_id, target_tip, owner=args.owner, apply=args.apply)
            print(f"{outcome['item']}: base {outcome['base'][:12]}" + (f" (after {outcome['predecessor']})" if outcome["predecessor"] else "") + f", integration {outcome['integration'][:12]}, tests " + (", ".join(outcome["tests"]) or "none"))
            for test in outcome["tests"]:
                print(f"  {test}: {registry.commands.get(test, 'no command registered')}")
            for item, sha, _note in outcome["members"]:
                print(f"  member {item} @{sha[:12]}")
            for item, reason in outcome["collisions"]:
                candidate = next(sha for i, sha, _n in outcome["ready"] if i == item)
                print(f"  member {item} awaits resolution ({reason}); it stays in this run; " + resolve_instructions(args.queue_id, packets[outcome["item"]].run[1], item, candidate, args.owner))
            for item, _sha, _note, reason in outcome["waiting"]:
                print(f"  {item} {reason}")
            print(f"opened {outcome['successor']}")
        elif args.command == "run":
            run = find_run(packets, args.queue_id, args.ordinal)
            if args.run_command == "fix":
                outcome = fix_run(root, registry, packets, run, args.commit, owner=args.owner, apply=args.apply)
                print(f"{run.item_id}: integration fix {outcome['previous'][:12]}..{outcome['integration'][:12]}, {len(outcome['paths'])} path(s); tests reset" + ("" if args.apply else "; pass --apply with --owner to record"))
            elif args.run_command == "refresh":
                outcome = refresh_run(root, registry, packets, run, owner=args.owner, apply=args.apply)
                if not outcome["moved"]:
                    print(f"{run.item_id} is already on its predecessor's tip {outcome['base'][:12]}: nothing pending")
                    return 0
                print(f"{run.item_id}: refreshed onto {outcome['base'][:12]}, integration {outcome['previous'][:12]}..{outcome['integration'][:12]}; tests reset" + ("" if args.apply else "; pass --apply with --owner to record"))
            elif args.run_command == "cancel":
                outcome = cancel_run(root, registry, packets, run, owner=args.owner, apply=args.apply)
                print(f"{run.item_id}: back to open with " + ", ".join(outcome["members"]) + f"; {outcome['removed']} removed" + ("" if args.apply else "; pass --apply to record"))
            elif args.run_command == "resolve":
                outcome = resolve_member(root, registry, packets, run, args.item_id, args.commit, owner=args.owner, apply=args.apply)
                print(f"{run.item_id}: {args.item_id} resolved as {outcome['integration'][:12]}; tests reset" + ("" if args.apply else "; pass --apply with --owner to record"))
            elif args.run_command == "review":
                if args.item_id is None:
                    for line in review_lines(root, packets, run, target_tip):
                        print(line)
                    pending = unreviewed_members(run)
                    print(f"{run.item_id}: {len(pending)} member(s) not yet reviewed" + (f": run review {args.queue_id} {args.ordinal} <ID> --ok | --reject <reason> | --hold <question> --owner <you> --apply" if pending else "; record tests"))
                else:
                    outcome = review_member(root, registry, packets, run, args.item_id, owner=args.owner, ok=args.ok, reject=args.reject, hold=args.hold, apply=args.apply, authorized_by=args.authorized_by)
                    if outcome["verdict"] == "ok":
                        print(f"{run.item_id}: {args.item_id} reviewed ok" + ("" if args.apply else "; pass --apply with --owner to record"))
                    elif not args.apply:
                        print(f"{run.item_id}: would {outcome['verdict']} {args.item_id}; pass --apply with --owner")
                    else:
                        print(f"{run.item_id}: {outcome['verdict']} {args.item_id}" + (", dependents returned to the open run: " + ", ".join(outcome["dependents"]) if outcome["dependents"] else "")
                              + (f"; merged again as {outcome['integration'][:12]}, tests reset" if outcome.get("integration") else "; no member left, run closed" if outcome.get("closed") else "")
                              + (f"; {args.item_id} waits in the open run for a decision by {outcome['decision_owner']}" if outcome["verdict"] == "hold" else "") + "")
            elif args.run_command == "test":
                run = record_test(root, packets, run, args.name, args.result, args.evidence, args.cost, owner=args.owner)
                print(f"{run.item_id}: {args.name} {args.result}, cost {run.fields['Cost']} of {run.fields['Budget']}")
            else:
                if run_state(run) in {"submitted", "completed"}:
                    print(f"{run.item_id} is already {run_state(run)} on {target}: nothing pending")
                    return 0
                outcome = complete_run(root, registry, packets, run, target_tip, reject=args.reject, apply=args.apply, authorized_by=args.authorized_by, owner=args.owner)
                if outcome.get("rebuild"):
                    print(f"{run.item_id}: rejected " + ", ".join(outcome["rejected"]) + (", returned to the open run: " + ", ".join(outcome["returned"]) if outcome.get("returned") else "") + (f"; merged again as {outcome['integration'][:12]}, tests reset" if outcome.get("integration") else "; no member left, run closed" if outcome.get("closed") else ""))
                else:
                    print(f"{run.item_id}: green, result {outcome['result'][:12]}" + (f", passed to {outcome['passed_to']}" if outcome["passed_to"] else f"; land with `biq land {run.item_id}`"))
                if not args.apply:
                    print("pass --apply to record")
        if control is not None:
            command = args.command if args.command != "run" else f"run {args.run_command}"
            owner = getattr(args, "owner", None) or "agent"
            outcome = commit_control_clone(root, control, command, control_item, owner, control_operation, target,
                                           tuple(deferred_publications), tuple(deferred_branch_moves))  # root is the control clone here, control its branch
            if outcome["commit"]:
                print(f"control commit {outcome['commit'][:12]} pushed to {outcome['branch']}")
            if is_ancestor(root, outcome["branch"], remote_ref_of(target)):
                print(f"{outcome['branch']} is on {target}: nothing pending")
                return 0
            followed = follow_control_landing(root, outcome["branch"], target, control_item, command, owner)
            code = report_landing(followed, outcome["branch"], target)
            if code == 0 and command == "abandon" and (read_claim(root, control_item)[1] or {}).get("owner") == owner:
                release_item(root, control_item, owner)
                print(f"released {control_item}")
            return code
    except BiqError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


def report_landing(followed: dict, branch: str, target: str) -> int:
    """One line and one exit code per landing state: 0 landed, 3 pending, 2 refused or failed."""
    pr = f" (change request {followed['pr']})" if followed.get("pr") else ""
    if followed["state"] == "landed":
        print(f"{branch} landed on {target} at {followed['detail'][:12]}{pr}")
        return 0
    print(f"{branch} {followed['state'].upper()}{pr}: {followed['detail']}", file=sys.stderr)
    return PENDING_EXIT if followed["state"] == "pending" else 2


if __name__ == "__main__":
    sys.exit(main())
