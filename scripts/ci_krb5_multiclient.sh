#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_krb5_multiclient.sh -- functional test for the multi-client
# Kerberos NFS driver.
#
# The driver (nfs_krb5_multiclient) owns the entire lifecycle: it
# starts an embedded mini-KDC, spawns a reffsd configured for that
# realm with a krb5 root export, provisions N principals, forks N
# krb5 client workers, and tears everything down.  This script is
# therefore thin -- it just invokes the driver at a few scales and
# checks exit codes.
#
# Usage: ci_krb5_multiclient.sh [REFFSD_BIN]
#   REFFSD_BIN  Path to the reffsd binary (default: /build/src/reffsd)
#
# Environment:
#   KRB5MC_BIG=1   also run the N=200 scale case (manual/nightly)
#
# Exit 0  = all cases passed, or skipped (krb5kdc not installed).
# Exit !0 = a case failed.

set -euo pipefail

REFFSD_BIN=${1:-/build/src/reffsd}
BUILD_DIR=$(dirname "$(dirname "$REFFSD_BIN")")
DRIVER=$BUILD_DIR/tools/nfs_krb5_multiclient

die() {
	echo "FATAL: $*" >&2
	exit 1
}

[ -x "$REFFSD_BIN" ] || die "reffsd binary not found: $REFFSD_BIN"
[ -x "$DRIVER" ] || die "nfs_krb5_multiclient not found: $DRIVER"

# Driver exit codes: 0 pass, 1 fail, 2 usage, 77 skip (no krb5kdc).
SKIP_EXIT=77

# run_case LABEL -- driver-args...
#
# Runs the driver; a 77 from the first case means krb5kdc is absent,
# so the whole test skips cleanly.  Any other non-zero is a failure.
run_case() {
	local label=$1
	shift
	local rc=0

	echo "=== ci_krb5_multiclient: $label ==="
	set +e
	"$DRIVER" --reffsd "$REFFSD_BIN" "$@"
	rc=$?
	set -e

	if [ "$rc" -eq "$SKIP_EXIT" ]; then
		echo "SKIP: krb5kdc not available -- skipping krb5" \
			"multiclient test"
		exit 0
	fi
	[ "$rc" -eq 0 ] || die "$label failed (driver rc=$rc)"
	echo "--- $label PASS ---"
}

# Proof, then scale, then the same-principal (one GSS identity, N
# clientids) variant.  N=200 is gated behind KRB5MC_BIG -- it is the
# manual/nightly scale point, not part of the default CI budget.
run_case "proof (N=2)" --clients 2
run_case "scale (N=50)" --clients 50
run_case "same-principal (N=50)" --clients 50 --same-principal

if [ "${KRB5MC_BIG:-0}" = "1" ]; then
	run_case "big scale (N=200)" --clients 200
fi

echo
echo "ci_krb5_multiclient: all cases passed"
