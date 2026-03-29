#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

"""
Integration test for probe1 superblock management ops.

Requires a running reffsd instance. By default connects to
localhost:20490 (the probe port).

Usage:
    python3 scripts/test_sb_probe.py [--host HOST] [--port PORT]

Exit code 0 on success, 1 on failure.
"""

import sys
import os
import argparse

# Set up import path for the reffs package.
# Need build/scripts/ which has the auto-generated XDR Python bindings.
script_dir = os.path.dirname(os.path.abspath(__file__))
for candidate in [os.path.join(script_dir, '..', 'build', 'scripts'),
                  os.path.join(script_dir, '..', 'build_asan', 'scripts'),
                  script_dir]:
    candidate = os.path.abspath(candidate)
    if os.path.isfile(os.path.join(candidate, 'reffs', 'probe1_xdr_const.py')):
        if candidate not in sys.path:
            sys.path.insert(0, candidate)
        break

from reffs.probe1_xdr_const import *
from reffs.probe_client import Probe1Client

passed = 0
failed = 0


def check(condition, msg):
    global passed, failed
    if condition:
        passed += 1
        print(f"  PASS: {msg}")
    else:
        failed += 1
        print(f"  FAIL: {msg}")


def check_status_ok(status, msg):
    """Check that a probe_stat1 value or union status is PROBE1_OK."""
    check(status == PROBE1_OK, f"{msg} (status={status})")


def test_sb_list_has_root(client):
    """SB_LIST should always return at least the root sb (id=1)."""
    print("\n--- test_sb_list_has_root ---")
    res = client.sb_list()
    check_status_ok(res.slr_status, "sb_list status")
    sbs = res.slr_resok.slr_sbs
    check(len(sbs) >= 1, f"at least 1 sb in list (got {len(sbs)})")
    root = [s for s in sbs if s.psi_id == 1]
    check(len(root) == 1, "root sb (id=1) present")
    if root:
        check(root[0].psi_state == PROBE1_SB_MOUNTED,
              "root sb state is MOUNTED")


def test_sb_create(client, sb_id, path):
    """SB_CREATE should create a new sb in CREATED state."""
    print(f"\n--- test_sb_create (id={sb_id}, path={path}) ---")
    res = client.sb_create(sb_id, path, PROBE1_STORAGE_RAM)
    check_status_ok(res.scr_status, "sb_create status")
    sb = res.scr_resok
    check(sb.psi_id == sb_id, f"created sb id={sb.psi_id}")
    check(sb.psi_state == PROBE1_SB_CREATED, "state is CREATED")


def test_sb_create_duplicate(client, sb_id):
    """SB_CREATE with existing id should fail with EXIST."""
    print(f"\n--- test_sb_create_duplicate (id={sb_id}) ---")
    res = client.sb_create(sb_id, "/dup", PROBE1_STORAGE_RAM)
    check(res.scr_status == PROBE1ERR_EXIST,
          f"duplicate create returns EXIST (got {res.scr_status})")


def test_sb_get(client, sb_id, expected_state):
    """SB_GET should return the sb info with expected state."""
    print(f"\n--- test_sb_get (id={sb_id}) ---")
    res = client.sb_get(sb_id)
    check_status_ok(res.sgr_status, "sb_get status")
    sb = res.sgr_resok
    check(sb.psi_id == sb_id, f"sb id matches ({sb.psi_id})")
    check(sb.psi_state == expected_state,
          f"state is {expected_state} (got {sb.psi_state})")
    return sb


def test_sb_set_flavors(client, sb_id, flavors):
    """SB_SET_FLAVORS should set flavors on the sb."""
    flavor_names = {
        PROBE1_AUTH_SYS: 'sys',
        PROBE1_AUTH_KRB5: 'krb5',
    }
    names = [flavor_names.get(f, str(f)) for f in flavors]
    print(f"\n--- test_sb_set_flavors (id={sb_id}, flavors={names}) ---")
    res = client.sb_set_flavors(sb_id, flavors)
    check_status_ok(res, "sb_set_flavors status")

    # Verify via sb_get
    get_res = client.sb_get(sb_id)
    check_status_ok(get_res.sgr_status, "sb_get after set_flavors")
    sb = get_res.sgr_resok
    check(len(sb.psi_flavors) == len(flavors),
          f"flavor count matches ({len(sb.psi_flavors)} == {len(flavors)})")


def test_sb_mount(client, sb_id, path):
    """SB_MOUNT should transition to MOUNTED."""
    print(f"\n--- test_sb_mount (id={sb_id}, path={path}) ---")
    res = client.sb_mount(sb_id, path)
    check_status_ok(res, "sb_mount status")


def test_sb_in_list(client, sb_id, should_exist=True):
    """Verify an sb is (or isn't) in the list."""
    print(f"\n--- test_sb_in_list (id={sb_id}, expect={should_exist}) ---")
    res = client.sb_list()
    check_status_ok(res.slr_status, "sb_list status")
    sbs = res.slr_resok.slr_sbs
    found = [s for s in sbs if s.psi_id == sb_id]
    if should_exist:
        check(len(found) == 1, f"sb {sb_id} found in list")
    else:
        check(len(found) == 0, f"sb {sb_id} not in list")


def test_sb_lint_flavors(client, expected_warnings=0):
    """SB_LINT_FLAVORS should return expected warning count."""
    print(f"\n--- test_sb_lint_flavors (expect {expected_warnings} warnings) ---")
    res = client.sb_lint_flavors()
    check_status_ok(res.lfr_status, "sb_lint_flavors status")
    resok = res.lfr_resok
    check(resok.lfr_warnings == expected_warnings,
          f"warnings={resok.lfr_warnings} (expected {expected_warnings})")


def test_fs_usage_per_sb(client):
    """FS_USAGE should include per-sb breakdown."""
    print("\n--- test_fs_usage_per_sb ---")
    res = client.fs_usage()
    check_status_ok(res.fur_status, "fs_usage status")
    resok = res.fur_resok
    check(hasattr(resok, 'fur_per_sb'), "fur_per_sb field exists")
    if hasattr(resok, 'fur_per_sb'):
        check(len(resok.fur_per_sb) >= 1,
              f"at least 1 sb in per-sb usage (got {len(resok.fur_per_sb)})")


def test_nfs4_op_stats_per_sb(client):
    """NFS4_OP_STATS should include per-sb breakdown."""
    print("\n--- test_nfs4_op_stats_per_sb ---")
    res = client.nfs4_op_stats()
    check_status_ok(res.nosr_status, "nfs4_op_stats status")
    resok = res.nosr_resok
    check(hasattr(resok, 'nosr_per_sb'), "nosr_per_sb field exists")
    if hasattr(resok, 'nosr_per_sb'):
        check(len(resok.nosr_per_sb) >= 1,
              f"at least 1 sb in per-sb stats (got {len(resok.nosr_per_sb)})")


def test_sb_unmount(client, sb_id):
    """SB_UNMOUNT should transition to UNMOUNTED."""
    print(f"\n--- test_sb_unmount (id={sb_id}) ---")
    res = client.sb_unmount(sb_id)
    check_status_ok(res, "sb_unmount status")


def test_sb_destroy(client, sb_id):
    """SB_DESTROY should transition to DESTROYED."""
    print(f"\n--- test_sb_destroy (id={sb_id}) ---")
    res = client.sb_destroy(sb_id)
    check_status_ok(res, "sb_destroy status")


def main():
    parser = argparse.ArgumentParser(
        description='Integration test for probe1 SB management ops')
    parser.add_argument('--host', default='localhost')
    parser.add_argument('--port', type=int, default=PROBE_PORT)
    args = parser.parse_args()

    print(f"Connecting to {args.host}:{args.port}")
    client = Probe1Client(args.host, args.port)

    TEST_SB_ID = 42
    TEST_PATH = "/test_export"
    DEEP_PATH = "/test/deep/export"

    # Phase 1: Verify root sb exists
    test_sb_list_has_root(client)

    # Phase 2: Create lifecycle
    test_sb_create(client, TEST_SB_ID, TEST_PATH)
    test_sb_create_duplicate(client, TEST_SB_ID)
    test_sb_get(client, TEST_SB_ID, PROBE1_SB_CREATED)

    # Phase 3: Set flavors before mount
    test_sb_set_flavors(client, TEST_SB_ID,
                        [PROBE1_AUTH_SYS, PROBE1_AUTH_KRB5])

    # Phase 4: Mount
    test_sb_mount(client, TEST_SB_ID, TEST_PATH)
    test_sb_get(client, TEST_SB_ID, PROBE1_SB_MOUNTED)
    test_sb_in_list(client, TEST_SB_ID, should_exist=True)

    # Phase 5: Verify per-sb stats
    test_fs_usage_per_sb(client)
    test_nfs4_op_stats_per_sb(client)

    # Phase 6: Set root sb flavors to match child, then lint.
    # Root sb starts with empty sb_flavors (global ss_flavors is the
    # fallback). Set per-sb flavors on root to cover child's needs.
    test_sb_set_flavors(client, 1,
                        [PROBE1_AUTH_SYS, PROBE1_AUTH_KRB5,
                         PROBE1_AUTH_KRB5I, PROBE1_AUTH_KRB5P])
    test_sb_lint_flavors(client, expected_warnings=0)

    # Phase 7: Unmount + destroy
    test_sb_unmount(client, TEST_SB_ID)
    test_sb_get(client, TEST_SB_ID, PROBE1_SB_UNMOUNTED)
    test_sb_destroy(client, TEST_SB_ID)

    # Phase 8: Deep path creation (mkdir -p)
    DEEP_SB_ID = 43
    test_sb_create(client, DEEP_SB_ID, DEEP_PATH)
    test_sb_mount(client, DEEP_SB_ID, DEEP_PATH)
    test_sb_get(client, DEEP_SB_ID, PROBE1_SB_MOUNTED)
    test_sb_unmount(client, DEEP_SB_ID)
    test_sb_destroy(client, DEEP_SB_ID)

    # Summary
    total = passed + failed
    print(f"\n{'='*50}")
    print(f"Results: {passed}/{total} passed, {failed} failed")
    if failed:
        print("FAIL")
        sys.exit(1)
    else:
        print("PASS")
        sys.exit(0)


if __name__ == '__main__':
    main()
