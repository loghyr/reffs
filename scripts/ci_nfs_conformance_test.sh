#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_nfs_conformance_test.sh -- Run nfs-conformance suite against reffsd.
#
# Runs the op_* tests from ~/nfs-conformance against provided NFS mounts,
# driven by `make check CHECK_DIR=<path>` (prove/TAP).
#
# Usage:
#   scripts/ci_nfs_conformance_test.sh --v3-mount PATH --v4-mount PATH \
#       [--nfs-conformance-dir DIR]
#
# Prerequisites:
#   - nfs-conformance repo: cd ~/nfs-conformance && make
#   - NFS mounts already in place (external mode)
#   - prove (perl Test::Harness) installed

set -euo pipefail

EXT_V3_MOUNT=""
EXT_V4_MOUNT=""
NFS_CONFORMANCE_DIR="${NFS_CONFORMANCE_DIR:-$HOME/nfs-conformance}"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--v3-mount)             EXT_V3_MOUNT="$2";          shift 2 ;;
		--v4-mount)             EXT_V4_MOUNT="$2";          shift 2 ;;
		--nfs-conformance-dir)  NFS_CONFORMANCE_DIR="$2";   shift 2 ;;
		*) echo "ci_nfs_conformance_test.sh: unknown option $1" >&2; exit 1 ;;
	esac
done

if [ -z "$EXT_V3_MOUNT" ] && [ -z "$EXT_V4_MOUNT" ]; then
	echo "ci_nfs_conformance_test.sh: at least one of --v3-mount or --v4-mount required" >&2
	exit 1
fi

if [ ! -d "$NFS_CONFORMANCE_DIR" ]; then
	echo "ci_nfs_conformance_test.sh: nfs-conformance dir not found: $NFS_CONFORMANCE_DIR" >&2
	exit 1
fi

# Update from upstream before each run.
echo "Updating nfs-conformance in $NFS_CONFORMANCE_DIR ..."
git -C "$NFS_CONFORMANCE_DIR" pull --ff-only 2>&1 | tail -5

# Always rebuild after pulling.
echo "Building nfs-conformance in $NFS_CONFORMANCE_DIR ..."
make -C "$NFS_CONFORMANCE_DIR" 2>&1 | tail -5

if [ ! -x "$NFS_CONFORMANCE_DIR/op_access" ]; then
	echo "ci_nfs_conformance_test.sh: build failed -- op_access not found" >&2
	exit 1
fi

info() { echo "[$(date +%H:%M:%S)] $*"; }

FAILED=0

# Per-mode result: "label:rc:files:tests:failed"
RESULTS=()

run_nfs_conformance() {
	local label=$1 mount_path=$2

	info "--- $label ---"

	local testdir="$mount_path/nfs_conformance_test"
	mkdir -p "$testdir" 2>/dev/null || sudo mkdir -p "$testdir"
	chmod 777 "$testdir" 2>/dev/null || sudo chmod 777 "$testdir"

	info "Running nfs-conformance on $testdir"

	set +e
	output=$(make -C "$NFS_CONFORMANCE_DIR" check CHECK_DIR="$testdir" 2>&1)
	rc=$?
	set -e

	echo "$output"

	# Parse prove summary line: "Files=N, Tests=N, ..."
	local nfiles ntests nfailed
	nfiles=$(echo "$output" | grep -oP 'Files=\K[0-9]+' | tail -1) || nfiles=0
	ntests=$(echo "$output" | grep -oP 'Tests=\K[0-9]+' | tail -1) || ntests=0
	# Sum all "Failed: N" counts from the Test Summary Report section.
	nfailed=$(echo "$output" | grep -oP 'Failed:\s+\K[0-9]+' | \
		awk '{s+=$1} END{print s+0}') || nfailed=0

	RESULTS+=("${label}:${rc}:${nfiles}:${ntests}:${nfailed}")

	rm -rf "$testdir" 2>/dev/null || sudo rm -rf "$testdir" 2>/dev/null || true

	if [ "$rc" -ne 0 ]; then
		info "$label: FAIL (prove exited $rc, $nfailed test(s) failed)"
		FAILED=1
	else
		info "$label: PASS ($ntests tests across $nfiles files)"
	fi
}

info "=== nfs-conformance Suite ==="
info "Using $NFS_CONFORMANCE_DIR"
info ""

if [ -n "$EXT_V4_MOUNT" ]; then
	info "========== NFSv4.2 (mount: $EXT_V4_MOUNT) =========="
	run_nfs_conformance "NFSv4.2" "$EXT_V4_MOUNT"
	info ""
fi

if [ -n "$EXT_V3_MOUNT" ]; then
	info "========== NFSv3 (mount: $EXT_V3_MOUNT) =========="
	run_nfs_conformance "NFSv3" "$EXT_V3_MOUNT"
	info ""
fi

# ---------- Summary ----------

info "=== nfs-conformance Summary ==="
info ""
printf "  %-12s  %-4s  %-7s  %-7s  %-7s\n" "Mode" "RC" "files" "tests" "failed"
printf "  %-12s  %-4s  %-7s  %-7s  %-7s\n" "----" "--" "-----" "-----" "------"
for entry in "${RESULTS[@]}"; do
	IFS=: read -r mode rc nf nt nfail <<< "$entry"
	printf "  %-12s  %-4s  %-7s  %-7s  %-7s\n" "$mode" "$rc" "$nf" "$nt" "$nfail"
done
info ""

if [ "$FAILED" -eq 0 ]; then
	info "=== nfs-conformance ALL PASSED ==="
else
	info "=== nfs-conformance FAILED ==="
	exit 1
fi
