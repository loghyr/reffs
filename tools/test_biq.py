#!/usr/bin/env python3
"""Tests for tools/biq.py on a throwaway repository with a bare origin."""
from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

# Every git the tests or the tool under test spawn: no detached auto-gc or maintenance in any
# throwaway repository (bare origins and control clones included), which would otherwise write
# into `objects` while a test's temporary directory is being removed.
os.environ.update({"GIT_CONFIG_COUNT": "2", "GIT_CONFIG_KEY_0": "gc.auto", "GIT_CONFIG_VALUE_0": "0",
                   "GIT_CONFIG_KEY_1": "maintenance.auto", "GIT_CONFIG_VALUE_1": "false"})

SCRIPT = Path(__file__).resolve().parent / "biq.py"
_spec = importlib.util.spec_from_file_location("biq", SCRIPT)
BIQ = importlib.util.module_from_spec(_spec)
sys.modules["biq"] = BIQ
_spec.loader.exec_module(BIQ)

REGISTRY = {
    "schema": 2,
    "target": "main",
    "workstream": "WI-QUEUE-WORKSTREAM",
    "commands": {"u-lane": "u.sh", "k-lane": "k.sh", "p-lane": "p.sh", "local-test": "t.sh"},
    "queues": [
        {"id": "U", "kind": "leaf", "feeds": "AGG", "budget_usd": 10, "tests": ["u-lane"], "prefixes": ["u/"]},
        {"id": "K", "kind": "leaf", "feeds": "AGG", "budget_usd": 10, "tests": ["k-lane"], "prefixes": ["k/"]},
        {"id": "P", "kind": "leaf", "feeds": None, "budget_usd": 1, "tests": ["p-lane"], "patterns": ["p/*.patch"]},
        {"id": "AGG", "kind": "aggregator", "feeds": None, "feeders": ["U", "K"], "budget_usd": 10, "tests": ["local-test"]},
    ],
}


# Exact retired schema-1 registry retained by pre-cutover candidates.
LEGACY_REGISTRY = """{
  "schema": 1,
  "target": "main",
  "tests": {
    "ci-stable-fast": {"target": "local", "cost": "free", "command": "rust/scripts/ci-stable-fast.sh"},
    "ci-kernel-production": {"target": "kernel", "cost": "paid", "command": "rust/scripts/ci-kernel-production.sh"},
    "ci-gpu-resident-noop": {"target": "gpu", "cost": "paid", "command": "rust/scripts/ci-gpu-resident-noop.sh"},
    "linux-patchset-replay": {"target": "local", "cost": "free", "command": "tools/biq-predicates/kernel-patch.py"},
    "linux-patchset-build": {"target": "kernel", "cost": "paid", "command": "tools/biq-predicates/kernel-patch.py"}
  },
  "queues": [
    {"id": "USERSPACE", "kind": "leaf", "predicate": "tools/biq-predicates/userspace.py", "feeds": "INLINEFS", "budget_usd": 100},
    {"id": "KERNEL", "kind": "leaf", "predicate": "tools/biq-predicates/kernel.py", "feeds": "INLINEFS", "budget_usd": 100},
    {"id": "GPU", "kind": "leaf", "predicate": "tools/biq-predicates/gpu.py", "feeds": "INLINEFS", "budget_usd": 100},
    {"id": "KERNEL-PATCH", "kind": "leaf", "predicate": "tools/biq-predicates/kernel-patch.py", "feeds": null, "budget_usd": 20},
    {"id": "INLINEFS", "kind": "aggregator", "predicate": "tools/biq-predicates/inlinefs.py", "feeders": ["USERSPACE", "KERNEL", "GPU"], "feeds": null, "budget_usd": 100}
  ]
}
"""


def packet_text(item_id: str, *, status: str = "open", candidate: str = "pending", dependencies: str = "none", replaces: str | None = None, gate: str = "not-required", decision_owner: str = "none",
                created: str | None = None, scope: tuple[str, ...] = ()) -> str:
    """A minimal packet; the documentation policy also builds its cold fixtures from this."""
    base = "pending" if candidate == "pending" else "b" * 40
    lines = [f"# {item_id}: test", "", f"- ID: `{item_id}`", f"- Status: `{status}`", "- Kind: `bug`"]
    if created:
        lines.append(f"- Created: `{created}`")
    lines += [f"- Architecture gate: `{gate}`", f"- Architecture decision owner: `{decision_owner}`", "- Closed At: `pending`", "- Completed human_owner: `pending`", "- Completed host_user: `pending`", "- Completed hostname: `pending`", f"- Dependencies: `{dependencies}`"]
    if replaces:
        lines.append(f"- Replaces: `{replaces}`")
    lines += [f"- Candidate base: `{base}`", f"- Candidate commit: `{candidate}`", "- Submitted At: `pending`", "- Submitted owner: `pending`", "",
              "## Objective", "", "Test.", ""]
    if scope:
        lines += ["## Allowed source/build scope", "", *[f"- `{path}`" for path in scope], ""]
    lines += ["## Resolution", "", "pending", ""]
    return "\n".join(lines)


def workstream_text(item_id: str, members: str) -> str:
    return f"# {item_id}: workstream\n\n- ID: `{item_id}`\n- Status: `open`\n- Kind: `tooling`\n- Dependencies: `none`\n- Container mode: `project`\n- Container members: `{members}`\n\n## Objective\n\nTest.\n\n## Resolution\n\npending\n"


def validation_packet_text(item_id: str) -> str:
    """A current authored packet, separate from the small lifecycle unit stubs."""
    text = (SCRIPT.parent.parent / "docs/work-queue/TEMPLATE.md").read_text().split("\n\n`Priority`", 1)[0]
    text = text.replace("WI-CI-CATEGORY-SUBJECT", item_id).replace("concise outcome", "validate a bounded change")
    values = {"Kind": "tooling", "Level of effort": "low", "Area": "synthetic owner",
              "Created": "2026-08-01T00:00:00Z", "Created human_owner": "Example Person",
              "Created host_user": "example", "Created hostname": "example-host",
              "Baseline": "origin/main " + "a" * 40, "Accountable owner": "Example Person",
              "Documentation impact rationale": "No product contract changes."}
    for key, value in values.items():
        text = re.sub(r"^- " + re.escape(key) + r": `[^`]*`$", "- " + key + ": `" + value + "`", text, flags=re.M)
    text = re.sub(r"^- Container (mode|members):[^\n]*\n", "", text, flags=re.M)
    for section in ("Objective", "Reason", "Allowed source/build scope", "Existing owner and required reuse", "Non-goals", "Required reading",
                    "Affected contracts and invariants", "Risks and constraints", "Implementation outline", "Acceptance and completion evidence",
                    "Residual risks and unresolved decisions", "Attempt history", "Resolution"):
        body = "Check the scoped synthetic contract."
        if section == "Required reading":
            body = "- governing-authority: `README.md#owner`\n- verification: `tools/check.py`"
        if section == "Resolution":
            body = "pending"
        text += "\n\n## " + section + "\n\n" + body
    return text + "\n"


def registry_from_json(source: str) -> BIQ.Registry:
    """Test seed only: the tool has no registry file; the runs define the queues."""
    try:
        data = json.loads(source)
    except json.JSONDecodeError as error:
        raise BIQ.BiqError(f"registry is not JSON: {error}") from error
    if not isinstance(data, dict) or data.get("schema") != 2 or not isinstance(data.get("target"), str):
        raise BIQ.BiqError("registry requires schema 2 and a target branch")
    queues: dict[str, Queue] = {}
    for raw in data.get("queues", []):
        qid = raw.get("id")
        if isinstance(qid, str):
            qid = BIQ.QUEUE_RENAMES.get(qid, qid)
        if not isinstance(qid, str) or not re.fullmatch(r"[A-Z][A-Z0-9-]*", qid) or qid in queues:
            raise BIQ.BiqError(f"malformed or duplicate queue id {qid!r}")
        kind = raw.get("kind")
        if kind not in {"leaf", "aggregator"}:
            raise BIQ.BiqError(f"queue {qid} kind must be leaf or aggregator")
        feeders = tuple(BIQ.QUEUE_RENAMES.get(f, f) for f in (raw.get("feeders") or ()))
        if kind == "leaf" and feeders:
            raise BIQ.BiqError(f"leaf queue {qid} must not declare feeders")
        if kind == "aggregator" and not feeders:
            raise BIQ.BiqError(f"aggregator {qid} must declare feeders")
        tests = tuple(raw.get("tests") or ())
        if not all(isinstance(t, str) and t for t in tests):
            raise BIQ.BiqError(f"queue {qid} tests must be names")
        qualification = raw.get("qualification")
        if qualification is not None and not (isinstance(qualification, str) and BIQ.ITEM_RE.match(qualification)):
            raise BIQ.BiqError(f"queue {qid} qualification must be a WQE id")
        queues[qid] = BIQ.Queue(
            qid, kind, BIQ.QUEUE_RENAMES.get(raw.get("feeds"), raw.get("feeds")), feeders, float(raw.get("budget_usd", 0)), tests,
            frozenset(raw.get("files") or ()), tuple(raw.get("prefixes") or ()), tuple(raw.get("patterns") or ()), qualification,
        )
    BIQ.validate_queues(queues)
    workstream = data.get("workstream")
    if workstream is not None and not (isinstance(workstream, str) and BIQ.ITEM_RE.match(workstream)):
        raise BIQ.BiqError("registry workstream must be a WQE id")
    commands = dict(data.get("commands") or {})
    if not isinstance(commands, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in commands.items()):
        raise BIQ.BiqError("registry commands must map test names to commands")
    tests_meta = data.get("tests") or {}
    if not isinstance(tests_meta, dict):
        raise BIQ.BiqError("registry tests must map test names to {command, target, cost}")
    for name, meta in tests_meta.items():
        if not isinstance(name, str) or not isinstance(meta, dict) or not isinstance(meta.get("command"), str) \
                or meta.get("target") not in {"local", "kernel", "gpu"} or meta.get("cost") not in {"free", "paid"}:
            raise BIQ.BiqError(f"registry test {name!r} needs command, target (local|kernel|gpu), and cost (free|paid)")
        commands[name] = meta["command"]
    for queue in queues.values():
        for test in queue.tests:
            if test not in commands:
                raise BIQ.BiqError(f"queue {queue.id} test {test} has no command in the registry")
    return BIQ.Registry(data["target"], queues, workstream, commands, {k: dict(v) for k, v in tests_meta.items()})




FAKE_GH = r"""#!/usr/bin/env python3
# A stand-in for gh: pull requests live in a JSON file; landing merges the branch into the fixture's origin.
import json, os, subprocess, sys, tempfile
db_path, repo, mode = os.environ["BIQ_FAKE_GH_DB"], os.environ["BIQ_FAKE_GH_ORIGIN"], os.environ.get("BIQ_FAKE_GH_MODE", "landed")
db = json.load(open(db_path)) if os.path.exists(db_path) else {"prs": []}
def save(): json.dump(db, open(db_path, "w"))
def find(key): return next((p for p in db["prs"] if p["url"] == key or str(p["number"]) == key or p["head"] == key), None)
a = sys.argv[1:]
if a[:2] == ["pr", "list"]:
    head = a[a.index("--head") + 1]; p = next((p for p in db["prs"] if p["head"] == head and p["state"] == "OPEN"), None)
    print(p["url"] if p else ""); sys.exit(0)
if a[:2] == ["pr", "create"]:
    head = a[a.index("--head") + 1]; n = len(db["prs"]) + 1
    db["prs"].append({"number": n, "url": f"https://example.invalid/pr/{n}", "head": head, "state": "OPEN", "auto": False, "commit": None}); save(); print(db["prs"][-1]["url"]); sys.exit(0)
if a[:2] == ["pr", "merge"]:
    p = find(a[2]); p["auto"] = True; save(); sys.exit(0)
if a[:2] == ["pr", "view"]:
    p = find(a[2])
    if p["state"] == "OPEN":
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(["git", "clone", "-q", repo, tmp], check=True)
            dirty = subprocess.run(["git", "-C", tmp, "merge-tree", "--write-tree", "origin/main", "origin/" + p["head"]], capture_output=True).returncode != 0
            if dirty:
                merge, checks = "DIRTY", []
            elif mode == "failed":
                merge, checks = "BLOCKED", [{"conclusion": "FAILURE"}]
            elif mode == "pending" or not p["auto"]:
                merge, checks = "BLOCKED", [{"state": "PENDING"}]
            else:
                subprocess.run(["git", "-C", tmp, "config", "user.email", "gh@example.com"], check=True)
                subprocess.run(["git", "-C", tmp, "config", "user.name", "gh"], check=True)
                subprocess.run(["git", "-C", tmp, "checkout", "-q", "main"], check=True)
                subprocess.run(["git", "-C", tmp, "merge", "-q", "--no-ff", "-m", "Merge " + p["head"], "origin/" + p["head"]], check=True)
                subprocess.run(["git", "-C", tmp, "push", "-q", "origin", "main"], check=True)
                p["state"], p["commit"] = "MERGED", subprocess.run(["git", "-C", tmp, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip(); save()
                merge, checks = "UNKNOWN", [{"conclusion": "SUCCESS"}]
    else:
        merge, checks = "UNKNOWN", [{"conclusion": "SUCCESS"}]
    print(json.dumps({"state": p["state"], "mergeStateStatus": merge, "statusCheckRollup": checks,
                      "mergeCommit": {"oid": p["commit"]} if p["commit"] else None, "autoMergeRequest": {} if p["auto"] else None})); sys.exit(0)
sys.exit(1)
"""

FAKE_GLAB = r"""#!/usr/bin/env python3
# A stand-in for glab: merge requests live in the same JSON shape as fake gh.
import json, os, subprocess, sys, tempfile
db_path, repo, mode = os.environ["BIQ_FAKE_GH_DB"], os.environ["BIQ_FAKE_GH_ORIGIN"], os.environ.get("BIQ_FAKE_GH_MODE", "landed")
db = json.load(open(db_path)) if os.path.exists(db_path) else {"prs": []}
def save(): json.dump(db, open(db_path, "w"))
def find(key): return next((p for p in db["prs"] if p["url"] == key or str(p["number"]) == key or p["head"] == key), None)
a = sys.argv[1:]
if a[:2] == ["mr", "list"]:
    head = a[a.index("--source-branch") + 1]; p = next((p for p in db["prs"] if p["head"] == head and p["state"] == "OPEN"), None)
    print(json.dumps([{"iid": p["number"], "web_url": p["url"]}] if p else [])); sys.exit(0)
if a[:2] == ["mr", "create"]:
    head = a[a.index("--source-branch") + 1]; n = len(db["prs"]) + 1
    url = f"https://gitlab.example/group/project/-/merge_requests/{n}"
    db["prs"].append({"number": n, "url": url, "head": head, "state": "OPEN", "auto": False, "remove": "--remove-source-branch" in a, "commit": None, "arm_attempts": 0}); save(); print(url); sys.exit(0)
if a[:2] == ["mr", "merge"]:
    p = find(a[2]); p["arm_attempts"] = p.get("arm_attempts", 0) + 1; save()
    if mode == "arm-race" and p["arm_attempts"] == 1:
        print("Branch cannot be merged", file=sys.stderr); sys.exit(1)
    p["auto"], p["remove"] = True, "--remove-source-branch" in a; save(); sys.exit(0)
if a[:2] == ["mr", "view"]:
    p = find(a[2]); conflict, pipeline = False, "success"
    if p["state"] == "OPEN":
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(["git", "clone", "-q", repo, tmp], check=True)
            conflict = subprocess.run(["git", "-C", tmp, "merge-tree", "--write-tree", "origin/main", "origin/" + p["head"]], capture_output=True).returncode != 0
            if mode == "failed": pipeline = "failed"
            elif mode == "pending" or not p["auto"]: pipeline = "running"
            elif not conflict:
                subprocess.run(["git", "-C", tmp, "config", "user.email", "glab@example.com"], check=True)
                subprocess.run(["git", "-C", tmp, "config", "user.name", "glab"], check=True)
                subprocess.run(["git", "-C", tmp, "checkout", "-q", "main"], check=True)
                subprocess.run(["git", "-C", tmp, "merge", "-q", "--no-ff", "-m", "Merge " + p["head"], "origin/" + p["head"]], check=True)
                subprocess.run(["git", "-C", tmp, "push", "-q", "origin", "main"], check=True)
                if p["remove"]: subprocess.run(["git", "-C", tmp, "push", "-q", "origin", ":refs/heads/" + p["head"]], check=True)
                p["state"], p["commit"] = "MERGED", subprocess.run(["git", "-C", tmp, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip(); save()
    print(json.dumps({"state": "merged" if p["state"] == "MERGED" else "opened", "has_conflicts": conflict,
                      "detailed_merge_status": "conflict" if conflict else "ci_still_running" if mode == "arm-race" and not p["auto"] else "mergeable", "head_pipeline": {"status": pipeline},
                      "merge_when_pipeline_succeeds": p["auto"], "merge_commit_sha": p["commit"]})); sys.exit(0)
sys.exit(1)
"""


def fake_gh_env(base: Path, origin: Path, mode: str = "landed", wait: str = "30") -> dict:
    """PATH with a stand-in gh; BIQ_FAKE_GH_MODE selects how its pull requests behave."""
    bin_dir = base / "bin"; bin_dir.mkdir(exist_ok=True)
    (bin_dir / "gh").write_text(FAKE_GH); (bin_dir / "gh").chmod(0o755)
    return {**os.environ, "BIQ_CONTROL_DIR": str(base / "control"), "PATH": f"{bin_dir}:/usr/bin:/bin",
            "BIQ_FAKE_GH_DB": str(base / "gh.json"), "BIQ_FAKE_GH_ORIGIN": str(origin), "BIQ_FAKE_GH_MODE": mode,
            "BIQ_LANDING_WAIT_SECONDS": wait, "BIQ_LANDING_POLL_SECONDS": "0.2", "BIQ_CHANGE_PROVIDER": "github"}


def fake_glab_env(base: Path, origin: Path, mode: str = "landed", wait: str = "30") -> dict:
    """PATH with a stand-in glab and an explicit provider for the local bare origin."""
    bin_dir = base / "bin"; bin_dir.mkdir(exist_ok=True)
    (bin_dir / "glab").write_text(FAKE_GLAB); (bin_dir / "glab").chmod(0o755)
    return {**os.environ, "BIQ_CONTROL_DIR": str(base / "control"), "PATH": f"{bin_dir}:/usr/bin:/bin",
            "BIQ_FAKE_GH_DB": str(base / "glab.json"), "BIQ_FAKE_GH_ORIGIN": str(origin), "BIQ_FAKE_GH_MODE": mode,
            "BIQ_LANDING_WAIT_SECONDS": wait, "BIQ_LANDING_POLL_SECONDS": "0.2", "BIQ_CHANGE_PROVIDER": "gitlab"}


class Repo:
    def __init__(self, base: Path):
        self.root = base / "repo"
        self.root.mkdir()
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "t@example.com")
        self.git("config", "user.name", "t")
        self.git("config", "gc.auto", "0")
        self.git("config", "maintenance.auto", "false")
        (self.root / "docs/work-queue/items").mkdir(parents=True)
        (self.root / "README").write_text("base\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        bare = base / "origin.git"
        subprocess.run(["git", "init", "-q", "--bare", str(bare)], check=True)
        self.git("remote", "add", "origin", str(bare))
        self.git("push", "-q", "origin", "main")

    def git(self, *args: str) -> str:
        return subprocess.run(["git", *args], cwd=self.root, text=True, capture_output=True, check=True).stdout.strip()

    def main(self) -> str:
        return self.git("rev-parse", "main")

    def commit_on(self, start: str, item_id: str | None, files: dict[str, str], *, trailer: bool = True) -> str:
        self.git("checkout", "-q", "--detach", start)
        for path, text in files.items():
            if path.startswith(BIQ.ITEMS_DIR + "/"):
                text = text.replace("origin/main " + "a" * 40, "origin/main " + start)
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_text(text)
        self.git("add", "--", *files)
        message = f"change {item_id or 'anon'}" + (f"\n\nWork-Queue-Item: {item_id}" if trailer and item_id else "")
        self.git("commit", "-q", "-m", message)
        sha = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "main")
        return sha

    def packet(self, item_id: str, *, baseline: str | None = None, **kwargs) -> None:
        source = packet_text(item_id, **kwargs)
        if baseline is not None:
            source = source.replace("## Objective", f"- Baseline: `{baseline}`\n\n## Objective", 1)
        (self.root / "docs/work-queue/items" / f"{item_id}.md").write_text(source)

    def commit_queue(self) -> str:
        """Queue state lives on main: commit the packets after every mutation."""
        self.git("checkout", "-q", "main")
        self.git("add", "-A", "--", "docs/work-queue")
        self.git("commit", "-q", "--allow-empty", "-m", "queue")
        self.git("push", "-q", "origin", "main")
        return self.main()

    def land(self, sha: str) -> str:
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--no-ff", "-m", f"land {sha[:8]}", sha)
        self.git("push", "-q", "origin", "main")
        return self.main()


class BiqTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.repo = Repo(Path(self.tmp.name))
        self.root = self.repo.root
        seed = registry_from_json(json.dumps(REGISTRY))  # the test's seed; the runs it materializes define the queues
        self.packets = BIQ.load_packets(self.root)
        BIQ.materialize(self.root, seed, self.packets, apply=True)
        self.repo.commit_queue()
        self.registry = BIQ.load_registry(self.root)

    def tearDown(self):
        self.tmp.cleanup()

    def reload(self):
        self.packets = BIQ.load_packets(self.root)
        return self.packets

    def git_only_path(self) -> str:
        """A deterministic PATH with Git but no provider CLI."""
        directory = self.root.parent / "git-only-bin"
        directory.mkdir(exist_ok=True)
        link = directory / "git"
        if not link.exists():
            link.symlink_to(shutil.which("git") or "/usr/bin/git")
        return str(directory)

    def submit(self, item_id: str, files: dict[str, str], *, start: str | None = None, dependencies: str = "none", replaces: str | None = None, owner: str = "agent",
               dependencies_after_claim: bool = False) -> str:
        """Claim, seal and submit; `dependencies_after_claim` declares a dependency the start gate would refuse as an amendment after the claim."""
        sha = self.repo.commit_on(start or self.repo.main(), item_id, files)
        self.repo.packet(item_id, candidate=sha, dependencies="none" if dependencies_after_claim else dependencies, replaces=replaces)
        self.reload()
        BIQ.claim_item(self.root, self.packets, item_id, owner)
        if dependencies_after_claim:
            self.repo.packet(item_id, candidate=sha, dependencies=dependencies, replaces=replaces)
            self.reload()
        BIQ.seal_item(self.root, self.packets, item_id, sha, self.repo.main(), owner)
        BIQ.submit_item(self.root, self.registry, self.packets, item_id, self.repo.main(), owner, apply=True)
        self.repo.commit_queue()
        return sha

    def start(self, queue: str, owner: str = "runner") -> dict:
        run = BIQ.open_run(self.packets, queue)
        BIQ.claim_item(self.root, self.packets, run.item_id, owner, authorized_by="Human Owner")
        outcome = BIQ.start_run(self.root, self.registry, self.packets, queue, self.repo.main(), owner=owner, apply=True)
        self.repo.commit_queue()
        return outcome

    def test_run_start_rerun_stays_with_the_claimed_in_flight_run(self):
        self.submit("WI-A", {"u/a": "a"})
        outcome = self.start("U")
        env = {
            **os.environ,
            "BIQ_CONTROL_DIR": str(self.root.parent / "control"),
            "PATH": self.git_only_path(),
            "BIQ_CHANGE_PROVIDER": "github",
        }
        command = [
            sys.executable,
            str(SCRIPT),
            "--repository",
            str(self.root),
            "--target-ref",
            "main",
            "run",
            "start",
            "U",
            "--owner",
            "runner",
            "--apply",
        ]
        rerun = subprocess.run(command, text=True, capture_output=True, env=env)
        self.assertEqual(rerun.returncode, 0, rerun.stdout + rerun.stderr)
        self.assertIn(f"{outcome['item']} is already started", rerun.stdout)
        self.assertNotIn("REPEAT-2 must be claimed", rerun.stderr)

    def test_run_fix_replaces_a_submitted_result_and_resets_tests(self):
        self.submit("WI-A", {"u/a": "a"})
        self.start("U")
        self.review_all("U", 1)
        run = BIQ.find_run(self.packets, "U", 1)
        run = BIQ.record_test(self.root, self.packets, run, "u-lane", "green", "first", 0.0, owner="runner")
        BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(), reject=[], apply=True, owner="runner")
        self.repo.commit_queue()
        run = BIQ.find_run(self.reload(), "U", 1)
        previous = run.fields["Result commit"]
        fixed = self.repo.commit_on(previous, run.item_id, {"u/integration-fix": "fixed\n"})
        env = {
            **os.environ,
            "BIQ_CONTROL_DIR": str(self.root.parent / "control-fix"),
            "PATH": self.git_only_path(),
            "BIQ_CHANGE_PROVIDER": "github",
        }
        command = [
            sys.executable, str(SCRIPT), "--repository", str(self.root), "--target-ref", "main",
            "run", "fix", "U", "1", "--commit", fixed, "--owner", "runner", "--apply",
        ]
        result = subprocess.run(command, text=True, capture_output=True, env=env)
        self.assertEqual(result.returncode, BIQ.PENDING_EXIT, result.stdout + result.stderr)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/runner/run-u:refs/remotes/origin/control/runner/run-u")
        repaired = BIQ.load_packets(self.root, "origin/control/runner/run-u")[run.item_id]
        self.assertEqual(BIQ.run_state(repaired), "started")
        self.assertEqual(repaired.fields["Integration commit"], fixed)
        self.assertEqual(repaired.fields["Result commit"], "pending")
        self.assertEqual(repaired.fields["Tests"], "u-lane=pending")
        self.assertIsNone(repaired.candidate)
        published = self.repo.git("ls-remote", "origin", f"refs/heads/{BIQ.REF_NAMESPACE}/candidates/{run.item_id}/{fixed[:12]}")
        self.assertTrue(published.startswith(fixed), published)

    def review_all(self, queue: str, ordinal: int, owner: str = "runner") -> None:
        run = BIQ.find_run(self.packets, queue, ordinal)
        for item, _sha, _note in run.members():
            BIQ.review_member(self.root, self.registry, self.packets, run, item, owner=owner, ok=True, reject=None, hold=None, apply=True)
            run = BIQ.find_run(self.packets, queue, ordinal)

    def green(self, queue: str, ordinal: int) -> dict:
        self.review_all(queue, ordinal)
        run = BIQ.find_run(self.packets, queue, ordinal)
        for name in run.tests:
            run = BIQ.record_test(self.root, self.packets, run, name, "green", None, 0.0, owner="runner")
        outcome = BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(), reject=[], apply=True, owner="runner")
        self.repo.commit_queue()
        return outcome

    # -- registry, materialize, lint -----------------------------------------
    def test_synthetic_repositories_disable_background_cleanup_races(self):
        for _ in range(5):
            with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
                repo = Repo(Path(directory))
                repo.commit_on(repo.main(), None, {"example.txt": "example\n"})
                self.assertEqual(repo.git("config", "gc.auto"), "0")
                self.assertEqual(repo.git("config", "maintenance.auto"), "false")

    def test_materialize_lists_each_queue_in_the_workstream(self):
        packet = "# WI-QUEUE-WORKSTREAM: queue workstream\n\n- ID: `WI-QUEUE-WORKSTREAM`\n- Status: `open`\n- Kind: `tooling`\n- Dependencies: `none`\n- Container mode: `project`\n- Container members: `WI-CI-INTEGRATION-U-REPEAT-1`\n\n## Objective\n\nGroup the runs.\n\n## Resolution\n\npending\n"
        (self.root / "docs/work-queue/items/WI-QUEUE-WORKSTREAM.md").write_text(packet)
        # A human lists the first run; the tool finds the workstream through that member and lists the rest.
        self.assertEqual(BIQ.load_registry(self.root).workstream, "WI-QUEUE-WORKSTREAM")
        added = BIQ.materialize(self.root, BIQ.load_registry(self.root), self.reload(), apply=True)
        self.assertEqual(len(added), 3)
        members = self.reload()["WI-QUEUE-WORKSTREAM"].fields["Container members"]
        self.assertEqual(members, ", ".join(BIQ.run_item_id(q, 1) for q in ("AGG", "K", "P", "U")))
        self.assertEqual(BIQ.materialize(self.root, BIQ.load_registry(self.root), self.packets, apply=True), [])
        self.assertNotIn("Workstream", BIQ.open_run(self.packets, "U").fields)

    def test_registry_and_lint(self):
        bad = json.loads(json.dumps(REGISTRY))
        bad["queues"][3]["feeds"] = "U"
        with self.assertRaisesRegex(BIQ.BiqError, "cycle|not an aggregator"):
            registry_from_json(json.dumps(bad))
        self.assertEqual(sorted(p for p in self.packets), [BIQ.run_item_id(q, 1) for q in ("AGG", "K", "P", "U")])
        self.assertEqual(BIQ.materialize(self.root, self.registry, self.packets, apply=True), [])
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])
        self.repo.packet("WI-BAD", dependencies="WI-MISSING")
        errors = BIQ.lint(self.registry, self.reload())
        self.assertTrue(any("WI-MISSING" in e for e in errors))
        text = (self.root / "docs/work-queue/items/WI-CI-INTEGRATION-U-REPEAT-1.md").read_text()
        for line in ("- Status: `open`", "- Queue: `U`", "- Budget: `$10`", "## Members"):
            self.assertIn(line, text)

    def test_dependencies_are_local_but_qualified_evidence_is_allowed(self):
        self.repo.packet("WI-LOCAL")
        self.repo.packet("WI-CLIENT", dependencies="WI-LOCAL")
        self.assertEqual(BIQ.lint(self.registry, self.reload()), [])
        self.repo.packet("WI-CLIENT", dependencies="other-repo:WI-LOCAL")
        # Packet.ids() filters the qualified string; lint must examine the raw field.
        self.assertEqual(self.reload()["WI-CLIENT"].dependencies, ())
        self.assertEqual(BIQ.lint(self.registry, self.packets), [
            "WI-CLIENT: Dependencies must name local WQEs; cross-repository needs are met by adopting a release"
        ])
        self.repo.packet("WI-CLIENT", dependencies="WI-LOCAL")
        path = self.root / BIQ.ITEMS_DIR / "WI-CLIENT.md"
        path.write_text(path.read_text().replace(
            "## Resolution", "Evidence: other-repo:WI-LOCAL\n\n## Required reading\n\n"
            "- historical-evidence: other-repo:WI-LOCAL\n\n## Resolution"))
        self.assertEqual(BIQ.lint(self.registry, self.reload()), [])

    def test_first_publisher_run_bootstraps_as_a_control_commit(self):
        base_dir = Path(self.tmp.name) / "publisher"
        base_dir.mkdir()
        publisher = Repo(base_dir)
        publisher.land(publisher.commit_on(publisher.main(), None, {
            "AGENTS.md": "# Start every task\n\nSynthetic publisher contract.\n",
            "tools/test_biq.py": "# Synthetic verification owner.\n",
        }))
        base = publisher.main()
        queue = BIQ.Queue("TOOLING", "leaf", None, (), 0.0, ("tooling-unit",),
                          prefixes=("tools/",))
        registry = BIQ.Registry("main", {"TOOLING": queue},
                                commands={"tooling-unit": "python3 -m unittest tools.test_biq"})
        with self.assertRaisesRegex(BIQ.BiqError, "no queue is defined"):
            BIQ.load_registry(publisher.root)
        run_id = BIQ.run_item_id("TOOLING", 1)
        head = publisher.commit_on(base, None, {
            f"{BIQ.ITEMS_DIR}/{run_id}.md":
                BIQ.run_packet_text(publisher.root, registry, queue, 1, base),
        })
        self.assertEqual(BIQ.verify_landing(publisher.root, base, head), "control commit")
        discovered = BIQ.load_registry(publisher.root, head)
        self.assertEqual(discovered.queues["TOOLING"], queue)
        self.assertEqual(discovered.commands, registry.commands)
        self.assertFalse((publisher.root / "docs/work-queue/queues.json").exists())

    # -- routing and submission ----------------------------------------------
    def test_materialized_reading_resolves_with_only_installed_files(self):
        fixture = Path(self.tmp.name) / "installed"
        for installed in BIQ.ADOPTION_FILES.values():
            path = fixture / installed
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("# Shared contract\n\n## Start every task\n" if installed == "AGENTS.md" else "fixture\n")
        (fixture / BIQ.ITEMS_DIR).mkdir(parents=True, exist_ok=True)
        packets = {}
        BIQ.materialize(fixture, self.registry, packets, apply=True)
        for packet in packets.values():
            reading = re.findall(r"^- [a-z-]+: `([^`]+)`$", packet.sections["Required reading"], re.M)
            self.assertEqual(reading, ["AGENTS.md#start-every-task", "tools/test_biq.py"])
            for reference in reading:
                self.assertTrue((fixture / reference.split("#", 1)[0]).is_file(), reference)
            # reference_errors normally trusts generated runs. Apply its ordinary
            # packet check explicitly, preserving the generated reading verbatim.
            ordinary = BIQ.parse_packet("WI-READING", packet.source.replace(packet.item_id, "WI-READING"))
            self.assertEqual(BIQ.reference_errors(fixture, {ordinary.item_id: ordinary}, None), [])
            self.assertEqual(packet.fields["Architecture gate"], "not-required")
            self.assertEqual(packet.fields["Architecture decision"], "none")

    def test_nondefault_namespace_publishes_fetches_and_admits_a_batch_landing(self):
        namespace = "fixture-work-queue"
        script = Path(self.tmp.name) / "biq_fixture.py"
        script.write_text(SCRIPT.read_text().replace(f'REF_NAMESPACE = "{BIQ.REF_NAMESPACE}"', f'REF_NAMESPACE = "{namespace}"', 1))
        spec = importlib.util.spec_from_file_location("biq_namespace_fixture", script)
        alternate = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = alternate
        self.addCleanup(sys.modules.pop, spec.name, None)
        spec.loader.exec_module(alternate)
        with mock.patch(__name__ + ".BIQ", alternate):
            self.assertEqual(BIQ.CLAIM_REF_PREFIX, f"refs/heads/{namespace}/claims/")
            self.assertEqual(BIQ.CANDIDATE_REF_PREFIX, f"refs/heads/{namespace}/candidates/")
            candidate = self.submit("WI-P", {"p/nondefault.patch": "synthetic\n"})
            clone = Path(self.tmp.name) / "fresh"
            subprocess.run(["git", "clone", "--quiet", "--no-local", "--single-branch", "--branch", "main",
                            self.repo.git("remote", "get-url", "origin"), str(clone)], check=True)
            self.assertFalse(BIQ.have_commit(clone, candidate))
            BIQ.ensure_commit(clone, candidate)
            self.assertEqual(BIQ.rev(clone, f"refs/remotes/origin/{namespace}/candidates/WI-P/{candidate[:12]}"), candidate)
            self.assertEqual(BIQ.read_claim(clone, "WI-P")[1]["owner"], "agent")
            self.start("P")
            self.green("P", 1)
            landing = BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-CI-INTEGRATION-P-REPEAT-1", self.repo.main(), apply=True)
            self.repo.git("push", "--quiet", "origin", f"{landing['head']}:refs/heads/fixture-landing")
            BIQ.git(clone, "fetch", "--quiet", "origin", "main", "refs/heads/fixture-landing:refs/remotes/origin/fixture-landing")
            self.assertEqual(BIQ.verify_landing(clone, self.repo.main(), landing["head"]), "result of P run 1")

    def test_submit_enqueues_into_every_caring_queue(self):
        a = self.submit("WI-A", {"u/a": "a", "k/a": "a"})
        packet = self.packets["WI-A"]
        self.assertEqual((packet.status, packet.queues), ("submitted", ("K", "U")))
        for queue in ("U", "K"):
            self.assertEqual([(m[0], m[1]) for m in BIQ.open_run(self.packets, queue).members()], [("WI-A", a)])
        self.assertEqual(BIQ.open_run(self.packets, "P").members(), [])
        self.submit("WI-DOC", {"docs/x": "x"})
        self.assertEqual(self.packets["WI-DOC"].queues, ())
        self.assertIn("WI-DOC", BIQ.render_show(self.root, self.registry, self.packets, "WI-DOC", self.repo.main()))
        self.assertIn("lands directly", BIQ.render_show(self.root, self.registry, self.packets, "WI-DOC", self.repo.main()))
        with self.assertRaisesRegex(BIQ.BiqError, "is submitted"):
            BIQ.submit_item(self.root, self.registry, self.packets, "WI-A", self.repo.main(), "agent", apply=False)

    def test_ancestry_is_implied_and_unsubmitted_work_is_refused(self):
        a = self.submit("WI-A", {"u/a": "a"})
        undeclared = self.repo.commit_on(a, "WI-B", {"u/b": "b"})
        self.repo.packet("WI-B", candidate=undeclared); self.repo.commit_queue(); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-B", "agent")
        with self.assertRaisesRegex(BIQ.BiqError, "carries the sealed candidate of WI-A"):
            BIQ.submit_item(self.root, self.registry, self.packets, "WI-B", self.repo.main(), "agent", apply=False)
        BIQ.release_item(self.root, "WI-B", "agent")
        b = self.submit("WI-B", {"u/b": "b"}, start=a, dependencies="WI-A")
        self.assertEqual(self.packets["WI-B"].dependencies, ("WI-A",))
        self.assertEqual(self.packets["WI-B"].fields["Implied dependencies"], "WI-A")
        stray = self.repo.commit_on(self.repo.main(), "WI-STRAY", {"u/s": "s"})
        self.repo.packet("WI-STRAY")
        c = self.repo.commit_on(stray, "WI-C", {"u/c": "c"})
        self.repo.packet("WI-C", candidate=c)
        self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-C", "agent")
        with self.assertRaisesRegex(BIQ.BiqError, "unsubmitted work"):
            BIQ.submit_item(self.root, self.registry, self.packets, "WI-C", self.repo.main(), "agent", apply=False)
        self.assertNotEqual(a, b)

    # -- runs -------------------------------------------------------------------
    def test_run_identity_requires_queue_and_excludes_human_sync_repeat(self):
        for queue in ("U", "LINUX-KERNEL", "INLINEFS-FRONTEND-GPU", "INLINEFS"):
            item_id = BIQ.run_item_id(queue, 2)
            self.assertEqual(item_id, f"WI-CI-INTEGRATION-{queue}-REPEAT-2")
            self.assertEqual(BIQ.run_identity(item_id, queue), (queue, 2))
            self.assertIsNone(BIQ.run_identity(item_id))
            self.assertIsNone(BIQ.run_identity(item_id, "WRONG"))
        sync = "WI-CI-INTEGRATION-LINUX-KERNEL-HAMMERSPACE-SYNC-REPEAT-1"
        self.assertIsNone(BIQ.run_identity(sync))
        self.assertIsNone(BIQ.run_identity(sync, "LINUX-KERNEL"))

    def test_queue_rename_keeps_live_run_identity_and_opens_named_successor(self):
        candidate = self.submit("WI-A", {"u/a": "a"})
        # Simulate an existing frontend run and submission before the rename.
        old_id = "WI-CI-INTEGRATION-BIQ-FRONTEND-USERSPACE-REPEAT-1"
        run = BIQ.open_run(self.packets, "U")
        old_source = run.source.replace(run.item_id, old_id)
        (self.root / BIQ.ITEMS_DIR / f"{run.item_id}.md").unlink()
        (self.root / BIQ.ITEMS_DIR / f"{old_id}.md").write_text(old_source)
        leaf = self.packets["WI-A"]
        BIQ.write_packet(self.root, self.packets, leaf.item_id, [("Queues", "FRONTEND-USERSPACE")])
        # The rename is an edit of the open runs: the queue's own and its aggregator's Feeders.
        old_path = self.root / BIQ.ITEMS_DIR / f"{old_id}.md"
        old_path.write_text(old_path.read_text().replace("- Queue: `U`", "- Queue: `INLINEFS-FRONTEND-USERSPACE`"))
        old_source = old_path.read_text()
        agg = BIQ.open_run(self.packets, "AGG")
        BIQ.write_packet(self.root, self.packets, agg.item_id, [("Feeders", agg.fields["Feeders"].replace("U", "INLINEFS-FRONTEND-USERSPACE"))])
        self.repo.commit_queue(); self.reload()
        self.registry = BIQ.load_registry(self.root)
        self.assertEqual(BIQ.materialize(self.root, self.registry, self.packets, apply=True), [])
        self.assertEqual(BIQ.open_run(self.packets, "INLINEFS-FRONTEND-USERSPACE").item_id, old_id)
        self.assertEqual(BIQ.find_run(self.packets, "INLINEFS-FRONTEND-USERSPACE", 1).source, old_source)
        self.assertEqual(self.packets["WI-A"].queues, ("INLINEFS-FRONTEND-USERSPACE",))
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])
        self.start("INLINEFS-FRONTEND-USERSPACE")
        self.assertEqual(self.packets[old_id].members()[0][:2], ("WI-A", candidate))
        self.assertEqual(BIQ.find_run(self.packets, "INLINEFS-FRONTEND-USERSPACE", 1).item_id, old_id)
        self.assertEqual(BIQ.open_run(self.packets, "INLINEFS-FRONTEND-USERSPACE").item_id,
                         "WI-CI-INTEGRATION-INLINEFS-FRONTEND-USERSPACE-REPEAT-2")
        self.green("INLINEFS-FRONTEND-USERSPACE", 1)
        self.assertEqual(BIQ.open_run(self.packets, "AGG").members()[0][0], old_id)

    def test_current_tool_verifies_control_edge_with_historical_queue_names(self):
        old_run = BIQ.open_run(self.packets, "U")
        old_id = "WI-CI-INTEGRATION-BIQ-FRONTEND-USERSPACE-REPEAT-1"
        (self.root / BIQ.ITEMS_DIR / f"{old_run.item_id}.md").unlink()
        (self.root / BIQ.ITEMS_DIR / f"{old_id}.md").write_text(old_run.source.replace(old_run.item_id, old_id).replace("- Queue: `U`", "- Queue: `FRONTEND-USERSPACE`"))
        agg = BIQ.open_run(self.packets, "AGG")
        BIQ.write_packet(self.root, self.packets, agg.item_id, [("Feeders", agg.fields["Feeders"].replace("U", "FRONTEND-USERSPACE"))])
        base = self.repo.commit_queue()
        authored = validation_packet_text("WI-DOC").replace("origin/main " + "a" * 40, "origin/main " + base).replace("README.md#owner", "README").replace("tools/check.py", "README")
        (self.root / BIQ.ITEMS_DIR / "WI-DOC.md").write_text(authored)
        head = self.repo.commit_queue()
        self.assertEqual(BIQ.verify_landing(self.root, base, head), "control commit")
        old_registry = BIQ.load_registry(self.root, base)
        self.assertEqual(old_registry.queues["INLINEFS-FRONTEND-USERSPACE"].tests, ("u-lane",))
        self.assertEqual(old_registry.queues["INLINEFS-FRONTEND-USERSPACE"].prefixes, ("u/",))
        with self.assertRaisesRegex(BIQ.BiqError, "duplicate queue"):
            registry_from_json(json.dumps({**REGISTRY, "queues": REGISTRY["queues"] + [dict(REGISTRY["queues"][0])]}))

    def test_a_run_defines_its_queue_and_its_successor_carries_the_definition(self):
        run = BIQ.open_run(self.packets, "U")
        self.assertEqual(BIQ.queue_from_run(run), self.registry.queues["U"])
        self.assertEqual(BIQ.queue_tests_of(run)["u-lane"]["command"], "u.sh")
        self.assertEqual(BIQ.queue_from_run(BIQ.open_run(self.packets, "AGG")).feeders, self.registry.queues["AGG"].feeders)
        self.submit("WI-D1", {"u/d1": "1"}); self.start("U")
        successor = BIQ.open_run(self.packets, "U")
        self.assertEqual(successor.item_id, "WI-CI-INTEGRATION-U-REPEAT-2")
        self.assertEqual(BIQ.queue_from_run(successor), self.registry.queues["U"])
        self.assertEqual(BIQ.load_registry(self.root), self.registry)
        # A definition the tool cannot read is refused where it is read.
        BIQ.write_packet(self.root, self.packets, successor.item_id, [("Queue kind", "bogus")])
        with self.assertRaisesRegex(BIQ.BiqError, "Queue kind must be leaf or aggregator"):
            BIQ.load_registry(self.root)
        BIQ.write_packet(self.root, self.packets, successor.item_id, [("Queue kind", "leaf"), ("Feeds", "U")])
        with self.assertRaisesRegex(BIQ.BiqError, "does not feed it|not an aggregator"):
            BIQ.load_registry(self.root)

    def test_start_freezes_ready_members_and_opens_the_successor(self):
        a = self.submit("WI-A", {"u/a": "a"})
        self.submit("WI-B", {"u/b": "b"}, dependencies="WI-LATER", dependencies_after_claim=True)
        self.repo.packet("WI-LATER")
        self.submit("WI-C", {"u/a": "conflict"})
        self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, "must be claimed"):
            BIQ.start_run(self.root, self.registry, self.packets, "U", self.repo.main(), owner="runner", apply=True)
        tip = self.repo.main()
        outcome = self.start("U")
        self.assertEqual([m[0] for m in outcome["members"]], ["WI-A"])
        self.assertEqual([c[0] for c in outcome["collisions"]], ["WI-C"])
        self.assertEqual([w[0] for w in outcome["waiting"]], ["WI-B"])
        run = self.packets["WI-CI-INTEGRATION-U-REPEAT-1"]
        self.assertEqual((BIQ.run_state(run), run.fields["Base"], run.tests), ("started", tip, {"u-lane": "pending"}))
        self.assertTrue(BIQ.is_ancestor(self.root, a, run.fields["Integration commit"]))
        # The collided member stays in the run awaiting the integrator's hand merge; nothing is excluded.
        self.assertEqual(sorted(m[0] for m in run.members()), ["WI-A", "WI-C"])
        self.assertEqual(BIQ.pending_resolutions(run), {"WI-C": "textual conflict with the batch"})
        self.assertEqual(BIQ.excluded_members(run), {})
        self.assertIn("awaiting resolution: WI-C", BIQ.render_show(self.root, self.registry, self.packets, run.item_id, self.repo.main()))
        with self.assertRaisesRegex(BIQ.BiqError, "awaiting resolution"):
            BIQ.record_test(self.root, self.packets, run, "u-lane", "green", None, 0.0, owner="runner")
        successor = BIQ.open_run(self.packets, "U")
        self.assertEqual((successor.item_id, sorted(m[0] for m in successor.members())), ("WI-CI-INTEGRATION-U-REPEAT-2", ["WI-B"]))
        self.assertEqual(self.repo.git("rev-parse", "biq/u/1"), run.fields["Integration commit"])
        report = BIQ.render_report(self.root, self.registry, self.packets, self.repo.main())
        self.assertIn("WI-CI-INTEGRATION-U-REPEAT-1 (started): 2 members", report)
        self.assertIn("WI-CI-INTEGRATION-U-REPEAT-2 (open): 1 members", report)
        self.assertIn("WI-B @", report)

    def test_next_run_chains_on_the_in_flight_predecessor(self):
        a = self.submit("WI-A", {"u/a": "a"})
        self.start("U")
        self.submit("WI-B", {"u/b": "b"}, start=a, dependencies="WI-A")
        outcome = self.start("U")
        self.assertEqual(outcome["predecessor"], "WI-CI-INTEGRATION-U-REPEAT-1")
        self.assertEqual(outcome["base"], self.packets["WI-CI-INTEGRATION-U-REPEAT-1"].fields["Integration commit"])
        self.assertEqual([m[0] for m in outcome["members"]], ["WI-B"])

    def test_a_successor_starts_on_its_predecessors_tip_without_a_dependency_and_refreshes_when_it_moves(self):
        self.submit("WI-A", {"u/a": "a"})
        self.start("U")
        first = BIQ.find_run(self.packets, "U", 1)
        self.submit("WI-C", {"u/c": "c"})  # depends on nothing in run 1
        outcome = self.start("U")
        self.assertEqual((outcome["predecessor"], outcome["base"]), (first.item_id, first.fields["Integration commit"]))
        second = BIQ.find_run(self.packets, "U", 2)
        self.assertEqual(second.fields["Base"], first.fields["Integration commit"])
        # nothing moved: refresh is a no-op
        self.assertFalse(BIQ.refresh_run(self.root, self.registry, self.packets, second, owner="runner", apply=True)["moved"])
        # the predecessor moves through an integration fix; the successor refreshes onto the new tip
        fixed = self.repo.commit_on(first.fields["Integration commit"], None, {"u/fix": "fix"})
        BIQ.fix_run(self.root, self.registry, self.packets, first, fixed, owner="runner", apply=True)
        self.repo.commit_queue(); self.reload()
        second = BIQ.find_run(self.packets, "U", 2)
        with self.assertRaisesRegex(BIQ.BiqError, "no predecessor"):
            BIQ.refresh_run(self.root, self.registry, self.packets, BIQ.find_run(self.packets, "U", 1), owner="runner", apply=True)
        outcome = BIQ.refresh_run(self.root, self.registry, self.packets, second, owner="runner", apply=True)
        self.assertTrue(outcome["moved"])
        self.assertEqual(outcome["base"], fixed)
        refreshed = BIQ.find_run(self.reload(), "U", 2)
        self.assertEqual((refreshed.fields["Base"], refreshed.fields["Integration commit"], refreshed.fields["Tests"]), (fixed, outcome["integration"], "u-lane=pending"))
        self.assertEqual(self.repo.git("rev-list", "--parents", "-n", "1", outcome["integration"]).split()[1:], [fixed, outcome["previous"]])
        self.assertEqual(self.repo.git("rev-parse", "biq/u/2"), outcome["integration"])
        self.assertTrue(BIQ.is_ancestor(self.root, "WI-C" and self.packets["WI-C"].candidate, outcome["integration"]))
        self.assertIn("Refreshed onto WI-CI-INTEGRATION-U-REPEAT-1", refreshed.sections["Resolution"])

    def test_a_run_lands_only_after_its_predecessor_landed(self):
        self.submit("WI-P1", {"p/one.patch": "1"})
        self.start("P")
        self.submit("WI-P2", {"p/two.patch": "2"})
        self.start("P")
        self.green("P", 2)
        with self.assertRaisesRegex(BIQ.BiqError, "waits for its predecessor WI-CI-INTEGRATION-P-REPEAT-1 to land"):
            BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-CI-INTEGRATION-P-REPEAT-2", self.repo.main(), apply=False)
        self.green("P", 1)
        landing = BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-CI-INTEGRATION-P-REPEAT-1", self.repo.main(), apply=True)
        self.repo.git("checkout", "-q", "main"); self.repo.git("merge", "-q", "--ff-only", landing["head"]); self.repo.git("push", "-q", "origin", "main"); self.reload()
        landing = BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-CI-INTEGRATION-P-REPEAT-2", self.repo.main(), apply=False)
        self.assertEqual(landing["reason"], "result of P run 2")

    def test_a_members_review_and_resolution_notes_survive_each_other_and_a_rebase(self):
        self.assertEqual(BIQ.merge_member_notes("resolution pending: textual conflict", "reviewed ok by r 1"), "resolution pending: textual conflict; reviewed ok by r 1")
        self.assertEqual(BIQ.merge_member_notes("resolution pending: x; reviewed ok by r 1", "resolved by r as abc"), "resolved by r as abc; reviewed ok by r 1")
        self.assertEqual(BIQ.merge_member_notes("resolved by r as abc", "reviewed ok by r 2"), "resolved by r as abc; reviewed ok by r 2")
        self.assertEqual(BIQ.merge_member_notes("reviewed ok by r 1", "reviewed ok by r 2"), "reviewed ok by r 2")
        # in the run: two collided members, one reviewed then resolved, the other resolved then reviewed
        self.submit("WI-A", {"u/a": "a"})
        self.submit("WI-B", {"u/a": "b"})
        self.submit("WI-C", {"u/a": "c"})
        self.start("U")
        run = BIQ.find_run(self.packets, "U", 1)
        self.assertEqual(sorted(BIQ.pending_resolutions(run)), ["WI-B", "WI-C"])
        def hand_merge(item):
            run = BIQ.find_run(self.packets, "U", 1)
            integration = run.fields["Integration commit"]
            return self.repo.git("commit-tree", f"{integration}^{{tree}}", "-p", integration, "-p", self.packets[item].candidate, "-m", f"resolve {item}")
        BIQ.review_member(self.root, self.registry, self.packets, run, "WI-B", owner="runner", ok=True, reject=None, hold=None, apply=True)
        BIQ.resolve_member(self.root, self.registry, self.packets, BIQ.find_run(self.packets, "U", 1), "WI-B", hand_merge("WI-B"), owner="runner", apply=True)
        BIQ.resolve_member(self.root, self.registry, self.packets, BIQ.find_run(self.packets, "U", 1), "WI-C", hand_merge("WI-C"), owner="runner", apply=True)
        BIQ.review_member(self.root, self.registry, self.packets, BIQ.find_run(self.packets, "U", 1), "WI-C", owner="runner", ok=True, reject=None, hold=None, apply=True)
        notes = {m[0]: m[2] for m in BIQ.find_run(self.reload(), "U", 1).members()}
        for item in ("WI-B", "WI-C"):
            self.assertIn("resolved by runner as ", notes[item]); self.assertIn("reviewed ok by runner ", notes[item]); self.assertNotIn("resolution pending", notes[item])
        # a rebased control branch that carried only the review keeps main's resolution
        ours = BIQ.find_run(self.packets, "U", 1).source
        theirs = ours.replace(notes["WI-B"], "reviewed ok by runner 2026-01-01T00:00:00Z")
        merged = BIQ.parse_packet("WI-CI-INTEGRATION-U-REPEAT-1", BIQ.members_union(ours, theirs, "WI-CI-INTEGRATION-U-REPEAT-1"))
        self.assertEqual({m[0]: m[2] for m in merged.members()}["WI-B"], notes["WI-B"].split("; ")[0] + "; reviewed ok by runner 2026-01-01T00:00:00Z")

    def test_green_passes_the_result_to_the_aggregator_and_red_rejects(self):
        a = self.submit("WI-A", {"u/a": "a"})
        self.submit("WI-B", {"u/b": "b"}, dependencies="WI-A")
        self.start("U")
        self.review_all("U", 1)
        run = self.packets["WI-CI-INTEGRATION-U-REPEAT-1"]
        with self.assertRaisesRegex(BIQ.BiqError, "exceed its budget"):
            BIQ.record_test(self.root, self.packets, run, "u-lane", "green", None, 11.0, owner="runner")
        with self.assertRaisesRegex(BIQ.BiqError, "unrecorded tests"):
            BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(), reject=[], apply=True, owner="runner")
        run = BIQ.record_test(self.root, self.packets, run, "u-lane", "red", "log", 4.0, owner="runner")
        self.assertEqual((run.tests, run.fields["Cost"]), ({"u-lane": "red"}, "$4"))
        with self.assertRaisesRegex(BIQ.BiqError, "--reject"):
            BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(), reject=[], apply=True, owner="runner")
        with self.assertRaisesRegex(BIQ.BiqError, "Accountable owner's word"):
            BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(), reject=["WI-A"], apply=True, owner="runner")
        outcome = BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(), reject=["WI-A"], apply=True, authorized_by="Human Owner", owner="runner")
        self.assertEqual((outcome["rejected"], outcome["returned"]), (["WI-A"], ["WI-B"]))
        self.assertEqual(self.packets["WI-A"].status, "failed-rejected")
        self.assertEqual([m[0] for m in BIQ.open_run(self.packets, "U").members()], ["WI-B"])
        # replacement: an ordinary submission that meets B's dependency
        self.submit("WI-A2", {"u/a": "a2"}, replaces="WI-A")
        self.assertEqual(sorted(m[0] for m in BIQ.open_run(self.packets, "U").members()), ["WI-A2", "WI-B"])
        self.assertEqual(BIQ.unmet_dependencies(self.root, self.packets, "WI-B", self.repo.main(), {"WI-A2"}), [])
        self.assertNotEqual(a, self.packets["WI-A2"].candidate)
        # the run had nothing left to integrate and closed; the open run carries on
        run = self.packets["WI-CI-INTEGRATION-U-REPEAT-1"]
        self.assertEqual((run.status, run.members()), ("failed-abandoned", []))
        self.submit("WI-D", {"u/d": "d"})
        self.assertEqual(sorted(m[0] for m in BIQ.open_run(self.packets, "U").members()), ["WI-A2", "WI-B", "WI-D"])

    def test_aggregator_waits_for_every_caring_feeder(self):
        self.submit("WI-A", {"u/a": "a", "k/a": "a"})
        self.start("U")
        outcome = self.green("U", 1)
        self.assertEqual(outcome["passed_to"], "AGG")
        self.assertEqual(self.packets["WI-CI-INTEGRATION-U-REPEAT-1"].status, "submitted")
        agg = BIQ.open_run(self.packets, "AGG")
        self.assertEqual([m[0] for m in agg.members()], ["WI-CI-INTEGRATION-U-REPEAT-1"])
        BIQ.claim_item(self.root, self.packets, agg.item_id, "runner", authorized_by="Human Owner")
        with self.assertRaisesRegex(BIQ.BiqError, "no member with met dependencies"):
            BIQ.start_run(self.root, self.registry, self.packets, "AGG", self.repo.main(), owner="runner", apply=True)
        report = BIQ.render_report(self.root, self.registry, self.packets, self.repo.main())
        self.assertIn("waiting: K result containing WI-A", report)
        self.start("K")
        self.green("K", 1)
        outcome = BIQ.start_run(self.root, self.registry, self.packets, "AGG", self.repo.main(), owner="runner", apply=True)
        self.assertEqual(sorted(m[0] for m in outcome["members"]), ["WI-CI-INTEGRATION-K-REPEAT-1", "WI-CI-INTEGRATION-U-REPEAT-1"])

    def test_a_dependency_joins_every_run_that_needs_it(self):
        y = self.submit("WI-Y", {"k/y": "y"})
        x = self.submit("WI-X", {"u/x": "x"}, dependencies="WI-Y")
        self.assertEqual([(m[0], m[2]) for m in BIQ.open_run(self.packets, "U").members()], [("WI-X", ""), ("WI-Y", "implied by WI-X")])
        self.assertEqual([m[0] for m in BIQ.open_run(self.packets, "K").members()], ["WI-Y"])
        # submitted the other way round, the dependency still joins
        self.submit("WI-B", {"u/b": "b"}, dependencies="WI-A", dependencies_after_claim=True)
        self.repo.packet("WI-A")
        self.assertEqual([m[0] for m in BIQ.open_run(self.packets, "U").members()], ["WI-X", "WI-Y", "WI-B"])
        self.submit("WI-A", {"k/a": "a"})
        self.assertEqual([m[0] for m in BIQ.open_run(self.packets, "U").members()], ["WI-X", "WI-Y", "WI-B", "WI-A"])
        outcome = self.start("U")
        self.assertEqual(sorted(m[0] for m in outcome["members"]), ["WI-A", "WI-B", "WI-X", "WI-Y"])
        self.assertTrue(BIQ.is_ancestor(self.root, y, self.packets["WI-CI-INTEGRATION-U-REPEAT-1"].fields["Integration commit"]))
        self.assertNotEqual(x, y)
        # rejected in K, Y must be shed from U's run too
        self.start("K")
        self.review_all("K", 1)
        run = BIQ.find_run(self.packets, "K", 1)
        run = BIQ.record_test(self.root, self.packets, run, "k-lane", "red", None, 0.0, owner="runner")
        BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(), reject=["WI-Y"], apply=True, authorized_by="Human Owner", owner="runner")
        self.review_all("U", 1)
        u_run = BIQ.find_run(self.packets, "U", 1)
        u_run = BIQ.record_test(self.root, self.packets, u_run, "u-lane", "green", None, 0.0, owner="runner")
        with self.assertRaisesRegex(BIQ.BiqError, "rejected elsewhere: WI-Y"):
            BIQ.complete_run(self.root, self.registry, self.packets, u_run, self.repo.main(), reject=[], apply=True, owner="runner")
        outcome = BIQ.complete_run(self.root, self.registry, self.packets, u_run, self.repo.main(), reject=["WI-Y"], apply=True, authorized_by="Human Owner", owner="runner")
        self.assertEqual((outcome["rejected"], outcome["returned"]), (["WI-Y"], ["WI-X"]))
        self.assertEqual(sorted(m[0] for m in self.packets["WI-CI-INTEGRATION-U-REPEAT-1"].members()), ["WI-A", "WI-B"])

    def test_abandon_closes_an_open_packet(self):
        self.repo.packet("WI-OLD")
        stream_path = self.root / BIQ.ITEMS_DIR / "WI-STREAM.md"
        stream_path.write_text(workstream_text("WI-STREAM", "WI-OLD, WI-OTHER"))
        stream_before = stream_path.read_bytes()
        self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, "--reason"):
            BIQ.abandon_item(self.root, self.packets, "WI-OLD", "agent", "  ", apply=True)
        packet = BIQ.abandon_item(self.root, self.packets, "WI-OLD", "agent", "superseded by WI-NEW", apply=True)
        self.assertEqual(packet.status, "failed-abandoned")
        self.assertIn("Abandoned by agent: superseded by WI-NEW", packet.sections["Resolution"])
        self.assertEqual(stream_path.read_bytes(), stream_before)
        with self.assertRaisesRegex(BIQ.BiqError, "is failed-abandoned"):
            BIQ.abandon_item(self.root, self.packets, "WI-OLD", "agent", "again", apply=True)

    def test_a_restart_names_its_successor_and_dependencies_follow_only_a_restart(self):
        self.repo.packet("WI-OLD"); self.repo.packet("WI-DEP", dependencies="WI-OLD")
        items = self.root / BIQ.ITEMS_DIR
        (items / "WI-STREAM.md").write_text(workstream_text("WI-STREAM", "WI-OLD, WI-OTHER"))
        (items / "WI-UNRELATED-STREAM.md").write_text(workstream_text("WI-UNRELATED-STREAM", "WI-UNRELATED"))
        (items / "WI-OPERATIVE.md").write_text(workstream_text("WI-OPERATIVE", "WI-OLD").replace("`project`", "`operative`"))
        self.reload()
        # A successor must be a fresh open packet naming the old attempt on both links.
        self.repo.packet("WI-OLD-ATTEMPT-2"); self.reload()
        before = {path.name: path.read_bytes() for path in items.glob("*.md")}
        with self.assertRaisesRegex(BIQ.BiqError, "must name WI-OLD in both Previous attempt and Replaces"):
            BIQ.abandon_item(self.root, self.packets, "WI-OLD", "agent", "fresh baseline", apply=True, successor="WI-OLD-ATTEMPT-2")
        with self.assertRaisesRegex(BIQ.BiqError, "has no packet"):
            BIQ.abandon_item(self.root, self.packets, "WI-OLD", "agent", "fresh baseline", apply=True, successor="WI-NOPE")
        self.assertEqual({path.name: path.read_bytes() for path in items.glob("*.md")}, before)
        source = packet_text("WI-OLD-ATTEMPT-2", replaces="WI-OLD").replace("- Dependencies: `none`", "- Previous attempt: `WI-OLD`\n- Dependencies: `none`", 1)
        (self.root / "docs/work-queue/items/WI-OLD-ATTEMPT-2.md").write_text(source); self.reload()
        before = {path.name: path.read_bytes() for path in items.glob("*.md")}
        BIQ.abandon_item(self.root, self.packets, "WI-OLD", "agent", "fresh baseline", apply=False, successor="WI-OLD-ATTEMPT-2")
        self.assertEqual({path.name: path.read_bytes() for path in items.glob("*.md")}, before)
        self.assertEqual(self.reload()["WI-OLD"].status, "open")
        old = BIQ.abandon_item(self.root, self.packets, "WI-OLD", "agent", "fresh baseline", apply=True, successor="WI-OLD-ATTEMPT-2")
        self.assertEqual((old.status, old.fields["Next attempt"]), ("failed-abandoned", "WI-OLD-ATTEMPT-2"))
        self.assertEqual(BIQ.container_members(self.reload()["WI-STREAM"]), ["WI-OLD-ATTEMPT-2", "WI-OTHER"])
        self.assertEqual(BIQ.workstream_of(self.packets, "WI-OLD-ATTEMPT-2"), "WI-STREAM")
        for name, contents in before.items():
            if name not in {"WI-OLD.md", "WI-STREAM.md"}:
                self.assertEqual((items / name).read_bytes(), contents, name)
        self.assertIn("Restarted as WI-OLD-ATTEMPT-2.", old.sections["Resolution"])
        # The dependent follows the restart, and is met only when the successor lands.
        self.assertEqual(BIQ.resolve_replacement(self.packets, "WI-OLD"), "WI-OLD-ATTEMPT-2")
        self.assertFalse(BIQ.landed(self.root, self.packets, BIQ.resolve_replacement(self.packets, "WI-OLD"), self.repo.main()))
        # A plain abandonment grants no replacement: the dependency stays on the abandoned packet.
        self.repo.packet("WI-PLAIN"); self.reload()
        BIQ.abandon_item(self.root, self.packets, "WI-PLAIN", "agent", "not worth it", apply=True)
        self.assertEqual(BIQ.resolve_replacement(self.packets, "WI-PLAIN"), "WI-PLAIN")

    def test_reseal_moves_the_submitted_candidate_in_every_open_run(self):
        a = self.submit("WI-A", {"u/a": "a", "k/a": "a"})
        b = self.repo.commit_on(self.repo.main(), "WI-A", {"u/a": "a2"})
        packet = BIQ.seal_item(self.root, self.packets, "WI-A", b, self.repo.main(), "agent")
        self.assertEqual((packet.status, packet.candidate, packet.queues), ("submitted", b, ("U",)))
        self.assertEqual([(m[0], m[1]) for m in BIQ.open_run(self.packets, "U").members()], [("WI-A", b)])
        self.assertEqual(BIQ.open_run(self.packets, "K").members(), [])
        self.assertNotEqual(a, b)

    def test_seal_inserts_the_candidate_fields_a_packet_lacks_and_next_attempt_replaces(self):
        sha = self.repo.commit_on(self.repo.main(), "WI-OLD", {"u/old": "x"})
        text = packet_text("WI-OLD", dependencies="WI-REJ")
        text = "\n".join(line for line in text.splitlines() if not line.startswith(("- Candidate ", "- Submitted "))) + "\n"
        items = self.root / "docs/work-queue/items"
        (items / "WI-OLD.md").write_text(text)
        rejected = packet_text("WI-REJ", status="failed-rejected").replace("- Dependencies: `none`", "- Dependencies: `none`\n- Next attempt: `WI-REJ-ATTEMPT-2`")
        (items / "WI-REJ.md").write_text(rejected)
        (items / "WI-REJ-ATTEMPT-2.md").write_text(packet_text("WI-REJ-ATTEMPT-2", status="completed"))
        self.repo.commit_queue()
        self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-OLD", "agent")
        BIQ.seal_item(self.root, self.packets, "WI-OLD", sha, self.repo.main(), "agent")
        BIQ.submit_item(self.root, self.registry, self.packets, "WI-OLD", self.repo.main(), "agent", apply=True)
        source = (self.root / "docs/work-queue/items/WI-OLD.md").read_text()
        order = [line.split(":")[0][2:] for line in source.splitlines() if line.startswith("- ")]
        self.assertLess(order.index("Queues"), order.index("Dependencies"))
        self.assertEqual([f for f in order if f.startswith(("Candidate", "Submitted"))], ["Candidate base", "Candidate commit", "Submitted At", "Submitted owner"])
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])
        self.assertEqual(BIQ.resolve_replacement(self.packets, "WI-REJ"), "WI-REJ-ATTEMPT-2")
        self.assertEqual(BIQ.unmet_dependencies(self.root, self.packets, "WI-OLD", self.repo.main(), set()), [])

    def test_a_stale_candidate_seals_and_the_integrator_resolves_its_conflict(self):
        base = self.repo.main()
        self.repo.land(self.repo.commit_on(base, None, {"u/shared": "main version\n"}))
        stale = self.repo.commit_on(base, "WI-STALE", {"u/shared": "branch version\n"})
        self.repo.packet("WI-STALE")
        stale_path = self.root / "docs/work-queue/items/WI-STALE.md"
        stale_path.write_text(stale_path.read_text().replace("- Dependencies: `none`", "- Claimed At: `pending`\n- Implementation Duration: `pending`\n- Dependencies: `none`"))
        self.repo.commit_queue(); self.reload()
        self.assertIn("python3 tools/biq.py claim WI-STALE", BIQ.render_show(self.root, self.registry, self.packets, "WI-STALE", self.repo.main()))
        BIQ.claim_item(self.root, self.packets, "WI-STALE", "agent")
        self.assertIn("your own branch", BIQ.render_show(self.root, self.registry, self.packets, "WI-STALE", self.repo.main()))
        BIQ.seal_item(self.root, self.packets, "WI-STALE", stale, self.repo.main(), "agent")  # the owner never chases main
        self.assertIn("next: python3 tools/biq.py submit WI-STALE --owner agent --apply", BIQ.render_show(self.root, self.registry, self.packets, "WI-STALE", self.repo.main()))
        BIQ.submit_item(self.root, self.registry, self.packets, "WI-STALE", self.repo.main(), "agent", apply=True); self.repo.commit_queue()
        stale_packet = self.packets["WI-STALE"]
        self.assertNotEqual(stale_packet.fields["Claimed At"], "pending")
        self.assertRegex(stale_packet.fields["Implementation Duration"], r"^PT\d+S$")
        outcome = self.start("U")
        self.assertEqual([c[0] for c in outcome["collisions"]], ["WI-STALE"])
        run = BIQ.find_run(self.packets, "U", 1)
        self.assertIn("WI-STALE", BIQ.pending_resolutions(run))
        self.assertNotIn("WI-STALE", [m[0] for m in BIQ.open_run(self.packets, "U").members()])
        self.assertIn("resolution pending", BIQ.render_show(self.root, self.registry, self.packets, "WI-STALE", self.repo.main()))
        self.repo.git("checkout", "-q", "--detach", run.fields["Integration commit"])
        merge = subprocess.run(["git", "merge", "-q", stale], cwd=self.root, capture_output=True)
        self.assertNotEqual(merge.returncode, 0)
        (self.root / "u/shared").write_text("resolved\n")
        self.repo.git("add", "u/shared"); self.repo.git("commit", "-q", "-m", "resolve WI-STALE")
        resolved = self.repo.git("rev-parse", "HEAD"); self.repo.git("checkout", "-q", "main")
        with self.assertRaisesRegex(BIQ.BiqError, "must be a merge of"):
            BIQ.resolve_member(self.root, self.registry, self.packets, run, "WI-STALE", stale, owner="runner", apply=False)
        # The integrator inspected on the run's branch in a worktree: a dirty one is named, a clean one moves with the branch.
        inspection = self.root.parent / "inspection"
        self.repo.git("worktree", "add", "-q", str(inspection), "biq/u/1")
        (inspection / "u/shared").write_text("scratch\n")
        with self.assertRaisesRegex(BIQ.BiqError, "biq/u/1 is checked out at .* with local changes"):
            BIQ.resolve_member(self.root, self.registry, self.packets, run, "WI-STALE", resolved, owner="runner", apply=True)
        subprocess.run(["git", "checkout", "-q", "--", "u/shared"], cwd=inspection, check=True)
        BIQ.resolve_member(self.root, self.registry, self.packets, run, "WI-STALE", resolved, owner="runner", apply=True)
        self.assertEqual(subprocess.run(["git", "rev-parse", "HEAD"], cwd=inspection, text=True, capture_output=True).stdout.strip(), resolved)
        self.repo.git("worktree", "remove", "--force", str(inspection))
        run = BIQ.find_run(self.packets, "U", 1)
        self.assertEqual(run.fields["Integration commit"], resolved)
        self.assertIn("WI-STALE", [m[0] for m in run.members()])
        self.assertEqual(BIQ.pending_resolutions(run), {})
        self.assertEqual(BIQ.excluded_members(run), {})
        self.assertNotIn("WI-STALE", [m[0] for m in BIQ.open_run(self.packets, "U").members()])
        self.assertTrue(all(v == "pending" for v in run.tests.values()))
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])
        self.green("U", 1)

    def test_explicit_candidate_base_repair_preserves_claim_and_uses_durable_packet(self):
        item = "WI-BASE-REPAIR"
        baseline = self.repo.main()
        self.repo.packet(item, baseline=f"origin/main {baseline}")
        acquisition = self.repo.commit_queue(); self.reload()
        BIQ.claim_item(self.root, self.packets, item, "agent")
        candidate = self.repo.commit_on(baseline, item, {"u/repair": "candidate"})
        self.repo.git("checkout", "-q", "-b", "candidate", candidate)
        refreshed = BIQ.claim_item(self.root, self.packets, item, "agent")
        self.assertEqual(refreshed["candidate_base"], acquisition)
        self.assertFalse(BIQ.is_ancestor(self.root, acquisition, candidate))
        oid, before = BIQ.read_claim(self.root, item)
        before.update({"started_at": "2026-09-01T00:00:00Z", "updated_at": "2026-09-01T00:00:00Z",
                       "usage_baseline": {"retained": 7}, "unknown_extension": {"retained": [1, 2]}, "claim_oid": "foreign-value"})
        oid = BIQ.push_claim(self.root, item, before, oid)
        # A stale or edited candidate packet cannot choose the repair baseline.
        self.packets[item].fields["Baseline"] = f"origin/main {acquisition}"
        with mock.patch.object(BIQ, "materialize_repeat_successor") as successor:
            repaired = BIQ.claim_item(self.root, self.packets, item, "agent", repair_candidate_base=True)
        successor.assert_not_called()
        new_oid, after = BIQ.read_claim(self.root, item)
        self.assertEqual(repaired["claim_oid"], new_oid)
        self.assertEqual(self.repo.git("rev-parse", f"{new_oid}^"), oid)
        expected = dict(before, candidate_base=baseline, updated_at=after["updated_at"])
        self.assertEqual(after, expected)
        self.assertNotEqual(after["updated_at"], before["updated_at"])
        self.assertEqual(self.repo.git("rev-parse", "HEAD"), candidate)
        self.assertEqual(BIQ.claim_item(self.root, self.packets, item, "agent")["candidate_base"], baseline)

    def test_candidate_base_repair_refuses_invalid_authority_without_writing(self):
        item = "WI-BASE-REPAIR"
        baseline = self.repo.main()
        self.repo.packet(item, baseline=f"origin/main {baseline}")
        self.repo.commit_queue(); self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, "no existing claim"):
            BIQ.claim_item(self.root, self.packets, item, "agent", repair_candidate_base=True)
        BIQ.claim_item(self.root, self.packets, item, "agent")
        oid, original = BIQ.read_claim(self.root, item)
        unrelated = self.repo.commit_on(baseline, None, {"elsewhere": "unrelated"})
        valid = self.packets[item].source
        invalid_packets = [
            (None, "open ordinary"),
            (BIQ.parse_packet(item, valid.replace(f"- Baseline: `origin/main {baseline}`\n", "")), "exact SHA"),
            (BIQ.parse_packet(item, valid.replace(f"origin/main {baseline}", "pending")), "exact SHA"),
            (BIQ.parse_packet(item, valid.replace(f"origin/main {baseline}", "origin/main")), "exact SHA"),
            (BIQ.parse_packet(item, valid.replace(baseline, baseline[:12])), "exact SHA"),
            (BIQ.parse_packet(item, valid.replace(baseline, "f" * 40)), "not a commit"),
            (BIQ.parse_packet(item, valid.replace(baseline, unrelated)), "not an ancestor"),
            (BIQ.parse_packet(item, valid.replace("Status: `open`", "Status: `submitted`")), "open ordinary"),
            (BIQ.parse_packet(item, valid.replace("Status: `open`", "Status: `completed`")), "open ordinary"),
            (BIQ.open_run(self.packets, "U"), "open ordinary"),
        ]
        for packet, error in invalid_packets:
            with self.subTest(error=error, packet=packet), mock.patch.object(BIQ, "durable_packet", return_value=("origin/main", packet)):
                with self.assertRaisesRegex(BIQ.BiqError, error):
                    BIQ.claim_item(self.root, self.packets, item, "agent", repair_candidate_base=True)
                self.assertEqual(BIQ.read_claim(self.root, item), (oid, original))
        for options in ({"note": "refresh"}, {"usage_snapshot": "missing.json"}, {"authorized_by": "Human"}):
            with self.subTest(options=options), self.assertRaisesRegex(BIQ.BiqError, "cannot be combined"):
                BIQ.claim_item(self.root, self.packets, item, "agent", repair_candidate_base=True, **options)
        with self.assertRaisesRegex(BIQ.BiqError, "claimed by"):
            BIQ.claim_item(self.root, self.packets, item, "other-owner", repair_candidate_base=True)
        self.assertEqual(BIQ.read_claim(self.root, item), (oid, original))
        for field, value in (("head", baseline), ("branch", "other"), ("worktree", str(self.root.parent)), ("ready", True)):
            modified = dict(original, **{field: value})
            oid = BIQ.push_claim(self.root, item, modified, oid)
            with self.subTest(field=field), self.assertRaisesRegex(BIQ.BiqError, "unchanged claim|open ordinary"):
                BIQ.claim_item(self.root, self.packets, item, "agent", repair_candidate_base=True)
            self.assertEqual(BIQ.read_claim(self.root, item), (oid, modified))

    def test_candidate_base_repair_cli_accepts_exact_sha_without_refresh(self):
        item = "WI-BASE-REPAIR"
        baseline = self.repo.main()
        self.repo.packet(item, baseline=baseline)
        self.repo.commit_queue(); self.reload()
        BIQ.claim_item(self.root, self.packets, item, "agent")
        result = subprocess.run([sys.executable, str(SCRIPT), "--repository", str(self.root), "claim", item,
                                 "--owner", "agent", "--repair-candidate-base"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"candidate base to {baseline}", result.stdout)
        self.assertEqual(BIQ.read_claim(self.root, item)[1]["candidate_base"], baseline)

    def test_frozen_checkout_reads_queue_state_from_main_and_a_claim_refresh_keeps_foreign_fields(self):
        self.repo.packet("WI-FROZEN"); self.repo.commit_queue(); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-FROZEN", "agent")
        _oid, claim = BIQ.read_claim(self.root, "WI-FROZEN")
        claim.update({"candidate_base": "f" * 40})
        BIQ.push_claim(self.root, "WI-FROZEN", claim, _oid)
        snapshot = self.root.parent / "usage.json"
        snapshot.write_text(json.dumps({"schema": 1, "provider": "anthropic", "source_id": "s", "observed_at": "2026-09-07T00:00:00Z",
                                        "counters": {k: (7 if k in ("input_tokens", "total_tokens") else 0) for k in BIQ.USAGE_COUNTER_FIELDS},
                                        "models": {"m": {k: (7 if k in ("input_tokens", "total_tokens") else 0) for k in BIQ.USAGE_MODEL_COUNTER_FIELDS}}}))
        refreshed = BIQ.claim_item(self.root, self.packets, "WI-FROZEN", "agent", note="again", usage_snapshot=str(snapshot))
        self.assertEqual(refreshed["candidate_base"], "f" * 40)
        refreshed = BIQ.claim_item(self.root, self.packets, "WI-FROZEN", "agent", note="again")
        for key in ("human_owner", "host_user", "hostname", "item_identity", "candidate_base", "usage_baseline", "head", "branch", "worktree", "started_at", "updated_at", "note"):
            self.assertIn(key, refreshed)
        self.assertEqual(refreshed["usage_baseline"]["counters"]["total_tokens"], 7)
        self.assertEqual(refreshed["note"], "again")
        self.assertEqual(refreshed["started_at"], claim["started_at"])
        frozen = self.root.parent / "frozen"
        self.repo.git("worktree", "add", "--detach", "-q", str(frozen), self.repo.main())
        self.repo.git("checkout", "-q", "--detach", self.repo.git("rev-list", "--max-count=1", "--skip=1", "main"))
        self.repo.git("checkout", "-q", "main")
        registry, packets, revision = BIQ.queue_state(frozen)
        self.assertIsNone(revision)  # the frozen tree's packets are main's today
        self.assertIn("WI-FROZEN", packets)
        self.assertEqual(registry, self.registry)
        out = subprocess.run([sys.executable, str(Path(__file__).resolve().parent / "biq.py"), "--repository", str(frozen), "show", "WI-FROZEN"], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("next: commit the work on your own branch", out.stdout)

    def test_frozen_committed_legacy_registry_reads_current_main_without_mutation(self):
        self.repo.packet("WI-FROZEN"); self.repo.commit_queue()
        frozen_head = self.repo.commit_on(self.repo.main(), "WI-FROZEN", {
            "docs/work-queue/queues.json": LEGACY_REGISTRY,
            f"{BIQ.ITEMS_DIR}/WI-FROZEN.md": packet_text("WI-FROZEN", status="failed-abandoned"),
        })
        frozen = self.root.parent / "frozen"
        self.repo.git("worktree", "add", "--detach", "-q", str(frozen), frozen_head)
        registry_path = frozen / "docs/work-queue/queues.json"
        before = (registry_path.read_bytes(), BIQ.git(frozen, "rev-parse", "HEAD"), BIQ.git(frozen, "status", "--porcelain"))
        self.assertEqual(before[2], "")
        registry, packets, revision = BIQ.queue_state(frozen)
        self.assertEqual(revision, "origin/main")
        self.assertEqual(registry, self.registry)
        self.assertEqual(packets["WI-FROZEN"].status, "open")
        self.assertEqual(BIQ.load_registry(frozen), self.registry)  # the runs define the queues; the retired file is ignored
        out = subprocess.run([sys.executable, str(SCRIPT), "--repository", str(frozen), "show", "WI-FROZEN"], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("WI-FROZEN: open", out.stdout)
        self.assertIn("queue state read from origin/main; this checkout's packets are not its", out.stderr)
        self.assertEqual(before, (registry_path.read_bytes(), BIQ.git(frozen, "rev-parse", "HEAD"), BIQ.git(frozen, "status", "--porcelain")))

    def test_run_cancel_returns_the_run_to_open_and_removes_the_successor(self):
        self.submit("WI-C1", {"u/c1": "1"}); self.submit("WI-C2", {"u/c2": "2"}, dependencies="WI-NOPE-LATER", dependencies_after_claim=True)
        self.repo.packet("WI-NOPE-LATER"); self.repo.commit_queue(); self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, "no authority to claim"):
            BIQ.claim_item(self.root, self.packets, BIQ.open_run(self.packets, "U").item_id, "rogue")
        self.start("U")
        run = BIQ.find_run(self.packets, "U", 1)
        self.assertEqual(BIQ.run_state(run), "started")
        self.assertIn("WI-CI-INTEGRATION-U-REPEAT-2", self.packets)
        BIQ.cancel_run(self.root, self.registry, self.packets, run, owner="runner", apply=True)
        run = BIQ.find_run(self.packets, "U", 1)
        self.assertEqual(BIQ.run_state(run), "open")
        self.assertEqual(sorted(m[0] for m in run.members()), ["WI-C1", "WI-C2"])
        self.assertNotIn("WI-CI-INTEGRATION-U-REPEAT-2", self.packets)
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])

    def test_registry_accepts_test_metadata_beside_the_command_map(self):
        registry = registry_from_json(json.dumps({"schema": 2, "target": "main", "queues": [{"id": "Q", "kind": "leaf", "tests": ["a", "b"], "prefixes": ["q/"]}],
                                                  "commands": {"a": "run-a.sh"}, "tests": {"b": {"command": "run-b.sh", "target": "kernel", "cost": "paid"}}}))
        self.assertEqual(registry.commands, {"a": "run-a.sh", "b": "run-b.sh"})
        self.assertEqual(registry.tests["b"]["target"], "kernel")
        with self.assertRaisesRegex(BIQ.BiqError, "needs command, target"):
            registry_from_json(json.dumps({"schema": 2, "target": "main", "queues": [], "tests": {"b": {"command": "x", "target": "moon", "cost": "paid"}}}))

    def test_submission_prices_usage_against_the_claim_baseline_and_writes_the_receipts(self):
        (self.root / "docs/work-queue/model-token-rates.json").write_text(json.dumps({"schema": 1, "basis": "test", "models": {"m1": {
            "input_per_million_usd": "1", "cached_input_per_million_usd": "0.1", "output_per_million_usd": "4", "cache_write_multiplier": "1.25",
            "long_context_threshold_input_tokens": 200000, "long_context_input_multiplier": "2", "long_context_output_multiplier": "1.5", "source": "test"}}}))
        def snap(n, at):
            m = {"input_tokens": n, "cached_input_tokens": 0, "cache_write_input_tokens": 0, "output_tokens": n, "reasoning_output_tokens": 0, "total_tokens": 2 * n,
                 "long_input_tokens": 0, "long_cached_input_tokens": 0, "long_cache_write_input_tokens": 0, "long_output_tokens": 0}
            return {"schema": 1, "provider": "test", "source_id": "s1", "observed_at": at, "counters": {k: m[k] for k in BIQ.USAGE_COUNTER_FIELDS}, "models": {"m1": m}}
        base, cut = self.root.parent / "base.json", self.root.parent / "cut.json"
        base.write_text(json.dumps(snap(1_000_000, "2026-09-06T00:00:00Z"))); cut.write_text(json.dumps(snap(2_000_000, "2026-09-06T01:00:00Z")))
        sha = self.repo.commit_on(self.repo.main(), "WI-ACC", {"docs/acc": "x"})
        self.repo.packet("WI-ACC")
        path = self.root / "docs/work-queue/items/WI-ACC.md"
        path.write_text(path.read_text().replace("- Dependencies: `none`", "- Claimed At: `pending`\n- Implementation Duration: `pending`\n- Tokens Used: `pending`\n- Model Used: `pending`\n- Estimated Cost: `pending`\n- Estimated AWS Cost: `pending`\n- Dependencies: `none`"))
        self.repo.commit_queue(); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-ACC", "agent", usage_snapshot=str(base))
        BIQ.seal_item(self.root, self.packets, "WI-ACC", sha, self.repo.main(), "agent")
        BIQ.submit_item(self.root, self.registry, self.packets, "WI-ACC", self.repo.main(), "agent", apply=True, usage_snapshot=str(cut), aws_cost="$2.5")
        p = self.packets["WI-ACC"]
        self.assertEqual((p.fields["Tokens Used"], p.fields["Model Used"], p.fields["Estimated Cost"], p.fields["Estimated AWS Cost"]), ("2000000", "m1", "$5.000000", "$2.5"))
        resolution = p.sections["Resolution"]
        self.assertIn(BIQ.USAGE_EVIDENCE_PREFIX, resolution); self.assertIn(BIQ.SUBSTANTIVE_WORK_PROVENANCE_PREFIX, resolution)
        usage = json.loads(resolution.split(BIQ.USAGE_EVIDENCE_PREFIX, 1)[1].splitlines()[0])
        self.assertEqual((usage["mode"], usage["usage"]["counters"]["total_tokens"], usage["estimated_cost"]), ("measured", 2000000, "$5.000000"))
        prov = json.loads(resolution.split(BIQ.SUBSTANTIVE_WORK_PROVENANCE_PREFIX, 1)[1].splitlines()[0])
        self.assertEqual((prov["item"], prov["role"], prov["owner"], prov["recorded_at"]), ("WI-ACC", "implementation", "agent", p.fields["Submitted At"]))

    def test_first_claim_of_a_repeat_tail_writes_the_next_ordinal(self):
        self.repo.packet("WI-Q-REPEAT-1"); self.repo.commit_queue(); self.reload()
        data = BIQ.claim_item(self.root, self.packets, "WI-Q-REPEAT-1", "agent")
        self.assertEqual(data["successor"], "WI-Q-REPEAT-2")
        successor = self.packets["WI-Q-REPEAT-2"]
        self.assertEqual((successor.status, successor.fields["ID"], successor.sections["Resolution"].strip()), ("open", "WI-Q-REPEAT-2", "pending"))
        self.assertTrue((self.root / "docs/work-queue/items/WI-Q-REPEAT-2.md").exists())
        self.assertIsNone(BIQ.claim_item(self.root, self.packets, "WI-Q-REPEAT-1", "agent").get("successor"))

    def test_invalid_repeat_reading_creates_neither_claim_nor_successor(self):
        item = "WI-CI-AUDIT-READING-REPEAT-1"
        target = "docs/work-queue/items/WI-CI-QUALIFICATION-HISTORY.md#objective"
        source = validation_packet_text(item).replace("README.md#owner", target)
        (self.root / BIQ.ITEMS_DIR / f"{item}.md").write_text(source)
        self.repo.commit_queue(); self.reload()
        successor = self.root / BIQ.ITEMS_DIR / "WI-CI-AUDIT-READING-REPEAT-2.md"
        with mock.patch.object(BIQ, "push_claim") as push:
            with self.assertRaisesRegex(BIQ.BiqError, "governing-authority cannot name a work-item packet"):
                BIQ.claim_item(self.root, self.packets, item, "agent")
            push.assert_not_called()
        self.assertFalse(successor.exists())
        self.assertNotIn("WI-CI-AUDIT-READING-REPEAT-2", self.packets)
        self.assertEqual(BIQ.read_claim(self.root, item), (None, None))

    def test_historical_work_item_reading_is_cloned_into_repeat_successor(self):
        item = "WI-CI-AUDIT-READING-REPEAT-1"
        historical = "docs/work-queue/items/WI-CI-QUALIFICATION-HISTORY.md"
        source = validation_packet_text(item).replace(
            "- verification: `tools/check.py`",
            f"- verification: `tools/check.py`\n- historical-evidence: `{historical}`",
        )
        (self.root / BIQ.ITEMS_DIR / f"{item}.md").write_text(source)
        self.repo.packet("WI-CI-QUALIFICATION-HISTORY", status="completed")
        self.repo.commit_queue(); self.reload()
        data = BIQ.claim_item(self.root, self.packets, item, "agent")
        self.assertEqual(data["successor"], "WI-CI-AUDIT-READING-REPEAT-2")
        copied = self.packets[data["successor"]].sections["Required reading"]
        self.assertIn(f"- historical-evidence: `{historical}`", copied)

    def test_gate_admits_a_landing_after_main_moved_on_the_same_file(self):
        base = self.repo.main()
        self.repo.land(self.repo.commit_on(base, None, {"docs/shared": "\n".join(f"line {i}" for i in range(12)) + "\n"}))
        base = self.repo.main()
        text = (self.root / "docs/shared").read_text().splitlines()
        first = self.repo.commit_on(base, "WI-D1", {"docs/shared": "\n".join(["first"] + text[1:]) + "\n"})
        second = self.repo.commit_on(base, "WI-D2", {"docs/shared": "\n".join(text[:-1] + ["second"]) + "\n"})
        for item, sha in (("WI-D1", first), ("WI-D2", second)):
            self.repo.packet(item, candidate=sha)
        self.repo.commit_queue(); self.reload()
        for item in ("WI-D1", "WI-D2"):
            BIQ.claim_item(self.root, self.packets, item, "agent")
            BIQ.submit_item(self.root, self.registry, self.packets, item, self.repo.main(), "agent", apply=True)
        self.repo.commit_queue()
        first_landing = BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-D1", self.repo.main(), apply=True)
        self.repo.git("checkout", "-q", "main"); self.repo.git("merge", "-q", "--ff-only", first_landing["head"]); self.repo.git("push", "-q", "origin", "main")
        self.reload()
        second_landing = BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-D2", self.repo.main(), apply=True)
        self.assertEqual(BIQ.verify_landing(self.root, self.repo.main(), second_landing["head"]), "leaf WI-D2 that no queue cares about")
        self.assertIn("first", self.repo.git("show", f"{second_landing['head']}:docs/shared"))
        self.assertIn("second", self.repo.git("show", f"{second_landing['head']}:docs/shared"))

    def test_gate_refuses_new_validation_errors_without_executing_legacy_code(self):
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {
            "tools/work-queue.py": "raise RuntimeError('legacy code must not execute')\n",
            "README.md": "# Owner\n", "tools/check.py": "pass\n"}))
        baseline = self.repo.main()
        def current(text):
            return text.replace("origin/main " + "a" * 40, "origin/main " + baseline)
        def malformed(item):
            return current(validation_packet_text(item)).replace("- Accountable owner: `Example Person`\n", "")
        head = self.repo.commit_on(self.repo.main(), None, {"docs/work-queue/items/WI-BAD-NEW.md": malformed("WI-BAD-NEW")})
        with self.assertRaisesRegex(BIQ.BiqError, "packet validation.*WI-BAD-NEW.*Accountable owner"):
            BIQ.verify_landing(self.root, self.repo.main(), head)
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"docs/work-queue/items/WI-BAD-OLD.md": malformed("WI-BAD-OLD")}))
        head = self.repo.commit_on(self.repo.main(), None, {"docs/work-queue/items/WI-GOOD.md": current(validation_packet_text("WI-GOOD"))})
        self.assertEqual(BIQ.verify_landing(self.root, self.repo.main(), head), "control commit")
        repaired = self.repo.commit_on(self.repo.main(), None, {"docs/work-queue/items/WI-BAD-OLD.md": current(validation_packet_text("WI-BAD-OLD"))})
        self.assertEqual(BIQ.verify_landing(self.root, self.repo.main(), repaired), "control commit")
        head = self.repo.commit_on(self.repo.main(), None, {"docs/work-queue/items/WI-BAD-NEW.md": malformed("WI-BAD-NEW")})
        with self.assertRaisesRegex(BIQ.BiqError, "WI-BAD-NEW"):
            BIQ.verify_landing(self.root, self.repo.main(), head)

    def test_gate_refuses_a_new_unresolved_or_unreachable_baseline(self):
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"README.md": "# Owner\n", "tools/check.py": "pass\n"}))
        base = self.repo.main()
        def packet(item, baseline):
            return validation_packet_text(item).replace("origin/main " + "a" * 40, "origin/main " + baseline)
        missing = "f" * 40
        head = self.repo.commit_on(base, None, {"docs/work-queue/items/WI-MISSING-BASE.md": packet("WI-MISSING-BASE", missing)})
        with self.assertRaisesRegex(BIQ.BiqError, r"WI-MISSING-BASE: Baseline commit f{40} does not resolve; use the full result"):
            BIQ.verify_landing(self.root, base, head)
        self.repo.land(head)  # model pre-rule history that already contains the bad value
        historical_base = self.repo.main()
        historical_head = self.repo.commit_on(historical_base, None, {"docs/work-queue/items/WI-VALID-BESIDE-HISTORY.md": packet("WI-VALID-BESIDE-HISTORY", historical_base)})
        self.assertEqual(BIQ.verify_landing(self.root, historical_base, historical_head), "control commit")
        tree = self.repo.git("rev-parse", f"{base}^{{tree}}")
        unrelated = self.repo.git("commit-tree", tree, "-m", "unreachable baseline")
        head = self.repo.commit_on(base, None, {"docs/work-queue/items/WI-UNREACHABLE-BASE.md": packet("WI-UNREACHABLE-BASE", unrelated)})
        with self.assertRaisesRegex(BIQ.BiqError, "WI-UNREACHABLE-BASE: Baseline commit .* is not reachable from origin/main"):
            BIQ.verify_landing(self.root, base, head)
        head = self.repo.commit_on(base, None, {"docs/work-queue/items/WI-VALID-BASE.md": packet("WI-VALID-BASE", base)})
        self.assertEqual(BIQ.verify_landing(self.root, base, head), "control commit")

    def test_resolution_receipts_of_one_kind_are_replaced_not_stacked(self):
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"docs/work-queue/items/WI-RECEIPT.md": packet_text("WI-RECEIPT")}))
        packets = BIQ.load_packets(self.root)
        BIQ.append_resolution_lines(self.root, packets, "WI-RECEIPT", ["Work-queue substantive work provenance v1: {\"attempt\": 1}"])
        packets = BIQ.load_packets(self.root)
        BIQ.append_resolution_lines(self.root, packets, "WI-RECEIPT", ["Work-queue substantive work provenance v1: {\"attempt\": 2}", "Work-queue usage v1: {\"tokens\": 1}"])
        body = BIQ.load_packets(self.root)["WI-RECEIPT"].sections["Resolution"]
        self.assertEqual(body.count("Work-queue substantive work provenance v1:"), 1)
        self.assertIn("\"attempt\": 2", body)
        self.assertEqual(body.count("Work-queue usage v1:"), 1)

    def test_seal_refuses_a_candidate_whose_history_discarded_main(self):
        branch = self.repo.commit_on(self.repo.main(), "WI-X", {"u/x": "x"})
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"u/gained": "main gained this"}))
        # A "merge" that names main as a parent but keeps the branch's tree: landing it would revert u/gained.
        fake = self.repo.git("commit-tree", f"{branch}^{{tree}}", "-p", self.repo.main(), "-p", branch, "-m", "Adopt base")
        top = self.repo.commit_on(fake, "WI-X", {"u/x": "x2"})
        self.repo.packet("WI-X"); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-X", "agent")
        with self.assertRaisesRegex(BIQ.BiqError, "discarded a parent: " + fake[:12] + r" \(Adopt base\)"):
            BIQ.seal_item(self.root, self.packets, "WI-X", top, self.repo.main(), "agent")
        # A true merge of main into the branch is fine, even when its tree happens to equal a parent's.
        merged = self.repo.git("commit-tree", self.repo.git("merge-tree", "--write-tree", self.repo.main(), branch).splitlines()[0], "-p", self.repo.main(), "-p", branch, "-m", "Merge main")
        packet = BIQ.seal_item(self.root, self.packets, "WI-X", merged, self.repo.main(), "agent")
        self.assertEqual(packet.candidate, merged)

    def test_seal_accepts_parent_tree_from_normal_conflict_resolution(self):
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"u/shared": "base\n"}))
        base = self.repo.main()
        left = self.repo.commit_on(base, "WI-X", {"u/shared": "left\n"})
        right = self.repo.commit_on(base, "WI-X", {"u/shared": "right\n"})
        unresolved = subprocess.run(["git", "merge-tree", "--write-tree", left, right], cwd=self.root, text=True, capture_output=True)
        self.assertEqual(unresolved.returncode, 1)
        self.repo.packet("WI-X"); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-X", "agent")
        for preference, parent in (("ours", left), ("theirs", right)):
            with self.subTest(preference=preference):
                tree = self.repo.git("rev-parse", f"{parent}^{{tree}}")
                resolved = self.repo.git("merge-tree", "--write-tree", f"-X{preference}", left, right).splitlines()[0]
                self.assertEqual(resolved, tree)
                merged = self.repo.git("commit-tree", tree, "-p", left, "-p", right, "-m", "Resolve conflict")
                packet = BIQ.seal_item(self.root, self.packets, "WI-X", merged, base, "agent")
                self.assertEqual(packet.candidate, merged)

    def test_seal_refuses_lost_independent_content_despite_a_conflict(self):
        padding = "".join(f"unchanged {i}\n" for i in range(20))
        original = "base\n" + padding + "old tail\n"
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"u/shared": original}))
        base = self.repo.main()
        kept = self.repo.commit_on(base, "WI-X", {"u/shared": original.replace("base", "left", 1)})
        tree = self.repo.git("rev-parse", f"{kept}^{{tree}}")
        for location in ("other-file", "same-file"):
            changes = {"u/shared": original.replace("base", "right", 1)}
            if location == "other-file":
                changes["u/independent"] = "must survive\n"
            else:
                changes["u/shared"] = changes["u/shared"].replace("old tail", "must survive")
            updated = self.repo.commit_on(base, "WI-X", changes)
            for preference, parents in (("ours", (kept, updated)), ("theirs", (updated, kept))):
                with self.subTest(location=location, preference=preference):
                    unresolved = subprocess.run(["git", "merge-tree", "--write-tree", *parents], cwd=self.root, text=True, capture_output=True)
                    self.assertEqual(unresolved.returncode, 1)
                    resolved = self.repo.git("merge-tree", "--write-tree", f"-X{preference}", *parents).splitlines()[0]
                    self.assertNotEqual(resolved, tree)
                    lost = self.repo.git("commit-tree", tree, "-p", parents[0], "-p", parents[1], "-m", "Drop independent content")
                    with self.assertRaisesRegex(BIQ.BiqError, "discarded a parent"):
                        BIQ.check_candidate_history(self.root, "WI-X", base, lost)

    def test_seal_refuses_unsuccessful_conflict_preference_and_extra_parents(self):
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"u/shared": "base\n"}))
        base = self.repo.main()
        left = self.repo.commit_on(base, "WI-X", {"u/shared": "left\n"})
        right = self.repo.commit_on(base, "WI-X", {"u/shared": "right\n"})
        tree = self.repo.git("rev-parse", f"{left}^{{tree}}")
        merged = self.repo.git("commit-tree", tree, "-p", left, "-p", right, "-m", "Resolve conflict")
        run = subprocess.run
        for failure in (1, 2):
            def unsuccessful_preference(args, **kwargs):
                if "merge-tree" in args and "-Xours" in args:
                    return subprocess.CompletedProcess(args, failure, tree + "\n", "not resolved")
                return run(args, **kwargs)
            with self.subTest(exit_code=failure), mock.patch.object(BIQ.subprocess, "run", side_effect=unsuccessful_preference):
                with self.assertRaisesRegex(BIQ.BiqError, "discarded a parent"):
                    BIQ.check_candidate_history(self.root, "WI-X", base, merged)
        third = self.repo.commit_on(base, "WI-X", {"u/third": "must survive\n"})
        extra = self.repo.git("commit-tree", tree, "-p", left, "-p", right, "-p", third, "-m", "Three parents")
        with self.assertRaisesRegex(BIQ.BiqError, "discarded a parent"):
            BIQ.check_candidate_history(self.root, "WI-X", base, extra)

    def test_seal_refuses_a_candidate_that_deletes_packets_or_touches_other_agent_work(self):
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"docs/work-queue/items/WI-OTHER.md": packet_text("WI-OTHER"), "agent-work/WI-OTHER/evidence/log.txt": "theirs"}))
        self.repo.packet("WI-X"); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-X", "agent")
        self.repo.git("checkout", "-q", "--detach", self.repo.main()); self.repo.git("rm", "-q", "docs/work-queue/items/WI-OTHER.md"); self.repo.git("commit", "-q", "-m", "drop a packet")
        deleting = self.repo.git("rev-parse", "HEAD"); self.repo.git("checkout", "-q", "main")
        with self.assertRaisesRegex(BIQ.BiqError, "deletes 1 packet"):
            BIQ.seal_item(self.root, self.packets, "WI-X", deleting, self.repo.main(), "agent")
        foreign = self.repo.commit_on(self.repo.main(), "WI-X", {"agent-work/WI-OTHER/evidence/log.txt": "overwritten", "u/x": "x"})
        with self.assertRaisesRegex(BIQ.BiqError, "other items' agent-work: WI-OTHER"):
            BIQ.seal_item(self.root, self.packets, "WI-X", foreign, self.repo.main(), "agent")
        shared = self.repo.commit_on(self.repo.main(), "WI-X", {"agent-work/pilot/REPORT.md": "shared evidence\n", "u/x": "x"})
        BIQ.seal_item(self.root, self.packets, "WI-X", shared, self.repo.main(), "agent")
        own = self.repo.commit_on(self.repo.main(), "WI-X", {"agent-work/WI-X/evidence/log.txt": "mine", "u/x": "x"})
        BIQ.seal_item(self.root, self.packets, "WI-X", own, self.repo.main(), "agent")
        self.assertEqual(BIQ.footprint_line(BIQ.candidate_footprint(self.root, self.repo.main(), own)), "footprint: 2 paths (2 added, 0 modified, 0 deleted)")

    def test_a_declared_dependency_stands_for_its_replacement_at_seal_and_submission(self):
        # WI-DEP is rejected and restarted as WI-DEP-ATTEMPT-2, which submits its own candidate; WI-X declares
        # WI-DEP, as the contract says, and merges the restart's published candidate.
        self.submit("WI-DEP", {"u/dep": "dep"})
        source = packet_text("WI-DEP-ATTEMPT-2", replaces="WI-DEP").replace("- Dependencies: `none`", "- Previous attempt: `WI-DEP`\n- Dependencies: `none`", 1)
        (self.root / "docs/work-queue/items/WI-DEP-ATTEMPT-2.md").write_text(source); self.reload()
        BIQ.reject_item(self.root, self.packets, "WI-DEP", "wrong objective", "Human Owner", apply=True, successor="WI-DEP-ATTEMPT-2", registry=self.registry, owner="agent")
        self.repo.commit_queue(); self.reload()
        self.assertEqual(BIQ.resolve_replacement(self.packets, "WI-DEP"), "WI-DEP-ATTEMPT-2")
        restart = self.repo.commit_on(self.repo.main(), "WI-DEP-ATTEMPT-2", {"agent-work/WI-DEP-ATTEMPT-2/notes.md": "v2", "u/dep": "dep2"})
        BIQ.claim_item(self.root, self.packets, "WI-DEP-ATTEMPT-2", "other")
        BIQ.seal_item(self.root, self.packets, "WI-DEP-ATTEMPT-2", restart, self.repo.main(), "other")
        BIQ.submit_item(self.root, self.registry, self.packets, "WI-DEP-ATTEMPT-2", self.repo.main(), "other", apply=True)
        self.repo.commit_queue(); self.reload()
        merged = self.repo.commit_on(restart, "WI-X", {"u/x": "x"})
        stray = self.repo.commit_on(restart, "WI-Y", {"u/y": "y"})
        self.repo.packet("WI-X", dependencies="WI-DEP"); self.repo.packet("WI-Y"); self.repo.commit_queue(); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-X", "agent")
        packet = BIQ.seal_item(self.root, self.packets, "WI-X", merged, self.repo.main(), "agent")
        self.assertEqual(packet.candidate, merged)
        self.assertIsNone(BIQ.member_diagnostics(self.root, self.packets, "WI-X", merged, self.repo.main())["problem"])
        outcome = BIQ.submit_item(self.root, self.registry, self.packets, "WI-X", self.repo.main(), "agent", apply=False)
        self.assertEqual(outcome["implied"], ["WI-DEP-ATTEMPT-2"])
        # An undeclared restart is still foreign work.
        BIQ.claim_item(self.root, self.packets, "WI-Y", "agent")
        with self.assertRaisesRegex(BIQ.BiqError, r"WI-DEP-ATTEMPT-2 \(not in Dependencies\)|carries the sealed candidate of WI-DEP-ATTEMPT-2"):
            BIQ.seal_item(self.root, self.packets, "WI-Y", stray, self.repo.main(), "agent")

    def test_seal_names_the_declared_dependency_that_carried_undeclared_work(self):
        # WI-B declares and carries WI-A; WI-C declares only WI-B and so carries WI-A's evidence through it.
        a = self.submit("WI-A", {"agent-work/WI-A/notes.md": "a", "u/a": "a"})
        b = self.submit("WI-B", {"u/b": "b"}, start=a, dependencies="WI-A")
        c = self.repo.commit_on(b, "WI-C", {"u/c": "c"})
        self.repo.packet("WI-C", dependencies="WI-B"); self.repo.commit_queue(); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-C", "agent")
        with self.assertRaisesRegex(BIQ.BiqError, rf"WI-A \(not in Dependencies; carried unchanged inside WI-B's published candidate {b[:12]}, so name WI-A in Dependencies as well\)"):
            BIQ.seal_item(self.root, self.packets, "WI-C", c, self.repo.main(), "agent")
        self.repo.packet("WI-C", dependencies="WI-A, WI-B"); self.repo.commit_queue(); self.reload()  # the declaration is the remedy
        self.assertEqual(BIQ.seal_item(self.root, self.packets, "WI-C", c, self.repo.main(), "agent").candidate, c)

        # WI-DEP is sealed and submitted with retained work under its own agent-work root; nothing of it is on main.
        dep = self.submit("WI-DEP", {"agent-work/WI-DEP/patches/0001.patch": "dep", "u/dep": "dep"})
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"docs/work-queue/items/WI-OPEN.md": packet_text("WI-OPEN")}))
        inherited = self.repo.commit_on(dep, "WI-X", {"u/x": "x"})
        # Undeclared: the same inherited content is refused until WI-X names WI-DEP in Dependencies.
        self.repo.packet("WI-X"); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-X", "agent")
        with self.assertRaisesRegex(BIQ.BiqError, r"other items' agent-work: WI-DEP \(not in Dependencies\)"):
            BIQ.seal_item(self.root, self.packets, "WI-X", inherited, self.repo.main(), "agent")
        # Declared, published, merged, unchanged: accepted at seal, at review, and at submission (as an implied dependency).
        self.repo.packet("WI-X", dependencies="WI-DEP"); self.reload()
        packet = BIQ.seal_item(self.root, self.packets, "WI-X", inherited, self.repo.main(), "agent")
        self.assertEqual(packet.candidate, inherited)
        self.assertIsNone(BIQ.member_diagnostics(self.root, self.packets, "WI-X", inherited, self.repo.main())["problem"])
        outcome = BIQ.submit_item(self.root, self.registry, self.packets, "WI-X", self.repo.main(), "agent", apply=False)
        self.assertEqual(outcome["implied"], ["WI-DEP"])
        # Additional edits inside the dependency's root are not the dependency's work.
        edited = self.repo.commit_on(dep, "WI-X", {"agent-work/WI-DEP/patches/0001.patch": "tampered", "u/x": "x"})
        with self.assertRaisesRegex(BIQ.BiqError, r"WI-DEP \(edited beyond published candidate " + dep[:12] + r"\)"):
            BIQ.seal_item(self.root, self.packets, "WI-X", edited, self.repo.main(), "agent")
        # A declared dependency without a published candidate, and one declared but not merged, are foreign work.
        unpublished = self.repo.commit_on(self.repo.main(), "WI-X", {"agent-work/WI-OPEN/notes.md": "theirs", "u/x": "x"})
        self.repo.packet("WI-X", dependencies="WI-OPEN"); self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, r"WI-OPEN \(no published candidate\)"):
            BIQ.seal_item(self.root, self.packets, "WI-X", unpublished, self.repo.main(), "agent")
        copied = self.repo.commit_on(self.repo.main(), "WI-X", {"agent-work/WI-DEP/patches/0001.patch": "dep", "u/x": "x"})
        self.repo.packet("WI-X", dependencies="WI-DEP"); self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, r"WI-DEP \(published candidate " + dep[:12] + r" is not merged into the candidate\)"):
            BIQ.seal_item(self.root, self.packets, "WI-X", copied, self.repo.main(), "agent")
        # The integrator sees the same verdict the seal guard gives.
        self.assertIn("edited beyond published candidate", BIQ.member_diagnostics(self.root, self.packets, "WI-X", edited, self.repo.main())["problem"])

    def test_seal_allows_preserved_run_rename_but_refuses_lost_or_frozen_records(self):
        old = BIQ.open_run(self.packets, "U")
        old_id = "WI-CI-INTEGRATION-BIQ-U-REPEAT-1"
        old_path = f"{BIQ.ITEMS_DIR}/{old_id}.md"
        new_id = "WI-CI-INTEGRATION-U-REPEAT-1"
        new_path = f"{BIQ.ITEMS_DIR}/{new_id}.md"
        source = old.source.replace(old.item_id, old_id).replace("## Members\n\nnone", "## Members\n\n- WI-KEPT @" + "a" * 40)
        renamed = source.replace(old_id, new_id)
        frozen = source.replace("Started At: `pending`", "Started At: `2026-09-07T00:00:00Z`")
        cases = (
            ("preserved", source, renamed, True),
            ("lost member", source, renamed.replace("- WI-KEPT @" + "a" * 40, "none"), False),
            ("changed member", source, renamed.replace("a" * 40, "b" * 40), False),
            ("changed state", source, renamed.replace("Status: `open`", "Status: `completed`"), False),
            ("frozen run", frozen, frozen.replace(old_id, new_id), False),
            ("mismatched queue", source, renamed.replace("Queue: `U`", "Queue: `K`"), False),
        )
        if old.item_id != old_id:
            self.repo.git("rm", f"{BIQ.ITEMS_DIR}/{old.item_id}.md")
            self.repo.git("commit", "-q", "-m", "prepare historical run fixture")
        for label, before, after, admitted in cases:
            with self.subTest(label=label):
                base = self.repo.commit_on(self.repo.main(), None, {old_path: before})
                self.repo.git("checkout", "-q", "--detach", base)
                self.repo.git("rm", "-q", old_path)
                (self.root / new_path).write_text(after)
                self.repo.git("add", new_path)
                self.repo.git("commit", "-q", "-m", "rename run")
                candidate = self.repo.git("rev-parse", "HEAD")
                self.repo.git("checkout", "-q", "main")
                if admitted:
                    BIQ.check_candidate_history(self.root, "WI-X", base, candidate)
                else:
                    with self.assertRaisesRegex(BIQ.BiqError, "deletes 1 packet"):
                        BIQ.check_candidate_history(self.root, "WI-X", base, candidate)

    def test_priority_holds_preserve_status_and_compose_membership(self):
        def packet(item_id, **fields):
            p = BIQ.parse_packet(item_id, packet_text(item_id))
            p.fields.update(fields)
            return p
        parent = packet("WI-STREAM", **{"Container mode": "project", "Container members": "WI-GROUP, WI-RECUR-REPEAT-1", "Priority": "on-hold"})
        group = packet("WI-GROUP", **{"Container mode": "operative", "Container members": "WI-CHILD"})
        child = packet("WI-CHILD", Priority="on-hold", Dependencies="WI-PREREQ")
        repeat = packet("WI-RECUR-REPEAT-1")
        future = packet("WI-RECUR-REPEAT-2")
        other = packet("WI-OTHER", Dependencies="WI-CHILD")
        packets = {p.item_id: p for p in (parent, group, child, repeat, future, other)}
        self.assertEqual(BIQ.priority_hold_sources(child.item_id, packets), ("WI-CHILD", "WI-STREAM"))
        self.assertEqual(BIQ.priority_hold_sources(future.item_id, packets), ("WI-STREAM",))
        self.assertEqual(BIQ.priority_hold_sources(other.item_id, packets), ())
        ordinary = BIQ.render_list(packets, "open")
        self.assertNotIn("WI-CHILD", ordinary)
        self.assertIn("WI-OTHER", ordinary)
        self.assertIn("open\tWI-CHILD\tOn Hold via WI-CHILD, WI-STREAM", BIQ.render_list(packets, "open", "on-hold"))
        self.assertIn("WI-CHILD", BIQ.render_list(packets, None, "all"))
        parent.fields["Priority"] = "production"
        self.assertEqual(BIQ.priority_hold_sources(child.item_id, packets), ("WI-CHILD",))
        child.fields["Priority"] = "production"
        self.assertEqual(BIQ.priority_hold_sources(child.item_id, packets), ())
        self.assertEqual(child.fields["Dependencies"], "WI-PREREQ")
        self.assertEqual(child.status, "open")
        parent.fields["Priority"] = "on-hold"
        child.fields["Status"] = "completed"
        self.assertEqual(BIQ.priority_hold_sources(child.item_id, packets), ())

    def test_fetched_hold_blocks_stale_claim_and_preserves_existing_claim(self):
        self.repo.packet("WI-HELD")
        self.reload()
        existing = BIQ.claim_item(self.root, self.packets, "WI-HELD", "agent")
        path = self.root / "docs/work-queue/items/WI-HELD.md"
        path.write_text(path.read_text().replace("## Objective", "- Priority: `on-hold`\n\n## Objective"))
        self.repo.commit_queue()
        # Both a stale in-memory packet and a local resume must lose to main.
        for owner in ("agent", "other"):
            with self.assertRaisesRegex(BIQ.BiqError, "On Hold via WI-HELD"):
                BIQ.claim_item(self.root, self.packets, "WI-HELD", owner)
        with self.assertRaisesRegex(BIQ.BiqError, "On Hold"):
            BIQ.seal_item(self.root, self.packets, "WI-HELD", "HEAD", self.repo.main(), "agent")
        with self.assertRaisesRegex(BIQ.BiqError, "On Hold"):
            BIQ.submit_item(self.root, self.registry, self.packets, "WI-HELD", self.repo.main(), "agent", apply=True)
        oid, claim = BIQ.read_claim(self.root, "WI-HELD")
        self.assertEqual(oid, existing["claim_oid"])
        self.assertEqual(claim["owner"], "agent")
        path.write_text(path.read_text().replace("`on-hold`", "`production`"))
        self.repo.commit_queue()
        self.reload()
        self.assertEqual(BIQ.claim_item(self.root, self.packets, "WI-HELD", "agent")["owner"], "agent")

    def test_inherited_hold_refuses_before_creating_any_claim(self):
        self.repo.packet("WI-CHILD")
        self.repo.packet("WI-STREAM")
        path = self.root / "docs/work-queue/items/WI-STREAM.md"
        path.write_text(path.read_text().replace("## Objective", "- Priority: `on-hold`\n- Container mode: `project`\n- Container members: `WI-CHILD`\n\n## Objective"))
        self.repo.commit_queue()
        self.reload()
        with mock.patch.object(BIQ, "push_claim") as push:
            with self.assertRaisesRegex(BIQ.BiqError, "On Hold via WI-STREAM"):
                BIQ.claim_item(self.root, self.packets, "WI-CHILD", "agent")
            push.assert_not_called()
        self.assertEqual(BIQ.read_claim(self.root, "WI-CHILD"), (None, None))
        self.packets["WI-STREAM"].fields["Priority"] = "invalid"
        self.assertTrue(any("Priority must be" in error for error in BIQ.lint(self.registry, self.packets)))

    def test_claim_names_the_packet_as_main_holds_it(self):
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"docs/work-queue/items/WI-NAMED.md": packet_text("WI-NAMED").replace("# WI-NAMED: ", "# WI-NAMED: renamed on main, ", 1)}))
        # The owner's branch still carries the draft title; the claim takes main's.
        self.repo.git("checkout", "-q", "--detach", self.repo.main())
        (self.root / "docs/work-queue/items/WI-NAMED.md").write_text(packet_text("WI-NAMED").replace("# WI-NAMED: ", "# WI-NAMED: draft title, ", 1))
        self.reload()
        claim = BIQ.claim_item(self.root, self.packets, "WI-NAMED", "agent")
        self.assertTrue(claim["title"].startswith("renamed on main"), claim["title"])
        self.assertIn(self.repo.git("rev-parse", f"{self.repo.main()}:docs/work-queue/items/WI-NAMED.md"), claim["item_identity"])
        self.repo.git("checkout", "-q", "--", "docs/work-queue/items/WI-NAMED.md")

    def test_no_member_is_tested_before_the_integrator_reviews_it(self):
        self.submit("WI-R1", {"u/r1": "1"}); self.submit("WI-R2", {"u/r2": "2"})
        self.start("U")
        run = BIQ.find_run(self.packets, "U", 1)
        with self.assertRaisesRegex(BIQ.BiqError, "not yet reviewed: WI-R1, WI-R2"):
            BIQ.record_test(self.root, self.packets, run, next(iter(run.tests)), "green", None, 0.0, owner="runner")
        lines = BIQ.review_lines(self.root, self.packets, run, self.repo.main())
        self.assertTrue(any(line.startswith("WI-R1 @") and "NOT REVIEWED" in line and "footprint: 1 paths" in line for line in lines), lines)
        BIQ.review_member(self.root, self.registry, self.packets, run, "WI-R1", owner="runner", ok=True, reject=None, hold=None, apply=True)
        run = BIQ.find_run(self.packets, "U", 1)
        self.assertTrue(BIQ.reviewed_ok(next(m[2] for m in run.members() if m[0] == "WI-R1")))
        with self.assertRaisesRegex(BIQ.BiqError, "not yet reviewed: WI-R2"):
            BIQ.record_test(self.root, self.packets, run, next(iter(run.tests)), "green", None, 0.0, owner="runner")
        with self.assertRaisesRegex(BIQ.BiqError, "not a member"):
            BIQ.review_member(self.root, self.registry, self.packets, run, "WI-NOPE", owner="runner", ok=True, reject=None, hold=None, apply=True)
        with self.assertRaisesRegex(BIQ.BiqError, "exactly one of"):
            BIQ.review_member(self.root, self.registry, self.packets, run, "WI-R2", owner="runner", ok=True, reject="x", hold=None, apply=True)
        self.green("U", 1)
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])

    def test_member_review_fetches_the_candidate_before_inspection(self):
        order = []
        with mock.patch.object(BIQ, "ensure_commit", side_effect=lambda root, sha: order.append((root, sha))):
            with mock.patch.object(BIQ, "git", side_effect=lambda root, *args, **kwargs: "b" * 40 if order else self.fail("merge-base ran before ensure_commit")):
                with mock.patch.object(BIQ, "candidate_footprint", return_value={"paths": [], "added": [], "modified": [], "deleted": []}):
                    with mock.patch.object(BIQ, "check_candidate_history"):
                        result = BIQ.member_diagnostics(self.root, self.packets, "WI-X", "a" * 40, "c" * 40)
        self.assertEqual(order, [(self.root, "a" * 40)])
        self.assertEqual(result["base"], "b" * 40)

    def check_rebuild_conflict_custody(self, mode):
        base = self.repo.main()
        self.repo.land(self.repo.commit_on(base, None, {"u/shared": "main version\n"}))
        stale = self.submit("WI-STALE", {"u/shared": "branch version\n"}, start=base)
        self.submit("WI-DROP", {"u/drop": "drop"})
        for item in ("WI-DEP1", "WI-DEP2"):
            self.submit(item, {f"u/{item}": "dependent"}, dependencies="WI-DROP")
        self.submit("WI-FINE", {"u/fine": "fine"})
        self.start("U")
        for attempt in range(2):
            run = BIQ.find_run(self.packets, "U", 1)
            self.assertEqual(set(BIQ.pending_resolutions(run)), {"WI-STALE"})
            self.assertNotIn("WI-STALE", [m[0] for m in BIQ.open_run(self.packets, "U").members()])
            with self.assertRaisesRegex(BIQ.BiqError, "awaiting resolution"):
                BIQ.record_test(self.root, self.packets, run, "u-lane", "green", None, 0.0, owner="runner")
            with self.assertRaisesRegex(BIQ.BiqError, "awaiting resolution"):
                BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(), reject=[], apply=True, owner="runner")
            self.repo.git("checkout", "-q", "--detach", run.fields["Integration commit"])
            merge = subprocess.run(["git", "merge", "-q", stale], cwd=self.root, capture_output=True)
            self.assertNotEqual(merge.returncode, 0)
            (self.root / "u/shared").write_text("resolved\n")
            self.repo.git("add", "u/shared"); self.repo.git("commit", "-q", "-m", "resolve WI-STALE")
            resolved = self.repo.git("rev-parse", "HEAD"); self.repo.git("checkout", "-q", "main")
            BIQ.resolve_member(self.root, self.registry, self.packets, run, "WI-STALE", resolved, owner="runner", apply=True)
            self.review_all("U", 1)
            run = BIQ.find_run(self.packets, "U", 1)
            self.assertEqual(BIQ.pending_resolutions(run), {})
            self.assertEqual(run.tests, {"u-lane": "pending"})
            self.assertEqual(run.sections["Evidence"].strip(), "none")
            if attempt:
                break
            receipt = Path(self.tmp.name) / "first-run.log"
            receipt.write_text("red test; synthetic cost $1.25\n")
            run = BIQ.record_test(self.root, self.packets, run, "u-lane", "red", str(receipt), 1.25, owner="runner")
            if mode == "complete":
                BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(),
                                 reject=["WI-DROP"], apply=True, authorized_by="Human Owner", owner="runner")
            else:
                BIQ.review_member(self.root, self.registry, self.packets, run, "WI-DROP", owner="runner", ok=False,
                                  reject="unrelated defect" if mode == "reject" else None,
                                  hold="intended?" if mode == "hold" else None,
                                  apply=True, authorized_by="Human Owner")
            run = BIQ.find_run(self.packets, "U", 1)
            self.assertEqual([m[0] for m in run.members()], ["WI-STALE", "WI-FINE"])
            self.assertTrue(BIQ.reviewed_ok(next(note for item, _sha, note in run.members() if item == "WI-FINE")))
            self.assertNotIn("WI-STALE", BIQ.excluded_members(run))
            expected = ["WI-DEP1", "WI-DEP2"] + (["WI-DROP"] if mode == "hold" else [])
            self.assertEqual(sorted(m[0] for m in BIQ.open_run(self.packets, "U").members()), expected)
            self.assertEqual(self.packets["WI-DROP"].status, "submitted" if mode == "hold" else "failed-rejected")
            self.assertEqual(run.tests, {"u-lane": "pending"})
            self.assertEqual(run.sections["Evidence"].strip(), "none")
            self.assertEqual(run.fields["Cost"], "$1.25")
            self.assertEqual(receipt.read_text(), "red test; synthetic cost $1.25\n")
            self.repo.commit_queue()
        self.assertEqual(run.fields["Cost"], "$1.25")
        self.assertEqual(receipt.read_text(), "red test; synthetic cost $1.25\n")
        self.assertEqual(self.green("U", 1)["passed_to"], "AGG")
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])

    def test_review_hold_retains_rebuild_conflicts(self):
        self.check_rebuild_conflict_custody("hold")

    def test_review_rejection_retains_rebuild_conflicts(self):
        self.check_rebuild_conflict_custody("reject")

    def test_red_test_rejection_retains_rebuild_conflicts(self):
        self.check_rebuild_conflict_custody("complete")

    def test_review_rebuild_clears_a_conflict_that_no_longer_exists(self):
        base = self.repo.main()
        self.submit("WI-A", {"u/shared": "a\n"})
        candidate = self.submit("WI-B", {"u/shared": "b\n"}, start=base)
        self.start("U"); self.review_all("U", 1)
        run = BIQ.find_run(self.packets, "U", 1)
        self.assertEqual(set(BIQ.pending_resolutions(run)), {"WI-B"})
        BIQ.review_member(self.root, self.registry, self.packets, run, "WI-A", owner="runner", ok=False,
                          reject=None, hold="intended?", apply=True)
        run = BIQ.find_run(self.packets, "U", 1)
        self.assertEqual(BIQ.pending_resolutions(run), {})
        self.assertEqual([m[0] for m in run.members()], ["WI-B"])
        self.assertTrue(BIQ.reviewed_ok(run.members()[0][2]))
        self.assertTrue(BIQ.is_ancestor(self.root, candidate, run.fields["Integration commit"]))
        self.assertEqual(self.green("U", 1)["passed_to"], "AGG")

    def test_a_rejected_member_leaves_the_run_with_its_dependents(self):
        self.submit("WI-BAD", {"u/bad": "1"}); self.submit("WI-DEP", {"u/dep": "2"}, dependencies="WI-BAD"); self.submit("WI-FINE", {"u/fine": "3"})
        self.start("U")
        run = BIQ.find_run(self.packets, "U", 1)
        before = run.fields["Integration commit"]
        with self.assertRaisesRegex(BIQ.BiqError, "Accountable owner's word"):
            BIQ.review_member(self.root, self.registry, self.packets, run, "WI-BAD", owner="runner", ok=False, reject="reverts main's changes to u/", hold=None, apply=True)
        outcome = BIQ.review_member(self.root, self.registry, self.packets, run, "WI-BAD", owner="runner", ok=False, reject="reverts main's changes to u/", hold=None, apply=True, authorized_by="Human Owner")
        self.assertEqual((outcome["shed"], outcome["dependents"], outcome["kept"]), (["WI-BAD"], ["WI-DEP"], ["WI-FINE"]))
        run = BIQ.find_run(self.packets, "U", 1)
        self.assertEqual([m[0] for m in run.members()], ["WI-FINE"])
        self.assertNotEqual(run.fields["Integration commit"], before)
        excluded = BIQ.excluded_members(run)
        self.assertIn("rejected at review by runner on the word of Human Owner: reverts main's changes to u/", excluded["WI-BAD"])
        self.assertIn("depends on WI-BAD; returned to WI-CI-INTEGRATION-U-REPEAT-2", excluded["WI-DEP"])
        self.assertEqual(self.packets["WI-BAD"].status, "failed-rejected")
        self.assertIn("at member review: reverts main's changes to u/", self.packets["WI-BAD"].sections["Resolution"])
        self.assertEqual([m[0] for m in BIQ.open_run(self.packets, "U").members()], ["WI-DEP"])
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])

    def test_a_held_member_returns_to_the_open_run_and_names_the_human(self):
        self.submit("WI-ODD", {"u/odd": "1"}); self.submit("WI-FINE", {"u/fine": "3"})
        self.start("U")
        run = BIQ.find_run(self.packets, "U", 1)
        outcome = BIQ.review_member(self.root, self.registry, self.packets, run, "WI-ODD", owner="runner", ok=False, reject=None, hold="deletes a fixture nobody mentioned; intended?", apply=True)
        self.assertEqual(outcome["verdict"], "hold")
        run = BIQ.find_run(self.packets, "U", 1)
        self.assertEqual([m[0] for m in run.members()], ["WI-FINE"])
        self.assertIn("held for", BIQ.excluded_members(run)["WI-ODD"])
        self.assertIn("intended?", BIQ.excluded_members(run)["WI-ODD"])
        self.assertEqual(self.packets["WI-ODD"].status, "submitted")
        self.assertEqual(self.packets["WI-ODD"].fields["Decision owner"], run.fields["Accountable owner"])
        self.assertEqual([m[0] for m in BIQ.open_run(self.packets, "U").members()], ["WI-ODD"])
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])

    def test_a_cared_leaf_lands_directly_only_on_the_accountable_humans_word(self):
        sha = self.submit("WI-L", {"u/l": "1"})
        self.assertEqual([m[0] for m in BIQ.open_run(self.packets, "U").members()], ["WI-L"])
        with self.assertRaisesRegex(BIQ.BiqError, "cared about by U; it lands inside a run, or directly on the accountable human's word"):
            BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-L", self.repo.main(), apply=True)
        outcome = BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-L", self.repo.main(), apply=True, authorized_by="Human Owner")
        self.assertIn("on the word of Human Owner without its queues' runs (U)", outcome["reason"])
        head = outcome["head"]
        landed = BIQ.load_packets(self.root, head)
        self.assertEqual(landed["WI-L"].status, "completed")
        self.assertIn("on the word of Human Owner without its queues' runs (U)", landed["WI-L"].sections["Resolution"])
        run = BIQ.open_run(landed, "U")
        self.assertEqual(run.members(), [])
        self.assertIn("landed directly on the word of Human Owner", BIQ.excluded_members(run)["WI-L"])
        # The gate reads the word from the landing tree's packet and admits the edge.
        self.assertIn("on the word of Human Owner", BIQ.verify_landing(self.root, self.repo.main(), head))
        self.assertTrue(BIQ.is_ancestor(self.root, sha, head))

    def test_claim_refuses_a_malformed_usage_snapshot(self):
        self.repo.packet("WI-U"); self.reload()
        bad = self.root / "bad-usage.json"
        bad.write_text(json.dumps({"schema": 1, "provider": "codex", "source_id": "s", "observed_at": "2026-09-07T00:00:00Z",
                                   "counters": {k: 0 for k in BIQ.USAGE_COUNTER_FIELDS}, "models": {"gpt": {k: 0 for k in BIQ.USAGE_COUNTER_FIELDS}}}))
        with self.assertRaisesRegex(BIQ.BiqError, "model gpt must carry exactly these counters: .*long_output_tokens"):
            BIQ.claim_item(self.root, self.packets, "WI-U", "agent", usage_snapshot=str(bad))
        self.assertIsNone(BIQ.read_claim(self.root, "WI-U")[1])
        good = self.root / "good-usage.json"
        good.write_text(json.dumps({"schema": 1, "provider": "anthropic", "source_id": "s", "observed_at": "2026-09-07T00:00:00Z",
                                    "counters": {k: 0 for k in BIQ.USAGE_COUNTER_FIELDS}, "models": {"m": {k: 0 for k in BIQ.USAGE_MODEL_COUNTER_FIELDS}}}))
        claim = BIQ.claim_item(self.root, self.packets, "WI-U", "agent", usage_snapshot=str(good))
        self.assertEqual(claim["usage_baseline"]["provider"], "anthropic")

    def test_the_control_clone_is_one_per_owner_and_a_staged_write_survives_another_owners_command(self):
        with mock.patch.dict(os.environ, {"BIQ_CONTROL_DIR": str(self.root.parent / "control")}):
            a = BIQ.control_clone(self.root, "origin/main", "agent a")
            b = BIQ.control_clone(self.root, "origin/main", "agent-b")
            self.assertNotEqual(a, b)
            self.assertEqual((a.parent, a.name.split("-", 1)[1], b.name.split("-", 1)[1]), (self.root.parent / "control", "agent-a", "agent-b"))
            self.assertEqual(a.name.split("-", 1)[0], b.name.split("-", 1)[0])
            staged = a / BIQ.ITEMS_DIR / "WI-STAGED.md"
            staged.write_text(packet_text("WI-STAGED"))  # a's write, staged but not yet committed
            self.assertEqual(BIQ.control_clone(self.root, "origin/main", "agent-b"), b)  # b's command resets b's clone only
            self.assertTrue(staged.is_file())
            self.assertEqual(BIQ.clone_owner(self.root, None), self.repo.git("config", "user.name"))
            self.assertEqual(BIQ.clone_owner(self.root, "x"), "x")

    def test_a_stale_checkout_reads_main_and_writes_through_the_control_clone(self):
        self.repo.packet("WI-S"); self.repo.commit_queue(); self.reload()
        candidate = self.repo.commit_on(self.repo.main(), "WI-S", {"u/s": "s"})
        older = self.repo.git("rev-list", "--max-count=1", "--skip=1", "main")
        self.repo.git("checkout", "-q", "--detach", older)
        env = {**os.environ, "BIQ_CONTROL_DIR": str(self.root.parent / "control"), "PATH": self.git_only_path(), "BIQ_CHANGE_PROVIDER": "github"}
        base = [sys.executable, str(SCRIPT), "--repository", str(self.root), "--target-ref", "main"]
        out = subprocess.run([*base, "show", "WI-S"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("queue state read from main", out.stderr)
        subprocess.run([*base, "claim", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        out = subprocess.run([*base, "seal", "WI-S", "--owner", "a", "--candidate", candidate], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, BIQ.PENDING_EXIT, out.stdout + out.stderr)  # no gh: pushed, not landed, and says so
        self.assertIn("writing queue state in the control clone", out.stderr)
        self.assertIn("pushed to control/a/wi-s", out.stdout)
        self.assertIn("control/a/wi-s PENDING: gh is not available: open the gated pull request from control/a/wi-s", out.stderr)
        # The agent's own checkout is untouched; the sealed packet is on the pushed control branch.
        self.assertEqual(self.repo.git("status", "--porcelain"), "")
        self.assertEqual(self.repo.git("rev-parse", "HEAD"), older)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/wi-s:refs/remotes/origin/control/a/wi-s")
        sealed = BIQ.load_packets(self.root, "origin/control/a/wi-s")["WI-S"]
        self.assertEqual(sealed.candidate, candidate)
        self.assertTrue(BIQ.is_ancestor(self.root, "main", "origin/control/a/wi-s"))
        # A different write cannot stack on the pending seal. It follows the
        # pending operation and tells the caller to rerun without mutating it.
        out = subprocess.run([*base, "submit", "WI-S", "--owner", "a", "--apply"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, BIQ.PENDING_EXIT, out.stdout + out.stderr)
        self.assertIn("carries seal WI-S; rerun submit WI-S after it lands", out.stderr)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/wi-s:refs/remotes/origin/control/a/wi-s")
        self.assertEqual(BIQ.load_packets(self.root, "origin/control/a/wi-s")["WI-S"].status, "open")
        self.assertEqual(self.repo.git("rev-list", "--count", "main..origin/control/a/wi-s"), "1")
        self.assertIn("BIQ-Operation: sha256:", self.repo.git("show", "-s", "--format=%B", "origin/control/a/wi-s"))
        self.repo.git("checkout", "-q", "main")

    def test_an_identical_pending_workstream_join_only_follows_its_first_commit(self):
        self.repo.packet("WI-S")
        (self.root / "docs/work-queue/items/WI-STREAM.md").write_text(workstream_text("WI-STREAM", "none"))
        self.repo.commit_queue()
        env = {**os.environ, "BIQ_CONTROL_DIR": str(self.root.parent / "control"), "PATH": self.git_only_path(), "BIQ_CHANGE_PROVIDER": "github"}
        base = [sys.executable, str(SCRIPT), "--repository", str(self.root), "--target-ref", "main"]
        command = [*base, "workstream", "WI-S", "--join", "WI-STREAM", "--owner", "a", "--apply"]
        first = subprocess.run(command, text=True, capture_output=True, env=env)
        self.assertEqual(first.returncode, BIQ.PENDING_EXIT, first.stdout + first.stderr)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/wi-s:refs/remotes/origin/control/a/wi-s")
        first_tip = self.repo.git("rev-parse", "origin/control/a/wi-s")
        second = subprocess.run(command, text=True, capture_output=True, env=env)
        self.assertEqual(second.returncode, BIQ.PENDING_EXIT, second.stdout + second.stderr)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/wi-s:refs/remotes/origin/control/a/wi-s")
        self.assertEqual(self.repo.git("rev-parse", "origin/control/a/wi-s"), first_tip)
        self.assertEqual(BIQ.container_members(BIQ.load_packets(self.root, "origin/control/a/wi-s")["WI-STREAM"]), ["WI-S"])

    # -- control landings are followed to the target ---------------------------------
    def test_change_provider_uses_origin_or_the_one_explicit_override(self):
        original = self.repo.git("remote", "get-url", "origin")
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("BIQ_CHANGE_PROVIDER", None)
            self.repo.git("remote", "set-url", "origin", "ssh://git@ssh.github.com:443/group/project.git")
            self.assertEqual(BIQ.change_provider(self.root), "github")
            self.repo.git("remote", "set-url", "origin", "ssh://git@gitlab.lab.example:2222/group/project.git")
            self.assertEqual(BIQ.change_provider(self.root), "gitlab")
            self.repo.git("remote", "set-url", "origin", original)
            with self.assertRaisesRegex(BIQ.BiqError, "cannot detect GitHub or GitLab"):
                BIQ.change_provider(self.root)
            os.environ["BIQ_CHANGE_PROVIDER"] = "gitlab"
            self.assertEqual(BIQ.change_provider(self.root), "gitlab")

    def test_gitlab_pending_retry_reuses_the_mr_then_lands_and_removes_its_source(self):
        candidate, base = self._stale()
        env = fake_glab_env(self.root.parent, self.root.parent / "origin.git", mode="pending", wait="0")
        subprocess.run([*base, "claim", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        command = [*base, "seal", "WI-S", "--owner", "a", "--candidate", candidate]
        first = subprocess.run(command, text=True, capture_output=True, env=env)
        self.assertEqual(first.returncode, BIQ.PENDING_EXIT, first.stdout + first.stderr)
        self.assertIn("merge_requests/1", first.stdout + first.stderr)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/wi-s:refs/remotes/origin/control/a/wi-s")
        tip = self.repo.git("rev-parse", "origin/control/a/wi-s")
        second = subprocess.run(command, text=True, capture_output=True, env=env)
        self.assertEqual(second.returncode, BIQ.PENDING_EXIT, second.stdout + second.stderr)
        self.assertEqual(len(json.loads((self.root.parent / "glab.json").read_text())["prs"]), 1)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/wi-s:refs/remotes/origin/control/a/wi-s")
        self.assertEqual(self.repo.git("rev-parse", "origin/control/a/wi-s"), tip)
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "landed", "30"
        landed = subprocess.run(command, text=True, capture_output=True, env=env)
        self.assertEqual(landed.returncode, 0, landed.stdout + landed.stderr)
        self.assertEqual(self.repo.git("ls-remote", "origin", "refs/heads/control/a/wi-s"), "")
        self.repo.git("fetch", "-q", "origin", "main")
        self.assertEqual(BIQ.load_packets(self.root, "origin/main")["WI-S"].candidate, candidate)

    def test_gitlab_transient_auto_merge_race_is_retried_in_one_command(self):
        candidate, base = self._stale()
        env = fake_glab_env(self.root.parent, self.root.parent / "origin.git", mode="arm-race")
        subprocess.run([*base, "claim", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        out = subprocess.run([*base, "seal", "WI-S", "--owner", "a", "--candidate", candidate], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        state = json.loads((self.root.parent / "glab.json").read_text())
        self.assertEqual(len(state["prs"]), 1)
        self.assertEqual(state["prs"][0]["arm_attempts"], 2)

    def test_gitlab_failed_and_conflicting_states_normalize_to_the_existing_contract(self):
        failed = subprocess.CompletedProcess([], 0, json.dumps({"state": "opened", "detailed_merge_status": "mergeable",
                                                                "head_pipeline": {"status": "failed"}}), "")
        conflict = subprocess.CompletedProcess([], 0, json.dumps({"state": "opened", "detailed_merge_status": "conflict",
                                                                  "has_conflicts": True, "head_pipeline": {"status": "running"}}), "")
        with mock.patch.object(BIQ, "change_cli", return_value=failed):
            self.assertEqual(BIQ.change_request_view(self.root, "1", "gitlab")["checks"], ["FAILURE"])
        with mock.patch.object(BIQ, "change_cli", return_value=conflict):
            self.assertEqual(BIQ.change_request_view(self.root, "1", "gitlab")["merge"], "DIRTY")

    def test_gitlab_failed_pipeline_returns_gate_failed(self):
        candidate, base = self._stale()
        env = fake_glab_env(self.root.parent, self.root.parent / "origin.git", mode="failed")
        subprocess.run([*base, "claim", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        out = subprocess.run([*base, "seal", "WI-S", "--owner", "a", "--candidate", candidate], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 2, out.stdout + out.stderr)
        self.assertIn("GATE-FAILED (change request https://gitlab.example/group/project/-/merge_requests/1)", out.stderr)

    def test_gitlab_without_glab_names_the_manual_merge_request_step(self):
        candidate, base = self._stale()
        env = {**os.environ, "BIQ_CONTROL_DIR": str(self.root.parent / "control"), "PATH": self.git_only_path(), "BIQ_CHANGE_PROVIDER": "gitlab"}
        subprocess.run([*base, "claim", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        out = subprocess.run([*base, "seal", "WI-S", "--owner", "a", "--candidate", candidate], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, BIQ.PENDING_EXIT, out.stdout + out.stderr)
        self.assertIn("glab is not available: open the gated merge request", out.stderr)

    def _stale(self):
        """A packet, a sealed candidate for it, and the agent's checkout detached one commit behind main."""
        self.repo.packet("WI-S"); self.repo.commit_queue(); self.reload()
        candidate = self.repo.commit_on(self.repo.main(), "WI-S", {"docs/s": "s"})  # no queue cares: it lands directly after submission
        older = self.repo.git("rev-list", "--max-count=1", "--skip=1", "main")
        self.repo.git("checkout", "-q", "--detach", older)
        return candidate, [sys.executable, str(SCRIPT), "--repository", str(self.root), "--target-ref", "main"]

    def test_a_control_write_is_reported_done_only_when_main_holds_it(self):
        candidate, base = self._stale()
        env = fake_gh_env(self.root.parent, self.root.parent / "origin.git")
        subprocess.run([*base, "claim", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        out = subprocess.run([*base, "seal", "WI-S", "--owner", "a", "--candidate", candidate], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("control/a/wi-s landed on origin/main at", out.stdout)
        self.repo.git("fetch", "-q", "origin", "main")
        self.assertEqual(BIQ.load_packets(self.root, "origin/main")["WI-S"].candidate, candidate)
        out = subprocess.run([*base, "submit", "WI-S", "--owner", "a", "--apply"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.repo.git("fetch", "-q", "origin", "main")
        self.assertEqual(BIQ.load_packets(self.root, "origin/main")["WI-S"].status, "submitted")
        # Nothing pending: show says so by saying nothing, and the claim may be released.
        out = subprocess.run([*base, "show", "WI-S"], text=True, capture_output=True, env=env)
        self.assertNotIn("control: PENDING", out.stdout)
        out = subprocess.run([*base, "release", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        # land --apply pushes, opens and follows its own landing the same way.
        subprocess.run([*base, "claim", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        out = subprocess.run([*base, "land", "WI-S", "--apply"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("biq/land/wi-s landed on origin/main at", out.stdout)
        self.repo.git("fetch", "-q", "origin", "main")
        self.assertEqual(BIQ.load_packets(self.root, "origin/main")["WI-S"].status, "completed")
        self.repo.git("checkout", "-q", "main")

    def test_a_refused_or_pending_control_landing_keeps_the_claim_and_is_visible(self):
        candidate, base = self._stale()
        env = fake_gh_env(self.root.parent, self.root.parent / "origin.git", mode="failed")
        subprocess.run([*base, "claim", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        out = subprocess.run([*base, "seal", "WI-S", "--owner", "a", "--candidate", candidate], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 2, out.stdout + out.stderr)
        self.assertIn("control/a/wi-s GATE-FAILED (change request https://example.invalid/pr/1): the required check refused", out.stderr)
        self.repo.git("fetch", "-q", "origin", "main")
        self.assertIsNone(BIQ.load_packets(self.root, "origin/main")["WI-S"].candidate)  # main is untouched
        out = subprocess.run([*base, "show", "WI-S"], text=True, capture_output=True, env=env)
        self.assertIn("control: PENDING on control/a/wi-s at", out.stdout)
        self.assertIn("main still shows open", out.stdout)
        out = subprocess.run([*base, "release", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 2)
        self.assertIn("has a control branch not yet on main: control/a/wi-s", out.stderr)
        # A pending gate times out with its own exit code; rerunning the same command follows the same branch without a second commit.
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "pending", "0"
        out = subprocess.run([*base, "seal", "WI-S", "--owner", "a", "--candidate", candidate], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, BIQ.PENDING_EXIT, out.stdout + out.stderr)
        self.assertIn("PENDING (change request https://example.invalid/pr/1): auto-merge is armed", out.stderr)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/wi-s:refs/remotes/origin/control/a/wi-s")
        self.assertEqual(self.repo.git("rev-list", "--count", "main..origin/control/a/wi-s"), "1")
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "landed", "30"
        out = subprocess.run([*base, "seal", "WI-S", "--owner", "a", "--candidate", candidate], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("landed on origin/main", out.stdout)
        self.repo.git("checkout", "-q", "main")

    def test_a_control_landing_keeps_both_sides_of_a_run_member_list_and_the_index(self):
        # WI-S submits into queue U's open run while main concurrently gains another member there and another retrospective line.
        index = "docs/work-queue/retrospectives/README.md"
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {index: "# Retrospectives\n\n- [one](one.md)\n"}))
        self.repo.packet("WI-S"); self.repo.commit_queue(); self.reload()
        candidate = self.repo.commit_on(self.repo.main(), "WI-S", {"u/s": "s"})
        older = self.repo.git("rev-list", "--max-count=1", "--skip=1", "main")
        self.repo.git("checkout", "-q", "--detach", older)
        base = [sys.executable, str(SCRIPT), "--repository", str(self.root), "--target-ref", "main"]
        env = fake_gh_env(self.root.parent, self.root.parent / "origin.git")
        subprocess.run([*base, "claim", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        out = subprocess.run([*base, "seal", "WI-S", "--owner", "a", "--candidate", candidate], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.repo.git("fetch", "-q", "origin", "main"); self.repo.git("branch", "-f", "main", "origin/main")
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "pending", "0"
        out = subprocess.run([*base, "submit", "WI-S", "--owner", "a", "--apply"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, BIQ.PENDING_EXIT, out.stdout + out.stderr)
        # Meanwhile main moves on the same two files.
        self.repo.git("checkout", "-q", "main")
        run = BIQ.open_run(self.reload(), "U")
        BIQ.add_member(self.root, self.packets, run, "WI-OTHER", "c" * 40)
        (self.root / index).write_text("# Retrospectives\n\n- [one](one.md)\n- [other](other.md)\n")
        self.repo.git("add", "-A"); self.repo.git("commit", "-q", "-m", "concurrent"); self.repo.git("push", "-q", "origin", "main")
        self.repo.git("checkout", "-q", "--detach", older)
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "landed", "30"
        out = subprocess.run([*base, "submit", "WI-S", "--owner", "a", "--apply"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.repo.git("fetch", "-q", "origin", "main")
        landed = BIQ.load_packets(self.root, "origin/main")
        self.assertEqual(landed["WI-S"].status, "submitted")
        members = {m[0] for m in BIQ.open_run(landed, "U").members()}
        self.assertEqual(members, {"WI-S", "WI-OTHER"})
        self.assertEqual(self.repo.git("show", f"origin/main:{index}"), "# Retrospectives\n\n- [one](one.md)\n- [other](other.md)")
        self.repo.git("checkout", "-q", "main")

    def test_members_union_keeps_both_additions_and_prefers_the_replayed_side(self):
        run = BIQ.open_run(self.packets, "U")
        ours = run.source.replace("## Members\n\nnone", "## Members\n\n- WI-A @" + "a" * 40 + "\n- WI-B @" + "b" * 40, 1)
        theirs = run.source.replace("## Members\n\nnone", "## Members\n\n- WI-B @" + "e" * 40 + " (reviewed ok by x)\n- WI-C @" + "c" * 40, 1)
        merged = BIQ.parse_packet(run.item_id, BIQ.members_union(ours, theirs, run.item_id))
        self.assertEqual([(m[0], m[1][:1], m[2]) for m in merged.members()], [("WI-A", "a", ""), ("WI-B", "e", "reviewed ok by x"), ("WI-C", "c", "")])
        self.assertEqual(merged.sections["Excluded"].strip(), run.sections["Excluded"].strip())

    # -- stranded submissions ---------------------------------------------------------
    def test_a_submission_no_run_holds_is_stranded_until_resealed_or_rejected(self):
        a = self.submit("WI-A", {"u/a": "a"}); self.submit("WI-B", {"u/b": "b"})
        run = BIQ.open_run(self.packets, "U")
        BIQ.write_packet(self.root, self.packets, run.item_id, sections={"Members": BIQ.member_lines([m for m in run.members() if m[0] != "WI-A"])})  # a closed run left WI-A behind
        self.repo.commit_queue(); self.reload()
        stranded = BIQ.stranded_submissions(self.packets)
        self.assertEqual(len(stranded), 1); self.assertTrue(stranded[0].startswith("WI-A: submitted but no open, started or unlanded run of U holds it"))
        self.assertIn(stranded[0], BIQ.validation_errors(self.root, None))
        self.assertTrue(BIQ.next_steps(self.root, self.registry, self.packets, self.packets["WI-A"], None, self.repo.main(), [])[0].startswith("STRANDED:"))
        # Unchanged strands are history; a new one is a regression.
        base = self.repo.main()
        self.assertEqual(BIQ.validation_regressions(self.root, base, base), [])
        BIQ.write_packet(self.root, self.packets, run.item_id, sections={"Members": "none"}); head = self.repo.commit_queue(); self.reload()
        self.assertEqual([e.split(":")[0] for e in BIQ.validation_regressions(self.root, base, head)], ["WI-B"])
        # Rejection outside a run needs a stranded leaf, a reason and the human's word; a member of a run is rejected there instead.
        with self.assertRaisesRegex(BIQ.BiqError, "needs the Accountable owner's word"):
            BIQ.reject_item(self.root, self.packets, "WI-A", "left behind", None, apply=True)
        with self.assertRaisesRegex(BIQ.BiqError, "only a submitted leaf"):
            BIQ.reject_item(self.root, self.packets, run.item_id, "x", "Human Owner", apply=True)
        BIQ.add_member(self.root, self.packets, self.packets[run.item_id], "WI-B", "b" * 40)
        with self.assertRaisesRegex(BIQ.BiqError, "is a member of " + run.item_id):
            BIQ.reject_item(self.root, self.packets, "WI-B", "x", "Human Owner", apply=True)
        self.assertEqual(BIQ.reject_item(self.root, self.packets, "WI-A", "left behind by run 1", "Human Owner", apply=False).status, "submitted")  # preview
        packet = BIQ.reject_item(self.root, self.packets, "WI-A", "left behind by run 1", "Human Owner", apply=True)
        self.assertEqual(packet.status, "failed-rejected")
        self.assertIn("Rejected by Human Owner outside a run: left behind by run 1", packet.sections["Resolution"])
        self.assertEqual(packet.candidate, a)  # the candidate stays recorded
        self.assertEqual([e for e in BIQ.stranded_submissions(self.reload()) if e.startswith("WI-A")], [])

    def test_a_rejection_leaves_a_valid_packet_and_names_what_meets_a_dependent(self):
        a = self.submit("WI-A", {"u/a": "a"}); self.submit("WI-B", {"u/b": "b"})
        BIQ.write_packet(self.root, self.packets, "WI-A", [("Decision owner", "Human Owner")])  # a held submission carries its decision owner
        self.repo.packet("WI-MET", status="completed", candidate="c" * 40)
        BIQ.write_packet(self.root, self.packets, "WI-MET", [("Closed At", BIQ.now_utc()), ("Completed human_owner", "Human Owner"), ("Completed host_user", "u"), ("Completed hostname", "h")])
        self.repo.packet("WI-DEP", dependencies="WI-A")  # open work waiting on WI-A
        run = BIQ.open_run(self.packets, "U")
        BIQ.write_packet(self.root, self.packets, run.item_id, sections={"Members": BIQ.member_lines([m for m in run.members() if m[0] != "WI-A"])})
        base = self.repo.commit_queue(); self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, "is a dependency of WI-DEP; name what meets it with --successor"):
            BIQ.reject_item(self.root, self.packets, "WI-A", "met elsewhere", "Human Owner", apply=True)
        with self.assertRaisesRegex(BIQ.BiqError, "successor WI-B is submitted"):  # neither a fresh restart nor landed work
            BIQ.reject_item(self.root, self.packets, "WI-A", "met elsewhere", "Human Owner", apply=True, successor="WI-B")
        packet = BIQ.reject_item(self.root, self.packets, "WI-A", "met elsewhere", "Human Owner", apply=True, successor="WI-MET")
        self.assertEqual((packet.status, packet.fields["Decision owner"], packet.fields.get("Next attempt", "none")), ("failed-rejected", "none", "none"))
        self.assertIn("Objective met by WI-MET; WI-DEP now depend on it.", packet.sections["Resolution"])
        self.assertEqual(self.packets["WI-DEP"].dependencies, ("WI-MET",))
        head = self.repo.commit_queue(); self.reload()
        self.assertEqual(BIQ.validation_regressions(self.root, base, head), [])  # the gate would admit it: no Decision owner left, WI-DEP waits on landed work
        self.assertEqual(BIQ.rejection_fields(self.packets["WI-B"])[:1], [("Status", "failed-rejected")])  # a run's rejection writes the same closing
        self.assertNotIn(("Decision owner", "none"), BIQ.rejection_fields(self.packets["WI-B"]))

    def test_a_held_submission_is_atomically_restarted_across_runs(self):
        old = self.submit("WI-A", {"u/a": "a", "k/a": "a"})
        self.submit("WI-DEP", {"u/dep": "d"}, dependencies="WI-A")
        self.submit("WI-FINE", {"u/fine": "f"})
        self.start("U")
        run = BIQ.find_run(self.packets, "U", 1)
        before_claim = BIQ.read_claim(self.root, run.item_id)
        before_integration = run.fields["Integration commit"]
        self.repo.packet("WI-A-ATTEMPT-2", replaces="WI-A")
        successor_path = self.root / BIQ.ITEMS_DIR / "WI-A-ATTEMPT-2.md"
        successor_path.write_text(successor_path.read_text().replace(
            "- Replaces: `WI-A`", "- Previous attempt: `WI-A`\n- Replaces: `WI-A`"))
        (self.root / BIQ.ITEMS_DIR / "WI-PROJECT.md").write_text(workstream_text("WI-PROJECT", "WI-A"))
        self.reload()

        packet = BIQ.reject_item(
            self.root, self.packets, "WI-A", "frozen scope is wrong", "Human Owner",
            apply=True, successor="WI-A-ATTEMPT-2", registry=self.registry, owner="runner",
        )

        self.assertEqual((packet.status, packet.fields["Next attempt"]), ("failed-rejected", "WI-A-ATTEMPT-2"))
        self.assertEqual(self.packets["WI-A-ATTEMPT-2"].status, "open")
        self.assertEqual(BIQ.read_claim(self.root, run.item_id), before_claim)
        rebuilt = BIQ.find_run(self.packets, "U", 1)
        self.assertNotEqual(rebuilt.fields["Integration commit"], before_integration)
        self.assertEqual([m[0] for m in rebuilt.members()], ["WI-FINE"])
        self.assertNotIn("WI-A", BIQ.excluded_members(rebuilt))
        self.assertEqual([m[0] for m in BIQ.open_run(self.packets, "U").members()], ["WI-DEP"])
        self.assertEqual([m[0] for m in BIQ.open_run(self.packets, "K").members()], [])
        self.assertFalse(any(m[0] == "WI-A-ATTEMPT-2" for run in BIQ.runs_of(self.packets, "U") + BIQ.runs_of(self.packets, "K") for m in run.members()))
        self.assertFalse(BIQ.is_ancestor(self.root, old, rebuilt.fields["Integration commit"]))
        self.assertEqual(BIQ.container_members(self.packets["WI-PROJECT"]), ["WI-A-ATTEMPT-2"])
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])

    def test_a_held_exclusion_is_removed_without_reinjecting_its_successor(self):
        self.submit("WI-A", {"u/a": "a", "k/a": "a"})
        self.submit("WI-FINE", {"u/fine": "f"})
        self.start("U")
        run = BIQ.find_run(self.packets, "U", 1)
        BIQ.review_member(self.root, self.registry, self.packets, run, "WI-A", owner="runner", ok=False,
                          reject=None, hold="replace it?", apply=True)
        self.repo.packet("WI-A-ATTEMPT-2", replaces="WI-A")
        successor_path = self.root / BIQ.ITEMS_DIR / "WI-A-ATTEMPT-2.md"
        successor_path.write_text(successor_path.read_text().replace(
            "- Replaces: `WI-A`", "- Previous attempt: `WI-A`\n- Replaces: `WI-A`"))
        self.reload()

        with self.assertRaisesRegex(BIQ.BiqError, "requires the queue registry"):
            BIQ.reject_item(self.root, self.packets, "WI-A", "wrong objective", "Human Owner",
                            apply=False, successor="WI-A-ATTEMPT-2", owner="runner")
        with self.assertRaisesRegex(BIQ.BiqError, "must be claimed by 'other' first"):
            BIQ.reject_item(self.root, self.packets, "WI-A", "wrong objective", "Human Owner",
                            apply=True, successor="WI-A-ATTEMPT-2", registry=self.registry, owner="other")
        self.assertEqual(self.packets["WI-A"].status, "submitted")
        self.assertIn("WI-A", BIQ.excluded_members(BIQ.find_run(self.packets, "U", 1)))
        BIQ.reject_item(self.root, self.packets, "WI-A", "wrong objective", "Human Owner",
                        apply=True, successor="WI-A-ATTEMPT-2", registry=self.registry, owner="runner")

        self.assertNotIn("WI-A", BIQ.excluded_members(BIQ.find_run(self.packets, "U", 1)))
        self.assertEqual([m[0] for m in BIQ.open_run(self.packets, "U").members()], [])
        self.assertEqual([m[0] for m in BIQ.open_run(self.packets, "K").members()], [])
        self.assertEqual(self.packets["WI-A"].fields["Decision owner"], "none")
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])

    def test_a_submitted_run_holding_the_submission_refuses_replacement_without_mutation(self):
        self.submit("WI-A", {"u/a": "a"})
        self.start("U")
        self.green("U", 1)
        self.repo.packet("WI-A-ATTEMPT-2", replaces="WI-A")
        successor_path = self.root / BIQ.ITEMS_DIR / "WI-A-ATTEMPT-2.md"
        successor_path.write_text(successor_path.read_text().replace(
            "- Replaces: `WI-A`", "- Previous attempt: `WI-A`\n- Replaces: `WI-A`"))
        self.reload()
        before = {item: (self.root / BIQ.ITEMS_DIR / f"{item}.md").read_text() for item in self.packets}

        with self.assertRaisesRegex(BIQ.BiqError, "held by submitted run"):
            BIQ.reject_item(
                self.root, self.packets, "WI-A", "wrong objective", "Human Owner", apply=True,
                successor="WI-A-ATTEMPT-2", registry=self.registry, owner="runner",
            )

        self.reload()
        self.assertEqual({item: (self.root / BIQ.ITEMS_DIR / f"{item}.md").read_text() for item in self.packets}, before)
        self.assertEqual(self.packets["WI-A"].status, "submitted")

    def test_cross_run_rebuild_failure_publishes_no_partial_candidate_refs(self):
        self.submit("WI-A", {"u/a": "a", "k/a": "a"})
        self.submit("WI-U-FINE", {"u/fine": "fine"})
        self.submit("WI-K-FINE", {"k/fine": "fine"})
        self.start("U")
        self.start("K")
        self.repo.packet("WI-A-ATTEMPT-2", replaces="WI-A")
        successor_path = self.root / BIQ.ITEMS_DIR / "WI-A-ATTEMPT-2.md"
        successor_path.write_text(successor_path.read_text().replace(
            "- Replaces: `WI-A`", "- Previous attempt: `WI-A`\n- Replaces: `WI-A`"))
        self.reload()
        before_refs = self.repo.git("ls-remote", "origin", BIQ.CANDIDATE_REF_PREFIX + "*")
        real_merge = BIQ.merge_members
        calls = 0

        def fail_second(*args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise BIQ.BiqError("synthetic second rebuild failure")
            return real_merge(*args, **kwargs)

        with mock.patch.object(BIQ, "merge_members", side_effect=fail_second):
            with self.assertRaisesRegex(BIQ.BiqError, "synthetic second rebuild failure"):
                BIQ.reject_item(
                    self.root, self.packets, "WI-A", "wrong objective", "Human Owner", apply=True,
                    successor="WI-A-ATTEMPT-2", registry=self.registry, owner="runner",
                )

        self.assertEqual(self.repo.git("ls-remote", "origin", BIQ.CANDIDATE_REF_PREFIX + "*"), before_refs)

    def test_atomic_candidate_publication_failure_rolls_back_local_transition(self):
        self.submit("WI-A", {"u/a": "a"})
        self.submit("WI-FINE", {"u/fine": "fine"})
        self.start("U")
        run = BIQ.find_run(self.packets, "U", 1)
        before_branch = self.repo.git("rev-parse", "biq/u/1")
        self.repo.packet("WI-A-ATTEMPT-2", replaces="WI-A")
        successor_path = self.root / BIQ.ITEMS_DIR / "WI-A-ATTEMPT-2.md"
        successor_path.write_text(successor_path.read_text().replace(
            "- Replaces: `WI-A`", "- Previous attempt: `WI-A`\n- Replaces: `WI-A`"))
        (self.root / BIQ.ITEMS_DIR / "WI-PROJECT.md").write_text(workstream_text("WI-PROJECT", "WI-A"))
        self.reload()
        before_packets = {item: (self.root / BIQ.ITEMS_DIR / f"{item}.md").read_text() for item in self.packets}
        before_refs = self.repo.git("ls-remote", "origin", BIQ.CANDIDATE_REF_PREFIX + "*")

        with mock.patch.object(BIQ, "publish_candidates_atomic", side_effect=BIQ.BiqError("synthetic atomic push failure")):
            with self.assertRaisesRegex(BIQ.BiqError, "synthetic atomic push failure"):
                BIQ.reject_item(
                    self.root, self.packets, "WI-A", "wrong objective", "Human Owner", apply=True,
                    successor="WI-A-ATTEMPT-2", registry=self.registry, owner="runner",
                )

        self.assertEqual(self.repo.git("ls-remote", "origin", BIQ.CANDIDATE_REF_PREFIX + "*"), before_refs)
        self.assertEqual(self.repo.git("rev-parse", "biq/u/1"), before_branch)
        self.assertEqual({item: (self.root / BIQ.ITEMS_DIR / f"{item}.md").read_text() for item in self.packets}, before_packets)
        self.assertEqual(self.packets[run.item_id].fields["Integration commit"], before_branch)
        self.assertEqual(BIQ.container_members(self.packets["WI-PROJECT"]), ["WI-A"])

    def test_control_gate_refusal_publishes_neither_rebuild_nor_control_ref(self):
        self.submit("WI-A", {"u/a": "a"})
        self.submit("WI-FINE", {"u/fine": "fine"})
        self.start("U")
        self.repo.packet("WI-A-ATTEMPT-2", replaces="WI-A")
        successor_path = self.root / BIQ.ITEMS_DIR / "WI-A-ATTEMPT-2.md"
        successor_path.write_text(successor_path.read_text().replace(
            "- Replaces: `WI-A`", "- Previous attempt: `WI-A`\n- Replaces: `WI-A`"))
        self.reload()
        before_refs = self.repo.git("ls-remote", "origin", BIQ.CANDIDATE_REF_PREFIX + "*")
        deferred = []
        branch_moves = []
        BIQ.reject_item(
            self.root, self.packets, "WI-A", "wrong objective", "Human Owner", apply=True,
            successor="WI-A-ATTEMPT-2", registry=self.registry, owner="runner", deferred_publications=deferred,
            deferred_branch_moves=branch_moves,
        )
        self.assertTrue(deferred)
        self.assertEqual(self.repo.git("ls-remote", "origin", BIQ.CANDIDATE_REF_PREFIX + "*"), before_refs)
        operation = {"token": "sha256:" + "1" * 64, "label": "reject WI-A"}

        with mock.patch.object(BIQ, "verify_landing", side_effect=BIQ.BiqError("synthetic gate refusal")):
            with self.assertRaisesRegex(BIQ.BiqError, "refused before push, nothing pending"):
                BIQ.commit_control_clone(
                    self.root, "control/runner/wi-a", "reject", "WI-A", "runner", operation,
                    "main", tuple(deferred), tuple(branch_moves),
                )

        self.assertEqual(self.repo.git("ls-remote", "origin", BIQ.CANDIDATE_REF_PREFIX + "*"), before_refs)
        self.assertEqual(self.repo.git("ls-remote", "origin", "refs/heads/control/runner/wi-a"), "")

    def test_control_commit_and_rebuild_candidates_publish_in_one_atomic_push(self):
        self.submit("WI-A", {"u/a": "a"})
        self.submit("WI-FINE", {"u/fine": "fine"})
        self.start("U")
        self.repo.packet("WI-A-ATTEMPT-2", replaces="WI-A")
        successor_path = self.root / BIQ.ITEMS_DIR / "WI-A-ATTEMPT-2.md"
        successor_path.write_text(successor_path.read_text().replace(
            "- Replaces: `WI-A`", "- Previous attempt: `WI-A`\n- Replaces: `WI-A`"))
        self.reload()
        deferred = []
        branch_moves = []
        before_run_branch = self.repo.git("rev-parse", "biq/u/1")
        BIQ.reject_item(
            self.root, self.packets, "WI-A", "wrong objective", "Human Owner", apply=True,
            successor="WI-A-ATTEMPT-2", registry=self.registry, owner="runner", deferred_publications=deferred,
            deferred_branch_moves=branch_moves,
        )
        self.assertEqual(self.repo.git("rev-parse", "biq/u/1"), before_run_branch)
        operation = {"token": "sha256:" + "2" * 64, "label": "reject WI-A"}

        outcome = BIQ.commit_control_clone(
            self.root, "control/runner/wi-a", "reject", "WI-A", "runner", operation,
            candidate_refs=tuple(deferred), branch_actions=tuple(branch_moves),
        )

        self.assertTrue(self.repo.git("ls-remote", "origin", "refs/heads/control/runner/wi-a").startswith(outcome["commit"]))
        for item, candidate in deferred:
            published = self.repo.git("ls-remote", "origin", f"{BIQ.CANDIDATE_REF_PREFIX}{item}/{candidate[:12]}")
            self.assertTrue(published.startswith(candidate), published)
        self.assertEqual(self.repo.git("rev-parse", "biq/u/1"), branch_moves[0][1])

    def test_atomic_control_publication_failure_drops_local_control_commit(self):
        self.submit("WI-A", {"u/a": "a"})
        self.submit("WI-FINE", {"u/fine": "fine"})
        self.start("U")
        self.repo.packet("WI-A-ATTEMPT-2", replaces="WI-A")
        successor_path = self.root / BIQ.ITEMS_DIR / "WI-A-ATTEMPT-2.md"
        successor_path.write_text(successor_path.read_text().replace(
            "- Replaces: `WI-A`", "- Previous attempt: `WI-A`\n- Replaces: `WI-A`"))
        self.reload()
        deferred = []
        branch_moves = []
        before_run_branch = self.repo.git("rev-parse", "biq/u/1")
        BIQ.reject_item(
            self.root, self.packets, "WI-A", "wrong objective", "Human Owner", apply=True,
            successor="WI-A-ATTEMPT-2", registry=self.registry, owner="runner", deferred_publications=deferred,
            deferred_branch_moves=branch_moves,
        )
        before_head = self.repo.git("rev-parse", "HEAD")
        before_refs = self.repo.git("ls-remote", "origin", BIQ.CANDIDATE_REF_PREFIX + "*")
        operation = {"token": "sha256:" + "3" * 64, "label": "reject WI-A"}
        real_git = BIQ.git

        def fail_push(root, *args, **kwargs):
            if args and args[0] == "push":
                raise BIQ.BiqError("synthetic push failure")
            return real_git(root, *args, **kwargs)

        with mock.patch.object(BIQ, "git", side_effect=fail_push):
            with self.assertRaisesRegex(BIQ.BiqError, "publication failed, nothing pending"):
                BIQ.commit_control_clone(
                    self.root, "control/runner/wi-a", "reject", "WI-A", "runner", operation,
                    candidate_refs=tuple(deferred), branch_actions=tuple(branch_moves),
                )

        self.assertEqual(self.repo.git("rev-parse", "HEAD"), before_head)
        self.assertEqual(self.repo.git("rev-parse", "biq/u/1"), before_run_branch)
        self.assertEqual(self.repo.git("ls-remote", "origin", BIQ.CANDIDATE_REF_PREFIX + "*"), before_refs)
        self.assertEqual(self.repo.git("ls-remote", "origin", "refs/heads/control/runner/wi-a"), "")

    def test_control_rebuild_does_not_require_a_local_run_branch(self):
        self.submit("WI-A", {"u/a": "a"})
        self.submit("WI-FINE", {"u/fine": "fine"})
        self.start("U")
        old_integration = BIQ.find_run(self.packets, "U", 1).fields["Integration commit"]
        self.repo.git("branch", "-D", "biq/u/1")
        self.repo.packet("WI-A-ATTEMPT-2", replaces="WI-A")
        successor_path = self.root / BIQ.ITEMS_DIR / "WI-A-ATTEMPT-2.md"
        successor_path.write_text(successor_path.read_text().replace(
            "- Replaces: `WI-A`", "- Previous attempt: `WI-A`\n- Replaces: `WI-A`"))
        self.reload()
        deferred, branch_moves = [], []

        BIQ.reject_item(
            self.root, self.packets, "WI-A", "wrong objective", "Human Owner", apply=True,
            successor="WI-A-ATTEMPT-2", registry=self.registry, owner="runner", deferred_publications=deferred,
            deferred_branch_moves=branch_moves,
        )

        self.assertTrue(deferred)
        self.assertEqual(branch_moves, [("biq/u/1", deferred[0][1])])
        self.assertNotEqual(branch_moves[0][1], old_integration)
        self.assertNotIn("refs/heads/biq/u/1", self.repo.git("show-ref"))

    def test_empty_run_branch_is_preserved_when_control_publication_fails(self):
        self.submit("WI-A", {"u/a": "a"})
        self.start("U")
        before_run_branch = self.repo.git("rev-parse", "biq/u/1")
        self.repo.packet("WI-A-ATTEMPT-2", replaces="WI-A")
        successor_path = self.root / BIQ.ITEMS_DIR / "WI-A-ATTEMPT-2.md"
        successor_path.write_text(successor_path.read_text().replace(
            "- Replaces: `WI-A`", "- Previous attempt: `WI-A`\n- Replaces: `WI-A`"))
        self.reload()
        deferred, branch_actions = [], []
        BIQ.reject_item(
            self.root, self.packets, "WI-A", "wrong objective", "Human Owner", apply=True,
            successor="WI-A-ATTEMPT-2", registry=self.registry, owner="runner", deferred_publications=deferred,
            deferred_branch_moves=branch_actions,
        )
        self.assertEqual((deferred, branch_actions), ([], [("biq/u/1", None)]))
        self.assertEqual(self.repo.git("rev-parse", "biq/u/1"), before_run_branch)
        before_head = self.repo.git("rev-parse", "HEAD")
        operation = {"token": "sha256:" + "4" * 64, "label": "reject WI-A"}
        real_git = BIQ.git

        def fail_push(root, *args, **kwargs):
            if args and args[0] == "push":
                raise BIQ.BiqError("synthetic push failure")
            return real_git(root, *args, **kwargs)

        with mock.patch.object(BIQ, "git", side_effect=fail_push):
            with self.assertRaisesRegex(BIQ.BiqError, "publication failed, nothing pending"):
                BIQ.commit_control_clone(
                    self.root, "control/runner/wi-a", "reject", "WI-A", "runner", operation,
                    branch_actions=tuple(branch_actions),
                )

        self.assertEqual(self.repo.git("rev-parse", "HEAD"), before_head)
        self.assertEqual(self.repo.git("rev-parse", "biq/u/1"), before_run_branch)
        self.assertEqual(self.repo.git("ls-remote", "origin", "refs/heads/control/runner/wi-a"), "")

    def test_empty_run_branch_is_deleted_after_control_publication_succeeds(self):
        self.submit("WI-A", {"u/a": "a"})
        self.start("U")
        self.repo.packet("WI-A-ATTEMPT-2", replaces="WI-A")
        successor_path = self.root / BIQ.ITEMS_DIR / "WI-A-ATTEMPT-2.md"
        successor_path.write_text(successor_path.read_text().replace(
            "- Replaces: `WI-A`", "- Previous attempt: `WI-A`\n- Replaces: `WI-A`"))
        self.reload()
        deferred, branch_actions = [], []
        BIQ.reject_item(
            self.root, self.packets, "WI-A", "wrong objective", "Human Owner", apply=True,
            successor="WI-A-ATTEMPT-2", registry=self.registry, owner="runner", deferred_publications=deferred,
            deferred_branch_moves=branch_actions,
        )
        operation = {"token": "sha256:" + "5" * 64, "label": "reject WI-A"}

        outcome = BIQ.commit_control_clone(
            self.root, "control/runner/wi-a", "reject", "WI-A", "runner", operation,
            branch_actions=tuple(branch_actions),
        )
        self.assertTrue(outcome["commit"])
        self.assertNotIn("refs/heads/biq/u/1", self.repo.git("show-ref"))

    def test_a_claimed_packet_is_fixed_and_a_packet_stays_bounded(self):
        self.repo.packet("WI-A"); self.repo.packet("WI-B"); self.repo.packet("WI-T", status="failed-abandoned")
        base = self.repo.commit_queue(); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-A", "agent")
        # A claimed packet's objective is fixed; an unclaimed draft's is not; the writable sections stay writable.
        BIQ.write_packet(self.root, self.packets, "WI-A", sections={"Objective": "Wider."})
        BIQ.write_packet(self.root, self.packets, "WI-B", sections={"Objective": "Wider."})
        head = self.repo.commit_queue(); self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, "WI-A: Objective changed after claim"):
            BIQ.verify_landing(self.root, base, head)
        self.assertEqual([e.split(":")[0] for e in BIQ.packet_change_errors(self.root, BIQ.load_packets(self.root, base), self.packets)], ["WI-A"])
        self.assertTrue(BIQ.next_steps(self.root, self.registry, self.packets, self.packets["WI-A"], None, base, [])[0].startswith("FIXED AT CLAIM: Objective changed after claim"))
        BIQ.write_packet(self.root, self.packets, "WI-A", [("Kind", "feature")], sections={"Objective": "Test."})  # objective restored; a frozen field is refused too
        head = self.repo.commit_queue(); self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, "WI-A: Kind changed after claim"):
            BIQ.verify_landing(self.root, base, head)
        BIQ.write_packet(self.root, self.packets, "WI-A", [("Kind", "bug")], sections={"Attempt history": "Tried once."}, resolution="Still open.")  # only writable text differs from base
        head = self.repo.commit_queue(); self.reload()
        self.assertEqual(BIQ.verify_landing(self.root, base, head), "control commit")
        # A packet over the bound is a problem; a new one or growth is refused, a shrink is admitted, a terminal one is history.
        base = head
        padding = "evidence line\n" * BIQ.PACKET_LINE_BOUND
        BIQ.write_packet(self.root, self.packets, "WI-B", sections={"Attempt history": padding})
        BIQ.write_packet(self.root, self.packets, "WI-T", sections={"Attempt history": padding})
        head = self.repo.commit_queue(); self.reload()
        self.assertEqual([e.split(":")[0] for e in BIQ.packet_bound_errors(self.packets)], ["WI-B"])
        self.assertTrue(BIQ.next_steps(self.root, self.registry, self.packets, self.packets["WI-B"], None, head, [])[0].startswith("OVER BOUND: packet is over the bound"))
        with self.assertRaisesRegex(BIQ.BiqError, "WI-B: packet is over the bound"):
            BIQ.verify_landing(self.root, base, head)
        base = head  # now history: unchanged it is admitted, larger refused, smaller admitted
        BIQ.write_packet(self.root, self.packets, "WI-B", sections={"Resolution": "pending"})
        head = self.repo.commit_queue(); self.reload()
        self.assertEqual(BIQ.verify_landing(self.root, base, head), "control commit")
        BIQ.write_packet(self.root, self.packets, "WI-B", sections={"Attempt history": padding + "one more\n"})
        head = self.repo.commit_queue(); self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, "WI-B: packet is over the bound .* and grew"):
            BIQ.verify_landing(self.root, base, head)
        BIQ.write_packet(self.root, self.packets, "WI-B", sections={"Attempt history": "moved to agent-work/WI-B/"})
        head = self.repo.commit_queue(); self.reload()
        self.assertEqual(BIQ.verify_landing(self.root, base, head), "control commit")

    # -- workstreams -----------------------------------------------------------------
    def test_a_new_packet_is_steered_to_its_predecessors_workstream_and_joins_it(self):
        self.repo.packet("WI-OLD", scope=("u/a", "u/b"))
        (self.root / "docs/work-queue/items/WI-NFS-WORKSTREAM.md").write_text(workstream_text("WI-NFS-WORKSTREAM", "WI-OLD"))
        (self.root / "docs/work-queue/items/WI-OTHER-WORKSTREAM.md").write_text(workstream_text("WI-OTHER-WORKSTREAM", "WI-CI-INTEGRATION-K-REPEAT-1"))
        self.repo.packet("WI-NEW-NFS-FIX", created=BIQ.WORKSTREAM_SINCE, dependencies="WI-OLD", scope=("u/a",))
        packets = self.reload()
        self.assertIsNone(BIQ.workstream_of(packets, "WI-NEW-NFS-FIX"))
        self.assertEqual(BIQ.workstream_of(packets, "WI-CI-INTEGRATION-K-REPEAT-2"), "WI-OTHER-WORKSTREAM")  # one ordinal lists the family
        ranked = BIQ.suggest_workstreams(packets, "WI-NEW-NFS-FIX")
        self.assertEqual(ranked[0][1], "WI-NFS-WORKSTREAM")
        self.assertTrue(any(r.startswith("related: WI-OLD") for r in ranked[0][2]))
        self.assertTrue(any(r.startswith("scope: u/a") for r in ranked[0][2]))
        lines = BIQ.workstream_lines(packets, "WI-NEW-NFS-FIX")
        self.assertIn("--join WI-NFS-WORKSTREAM", lines[-1])
        self.assertTrue(any("no workstream" in e for e in BIQ.lint(self.registry, packets)))
        self.assertIn("workstream WI-NEW-NFS-FIX", BIQ.next_steps(self.root, self.registry, packets, packets["WI-NEW-NFS-FIX"], None, self.repo.main(), [])[0])
        BIQ.join_workstream(self.root, packets, "WI-NFS-WORKSTREAM", "WI-NEW-NFS-FIX")
        self.assertEqual(packets["WI-NFS-WORKSTREAM"].fields["Container members"], "WI-NEW-NFS-FIX, WI-OLD")
        self.assertEqual(BIQ.workstream_of(packets, "WI-NEW-NFS-FIX"), "WI-NFS-WORKSTREAM")
        self.assertEqual([e for e in BIQ.lint(self.registry, packets) if "workstream" in e], [])
        self.assertEqual(BIQ.workstream_lines(packets, "WI-NEW-NFS-FIX"), ["WI-NEW-NFS-FIX: workstream WI-NFS-WORKSTREAM"])
        with self.assertRaisesRegex(BIQ.BiqError, "at most one workstream"):
            BIQ.join_workstream(self.root, packets, "WI-OTHER-WORKSTREAM", "WI-NEW-NFS-FIX")
        with self.assertRaisesRegex(BIQ.BiqError, "not an open workstream"):
            BIQ.join_workstream(self.root, packets, "WI-OLD", "WI-NEW-NFS-FIX")
        with self.assertRaisesRegex(BIQ.BiqError, "runs join through their queue"):
            BIQ.join_workstream(self.root, packets, "WI-NFS-WORKSTREAM", "WI-CI-INTEGRATION-U-REPEAT-1")
        # Container relationships are flat: an item held by an operative container that a
        # workstream lists is not thereby a member; it must be listed itself, as the report projects.
        self.repo.packet("WI-CHILD", created=BIQ.WORKSTREAM_SINCE)
        (self.root / "docs/work-queue/items/WI-BOX.md").write_text(workstream_text("WI-BOX", "WI-CHILD").replace("`project`", "`operative`"))
        BIQ.join_workstream(self.root, self.reload(), "WI-NFS-WORKSTREAM", "WI-BOX")
        self.assertIsNone(BIQ.workstream_of(self.packets, "WI-CHILD"))
        BIQ.join_workstream(self.root, self.packets, "WI-NFS-WORKSTREAM", "WI-CHILD")
        self.assertEqual(BIQ.workstream_of(self.packets, "WI-CHILD"), "WI-NFS-WORKSTREAM")

    def test_submission_and_the_gate_refuse_a_new_packet_outside_every_workstream(self):
        (self.root / "docs/work-queue/items/WI-NFS-WORKSTREAM.md").write_text(workstream_text("WI-NFS-WORKSTREAM", "WI-CI-INTEGRATION-U-REPEAT-1"))
        self.repo.packet("WI-LEGACY")  # no Created: predates the rule
        self.repo.packet("WI-FRESH", created="2026-09-10T00:00:00Z")
        base = self.repo.commit_queue()
        candidate = self.repo.commit_on(base, "WI-FRESH", {"u/f": "f"})
        self.repo.packet("WI-FRESH", created="2026-09-10T00:00:00Z", candidate=candidate)
        self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-FRESH", "agent")
        BIQ.seal_item(self.root, self.packets, "WI-FRESH", candidate, self.repo.main(), "agent")
        with self.assertRaisesRegex(BIQ.BiqError, "belongs to no workstream"):
            BIQ.submit_item(self.root, self.registry, self.packets, "WI-FRESH", self.repo.main(), "agent", apply=True)
        head = self.repo.commit_queue()  # the sealed, unassigned packet as a control commit
        with self.assertRaisesRegex(BIQ.BiqError, "no workstream"):
            BIQ.verify_landing(self.root, base, head)
        BIQ.join_workstream(self.root, self.packets, "WI-NFS-WORKSTREAM", "WI-FRESH")
        self.retrospective("WI-FRESH", slug="2026-09-fresh")  # the retrospective rule applies to the same packets
        head = self.repo.commit_queue()
        self.assertEqual(BIQ.verify_landing(self.root, base, head), "control commit")
        BIQ.submit_item(self.root, self.registry, self.packets, "WI-FRESH", self.repo.main(), "agent", apply=True)
        self.assertEqual(self.packets["WI-FRESH"].status, "submitted")
        self.submit("WI-LEGACY", {"u/l": "l"})  # older packets still submit without a workstream
        self.assertEqual(self.packets["WI-LEGACY"].status, "submitted")

    def test_workstream_join_from_a_stale_checkout_writes_through_the_control_clone(self):
        (self.root / "docs/work-queue/items/WI-NFS-WORKSTREAM.md").write_text(workstream_text("WI-NFS-WORKSTREAM", "WI-CI-INTEGRATION-U-REPEAT-1"))
        registration_base = self.repo.commit_queue()
        older = self.repo.git("rev-list", "--max-count=1", "--skip=1", "main")
        self.repo.git("checkout", "-q", "--detach", older)
        draft = validation_packet_text("WI-DRAFT").replace("origin/main " + "a" * 40, "origin/main " + registration_base).replace("README.md#owner", "README").replace("tools/check.py", "README")
        draft = BIQ.set_field(draft, "Created", BIQ.WORKSTREAM_SINCE)
        draft = BIQ.set_field(draft, "Dependencies", "WI-CI-INTEGRATION-U-REPEAT-1")
        (self.root / BIQ.ITEMS_DIR / "WI-DRAFT.md").write_text(draft)  # drafted, not yet on main
        env = {**os.environ, "BIQ_CONTROL_DIR": str(self.root.parent / "control"), "PATH": self.git_only_path(), "BIQ_CHANGE_PROVIDER": "github"}
        base = [sys.executable, str(SCRIPT), "--repository", str(self.root), "--target-ref", "main"]
        out = subprocess.run([*base, "workstream", "WI-DRAFT"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 2, out.stdout + out.stderr)  # main does not hold the draft yet
        out = subprocess.run([*base, "workstream", "WI-DRAFT", "--join", "WI-NFS-WORKSTREAM", "--owner", "a", "--apply"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, BIQ.PENDING_EXIT, out.stdout + out.stderr)  # pushed; without gh the landing is the operator's
        self.assertIn("listed in WI-NFS-WORKSTREAM", out.stdout)
        self.assertIn("pushed to control/a/wi-draft", out.stdout)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/wi-draft:refs/remotes/origin/control/a/wi-draft")
        pushed = BIQ.load_packets(self.root, "origin/control/a/wi-draft")
        self.assertEqual(pushed["WI-NFS-WORKSTREAM"].fields["Container members"], "WI-CI-INTEGRATION-U-REPEAT-1, WI-DRAFT")
        self.assertIn("WI-DRAFT", pushed)  # the draft travelled with the join
        self.assertEqual(BIQ.verify_landing(self.root, "main", "origin/control/a/wi-draft"), "control commit")
        (self.root / "docs/work-queue/items/WI-DRAFT.md").unlink()
        self.repo.git("checkout", "-q", "main")

    # -- retrospectives ----------------------------------------------------------------
    def retrospective(self, item_id: str, slug: str = "2026-09-test", text: str = "evidence") -> str:
        """Write and index a retrospective naming the item; the caller commits it."""
        rel = f"docs/work-queue/retrospectives/{slug}.md"
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"# Test retrospective\n\n## Workstream and timeline\n\n- WQE: `{item_id}`\n\n{text}\n")
        index = self.root / "docs/work-queue/retrospectives/README.md"
        body = index.read_text() if index.is_file() else "# Retrospectives\n\n## Entries\n"
        if f"]({slug}.md)" not in body:
            index.write_text(body.rstrip("\n") + f"\n- [{slug}]({slug}.md)\n")
        return rel

    def test_submission_requires_a_committed_indexed_retrospective_and_the_gate_proves_it(self):
        (self.root / "docs/work-queue/items/WI-WS.md").write_text(workstream_text("WI-WS", "WI-CI-INTEGRATION-U-REPEAT-1"))
        self.repo.packet("WI-R", created=BIQ.WORKSTREAM_SINCE)
        base = self.repo.commit_queue()
        candidate = self.repo.commit_on(base, "WI-R", {"docs/r": "r"})  # no queue cares: lands directly
        self.repo.packet("WI-R", created=BIQ.WORKSTREAM_SINCE, candidate=candidate)
        BIQ.join_workstream(self.root, self.reload(), "WI-WS", "WI-R")
        BIQ.claim_item(self.root, self.packets, "WI-R", "agent")
        BIQ.seal_item(self.root, self.packets, "WI-R", candidate, self.repo.main(), "agent")
        with self.assertRaisesRegex(BIQ.BiqError, "has no retrospective naming it"):
            BIQ.submit_item(self.root, self.registry, self.packets, "WI-R", self.repo.main(), "agent", apply=True)
        self.assertIn("publish the retrospective", BIQ.next_steps(self.root, self.registry, self.packets, self.packets["WI-R"], {"owner": "agent"}, self.repo.main(), [])[0])
        rel = self.retrospective("WI-R")
        with self.assertRaisesRegex(BIQ.BiqError, "not committed"):
            BIQ.submit_item(self.root, self.registry, self.packets, "WI-R", self.repo.main(), "agent", apply=True)
        self.repo.commit_queue()  # retrospective and index committed
        retro_commit = self.repo.git("log", "-n", "1", "--format=%H", "HEAD", "--", rel)
        BIQ.submit_item(self.root, self.registry, self.packets, "WI-R", self.repo.main(), "agent", apply=True)
        reference = BIQ.retrospective_reference(self.packets["WI-R"])
        self.assertEqual((reference["path"], reference["commit"]), (rel, retro_commit))
        self.assertEqual(BIQ.lint(self.registry, self.packets), [])
        # A second submission replaces the sentence instead of stacking it.
        self.assertEqual(self.packets["WI-R"].sections["Resolution"].count("Retrospective: "), 1)
        main = self.repo.commit_queue()
        outcome = BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-R", main, apply=True)
        self.assertEqual(BIQ.verify_landing(self.root, main, outcome["head"]), "leaf WI-R that no queue cares about")
        # The gate refuses a landing whose retrospective commit is gone (rebase or squash) or whose content moved.
        forged = self.packets["WI-R"].source.replace(retro_commit, "f" * 40)
        (self.root / "docs/work-queue/items/WI-R.md").write_text(forged)
        rewritten = self.repo.commit_queue()
        with self.assertRaisesRegex(BIQ.BiqError, "not on the landing"):
            BIQ.verify_landing(self.root, rewritten, BIQ.prepare_landing(self.root, self.registry, self.reload(), "WI-R", rewritten, apply=True)["head"])
        (self.root / "docs/work-queue/items/WI-R.md").write_text(self.packets["WI-R"].source.replace("f" * 40, retro_commit))
        (self.root / rel).write_text("rewritten after submission\n")
        moved = self.repo.commit_queue()
        with self.assertRaisesRegex(BIQ.BiqError, "differs from the recorded retrospective"):
            BIQ.verify_landing(self.root, moved, BIQ.prepare_landing(self.root, self.registry, self.reload(), "WI-R", moved, apply=True)["head"])

    def test_older_packets_submit_without_a_retrospective_and_lint_names_a_submitted_new_one_lacking_it(self):
        self.submit("WI-OLD", {"u/o": "o"})  # no Created field: predates the rule
        self.assertIsNone(BIQ.retrospective_reference(self.packets["WI-OLD"]))
        self.assertEqual([e for e in BIQ.lint(self.registry, self.packets) if "retrospective" in e], [])
        text = (self.root / "docs/work-queue/items/WI-OLD.md").read_text().replace("- Kind: `bug`", f"- Kind: `bug`\n- Created: `{BIQ.WORKSTREAM_SINCE}`", 1)
        (self.root / "docs/work-queue/items/WI-OLD.md").write_text(text)
        (self.root / "docs/work-queue/items/WI-WS.md").write_text(workstream_text("WI-WS", "WI-OLD"))
        self.assertTrue(any("submitted without a retrospective reference" in e for e in BIQ.lint(self.registry, self.reload())))

    def test_submit_from_a_stale_checkout_carries_the_retrospective_into_the_control_clone(self):
        (self.root / "docs/work-queue/items/WI-WS.md").write_text(workstream_text("WI-WS", "WI-S"))
        self.repo.packet("WI-S", created=BIQ.WORKSTREAM_SINCE); self.repo.commit_queue(); self.reload()
        candidate = self.repo.commit_on(self.repo.main(), "WI-S", {"u/s": "s"})
        older = self.repo.git("rev-list", "--max-count=1", "--skip=1", "main")
        self.repo.git("checkout", "-q", "--detach", older)
        rel = self.retrospective("WI-S", slug="2026-09-stale")
        self.repo.git("add", "-A", "--", "docs/work-queue/retrospectives")
        self.repo.git("commit", "-q", "-m", "retrospective on the owner's branch")
        env = fake_gh_env(self.root.parent, self.root.parent / "origin.git")
        base = [sys.executable, str(SCRIPT), "--repository", str(self.root), "--target-ref", "origin/main"]
        subprocess.run([*base, "claim", "WI-S", "--owner", "a"], text=True, capture_output=True, env=env)
        out = subprocess.run([*base, "seal", "WI-S", "--owner", "a", "--candidate", candidate], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.repo.git("fetch", "-q", "origin", "main")
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "pending", "0"
        out = subprocess.run([*base, "submit", "WI-S", "--owner", "a", "--apply"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, BIQ.PENDING_EXIT, out.stdout + out.stderr)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/wi-s:refs/remotes/origin/control/a/wi-s")
        submitted = BIQ.load_packets(self.root, "origin/control/a/wi-s")["WI-S"]
        reference = BIQ.retrospective_reference(submitted)
        self.assertEqual(reference["path"], rel)
        self.assertTrue(BIQ.is_ancestor(self.root, reference["commit"], "origin/control/a/wi-s"))
        self.assertEqual(self.repo.git("show", f"origin/control/a/wi-s:{rel}").strip(), (self.root / rel).read_text().strip())
        self.assertIn("2026-09-stale.md", self.repo.git("show", "origin/control/a/wi-s:docs/work-queue/retrospectives/README.md"))
        self.assertEqual(BIQ.verify_landing(self.root, "main", "origin/control/a/wi-s"), "control commit")
        self.repo.git("checkout", "-q", "main")

    def test_abandonment_of_a_new_packet_requires_and_records_the_retrospective(self):
        (self.root / "docs/work-queue/items/WI-WS.md").write_text(workstream_text("WI-WS", "WI-GONE"))
        self.repo.packet("WI-GONE", created=BIQ.WORKSTREAM_SINCE)
        self.repo.commit_queue(); self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, "has no retrospective naming it"):
            BIQ.abandon_item(self.root, self.packets, "WI-GONE", "David Flynn", "superseded", apply=True)
        self.assertEqual(self.packets["WI-GONE"].status, "open")
        rel = self.retrospective("WI-GONE", slug="2026-09-gone")
        self.repo.commit_queue()
        packet = BIQ.abandon_item(self.root, self.packets, "WI-GONE", "David Flynn", "superseded", apply=True)
        self.assertEqual(packet.status, "failed-abandoned")
        self.assertEqual(BIQ.retrospective_reference(packet)["path"], rel)
        self.assertEqual([e for e in BIQ.lint(self.registry, self.reload()) if "retrospective" in e], [])
        # A closed new packet without the reference is named by lint.
        self.repo.packet("WI-GONE-2", created=BIQ.WORKSTREAM_SINCE, status="failed-abandoned")
        (self.root / "docs/work-queue/items/WI-WS.md").write_text(workstream_text("WI-WS", "WI-GONE, WI-GONE-2"))
        self.assertTrue(any("WI-GONE-2: failed-abandoned without a retrospective reference" in e for e in BIQ.lint(self.registry, self.reload())))

    def test_a_checkout_runs_the_targets_copy_of_the_tool_not_its_own(self):
        """A checkout whose tools/biq.py differs from main's re-executes main's copy from the control clone."""
        marker = "MAIN-TOOL-MARKER"
        source = SCRIPT.read_text()
        tagged = source.replace('if __name__ == "__main__":', f'print("{marker}", file=sys.stderr)\nif __name__ == "__main__":', 1)
        self.assertNotEqual(tagged, source)
        (self.root / "tools").mkdir(exist_ok=True)
        (self.root / "tools/biq.py").write_text(tagged)  # main's tool carries the marker; the copy the agent runs does not
        self.repo.git("add", "tools/biq.py"); self.repo.git("commit", "-q", "-m", "tool on main"); self.repo.git("push", "-q", "origin", "main")
        self.repo.packet("WI-T"); self.repo.commit_queue()
        env = {**os.environ, "BIQ_CONTROL_DIR": str(self.root.parent / "control"), "PATH": self.git_only_path(), "BIQ_CHANGE_PROVIDER": "github"}
        env.pop(BIQ.RELOCATED_ENV, None)
        out = subprocess.run([sys.executable, str(SCRIPT), "--repository", str(self.root), "show", "WI-T"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("running origin/main's tool", out.stderr)
        self.assertIn(marker, out.stderr)  # main's copy ran
        self.assertIn("WI-T", out.stdout)
        # A pinned target keeps the local copy, and verify always runs the copy it is given.
        out = subprocess.run([sys.executable, str(SCRIPT), "--repository", str(self.root), "--target-ref", "main", "show", "WI-T"], text=True, capture_output=True, env=env)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertNotIn(marker, out.stderr)
        out = subprocess.run([sys.executable, str(SCRIPT), "--repository", str(self.root), "verify", "--base", "main~1", "--head", "main"], text=True, capture_output=True, env=env)
        self.assertNotIn(marker, out.stderr)

    def test_a_test_result_and_a_completion_are_recorded_only_under_the_runs_claim(self):
        self.submit("WI-A", {"u/a": "a"})
        self.start("U")  # claimed by "runner" with --authorized-by
        self.review_all("U", 1)
        run = BIQ.find_run(self.packets, "U", 1)
        with self.assertRaisesRegex(BIQ.BiqError, "--owner is required"):
            BIQ.record_test(self.root, self.packets, run, "u-lane", "green", None, 0.0, owner=None)
        with self.assertRaisesRegex(BIQ.BiqError, "must be claimed by 'someone-else' first"):
            BIQ.record_test(self.root, self.packets, run, "u-lane", "green", None, 0.0, owner="someone-else")
        self.assertEqual(self.reload()[run.item_id].tests["u-lane"], "pending")
        run = BIQ.record_test(self.root, self.packets, run, "u-lane", "green", None, 0.0, owner="runner")
        self.assertEqual(run.tests["u-lane"], "green")
        with self.assertRaisesRegex(BIQ.BiqError, "--owner is required"):
            BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(), reject=[], apply=True, owner=None)
        with self.assertRaisesRegex(BIQ.BiqError, "must be claimed by 'someone-else' first"):
            BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(), reject=[], apply=True, owner="someone-else")
        self.assertEqual(BIQ.run_state(self.reload()[run.item_id]), "started")
        outcome = BIQ.complete_run(self.root, self.registry, self.packets, run, self.repo.main(), reject=[], apply=True, owner="runner")
        self.assertEqual(outcome["passed_to"], "AGG")

        self.submit("WI-H", {"u/h": "h"})  # claim still held by "agent"
        shown = BIQ.render_show(self.root, self.registry, self.packets, "WI-H", self.repo.main())
        self.assertIn("handoff: claim still held by agent", shown)
        self.assertIn("next: the submission is handed off; release the member claim: python3 tools/biq.py release WI-H --owner agent", shown)
        BIQ.release_item(self.root, "WI-H", "agent")
        shown = BIQ.render_show(self.root, self.registry, self.packets, "WI-H", self.repo.main())
        self.assertIn("handoff: complete; no claim held", shown)
        self.assertIn("next: nothing; the queue runs it", shown)

    def test_dependencies_gate_the_start_and_show_names_each_dependency_state(self):
        self.repo.packet("WI-OPEN")
        self.repo.packet("WI-X", dependencies="WI-OPEN"); self.repo.commit_queue(); self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, "WI-X cannot start: WI-OPEN is open, not submitted; a WQE starts only after every dependency is submitted"):
            BIQ.claim_item(self.root, self.packets, "WI-X", "agent")
        shown = BIQ.render_show(self.root, self.registry, self.packets, "WI-X", self.repo.main())
        self.assertIn("dependencies: WI-OPEN\n  WI-OPEN: open, not submitted; not startable until it is submitted", shown)
        self.assertIn("next: not startable: WI-OPEN is open, not submitted; a WQE starts only after every dependency is submitted", shown)
        self.assertNotIn("claim WI-X", shown)
        # sealed but not submitted is still the dependency owner's work
        sha = self.repo.commit_on(self.repo.main(), "WI-OPEN", {"u/o": "o"})
        BIQ.claim_item(self.root, self.packets, "WI-OPEN", "other")
        BIQ.seal_item(self.root, self.packets, "WI-OPEN", sha, self.repo.main(), "other")
        self.repo.commit_queue(); self.reload()
        with self.assertRaisesRegex(BIQ.BiqError, "WI-OPEN is sealed, not submitted"):
            BIQ.claim_item(self.root, self.packets, "WI-X", "agent")
        # submitted: startable now, by merging the published candidate ref; its run and landing are not waited on
        BIQ.submit_item(self.root, self.registry, self.packets, "WI-OPEN", self.repo.main(), "other", apply=True)
        self.repo.commit_queue(); self.reload()
        shown = BIQ.render_show(self.root, self.registry, self.packets, "WI-X", self.repo.main())
        self.assertIn(f"  WI-OPEN: submitted; merge {BIQ.CANDIDATE_REF_PREFIX}WI-OPEN/{sha[:12]} into your branch; its integration and landing are not prerequisites", shown)
        self.assertIn("next: python3 tools/biq.py claim WI-X --owner <you>", shown)
        BIQ.claim_item(self.root, self.packets, "WI-X", "agent")
        # a dependency amended in after the claim never strands the claimant's refresh; a fresh claim is gated again
        self.repo.packet("WI-LATE"); self.repo.packet("WI-X", dependencies="WI-OPEN, WI-LATE"); self.repo.commit_queue(); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-X", "agent")
        with self.assertRaisesRegex(BIQ.BiqError, "claimed by 'agent'"):
            BIQ.claim_item(self.root, self.packets, "WI-X", "someone-else")
        BIQ.release_item(self.root, "WI-X", "agent")
        with self.assertRaisesRegex(BIQ.BiqError, "WI-X cannot start: WI-LATE is open, not submitted"):
            BIQ.claim_item(self.root, self.packets, "WI-X", "agent")
        # the other states show their action: landed, rejected without a successor, and no packet at all
        self.repo.packet("WI-MET", status="completed", candidate="c" * 40)
        self.repo.packet("WI-GONE", status="failed-rejected")
        self.repo.packet("WI-Y", dependencies="WI-MET, WI-GONE, WI-NOPE"); self.repo.commit_queue(); self.reload()
        shown = BIQ.render_show(self.root, self.registry, self.packets, "WI-Y", self.repo.main())
        self.assertIn(f"  WI-MET: landed; merge {BIQ.CANDIDATE_REF_PREFIX}WI-MET/{'c' * 12} only if your Baseline predates it", shown)
        self.assertIn("  WI-GONE: failed-rejected with no successor; not startable; the accountable human names what meets it", shown)
        self.assertIn("  WI-NOPE: not a packet; not startable; fix Dependencies as a control commit", shown)
        with self.assertRaisesRegex(BIQ.BiqError, "WI-Y cannot start: WI-GONE is failed-rejected with no successor; WI-NOPE is not a packet"):
            BIQ.claim_item(self.root, self.packets, "WI-Y", "agent")

    def test_a_submitted_packets_dependency_wait_belongs_to_the_queue(self):
        self.submit("WI-A", {"u/a": "a"})
        self.submit("WI-DOC", {"docs/x": "x"}, dependencies="WI-A")
        BIQ.release_item(self.root, "WI-DOC", "agent")
        shown = BIQ.render_show(self.root, self.registry, self.packets, "WI-DOC", self.repo.main())
        self.assertIn("landing waits for: WI-A", shown)
        self.assertIn("next: landing waits for WI-A to reach main or a run; the queue owns that wait and there is nothing for the owner to do", shown)
        self.assertNotIn("waiting for", shown)

    # -- landing and the gate ------------------------------------------------------
    def test_result_of_a_consumer_less_queue_lands_with_its_completions(self):
        self.submit("WI-P", {"p/x.patch": "x"})
        self.start("P")
        outcome = self.green("P", 1)
        self.assertIsNone(outcome["passed_to"])
        landing = BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-CI-INTEGRATION-P-REPEAT-1", self.repo.main(), apply=True)
        self.assertEqual(sorted(landing["completes"]), ["WI-CI-INTEGRATION-P-REPEAT-1", "WI-P"])
        head = self.repo.git("rev-parse", "biq/land/wi-ci-integration-p-repeat-1")
        self.assertEqual(len(self.repo.git("rev-list", "--parents", "-n", "1", head).split()), 3)  # one merge commit
        self.assertEqual(BIQ.verify_landing(self.root, self.repo.main(), head), "result of P run 1")
        published = self.repo.git("ls-remote", "origin", BIQ.CANDIDATE_REF_PREFIX + "*")
        self.assertIn(f"candidates/WI-P/{self.packets['WI-P'].candidate[:12]}", published)
        self.assertIn(f"candidates/WI-CI-INTEGRATION-P-REPEAT-1/{outcome['result'][:12]}", published)
        self.repo.git("checkout", "-q", "main")
        self.repo.git("merge", "-q", "--ff-only", head)
        self.assertEqual({p.status for p in self.reload().values() if p.item_id in landing["completes"]}, {"completed"})
        self.assertIn("Landed on `main` by WI-CI-INTEGRATION-P-REPEAT-1", self.packets["WI-P"].sections["Resolution"])

    def test_uncared_leaf_lands_directly_and_cared_leaf_is_refused(self):
        self.submit("WI-DOC", {"docs/x": "x"})
        landing = BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-DOC", self.repo.main(), apply=True)
        self.assertEqual(BIQ.verify_landing(self.root, self.repo.main(), landing["head"]), "leaf WI-DOC that no queue cares about")
        self.submit("WI-A", {"u/a": "a"})
        with self.assertRaisesRegex(BIQ.BiqError, "cared about by U"):
            BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-A", self.repo.main(), apply=False)
        cared = self.repo.commit_on(self.repo.main(), None, {"u/z": "z"})
        with self.assertRaisesRegex(BIQ.BiqError, "exactly one merge|neither a control commit"):
            BIQ.verify_landing(self.root, self.repo.main(), cared)
        note = validation_packet_text("WI-NOTE").replace("README.md#owner", "README").replace("tools/check.py", "README")
        control = self.repo.commit_on(self.repo.main(), None, {"docs/work-queue/items/WI-NOTE.md": note})
        self.assertEqual(BIQ.verify_landing(self.root, self.repo.main(), control), "control commit")
        broken = self.repo.commit_on(self.repo.main(), None, {"docs/work-queue/items/WI-NOTE.md": BIQ.set_field(note, "Dependencies", "WI-NOPE")})
        with self.assertRaisesRegex(BIQ.BiqError, "WI-NOPE"):
            BIQ.verify_landing(self.root, self.repo.main(), broken)

    def test_dependency_on_unlanded_work_blocks_a_direct_landing(self):
        self.submit("WI-A", {"u/a": "a"})
        self.submit("WI-DOC", {"docs/x": "x"}, dependencies="WI-A")
        with self.assertRaisesRegex(BIQ.BiqError, "waits for WI-A"):
            BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-DOC", self.repo.main(), apply=False)

    def test_an_unfetchable_main_is_never_taken_for_this_checkout(self):
        # The checkout's packets equal the last fetched main, which is normally the control checkout's own state.
        self.assertIsNone(BIQ.queue_state(self.root, "origin/main")[2])
        # After a failed fetch that equality proves nothing: reads say where the state came from, writes leave the tree alone.
        self.assertEqual(BIQ.queue_state(self.root, "origin/main", fresh=False)[2], "origin/main")
        base = [sys.executable, str(SCRIPT), "--repository", str(self.root)]
        sha = self.repo.commit_on(self.repo.main(), "WI-P", {"p/y.patch": "y"})
        self.repo.packet("WI-P"); self.repo.commit_queue()
        empty = Path(self.tmp.name) / "empty.git"  # answers, but holds no main: the fetch fails, claim refs stay readable
        subprocess.run(["git", "init", "-q", "--bare", str(empty)], check=True)
        self.repo.git("remote", "set-url", "origin", str(empty))
        show = subprocess.run([*base, "show", "WI-P"], text=True, capture_output=True)
        self.assertEqual(show.returncode, 0, show.stderr)
        self.assertIn("warning: could not fetch origin/main", show.stderr)
        subprocess.run([*base, "claim", "WI-P", "--owner", "a"], text=True, capture_output=True)
        sealed = subprocess.run([*base, "seal", "WI-P", "--owner", "a", "--candidate", sha], text=True, capture_output=True)
        self.assertNotIn("Candidate commit: `" + sha, (self.root / "docs/work-queue/items/WI-P.md").read_text(), sealed.stderr)
        self.assertEqual(self.repo.git("status", "--porcelain"), "", sealed.stderr)

    def test_required_reading_must_name_an_existing_heading(self):
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"docs/guide.md": "# Guide\n\n## Operator order: the tool enforces\n\n```\n## not a heading\n```\n\n## Duplicate\n\n## Duplicate\n"}))
        self.assertEqual(BIQ.markdown_anchors((self.root / "docs/guide.md").read_text()),
                         {"guide", "operator-order-the-tool-enforces", "duplicate", "duplicate-1"})
        reading = "\n## Required reading\n\n- governing-authority: `docs/guide.md#{}`\n- verification: `docs/{}.md#guide`\n"
        good = validation_packet_text("WI-GOOD").replace("origin/main " + "a" * 40, "origin/main " + self.repo.main()).replace("README.md#owner", "docs/guide.md#operator-order-the-tool-enforces").replace("tools/check.py", "docs/guide.md#guide")
        bad = packet_text("WI-BAD").replace("\n## Resolution", reading.format("batch-integration-v2", "missing") + "\n## Resolution", 1)
        done = ("# WI-DONE: historical record\n\n- ID: `WI-DONE`\n- Status: `completed`\n- Kind: `documentation`\n"
                "- Level of effort: `low`\n- Area: `historical owner`\n- Created: `2026-08-01`\n- Baseline: `historical baseline`\n"
                "- Architecture gate: `not-required`\n- Dependencies: `none`\n\n## Objective\n\nHistorical work.\n"
                + reading.format("nope", "missing") + "\n## Resolution\n\nDone.\n")
        for item_id, text in (("WI-GOOD", good), ("WI-BAD", bad), ("WI-DONE", done)):
            (self.root / f"docs/work-queue/items/{item_id}.md").write_text(text)
        errors = BIQ.reference_errors(self.root, self.reload(), None)
        self.assertEqual(errors, ["WI-BAD: governing-authority docs/guide.md#batch-integration-v2 names no heading there",
                                  "WI-BAD: verification must name an existing input (docs/missing.md); use creates-output for a path this WQE will create"])
        # The gate refuses the new error; an unversioned terminal packet's stale reading is history.
        base = self.repo.main()
        head = self.repo.commit_queue()
        with self.assertRaisesRegex(BIQ.BiqError, "WI-BAD: governing-authority docs/guide.md#batch-integration-v2 names no heading"):
            BIQ.verify_landing(self.root, base, head)
        (self.root / "docs/work-queue/items/WI-BAD.md").unlink()
        self.assertEqual(BIQ.verify_landing(self.root, base, self.repo.commit_queue()), "control commit")

    def test_a_claim_on_a_terminal_packet_is_released_by_the_next_command(self):
        self.repo.packet("WI-LIVE"); self.repo.packet("WI-DONE"); self.repo.commit_queue(); self.reload()
        BIQ.claim_item(self.root, self.packets, "WI-LIVE", "a")
        BIQ.claim_item(self.root, self.packets, "WI-DONE", "b")
        self.assertEqual(BIQ.release_stale_claims(self.root, self.packets), [])
        self.repo.packet("WI-DONE", status="completed"); self.repo.commit_queue()
        self.assertEqual(BIQ.release_stale_claims(self.root, self.reload()), ["WI-DONE"])
        self.assertIsNone(BIQ.read_claim(self.root, "WI-DONE")[1])
        self.assertEqual(BIQ.read_claim(self.root, "WI-LIVE")[1]["owner"], "a")
        # Any command does it, and says so.
        BIQ.claim_item(self.root, self.packets, "WI-DONE", "b")
        out = subprocess.run([sys.executable, str(SCRIPT), "--repository", str(self.root), "show", "WI-LIVE"], text=True, capture_output=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("released the claim of WI-DONE: origin/main shows it completed", out.stderr)
        self.assertIsNone(BIQ.read_claim(self.root, "WI-DONE")[1])

    def test_every_command_says_how_far_behind_main_this_checkout_is(self):
        base = [sys.executable, str(SCRIPT), "--repository", str(self.root)]
        old = self.repo.main()
        self.repo.packet("WI-P"); self.repo.commit_queue()
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"docs/newer": "x"}))
        current = subprocess.run([*base, "show", "WI-P"], text=True, capture_output=True)
        self.assertEqual(current.returncode, 0, current.stderr)
        self.assertNotIn("behind origin/main", current.stderr)
        self.repo.git("checkout", "-q", "--detach", old)
        stale = subprocess.run([*base, "show", "WI-P"], text=True, capture_output=True)
        self.assertEqual(stale.returncode, 0, stale.stderr)
        self.assertIn("this checkout is 3 commits behind origin/main: its docs, tools and source are not the current system", stale.stderr)
        self.repo.git("checkout", "-q", "main")
    def test_a_landing_keeps_both_sides_of_the_retrospective_index(self):
        index = "docs/work-queue/retrospectives/README.md"
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {index: "# Retrospectives\n\n- [one](one.md)\n"}))
        base = self.repo.main()
        # Two owners each append a line; the first lands, the second's candidate now conflicts on that one file.
        first = self.submit("WI-ONE", {"docs/one": "1", index: "# Retrospectives\n\n- [one](one.md)\n- [first](first.md)\n"}, start=base)
        second = self.submit("WI-TWO", {"docs/two": "2", index: "# Retrospectives\n\n- [one](one.md)\n- [second](second.md)\n"}, start=base)
        head = BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-ONE", self.repo.main(), apply=True)["head"]
        self.repo.land(head); self.reload()
        self.assertTrue(BIQ.index_only_conflict(self.root, self.repo.main(), second))
        head = BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-TWO", self.repo.main(), apply=True)["head"]
        self.assertEqual(self.repo.git("show", f"{head}:{index}"), "# Retrospectives\n\n- [one](one.md)\n- [first](first.md)\n- [second](second.md)")
        self.assertIn("no queue cares", BIQ.verify_landing(self.root, self.repo.main(), head))
        # Any other conflict, or another resolution of the index, is still refused.
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"docs/three": "main"}))
        third = self.submit("WI-THREE", {"docs/three": "mine", index: "# Retrospectives\n\n- [one](one.md)\n- [third](third.md)\n"}, start=base)
        with self.assertRaisesRegex(BIQ.BiqError, "no longer merges cleanly"):
            BIQ.prepare_landing(self.root, self.registry, self.packets, "WI-THREE", self.repo.main(), apply=True)
        self.repo.git("checkout", "-q", "--detach", head)
        (self.root / index).write_text("# Retrospectives\n\n- [second](second.md)\n")
        self.repo.git("commit", "-q", "-a", "--amend", "--no-edit")
        tampered = self.repo.git("rev-parse", "HEAD"); self.repo.git("checkout", "-q", "main")
        with self.assertRaisesRegex(BIQ.BiqError, "other than by keeping both sides"):
            BIQ.verify_landing(self.root, self.repo.main(), tampered)

    def test_supported_accounting_families_are_checked_at_the_landing_gate(self):
        receipt = ReceiptValidationTest()
        samples = [receipt.packet(receipt.measured(), provenance=True),
                   receipt.packet({"schema": 1, "mode": "human-only"}),
                   receipt.packet({"schema": 1, "mode": "charged-to", "item": "TD-HISTORICAL"}),
                   receipt.proposal_packet([receipt.segment(state="released"), receipt.segment(index=1)]),
                   receipt.packet(receipt.owner_loss(), item=receipt.batch, status="completed")]
        base = self.repo.main()
        for sample in samples:
            mode = BIQ.receipt_objects(sample, BIQ.USAGE_EVIDENCE_PREFIX)[0]["mode"]
            with self.subTest(mode=mode):
                text = validation_packet_text(sample.item_id).replace("README.md#owner", "README").replace("tools/check.py", "README")
                fields = dict(sample.fields)
                if sample.status in BIQ.TERMINAL:
                    fields.update({"Completed human_owner": "Example Person", "Completed host_user": "example", "Completed hostname": "synthetic",
                                   "Closed At": receipt.t1})
                if BIQ.RECEIPT_BATCH_RE.fullmatch(sample.item_id):
                    fields.update({"Kind": "integration", "Level of effort": "high", "Area": "batch integration",
                                   "Candidate base": "none", "Candidate commit": "none", "Submitted At": "none", "Submitted owner": "none"})
                    text = text.replace("- Decision owner:", "- Batch members: `WI-CI-TOOLING-MEMBER@" + "a" * 40 + "`\n- Decision owner:")
                for key, value in fields.items():
                    text = BIQ.set_field(text, key, value)
                resolution = sample.sections["Resolution"]
                files = {}
                if BIQ.receipt_proposed_id(sample.item_id):
                    resolution += receipt.line("Work-queue proposal v1: ", {"schema": 1, "producer": "WI-CI-TOOLING-PRODUCER"}) + "\n"
                    files[BIQ.ITEMS_DIR + "/WI-CI-TOOLING-PRODUCER.md"] = validation_packet_text("WI-CI-TOOLING-PRODUCER").replace("README.md#owner", "README").replace("tools/check.py", "README")
                text = text.rsplit("## Resolution", 1)[0] + "## Resolution\n\n" + resolution
                path = BIQ.ITEMS_DIR + "/" + sample.item_id + ".md"
                files[path] = text
                head = self.repo.commit_on(base, None, files)
                self.assertEqual(BIQ.verify_landing(self.root, base, head), "control commit")
                files[path] = BIQ.set_field(text, "Estimated Cost", "$999")
                head = self.repo.commit_on(base, None, files)
                with self.assertRaises(BIQ.BiqError):
                    BIQ.verify_landing(self.root, base, head)

    def test_candidate_and_lifecycle_scalars_are_checked_at_landing(self):
        receipt = ReceiptValidationTest()
        item = "WI-CI-TOOLING-LIFECYCLE"
        text = validation_packet_text(item).replace("README.md#owner", "README").replace("tools/check.py", "README")
        for key, value in {"Status": "submitted", "Candidate base": "a" * 40, "Candidate commit": "c" * 40,
                           "Submitted At": receipt.t1, "Submitted owner": "agent/example", "Claimed At": receipt.t0,
                           "Implementation Duration": "PT60S", "Tokens Used": "0", "Model Used": "none",
                           "Estimated Cost": "$0", "Estimated AWS Cost": "$0"}.items():
            text = BIQ.set_field(text, key, value)
        text += "\n" + receipt.line(BIQ.USAGE_EVIDENCE_PREFIX, {"schema": 1, "mode": "human-only"}) + "\n"
        base = self.repo.main()
        path = BIQ.ITEMS_DIR + "/" + item + ".md"
        open_seal = validation_packet_text(item).replace("README.md#owner", "README").replace("tools/check.py", "README")
        open_seal = BIQ.set_field(BIQ.set_field(open_seal, "Candidate base", "a" * 40), "Candidate commit", "c" * 40)
        head = self.repo.commit_on(base, None, {path: open_seal})
        self.assertEqual(BIQ.verify_landing(self.root, base, head), "control commit")
        lifecycle_bad = {"Implementation Duration": "PT-99S", "Claimed At": "not-a-time", "Closed At": receipt.t1}
        for admitted in (False, True):
            source = BIQ.set_field(text, "Queues", "none") if admitted else text
            head = self.repo.commit_on(base, None, {path: source})
            self.assertEqual(BIQ.verify_landing(self.root, base, head), "control commit")
            for key, value in {"Candidate base": "not-a-commit", "Submitted At": "not-a-time", "Submitted owner": "pending"}.items():
                with self.subTest(admitted=admitted, field=key):
                    head = self.repo.commit_on(base, None, {path: BIQ.set_field(source, key, value)})
                    with self.assertRaises(BIQ.BiqError):
                        BIQ.verify_landing(self.root, base, head)
            for key, value in lifecycle_bad.items():
                with self.subTest(admitted=admitted, field=key):
                    head = self.repo.commit_on(base, None, {path: BIQ.set_field(source, key, value)})
                    if admitted:
                        self.assertEqual(BIQ.verify_landing(self.root, base, head), "control commit")
                    else:
                        with self.assertRaises(BIQ.BiqError):
                            BIQ.verify_landing(self.root, base, head)
        head = self.repo.commit_on(base, None, {path: BIQ.set_field(text, "Claimed At", "2029-01-01T00:00:00Z")})
        with self.assertRaises(BIQ.BiqError):
            BIQ.verify_landing(self.root, base, head)

    def test_historical_immediate_exception_requires_valid_passive_routes(self):
        receipt = ReceiptValidationTest()
        item = "WI-CI-TOOLING-IMMEDIATE"
        text = validation_packet_text(item).replace("README.md#owner", "README").replace("tools/check.py", "README")
        fields = {"Status": "completed", "Candidate base": "a" * 40, "Candidate commit": "c" * 40,
                  "Submitted At": receipt.t1, "Submitted owner": "agent/example", "Claimed At": receipt.t0,
                  "Closed At": receipt.t2, "Implementation Duration": "PT120S", "Tokens Used": "0", "Model Used": "none",
                  "Estimated Cost": "$0", "Estimated AWS Cost": "$0", "Completed human_owner": "Example Person",
                  "Completed host_user": "example", "Completed hostname": "synthetic"}
        for key, value in fields.items():
            text = BIQ.set_field(text, key, value)
        text += "\n" + receipt.line(BIQ.USAGE_EVIDENCE_PREFIX, {"schema": 1, "mode": "human-only"}) + "\n"
        route = {"family": "WI-CI-INTEGRATION-BATCH", "tail": "WI-CI-INTEGRATION-BATCH-REPEAT-1",
                 "filter": "tools/integration-filters/main.py", "result": {"schema": 1, "policy_commit": "9" * 40,
                 "base": "c" * 40, "head": "d" * 40, "action": "immediate", "reason": "Main accepts the landed edge now", "checks": []}}
        value = {"schema": 1, "item": item, "landed_main": "f" * 40, "submitted_packet_sha256": "a" * 64, "routes": [route]}
        prefix = "Work-queue Main immediate disposition v1: "
        path, base = BIQ.ITEMS_DIR + "/" + item + ".md", self.repo.main()
        ordinary = BIQ.set_field(text, "Implementation Duration", "PT60S")
        self.assertEqual(BIQ.verify_landing(self.root, base, self.repo.commit_on(base, None, {path: ordinary})), "control commit")
        with self.assertRaises(BIQ.BiqError):
            BIQ.verify_landing(self.root, base, self.repo.commit_on(base, None, {path: text}))
        immediate = text + receipt.line(prefix, value) + "\n"
        self.assertEqual(BIQ.verify_landing(self.root, base, self.repo.commit_on(base, None, {path: immediate})), "control commit")
        enqueue = receipt.clone(route); enqueue["result"]["action"] = "enqueue"
        for routes in ([None], [enqueue], [route, route]):
            changed = text + receipt.line(prefix, {**value, "routes": routes}) + "\n"
            with self.subTest(routes=routes), self.assertRaises(BIQ.BiqError):
                BIQ.verify_landing(self.root, base, self.repo.commit_on(base, None, {path: changed}))

    def test_historical_scalar_accounting_without_receipts_keeps_its_checks(self):
        receipt = ReceiptValidationTest()
        item = "WI-CI-TOOLING-SCALAR-HISTORY"
        text = validation_packet_text(item).replace("README.md#owner", "README").replace("tools/check.py", "README")
        text = re.sub(r"^- (Previous attempt|Next attempt|Candidate base|Candidate commit|Submitted At|Submitted owner):.*\n", "", text, flags=re.M)
        for key, value in {"Task packet": "2", "Status": "completed", "Claimed At": receipt.t0, "Closed At": receipt.t1,
                           "Implementation Duration": "PT60S", "Tokens Used": "0", "Model Used": "none", "Estimated Cost": "$0",
                           "Estimated AWS Cost": "$0", "Completed human_owner": "Example Person",
                           "Completed host_user": "example", "Completed hostname": "synthetic"}.items():
            text = BIQ.set_field(text, key, value)
        base, path = self.repo.main(), BIQ.ITEMS_DIR + "/" + item + ".md"
        self.assertEqual(BIQ.verify_landing(self.root, base, self.repo.commit_on(base, None, {path: text})), "control commit")
        absent = re.sub(r"^- (Tokens Used|Model Used|Estimated Cost|Estimated AWS Cost):.*\n", "", text, flags=re.M)
        self.assertEqual(BIQ.verify_landing(self.root, base, self.repo.commit_on(base, None, {path: absent})), "control commit")
        for key, value in {"Tokens Used": "-1", "Estimated Cost": "garbage", "Estimated AWS Cost": "garbage", "Model Used": "some-model"}.items():
            with self.subTest(field=key):
                changed = BIQ.set_field(text, key, value)
                with self.assertRaises(BIQ.BiqError):
                    BIQ.verify_landing(self.root, base, self.repo.commit_on(base, None, {path: changed}))
                admitted = BIQ.set_field(changed, "Queues", "none")
                self.assertEqual(BIQ.verify_landing(self.root, base, self.repo.commit_on(base, None, {path: admitted})), "control commit")

    # -- cli ---------------------------------------------------------------------------
    def test_cli_round_trip(self):
        base = [sys.executable, str(SCRIPT), "--repository", str(self.root), "--target-ref", "main"]
        sha = self.repo.commit_on(self.repo.main(), "WI-P", {"p/y.patch": "y"})
        self.repo.land(self.repo.commit_on(self.repo.main(), None, {"AGENTS.md": "# Start every task\n\nSynthetic contract.\n", "tools/test_biq.py": "# Synthetic verification owner.\n"}))  # a run packet's required reading
        self.repo.packet("WI-P", candidate=sha); self.repo.commit_queue()  # the minimal fixture packet is history: a control write is verified against the gate before it is pushed
        env = fake_gh_env(self.root.parent, self.root.parent / "origin.git")  # every write lands through its control pull request

        def run(*args, expect=0):
            out = subprocess.run([*base, *args], text=True, capture_output=True, env=env)
            self.assertEqual(out.returncode, expect, out.stdout + out.stderr)
            return out.stdout + out.stderr

        def synced():  # the checkout follows what landed
            self.repo.git("fetch", "-q", "origin", "main"); self.repo.git("checkout", "-q", "main"); self.repo.git("reset", "-q", "--hard", "origin/main")

        run("claim", "WI-P", "--owner", "a")
        self.assertIn("enqueued into WI-CI-INTEGRATION-P-REPEAT-1", run("submit", "WI-P", "--owner", "a", "--apply"))
        synced()
        self.assertIn("no authority to claim", run("claim", "WI-CI-INTEGRATION-P-REPEAT-1", "--owner", "a", expect=2))
        run("claim", "WI-CI-INTEGRATION-P-REPEAT-1", "--owner", "a", "--authorized-by", "Human Owner")
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "pending", "0"
        self.assertIn("opened WI-CI-INTEGRATION-P-REPEAT-2", run("run", "start", "P", "--owner", "a", "--apply", expect=BIQ.PENDING_EXIT))
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/run-p:refs/remotes/origin/control/a/run-p")
        start_tip = self.repo.git("rev-parse", "origin/control/a/run-p")
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "landed", "30"
        self.assertIn("landed on origin/main", run("run", "start", "P", "--owner", "a", "--apply"))
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/run-p:refs/remotes/origin/control/a/run-p")
        self.assertEqual(self.repo.git("rev-parse", "origin/control/a/run-p"), start_tip)
        synced()
        self.assertIn("not yet reviewed: WI-P", run("run", "test", "P", "1", "p-lane", "--result", "green", "--cost", "0.5", "--owner", "a", expect=2))
        self.assertIn("NOT REVIEWED; footprint: 1 paths", run("run", "review", "P", "1"))
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "pending", "0"
        self.assertIn("WI-P reviewed ok", run("run", "review", "P", "1", "WI-P", "--ok", "--owner", "a", "--apply", expect=BIQ.PENDING_EXIT))
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/run-p:refs/remotes/origin/control/a/run-p")
        review_tip = self.repo.git("rev-parse", "origin/control/a/run-p")
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "landed", "30"
        run("run", "review", "P", "1", "WI-P", "--ok", "--owner", "a", "--apply")
        self.repo.git("fetch", "-q", "origin", "main", "+refs/heads/control/a/run-p:refs/remotes/origin/control/a/run-p")
        self.assertEqual(self.repo.git("rev-parse", "origin/control/a/run-p"), review_tip)
        self.assertEqual(BIQ.find_run(BIQ.load_packets(self.root, "origin/main"), "P", 1).members()[0][2].count("reviewed ok by a "), 1)
        synced()
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "pending", "0"
        run("run", "test", "P", "1", "p-lane", "--result", "green", "--cost", "0.5", "--owner", "a", expect=BIQ.PENDING_EXIT)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/control/a/run-p:refs/remotes/origin/control/a/run-p")
        test_tip = self.repo.git("rev-parse", "origin/control/a/run-p")
        pending_run = BIQ.find_run(BIQ.load_packets(self.root, "origin/control/a/run-p"), "P", 1)
        self.assertEqual((pending_run.fields["Cost"], pending_run.sections["Evidence"].count("p-lane: green")), ("$0.5", 1))
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "landed", "30"
        run("run", "test", "P", "1", "p-lane", "--result", "green", "--cost", "0.5", "--owner", "a")
        self.repo.git("fetch", "-q", "origin", "main", "+refs/heads/control/a/run-p:refs/remotes/origin/control/a/run-p")
        self.assertEqual(self.repo.git("rev-parse", "origin/control/a/run-p"), test_tip)
        landed_run = BIQ.find_run(BIQ.load_packets(self.root, "origin/main"), "P", 1)
        self.assertEqual((landed_run.fields["Cost"], landed_run.sections["Evidence"].count("p-lane: green")), ("$0.5", 1))
        synced()
        self.assertIn("land with", run("run", "complete", "P", "1", "--owner", "a", "--apply"))
        self.assertIn("already submitted", run("run", "complete", "P", "1", "--owner", "a", "--apply"))
        synced()
        # A staging that no longer carries the run's result is restaged, not followed: stage while the
        # gate is pending, fix the run, and land again.
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "pending", "0"
        run("land", "WI-CI-INTEGRATION-P-REPEAT-1", "--apply", expect=BIQ.PENDING_EXIT)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/biq/land/*:refs/remotes/origin/biq/land/*")
        stale = self.repo.git("rev-parse", "origin/biq/land/wi-ci-integration-p-repeat-1")
        old_result = BIQ.find_run(BIQ.load_packets(self.root, "origin/main"), "P", 1).fields["Result commit"]
        self.assertEqual(self.repo.git("rev-parse", stale + "^2"), old_result)
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "landed", "30"
        fixed = self.repo.commit_on(old_result, None, {"p/fix.patch": "fix"})
        run("run", "fix", "P", "1", "--commit", fixed, "--owner", "a", "--apply")
        run("run", "test", "P", "1", "p-lane", "--result", "green", "--cost", "0", "--owner", "a")
        self.assertIn("land with", run("run", "complete", "P", "1", "--owner", "a", "--apply"))
        synced()
        self.assertEqual(BIQ.find_run(BIQ.load_packets(self.root, "origin/main"), "P", 1).fields["Result commit"], fixed)
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "pending", "0"
        out = run("land", "WI-CI-INTEGRATION-P-REPEAT-1", "--apply", expect=BIQ.PENDING_EXIT)
        self.assertIn(f"carries {old_result[:12]}, not the current result {fixed[:12]}; restaging", out)
        self.repo.git("fetch", "-q", "origin", "+refs/heads/biq/land/*:refs/remotes/origin/biq/land/*")
        self.assertEqual(self.repo.git("rev-parse", "origin/biq/land/wi-ci-integration-p-repeat-1^2"), fixed)
        self.assertIn("already has a pending staged landing", run("land", "WI-CI-INTEGRATION-P-REPEAT-1", "--apply", expect=BIQ.PENDING_EXIT))
        env["BIQ_FAKE_GH_MODE"], env["BIQ_LANDING_WAIT_SECONDS"] = "landed", "30"
        synced(); before = self.repo.main()
        self.assertIn("biq/land/wi-ci-integration-p-repeat-1 landed on origin/main", run("land", "WI-CI-INTEGRATION-P-REPEAT-1", "--apply"))
        self.repo.git("fetch", "-q", "origin", "+refs/heads/biq/land/*:refs/remotes/origin/biq/land/*")
        self.assertIn("landing admitted: result of P run 1", run("verify", "--base", before, "--head", "origin/biq/land/wi-ci-integration-p-repeat-1"))
        synced()
        self.repo.git("push", "-q", "origin", ":refs/heads/biq/land/wi-ci-integration-p-repeat-1")
        self.assertIn("already completed", run("land", "WI-CI-INTEGRATION-P-REPEAT-1", "--apply"))
        self.assertIn("0 problems", run("lint"))
        self.assertIn("completed\tWI-CI-INTEGRATION-P-REPEAT-1", run("list"))  # the landing really landed
        self.assertIn("P (leaf, $1/run)", run("report"))


class AdoptionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.base = base
        for name in ("release", "consumer"):
            (base / name).mkdir()
        release = Repo(base / "release")
        self.repo = Repo(base / "consumer")
        files = {path: f"shared fixture: {path}\n" for path in BIQ.ADOPTION_FILES}
        files["tools/biq.py"] = ('#!/usr/bin/env python3\n# engineering-workflow v1.0.0\n'
                                 'VERSION = "1.0.0"\nREF_NAMESPACE = "upstream-work-queue"\n'
                                 'raise SystemExit("candidate code must not execute")\n')
        files["AGENTS.md"] = "# Shared contract\n\n## Start every task\n\nRead the required [local addendum](LOCAL.md) before starting work.\n"
        files["templates/verify-product-integration.yml"] = "name: verify-product-integration\nenv:\n  BIQ_REF_NAMESPACE: upstream-work-queue\n"
        files["templates/verify-product-integration.gitlab.yml"] = "variables:\n  BIQ_REF_NAMESPACE: upstream-work-queue\n"
        self.release_files = files
        self.release_sha = release.commit_on(release.main(), None, files)
        release.git("tag", "-a", "v1.0.0", "-m", "release", self.release_sha)
        self.upstream = base / "upstream"
        subprocess.run(["git", "clone", "--quiet", "--no-local", str(release.root), str(self.upstream)], check=True)
        self.installed = {installed: files[source] for source, installed in BIQ.ADOPTION_FILES.items()}
        self.installed["tools/biq.py"] = self.installed["tools/biq.py"].replace("# engineering-workflow v1.0.0\n", f"# engineering-workflow v1.0.0 {self.release_sha}\n").replace('REF_NAMESPACE = "upstream-work-queue"', 'REF_NAMESPACE = "consumer-work-queue"')
        self.installed["AGENTS.md"] = self.installed["AGENTS.md"].replace("(LOCAL.md)", "(docs/local/RULES.md)")
        workflow = ".github/workflows/verify-product-integration.yml"
        self.installed[workflow] = self.installed[workflow].replace("BIQ_REF_NAMESPACE: upstream-work-queue", "BIQ_REF_NAMESPACE: consumer-work-queue")
        self.commit = self.repo.commit_on(self.repo.main(), None, self.installed)
        self.repo.git("merge", "--ff-only", self.commit)
        self.repo.git("remote", "remove", "origin")  # adoption needs neither a remote nor any queue definition

    def check(self, tree: str | None = None, expect: int = 0, provider: str = "github") -> str:
        command = [sys.executable, str(SCRIPT), "--repository", str(self.repo.root), "adoption", "--upstream", str(self.upstream)]
        command += ["--provider", provider]
        if tree is not None:
            command += ["--tree", tree]
        result = subprocess.run(command, text=True, capture_output=True)
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, expect, output)
        return output

    def test_released_copy_all_four_bindings_passes_without_queue_setup(self):
        refs = self.repo.git("show-ref")
        self.assertIn("adoption: 0 mismatches", self.check())
        self.assertEqual(self.repo.git("show-ref"), refs)
        self.assertEqual(self.repo.git("status", "--porcelain"), "")
        # The upstream working tree is irrelevant; all comparisons use the tag's blobs.
        (self.upstream / "tools").mkdir(exist_ok=True)
        (self.upstream / "tools/biq.py").write_text("not the tagged file\n")
        self.assertEqual(BIQ.adoption_errors(self.repo.root, self.upstream), [])

    def test_adopting_examples_are_version_neutral_and_use_one_manifest(self):
        guide_path = SCRIPT.parent.parent / "ADOPTING.md"
        if not guide_path.is_file():
            self.skipTest("publisher-only ADOPTING.md is not installed in consumers")
        guide = guide_path.read_text()
        shell = "\n".join(re.findall(r"```sh\n(.*?)```", guide, re.S))
        self.assertIsNone(re.search(r"\bv[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?\b", shell))
        self.assertEqual(guide.count("while IFS='|' read -r source destination"), 1)
        manifest = re.search(r"while IFS='\|' read -r source destination; do.*?done <<EOF\n(.*?)\nEOF", guide, re.S).group(1)
        self.assertEqual(len(manifest.splitlines()), 6)
        self.assertIn("items.mkdir(parents=True, exist_ok=True)", guide)
        for heading in ("## Shared preparation", "## GitHub track", "## GitLab track", "## Shared six-file installation",
                        "## Upgrade a consumer", "## Publish a release candidate and final release"):
            self.assertIn(heading, guide)
        self.assertRegex(guide, r"first land the rule\s+change while the old gate can still admit it")

    def test_gitlab_provider_selects_its_gate_and_keeps_six_files(self):
        files = BIQ.adoption_files("gitlab")
        self.assertEqual(len(files), 6)
        self.assertEqual(files["templates/verify-product-integration.gitlab.yml"], ".gitlab-ci.yml")
        self.assertNotIn("templates/verify-product-integration.yml", files)

        gitlab_base = self.base / "gitlab-consumer"
        gitlab_base.mkdir()
        repo = Repo(gitlab_base)
        installed = {destination: self.release_files[source] for source, destination in files.items()}
        installed["tools/biq.py"] = installed["tools/biq.py"].replace(
            "# engineering-workflow v1.0.0\n", f"# engineering-workflow v1.0.0 {self.release_sha}\n"
        ).replace('REF_NAMESPACE = "upstream-work-queue"', 'REF_NAMESPACE = "consumer-work-queue"')
        installed["AGENTS.md"] = installed["AGENTS.md"].replace("(LOCAL.md)", "(docs/local/RULES.md)")
        installed[".gitlab-ci.yml"] = installed[".gitlab-ci.yml"].replace(
            "BIQ_REF_NAMESPACE: upstream-work-queue", "BIQ_REF_NAMESPACE: consumer-work-queue"
        )
        commit = repo.commit_on(repo.main(), None, installed)
        repo.git("merge", "--ff-only", commit)
        self.assertEqual(BIQ.adoption_errors(repo.root, self.upstream, provider="gitlab"), [])

        changed = repo.commit_on(commit, None, {".gitlab-ci.yml": installed[".gitlab-ci.yml"] + "changed\n"})
        errors = BIQ.adoption_errors(repo.root, self.upstream, changed, "gitlab")
        self.assertTrue(any(error.startswith(".gitlab-ci.yml: bytes differ") for error in errors), errors)

    def test_gitlab_gate_requires_merged_result_identity(self):
        source_gate = SCRIPT.parent.parent / "templates" / "verify-product-integration.gitlab.yml"
        installed_gate = SCRIPT.parent.parent / ".gitlab-ci.yml"
        if not source_gate.exists() and not installed_gate.exists():
            self.skipTest("the GitLab gate is not installed in this GitHub consumer")
        gate = (source_gate if source_gate.exists() else installed_gate).read_text()
        prerequisite = "command -v git >/dev/null 2>&1"
        self.assertIn(prerequisite, gate)
        self.assertIn("error: Git is required by verify-product-integration", gate)
        self.assertLess(gate.index(prerequisite), gate.index('test "${CI_PIPELINE_SOURCE}"'))
        self.assertIn("CI_MERGE_REQUEST_EVENT_TYPE", gate)
        self.assertIn("CI_MERGE_REQUEST_SOURCE_BRANCH_SHA", gate)
        self.assertIn("CI_MERGE_REQUEST_TARGET_BRANCH_SHA", gate)
        self.assertIn('remote_source_ref="refs/merge-requests/${CI_MERGE_REQUEST_IID}/head"', gate)
        self.assertIn('remote_landing_ref="refs/merge-requests/${CI_MERGE_REQUEST_IID}/merge"', gate)
        self.assertIn('test "${CI_MERGE_REQUEST_REF_PATH}" = "${remote_source_ref}"', gate)
        self.assertIn('"+${remote_source_ref}:${source_ref}"', gate)
        self.assertIn('"+${remote_landing_ref}:${landing_ref}"', gate)
        self.assertIn('test "${head_sha}" = "${CI_COMMIT_SHA}"', gate)
        self.assertIn('test "$(git rev-parse "${head_sha}^1")" = "${target_sha}"', gate)
        self.assertIn('test "$(git rev-parse "${head_sha}^2")" = "${source_sha}"', gate)
        self.assertIn('verification_head="${head_sha}"', gate)
        self.assertIn('source_first_parent="$(git rev-parse --verify "${source_sha}^1" 2>/dev/null)"', gate)
        self.assertIn('git rev-parse --verify "${source_sha}^2" >/dev/null 2>&1', gate)
        self.assertIn('test "${source_first_parent}" = "${target_sha}"', gate)
        self.assertIn('verification_head="${source_sha}"', gate)
        self.assertIn('verify --base "${target_sha}" --head "${verification_head}"', gate)

    def test_changed_byte_is_named_at_candidate_while_main_passes(self):
        path = "tools/test_biq.py"
        candidate = self.repo.commit_on(self.commit, None, {path: self.installed[path] + "changed byte\n"})
        self.check()  # HEAD is still main, whose installed files are clean
        self.assertIn(path + ": bytes differ", self.check(candidate, expect=2))

    def test_moved_tag_is_named(self):
        BIQ.git(self.upstream, "tag", "-f", "v1.0.0", "HEAD")
        self.assertIn("moved tag or incorrect SHA", self.check(expect=2))

    def test_unlisted_difference_and_every_changed_file_are_named(self):
        paths = ["AGENTS.md", "docs/work-queue/TEMPLATE.md", "docs/work-queue/retrospectives/TEMPLATE.md"]
        edits = {path: self.installed[path] + "unlisted local rule\n" for path in paths}
        candidate = self.repo.commit_on(self.commit, None, edits)
        output = self.check(candidate, expect=2)
        for path in paths:
            self.assertIn(path + ": bytes differ", output)
        self.assertIn("adoption: 3 mismatches", output)

    def test_unreleased_unparsable_and_duplicate_headers_are_named(self):
        path = "tools/biq.py"
        released = f"# engineering-workflow v1.0.0 {self.release_sha}"
        for header in ("# engineering-workflow unreleased", "# engineering-workflow v1.0.0 not-a-sha", released + "\n" + released):
            with self.subTest(header=header):
                candidate = self.repo.commit_on(self.commit, None, {path: self.installed[path].replace(released, header)})
                self.assertIn("tools/biq.py: unreleased or unparsable release header", self.check(candidate, expect=2))

    def test_bindings_must_remain_single_literal_lines(self):
        cases = {
            "tools/biq.py": self.installed["tools/biq.py"].replace('REF_NAMESPACE = "consumer-work-queue"', 'REF_NAMESPACE = "consumer-work-queue"; print("extra code")'),
            "AGENTS.md": self.installed["AGENTS.md"] + "Read the required [local addendum](SECOND.md) before starting work.\n",
            ".github/workflows/verify-product-integration.yml": self.installed[".github/workflows/verify-product-integration.yml"] + "  BIQ_REF_NAMESPACE: second\n",
        }
        for path, contents in cases.items():
            with self.subTest(path=path):
                candidate = self.repo.commit_on(self.commit, None, {path: contents})
                self.assertIn("expected exactly one literal adoption binding", self.check(candidate, expect=2))

    def test_file_modes_missing_files_and_symlinks_are_named(self):
        path = "tools/test_biq.py"
        self.repo.git("update-index", "--chmod=+x", path)
        self.repo.git("commit", "-q", "-m", "change file mode")
        self.assertIn(path + ": file mode", self.check(expect=2))
        # Use the fixture index to model a missing installed file and a symlink,
        # without dereferencing anything outside this disposable repository.
        self.repo.git("update-index", "--force-remove", path)
        self.repo.git("commit", "-q", "-m", "missing installed file")
        self.assertIn(path + ": missing installed file", self.check(expect=2))
        blob = self.repo.git("rev-parse", f"{self.commit}:{path}")
        self.repo.git("update-index", "--add", "--cacheinfo", f"120000,{blob},{path}")
        self.repo.git("commit", "-q", "-m", "symlink installed file")
        self.assertIn(path + ": expected a regular file", self.check(expect=2))


class PacketValidationTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "tools").mkdir()
        (self.root / "tools/check.py").write_text("pass\n")
        (self.root / "README.md").write_text("# Owner\n")

    def packet(self, item="WI-CI-TOOLING-SAMPLE", **fields):
        text = validation_packet_text(item)
        if fields.get("Status") in BIQ.TERMINAL:
            fields = {"Completed human_owner": "Example Person", "Completed host_user": "example", "Completed hostname": "example-host",
                      "Candidate base": "none", "Candidate commit": "none", "Submitted At": "none", "Submitted owner": "none", "Queues": "none", **fields}
        for key, value in fields.items():
            if key in {"Container mode", "Container members"}:
                text = text.replace("- Decision owner:", f"- {key}: `{value}`\n- Decision owner:")
            else:
                text = BIQ.set_field(text, key, value)
        return BIQ.parse_packet(item, text)

    def errors(self, packet):
        return BIQ.packet_contract_errors(packet) + BIQ.packet_reference_errors(self.root, {packet.item_id: packet}, None)

    def test_valid_dates_baselines_and_fenced_examples(self):
        for fields in ({}, {"Created": "2026-08-01"}, {"Baseline": "qualified-local-baseline"}):
            self.assertEqual(self.errors(self.packet(**fields)), [])
        packet = self.packet()
        for marker in ("```", "~~~~", "````"):
            source = packet.source.replace("## Objective", f"{marker}text\n- Accountable owner: `pending`\n## False section\n{marker}\n\n## Objective", 1)
            self.assertEqual(self.errors(BIQ.parse_packet(packet.item_id, source)), [])

    def test_required_scalar_section_and_target_failures(self):
        packet = self.packet()
        cases = {
            "missing scalar": packet.source.replace("- Accountable owner: `Example Person`\n", ""),
            "duplicate scalar": packet.source.replace("- Accountable owner: `Example Person`", "- Accountable owner: `Example Person`\n- Accountable owner: `Example Person`"),
            "placeholder owner": packet.source.replace("- Accountable owner: `Example Person`", "- Accountable owner: `pending`"),
            "missing section": packet.source.replace("## Allowed source/build scope\n\nCheck the scoped synthetic contract.\n\n", ""),
            "empty section": packet.source.replace("## Allowed source/build scope\n\nCheck the scoped synthetic contract.", "## Allowed source/build scope"),
            "duplicate section": packet.source + "\n## Objective\n\nAnother objective.\n",
            "untyped reading": packet.source.replace("- verification: `tools/check.py`", "- `tools/check.py`"),
            "missing code file": packet.source.replace("`tools/check.py`", "`tools/missing.py`"),
            "invalid date": packet.source.replace("2026-08-01T00:00:00Z", "2026-99-01T00:00:00Z"),
        }
        for name, source in cases.items():
            with self.subTest(case=name):
                self.assertTrue(self.errors(BIQ.parse_packet(packet.item_id, source)))

    def test_required_reading_distinguishes_existing_inputs_and_created_outputs(self):
        packet = self.packet()
        planned = BIQ.parse_packet(packet.item_id, packet.source.replace(
            "- verification: `tools/check.py`", "- verification: `tools/check.py`\n- creates-output: `docs/generated.md`"))
        self.assertEqual(self.errors(planned), [])
        missing = BIQ.parse_packet(packet.item_id, packet.source.replace("`tools/check.py`", "`tools/missing.py`"))
        self.assertTrue(any("verification must name an existing input (tools/missing.py); use creates-output" in error for error in self.errors(missing)))
        malformed = BIQ.parse_packet(packet.item_id, packet.source.replace("- verification: `tools/check.py`", "- prerequisite-contract: `tools/check.py`"))
        message = "; ".join(self.errors(malformed))
        self.assertIn("role is governing-authority, task-design, verification, historical-evidence, creates-output", message)

    def test_only_historical_evidence_may_name_a_work_item_packet(self):
        packet = self.packet()
        target = "docs/work-queue/items/WI-CI-QUALIFICATION-HISTORY.md"
        for role, suffix in (("governing-authority", "#objective"), ("task-design", "#objective"), ("verification", "")):
            with self.subTest(role=role):
                source = packet.source.replace(
                    "- verification: `tools/check.py`",
                    f"- verification: `tools/check.py`\n- {role}: `{target}{suffix}`",
                )
                with self.assertRaisesRegex(BIQ.BiqError, rf"{role} cannot name a work-item packet"):
                    BIQ.packet_reading(BIQ.parse_packet(packet.item_id, source))
        source = packet.source.replace(
            "- verification: `tools/check.py`",
            f"- verification: `tools/check.py`\n- historical-evidence: `{target}`",
        )
        historical = BIQ.parse_packet(packet.item_id, source)
        self.assertIn(("historical-evidence", target, None), BIQ.packet_reading(historical))
        path = self.root / target
        path.parent.mkdir(parents=True)
        path.write_text("# Historical WQE\n")
        self.assertEqual(self.errors(historical), [])

    def test_architecture_metadata_obeys_fences_and_header_order(self):
        (self.root / "docs/specs").mkdir(parents=True)
        path = self.root / "docs/specs/design.md"
        header = "# Design\n\n- **Decision status:** `accepted`\n- **Decision owner:** `Example Person`\n\n"
        body = "## Interface\n\nThe bounded design.\n"
        packet = self.packet(**{"Architecture gate": "accepted", "Architecture decision": "docs/specs/design.md#interface"})
        packet = BIQ.parse_packet(packet.item_id, packet.source.replace("## Required reading\n", "## Required reading\n\n- task-design: `docs/specs/design.md#interface`\n"))
        path.write_text(header + "```md\n- **Decision status:** `proposed`\n- **Decision owner:** `Different Person`\n```\n\n" + body)
        self.assertEqual(self.errors(packet), [])
        for text in (header.replace("`accepted`", "`proposed`") + body,
                     body + header,
                     header.replace("- **Decision status:** `accepted`", "- **Decision status:** `accepted`\n- **Decision status:** malformed") + body,
                     header.replace("- **Decision status:** `accepted`\n- **Decision owner:** `Example Person`", "- **Decision owner:** `Example Person`\n- **Decision status:** `accepted`") + body):
            path.write_text(text)
            self.assertTrue(self.errors(packet))

    def test_graph_dependencies_and_container_ownership(self):
        a = self.packet("WI-CI-TOOLING-ALPHA")
        b = self.packet("WI-CI-TOOLING-BETA", Dependencies=a.item_id)
        stream = self.packet("WI-CI-TOOLING-STREAM", **{"Container mode": "project", "Container members": f"{a.item_id}, {b.item_id}"})
        base = {p.item_id: p for p in (a, b, stream)}
        self.assertEqual(BIQ.packet_graph_errors(base), [])
        for value in (b.item_id, f"{a.item_id}, {a.item_id}", "not-an-id"):
            changed = self.packet(b.item_id, Dependencies=value)
            self.assertTrue(BIQ.packet_graph_errors({**base, changed.item_id: changed}))
        cyclic = self.packet(a.item_id, Dependencies=b.item_id)
        self.assertTrue(BIQ.packet_graph_errors({**base, a.item_id: cyclic}))
        for members in (stream.item_id, a.item_id + ", " + a.item_id, "WI-CI-TOOLING-MISSING"):
            changed = self.packet(stream.item_id, **{"Container mode": "project", "Container members": members})
            self.assertTrue(BIQ.packet_graph_errors({**base, stream.item_id: changed}))
        second = self.packet("WI-CI-TOOLING-SECOND", **{"Container mode": "project", "Container members": a.item_id})
        self.assertTrue(BIQ.packet_graph_errors({**base, second.item_id: second}))
        op = self.packet("WI-CI-TOOLING-GROUP", **{"Container mode": "operative", "Container members": a.item_id, "Status": "completed"})
        self.assertEqual(BIQ.packet_graph_errors({**base, op.item_id: op}), [])
        other = self.packet("WI-CI-TOOLING-GROUP-TWO", **{"Container mode": "operative", "Container members": a.item_id})
        self.assertTrue(BIQ.packet_graph_errors({**base, op.item_id: op, other.item_id: other}))

    def test_restart_links_and_closed_workstream_families(self):
        old = self.packet("WI-CI-BUG-OLD", **{"Status": "failed-abandoned", "Next attempt": "WI-CI-BUG-NEW"})
        new = self.packet("WI-CI-BUG-NEW", **{"Previous attempt": old.item_id})
        new = BIQ.parse_packet(new.item_id, new.source.replace("- Dependencies:", f"- Replaces: `{old.item_id}`\n- Dependencies:"))
        dependent = self.packet("WI-CI-TOOLING-DEPENDENT", Dependencies=old.item_id)
        packets = {p.item_id: p for p in (old, new, dependent)}
        self.assertEqual(BIQ.packet_graph_errors(packets), [])
        invalid = self.packet(new.item_id, **{"Previous attempt": old.item_id})
        self.assertTrue(BIQ.packet_graph_errors({**packets, new.item_id: invalid}))
        stream = self.packet("WI-CI-TOOLING-STREAM", **{"Container mode": "project", "Container members": new.item_id, "Status": "completed"})
        self.assertTrue(BIQ.packet_graph_errors({**packets, stream.item_id: stream}))

    def test_validation_reads_historical_unquoted_fields(self):
        source = validation_packet_text("TD-HISTORICAL")
        header, body = source.split("## Objective", 1)
        source = re.sub(r"^(- [^:]+: )`([^`]*)`$", r"\1\2", header, flags=re.M) + "## Objective" + body
        path = self.root / "docs/work-queue/items/TD-HISTORICAL.md"
        path.parent.mkdir(parents=True)
        path.write_text(source)
        packet = BIQ.validation_packets(self.root, None)["TD-HISTORICAL"]
        self.assertEqual(packet.fields["Accountable owner"], "Example Person")
        self.assertEqual(BIQ.validation_errors(self.root, None), [])


class ReceiptValidationTest(unittest.TestCase):
    """Pure durable-receipt checks; no live claims, remote state or rate lookup."""
    item = "WI-CI-TOOLING-RECEIPT"
    proposal = "WI-CI-PROPOSED-TOOLING-RECEIPT"
    batch = "WI-CI-INTEGRATION-BATCH-REPEAT-1"
    t0, t1, t2, t3 = (f"2026-09-09T00:0{n}:00Z" for n in range(4))

    @staticmethod
    def clone(value):
        return json.loads(json.dumps(value))

    def snapshot(self, n, *, source="root", at=None):
        counts = {f: 0 for f in BIQ.USAGE_MODEL_COUNTER_FIELDS}
        counts.update(input_tokens=n, output_tokens=n, total_tokens=2*n)
        return {"schema": 1, "provider": "synthetic", "source_id": source, "observed_at": at or self.t0,
                "counters": {f: counts[f] for f in BIQ.USAGE_COUNTER_FIELDS}, "models": {"m": counts}}

    def measured(self, *, before=1, after=2, start=None, end=None, source="root"):
        baseline = self.snapshot(before, source=source, at=start or self.t0)
        cutoff = self.snapshot(after, source=source, at=end or self.t1)
        usage = BIQ.usage_delta(baseline, cutoff, allow_zero=True)
        rate = {"input_per_million_usd": "1", "cached_input_per_million_usd": "0.1", "output_per_million_usd": "4",
                "cache_write_multiplier": "1.25", "long_context_threshold_input_tokens": 200000,
                "long_context_input_multiplier": "2", "long_context_output_multiplier": "1.5", "source": "synthetic"}
        rates = {"basis": "synthetic", "path": BIQ.USAGE_RATE_CARD_PATH, "sha256": "a" * 64, "models": {m: rate for m in usage["models"]}}
        return {"schema": 1, "mode": "measured", "baseline": baseline, "cutoff": cutoff, "delegates": [], "delegate_segments": [],
                "usage": usage, "rates": rates, "estimated_cost": BIQ.estimate_usage_cost(usage, rates, "fixture")}

    def line(self, prefix, value):
        return prefix + json.dumps(value, sort_keys=True, separators=(",", ":"))

    def packet(self, usage=None, *, item=None, status="submitted", fields=None, extra=(), provenance=False, admitted=True):
        item = item or self.item
        metadata = {"ID": item, "Status": status, "Kind": "tooling", "Task packet": "3", "Accountable owner": "Example Person",
                    "Created": "2026-09-08T00:00:00Z", "Claimed At": self.t0, "Closed At": "pending", "Implementation Duration": "PT60S",
                    "Tokens Used": "pending", "Model Used": "pending", "Estimated Cost": "pending", "Estimated AWS Cost": "$0",
                    "Candidate base": "a" * 40, "Candidate commit": "c" * 40, "Submitted At": self.t1,
                    "Submitted owner": "agent/example", "Decision owner": "none"}
        if admitted:
            metadata["Queues"] = "none"
        if usage:
            if usage["mode"] in {"measured", "claim-segmented"}:
                summary = (str(usage["usage"]["counters"]["total_tokens"]), ", ".join(sorted(usage["usage"]["models"])) or "none", usage["estimated_cost"])
            elif usage["mode"] == "owner-loss-unavailable":
                summary = ("unavailable",) * 3
                metadata["Estimated AWS Cost"] = usage["authorization"]["aws_cost"]
            else:
                summary = ("0", "none", "$0")
            metadata.update(zip(("Tokens Used", "Model Used", "Estimated Cost"), summary))
        metadata.update(fields or {})
        lines = [self.line(BIQ.USAGE_EVIDENCE_PREFIX, usage)] if usage else []
        if provenance:
            lines.append(self.line(BIQ.SUBSTANTIVE_WORK_PROVENANCE_PREFIX, self.provenance(item)))
        source = f"# {item}: synthetic receipt\n\n" + "\n".join(f"- {k}: `{v}`" for k, v in metadata.items()) + "\n\n## Resolution\n\n" + "\n".join([*lines, *extra]) + "\n"
        return BIQ.parse_packet(item, source)

    def provenance(self, item=None):
        return {"schema": 1, "item": item or self.item, "role": "implementation", "human_owner": "Example Person",
                "owner": "agent/example", "claim_head": "b" * 40, "recorded_at": self.t1}

    def assert_valid(self, packet):
        self.assertEqual(BIQ.receipt_validation_errors(packet), [])

    def assert_invalid(self, packet, message):
        errors = BIQ.receipt_validation_errors(packet)
        self.assertTrue(errors, "invalid control was accepted")
        self.assertIn(message, errors[0])

    def test_current_and_historical_measured_shapes(self):
        measured = self.measured()
        controls = [measured]
        no_segments = self.clone(measured); del no_segments["delegate_segments"]
        no_delegates = self.clone(no_segments); del no_delegates["delegates"]
        controls.extend((no_segments, no_delegates))
        cumulative = self.clone(measured)
        cumulative["delegates"] = [self.snapshot(1, source="delegate", at=self.t1)]
        cumulative["usage"] = BIQ.usage_delta(self.snapshot(0), self.snapshot(2))
        cumulative["estimated_cost"] = "$0.000010"
        bounded = self.clone(measured)
        bounded["delegate_segments"] = [{"schema": 1, "claim_started_at": self.t0, "accounting_cutoff_at": self.t1,
            "baseline": self.snapshot(1, source="delegate"), "cutoff": self.snapshot(2, source="delegate", at=self.t1), "usage": measured["usage"]}]
        bounded["usage"] = cumulative["usage"]; bounded["estimated_cost"] = "$0.000010"
        controls.extend((cumulative, bounded))
        for index, value in enumerate(controls):
            with self.subTest(shape=index):
                self.assert_valid(self.packet(value, provenance=True))
        overlap = self.clone(bounded); overlap["delegates"] = cumulative["delegates"]
        self.assert_invalid(self.packet(overlap), "duplicate measured usage source")
        bad = self.clone(bounded); bad["delegate_segments"][0]["accounting_cutoff_at"] = self.t2
        self.assert_invalid(self.packet(bad), "differs from root cutoff")
        self.assert_invalid(self.packet(bounded, fields={"Claimed At": self.t2}), "differs from Claimed At")
        bad = self.clone(measured); bad["usage"]["counters"]["total_tokens"] += 1
        self.assert_invalid(self.packet(bad), "stored usage delta")
        bad = self.clone(measured); bad["estimated_cost"] = "$99"
        self.assert_invalid(self.packet(bad), "stored usage cost")
        self.assert_invalid(self.packet(measured, fields={"Tokens Used": "99"}), "accounting summary")
        self.assert_invalid(self.packet(measured, status="open"), "only a sealed item")
        self.assert_invalid(self.packet(measured, extra=[self.line(BIQ.USAGE_EVIDENCE_PREFIX, measured)]), "duplicate")

    def test_usage_timestamps_and_embedded_rates(self):
        measured = self.measured(start="2026-09-09T00:00:00+00:00")
        self.assert_valid(self.packet(measured))
        bad = self.clone(measured); bad["baseline"]["observed_at"] = "not-a-date"
        self.assert_invalid(self.packet(bad), "invalid UTC timestamp")
        with self.assertRaises(BIQ.BiqError):
            BIQ.validate_usage_snapshot(bad["baseline"], "invalid input")
        # A broader live configuration remains valid for pricing a selected model;
        # an embedded receipt must instead name exactly the measured model set.
        broader = self.clone(measured["rates"]); broader["models"]["unused"] = broader["models"]["m"]
        self.assertEqual(BIQ.estimate_usage_cost(measured["usage"], broader, "live configuration"), "$0.000005")
        bad = self.clone(measured); bad["rates"] = broader
        self.assert_invalid(self.packet(bad), "rate models differ")
        bad = self.clone(measured); bad["rates"] = None
        self.assert_invalid(self.packet(bad), "rate models differ")

    def test_simple_modes_absence_and_provenance_binding(self):
        for usage in (None, {"schema": 1, "mode": "human-only"}, {"schema": 1, "mode": "charged-to", "item": "WI-CI-TOOLING-OTHER"}, {"schema": 1, "mode": "charged-to", "item": "TD-OTHER"}):
            self.assert_valid(self.packet(usage, provenance=True))
        self.assert_valid(self.packet(item=self.proposal))  # Missing usage remains unmeasured.
        self.assert_invalid(self.packet({"schema": 1, "mode": "charged-to", "item": self.item}), "same WQE")
        self.assert_invalid(self.packet({"schema": 1, "mode": "charged-to", "item": "WI--OTHER"}), "charged-to")
        self.assert_invalid(self.packet({"schema": 1, "mode": "human-only"}, fields={"Estimated Cost": "$1"}), "accounting summary")
        control = self.provenance()
        for field, value in (("item", "WI-CI-TOOLING-OTHER"), ("role", "integration"), ("owner", "agent/other"), ("recorded_at", self.t0), ("human_owner", "pending"), ("claim_head", "invalid")):
            changed = {**control, field: value}
            with self.subTest(field=field):
                self.assertTrue(BIQ.receipt_validation_errors(self.packet(extra=[self.line(BIQ.SUBSTANTIVE_WORK_PROVENANCE_PREFIX, changed)])))
        self.assert_invalid(self.packet(extra=[self.line(BIQ.SUBSTANTIVE_WORK_PROVENANCE_PREFIX, control)] * 2), "duplicate")
        self.assert_invalid(self.packet(extra=[BIQ.SUBSTANTIVE_WORK_PROVENANCE_PREFIX + json.dumps(control)]), "canonical compact JSON")
        self.assert_invalid(self.packet(extra=[BIQ.USAGE_EVIDENCE_PREFIX + "{"]), "malformed")
        self.assert_invalid(self.packet(provenance=True, admitted=False), "claim head")
        self.assert_valid(self.packet(provenance=True, admitted=False, fields={"Candidate commit": "b" * 40}))
        reseal = {"schema": 1, "item": self.item, "recovery_item": "WI-CI-TOOLING-RECOVERY", "recovery_owner": "agent/recovery",
                  "original_base": "a"*40, "original_candidate": "b"*40, "replacement_base": "a"*40, "replacement_candidate": "c"*40, "recovered_at": self.t2}
        prefix = "Work-queue submitted candidate reseal v1: "
        self.assert_valid(self.packet(provenance=True, admitted=False, extra=[self.line(prefix, reseal)]))
        self.assert_invalid(self.packet(provenance=True, extra=[self.line(prefix, {**reseal, "original_candidate": "bad"})]), "reseal evidence")

    def authorization(self, *, authorizer="Example Person", at=None):
        return {"schema": 1, "item": self.proposal, "authorizer": authorizer, "authorized_at": at or self.t0,
                "reason": "Continue the scoped work.", "item_identity": "100644 blob " + "d" * 40 + "\t" + BIQ.ITEMS_DIR + "/" + self.proposal + ".md"}

    def segment(self, *, index=0, state="sealed", human=False, zero=False, authorizer="Example Person"):
        start, end = (self.t0, self.t1) if index == 0 else (self.t2, self.t3)
        usage = {"schema": 1, "mode": "human-only"} if human else self.measured(before=index+1, after=index+1 if zero else index+2, start=start, end=end)
        auth = self.authorization(authorizer=authorizer)
        return {"schema": 1, "state": state, "claim_oid": str(index+1)*40, "owner": "agent/example", "head": "b"*40,
                "started_at": start, "ended_at": end, "note": "Scope finished.",
                "actor": {"human_owner": "Example Person", "host_user": "example", "hostname": "synthetic"},
                "run_authorization": auth, "item_identity": auth["item_identity"], "usage": usage}

    def segmented(self, segments):
        from decimal import Decimal
        counts = {f: 0 for f in BIQ.USAGE_COUNTER_FIELDS}; models = {}; cost = Decimal(0)
        for segment in segments:
            if segment["usage"]["mode"] != "measured":
                continue
            usage = segment["usage"]
            for field in counts:
                counts[field] += usage["usage"]["counters"][field]
            for model, values in usage["usage"]["models"].items():
                target = models.setdefault(model, {f: 0 for f in values})
                for field in target:
                    target[field] += values[field]
            cost += Decimal(usage["estimated_cost"][1:])
        return {"schema": 1, "mode": "claim-segmented", "item": self.proposal, "segments": segments,
                "usage": {"counters": counts, "models": models}, "estimated_cost": "$0" if cost == 0 else f"${cost:.6f}"}

    def proposal_packet(self, segments, *, fields=None, extra=()):
        usage = self.segmented(segments)
        duration = sum(int((BIQ.validation_time(s["ended_at"]) - BIQ.validation_time(s["started_at"])).total_seconds()) for s in segments)
        metadata = {"Submitted At": segments[-1]["ended_at"], "Implementation Duration": f"PT{duration}S"}
        metadata.update(fields or {})
        runs = [self.line(BIQ.RECEIPT_RUN_PREFIX, s["run_authorization"]) for s in segments]
        return self.packet(usage, item=self.proposal, fields=metadata, extra=[*runs, *extra])

    def test_claim_segmented_controls_and_local_limits(self):
        self.assertTrue(BIQ.receipt_proposed_id("WI-PROPOSED-LEGACY"))
        self.assertFalse(BIQ.receipt_proposed_id("WI-CI-PROPOSED-BOGUS-THING"))
        for segments in ([self.segment(human=True)], [self.segment(zero=True)], [self.segment()],
                         [self.segment(state="released"), self.segment(index=1)]):
            self.assert_valid(self.proposal_packet(segments))
        rotated = [self.segment(state="released"), self.segment(index=1, authorizer="Another Person")]
        self.assert_valid(self.proposal_packet(rotated, fields={"Accountable owner": "Another Person"}))
        # Historical authorization times/heads are not rebound to the live claim.
        changed = [self.segment()]; changed[0]["run_authorization"]["authorized_at"] = "2026-09-08T00:00:00+00:00"
        self.assert_valid(self.proposal_packet(changed))
        bad = [self.segment(state="released"), self.segment(index=1)]
        bad[1]["claim_oid"] = bad[0]["claim_oid"]
        self.assert_invalid(self.proposal_packet(bad), "overlap, repeat")
        bad = [self.segment(), self.segment(index=1)]
        self.assert_invalid(self.proposal_packet(bad), "invalid proposal claim segment")
        bad = [self.segment(state="released"), self.segment(index=1)]
        bad[1]["usage"] = self.measured(before=0, after=1, start=self.t2, end=self.t3)
        self.assert_invalid(self.proposal_packet(bad), "counters precede")
        bad = [self.segment()]; bad[0]["usage"]["rates"]["sha256"] = "invalid"
        self.assert_invalid(self.proposal_packet(bad), "proposal usage rates")
        bad = [self.segment()]; bad[0]["actor"]["human_owner"] = "pending"
        self.assert_invalid(self.proposal_packet(bad), "segment actor")
        self.assert_invalid(self.proposal_packet([self.segment()], fields={"Implementation Duration": "PT99S"}), "ordered runs and lifecycle")
        self.assert_invalid(self.packet(self.segmented([self.segment()])), "names another item")

    def test_proposal_rejection_preclaim_and_released_history(self):
        rejection = self.authorization(); rejection.pop("authorized_at")
        rejection.update(outcome="failed-rejected", rejected_at=self.t3)
        metadata = {f: "none" for f in ("Candidate base", "Candidate commit", "Submitted At", "Submitted owner", "Decision owner")}
        metadata.update({"Closed At": self.t3, "Completed human_owner": "Example Person", "Claimed At": "none", "Implementation Duration": "none"})
        line = self.line(BIQ.RECEIPT_REJECTION_PREFIX, rejection)
        self.assert_valid(self.packet({"schema": 1, "mode": "human-only"}, item=self.proposal, status="failed-rejected", fields=metadata, extra=[line]))
        segments = [self.segment(state="released")]
        metadata.update({"Claimed At": self.t0, "Implementation Duration": "PT60S"})
        runs = [self.line(BIQ.RECEIPT_RUN_PREFIX, segments[0]["run_authorization"])]
        released = self.packet(self.segmented(segments), item=self.proposal, status="failed-rejected", fields=metadata, extra=[*runs, line])
        self.assert_valid(released)  # Rejection may occur after the last release.
        bad = self.clone(segments); bad[0]["state"] = "sealed"
        self.assert_invalid(self.packet(self.segmented(bad), item=self.proposal, status="failed-rejected", fields=metadata, extra=[*runs, line]), "released history")
        self.assert_invalid(self.packet({"schema": 1, "mode": "human-only"}, item=self.proposal, status="failed-rejected", fields={**metadata, "Completed human_owner": "Another Person"}, extra=[line]), "rejection")

    def owner_loss(self):
        claim = {"item": self.batch, "oid": "d"*40, "owner": "agent/example", "head": "b"*40, "started_at": self.t0, "updated_at": self.t1,
                 "human_owner": "Example Person", "host_user": "example", "hostname": "synthetic", "worktree": "/synthetic/frozen", "branch": "frozen", "provider": "synthetic", "source_id": "root"}
        authorization = {"schema": 1, "authorizer": "Example Person", "authorized_at": self.t3, "aws_cost": "$1.25", "aws_cost_evidence": "retained-cost-record",
                         "contact_attempted_at": self.t2, "contact_evidence": "retained-contact-record", "integration_commit": "e"*40, "outcome": "completed",
                         "packet_set_sha256": "a"*64, "recovery_item": "WI-CI-TOOLING-RECOVERY", "recovery_owner": "agent/recovery",
                         "batch": claim["item"], "claim_oid": claim["oid"]}
        authorization.update({"claim_" + k: v for k, v in claim.items() if k not in {"item", "oid", "updated_at"}})
        return {"schema": 1, "mode": "owner-loss-unavailable", "baseline": self.snapshot(1), "claim": claim, "authorization": authorization,
                "unavailable_reason": "claimant-and-usage-source-unavailable", **{f: "unavailable" for f in BIQ.RECEIPT_UNAVAILABLE}}

    def test_owner_loss_exact_shape_and_local_limits(self):
        usage = self.owner_loss()
        self.assert_valid(self.packet(usage, item=self.batch, status="completed"))
        historical = self.clone(usage); historical["authorization"]["recovery_item"] = "TD-RECOVERY"
        self.assert_valid(self.packet(historical, item=self.batch, status="completed"))
        # The enclosing batch/outcome is checked by historical transition logic,
        # not by the local receipt parser.
        self.assert_valid(self.packet(usage, item="WI-CI-INTEGRATION-BATCH-REPEAT-2", status="failed-rejected"))
        self.assert_invalid(self.packet(usage, status="completed"), "terminal batch")
        self.assert_invalid(self.packet(usage, item=self.batch, status="completed", fields={"Estimated AWS Cost": "$0"}), "AWS summary")
        bad = self.clone(usage); bad["claim"]["owner"] = "agent/other"
        self.assert_invalid(self.packet(bad, item=self.batch, status="completed"), "differs from authorization")
        bad = self.clone(usage); bad["cutoff"] = None
        self.assert_invalid(self.packet(bad, item=self.batch, status="completed"), "unavailable fields")
        bad = self.clone(usage); bad["authorization"]["aws_cost"] = "$01.25"
        self.assert_invalid(self.packet(bad, item=self.batch, status="completed"), "authorization values")
        bad = self.clone(usage); bad["baseline"]["schema"] = True
        self.assert_invalid(self.packet(bad, item=self.batch, status="completed"), "canonical normalized JSON")
        bad = self.clone(usage); bad["authorization"]["authorized_at"] = self.t3.replace("Z", "+00:00")
        self.assert_invalid(self.packet(bad, item=self.batch, status="completed"), "whole-second UTC")
        packet = self.packet(usage, item=self.batch, status="completed")
        packet = BIQ.parse_packet(packet.item_id, packet.source.replace(self.line(BIQ.USAGE_EVIDENCE_PREFIX, usage), BIQ.USAGE_EVIDENCE_PREFIX + json.dumps(usage)))
        self.assert_invalid(packet, "canonical compact JSON")
        authorization = usage["authorization"]
        recovery = self.packet(item=authorization["recovery_item"], status="open", fields={"Architecture gate": "accepted"}, extra=[self.line(BIQ.RECEIPT_OWNER_LOSS_PREFIX, authorization)])
        self.assert_valid(recovery)
        self.assert_invalid(self.packet(item=authorization["recovery_item"], status="open", fields={"Architecture gate": "pending"}, extra=[self.line(BIQ.RECEIPT_OWNER_LOSS_PREFIX, authorization)]), "recovery WQE")

    def test_quarantine_authorizer_exception_is_passive_and_canonical(self):
        segment = self.segment(authorizer="Another Person")
        disposition = {"schema": 1, "container": self.proposal, "decision": "accepted", "authorizer": "Another Person", "decided_at": self.t3,
                       "group_sha256": "a"*64, "prepared_commit": "d"*40, "packet_set_sha256": "b"*64}
        line = self.line("Work-queue quarantine disposition v1: ", disposition)
        self.assert_valid(self.proposal_packet([segment], extra=[line]))
        self.assert_invalid(self.proposal_packet([segment]), "latest proposal run authorizer")
        bad = {**disposition, "prepared_commit": "none"}
        self.assert_invalid(self.proposal_packet([segment], extra=[self.line("Work-queue quarantine disposition v1: ", bad)]), "quarantine accounting")


if __name__ == "__main__":
    unittest.main()
