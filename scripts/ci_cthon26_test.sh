#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_cthon26_test.sh -- Run nfsv42-tests (cthon26) suite against reffsd.
#
# Runs the op_* tests from ~/cthon26/nfsv42-tests against provided NFS
# mounts.  Tests are driven by the runtests script in that directory.
#
# Per-test exit codes (GNU automake convention):
#   0   PASS
#   1   FAIL
#   77  SKIP  (not a failure -- kernel/server feature unavailable)
#   99  BUG   (unexpected crash)
#
# Suite-level exit codes from runtests:
#   0   all PASS or SKIP
#   1   at least one FAIL
#   98  at least one binary missing (not built)
#   99  at least one BUG
#
# Usage:
#   scripts/ci_cthon26_test.sh --v3-mount PATH --v4-mount PATH \
#       [--cthon26-dir DIR]
#
# Prerequisites:
#   - cthon26 repo built: cd ~/cthon26/nfsv42-tests && make
#   - NFS mounts already in place (external mode)

set -euo pipefail

EXT_V3_MOUNT=""
EXT_V4_MOUNT=""
CTHON26_DIR="${CTHON26_DIR:-$HOME/cthon26/nfsv42-tests}"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--v3-mount)    EXT_V3_MOUNT="$2";  shift 2 ;;
		--v4-mount)    EXT_V4_MOUNT="$2";  shift 2 ;;
		--cthon26-dir) CTHON26_DIR="$2";   shift 2 ;;
		*) echo "ci_cthon26_test.sh: unknown option $1" >&2; exit 1 ;;
	esac
done

if [ -z "$EXT_V3_MOUNT" ] && [ -z "$EXT_V4_MOUNT" ]; then
	echo "ci_cthon26_test.sh: at least one of --v3-mount or --v4-mount required" >&2
	exit 1
fi

if [ ! -d "$CTHON26_DIR" ]; then
	echo "ci_cthon26_test.sh: cthon26 dir not found: $CTHON26_DIR" >&2
	echo "  Build it: cd ~/cthon26/nfsv42-tests && make" >&2
	exit 1
fi

if [ ! -x "$CTHON26_DIR/runtests" ]; then
	echo "ci_cthon26_test.sh: runtests not found in $CTHON26_DIR" >&2
	exit 1
fi

# Check at least one op_* binary exists.
if ! ls "$CTHON26_DIR"/op_access >/dev/null 2>&1; then
	echo "ci_cthon26_test.sh: op_* binaries not built in $CTHON26_DIR" >&2
	echo "  Build: cd $CTHON26_DIR && make" >&2
	exit 1
fi

info() { echo "[$(date +%H:%M:%S)] $*"; }

FAILED=0

# "label:rc:passed:failed:skipped"
RESULTS=()

run_cthon26() {
	local label=$1 mount_path=$2

	info "--- $label ---"

	local testdir="$mount_path/cthon26_test"
	mkdir -p "$testdir" 2>/dev/null || sudo mkdir -p "$testdir"
	chmod 777 "$testdir" 2>/dev/null || sudo chmod 777 "$testdir"

	info "Running nfsv42-tests on $testdir"

	set +e
	output=$("$CTHON26_DIR/runtests" -d "$testdir" 2>&1)
	rc=$?
	set -e

	echo "$output"

	# Parse "summary: N passed, N failed, N skipped, N missing, N bugs"
	local summary_line
	summary_line=$(echo "$output" | grep '^summary:' | tail -1)
	local npassed nfailed nskipped
	npassed=$(echo "$summary_line" | grep -oP '\d+(?= passed)')  || npassed=0
	nfailed=$(echo "$summary_line" | grep -oP '\d+(?= failed)')  || nfailed=0
	nskipped=$(echo "$summary_line" | grep -oP '\d+(?= skipped)') || nskipped=0

	RESULTS+=("${label}:${rc}:${npassed}:${nfailed}:${nskipped}")

	# Cleanup test directory
	rm -rf "$testdir" 2>/dev/null || sudo rm -rf "$testdir" 2>/dev/null || true

	# runtests exit 0 = all pass/skip, 1 = at least one fail,
	# 98 = missing binary, 99 = BUG.  All non-zero are failures here.
	if [ "$rc" -ne 0 ]; then
		info "$label: FAIL (runtests exited $rc)"
		FAILED=1
	else
		info "$label: PASS ($npassed passed, $nskipped skipped)"
	fi
}

info "=== cthon26 / nfsv42-tests Suite ==="
info "Using $CTHON26_DIR"
info ""

if [ -n "$EXT_V4_MOUNT" ]; then
	info "========== NFSv4.2 (mount: $EXT_V4_MOUNT) =========="
	run_cthon26 "NFSv4.2" "$EXT_V4_MOUNT"
	info ""
fi

if [ -n "$EXT_V3_MOUNT" ]; then
	info "========== NFSv3 (mount: $EXT_V3_MOUNT) =========="
	run_cthon26 "NFSv3" "$EXT_V3_MOUNT"
	info ""
fi

# ---------- Summary ----------

info "=== cthon26 Summary ==="
info ""
printf "  %-12s  %-4s  %-6s  %-6s  %-7s\n" "Mode" "RC" "passed" "failed" "skipped"
printf "  %-12s  %-4s  %-6s  %-6s  %-7s\n" "----" "--" "------" "------" "-------"
for entry in "${RESULTS[@]}"; do
	IFS=: read -r mode rc np nf ns <<< "$entry"
	printf "  %-12s  %-4s  %-6s  %-6s  %-7s\n" "$mode" "$rc" "$np" "$nf" "$ns"
done
info ""

if [ "$FAILED" -eq 0 ]; then
	info "=== cthon26 ALL PASSED ==="
else
	info "=== cthon26 FAILED ==="
	exit 1
fi
