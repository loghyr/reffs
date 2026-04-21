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
	echo "ci_nfs_conformance_test.sh: nfs-conformance dir not found: $NFS_CONFORMANCE_DIR -- skipping"
	exit 0
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

# NFSv3 known-failure list (space-separated binary names, no path prefix).
#
# op_access subtest 4: access(W_OK) on a 0444 file owned by the caller.
#   POSIX says EACCES; reffs NFSv3 returns 0 (allowed).
#
# Why: inode_access_check_flags() carries REFFS_ACCESS_OWNER_OVERRIDE for
# NFSv3 ACCESS, WRITE, and CREATE (see lib/utils/identity.c).  This flag
# lets the file owner write to any regular file regardless of mode bits,
# matching the behaviour of Linux nfsd and other production NFS servers.
# Without it, git-over-NFS breaks: git does stat+access before open, and
# some git operations (e.g. pack-objects) touch files it just chmod'd
# read-only.  Whether git actually calls access(2) before writing is
# hit-or-miss, but real deployments (Hammerspace, Linux nfsd) have all
# settled on this workaround.
#
# NFSv4 is not affected: the NFSv4 ACCESS op uses inode_access_check()
# without the override flag, so op_access passes cleanly on v4 mounts.
#
# To restore strict POSIX on NFSv3 at the cost of git-over-NFS compat,
# configure with --enable-strict-posix.  The nightly does not do this
# because ci_integration_test.sh exercises git clone over both v3 and v4.
NFSv3_XFAIL="op_access"

# Return 0 if every failing binary in PROVE_OUTPUT is in XFAIL_LIST.
all_failures_are_known() {
	local prove_output=$1 xfail_list=$2

	# Extract failing binary names from the Test Summary Report block.
	# prove prints them as "./op_foo  (Wstat: ...)" in the summary.
	local failing
	failing=$(echo "$prove_output" | \
		awk '/^\.\/op_.*\(Wstat/{gsub(/^\.\//,""); gsub(/ .*/,""); print}' | \
		sort -u)

	[ -z "$failing" ] && return 1

	local known
	known=$(echo "$xfail_list" | tr ' ' '\n' | sort -u)

	# Any failing test NOT in the known list?
	local unexpected
	unexpected=$(comm -23 <(echo "$failing") <(echo "$known"))
	[ -z "$unexpected" ]
}

run_nfs_conformance() {
	local label=$1 mount_path=$2 xfail="${3:-}"

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
		if [ -n "$xfail" ] && all_failures_are_known "$output" "$xfail"; then
			info "$label: PASS ($ntests tests across $nfiles files," \
			     "$nfailed known-failure(s): $xfail)"
		else
			info "$label: FAIL (prove exited $rc, $nfailed test(s) failed)"
			FAILED=1
		fi
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
	run_nfs_conformance "NFSv3" "$EXT_V3_MOUNT" "$NFSv3_XFAIL"
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
