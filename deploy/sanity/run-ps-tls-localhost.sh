#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Single-host driver for the slice plan-1-tls.c PS-MDS smoke (#139).
#
# Spins up an MDS and a PS on localhost using the deploy/sanity/
# {mds,ps}-tls.toml configs, with a freshly minted mini-CA and the
# MDS [[allowed_ps]] block wired to the PS cert fingerprint.  Then
# greps both reffsd logs for the registration markers via
# run-ps-tls-smoke.sh and exits 0/non-zero.
#
# This avoids the docker-compose plumbing of the regular sanity
# stack (build images, multi-DS topology, etc.) so the smoke can
# run from any reffsd build directory in well under a minute.
#
# Usage:
#   run-ps-tls-localhost.sh <reffsd_path>
#
# Example:
#   ./deploy/sanity/run-ps-tls-localhost.sh build/src/reffsd
#
# Cleanup: kills both reffsd children on EXIT; leaves the
# /tmp/reffs_ps_tls* state directories for post-mortem inspection.

set -uo pipefail

REFFSD="${1:?usage: $0 <reffsd_path>}"
if [[ ! -x "$REFFSD" ]]; then
    echo "FAIL: reffsd not executable at $REFFSD" >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK="/tmp/reffs_ps_tls_smoke"
CA_DIR="/tmp/reffs_ps_tls"
MDS_LOG="$WORK/mds.log"
PS_LOG="$WORK/ps.log"
MDS_CONF="$WORK/mds.toml"
PS_CONF="$WORK/ps.toml"

mkdir -p "$WORK" /tmp/reffs_mds_data /tmp/reffs_mds_state \
         /tmp/reffs_ps_data /tmp/reffs_ps_state
# Wipe any prior run's state so the test starts fresh; mkdir
# afterwards so the dirs exist for the server_persist_save path.
rm -rf /tmp/reffs_mds_data/* /tmp/reffs_mds_state/* \
       /tmp/reffs_ps_data/* /tmp/reffs_ps_state/*

# ---- mini-CA ----
"$ROOT_DIR/setup-mini-ca.sh" "$CA_DIR" >/dev/null
FPR=$(cat "$CA_DIR/ps.fpr")
if [[ -z "$FPR" ]]; then
    echo "FAIL: setup-mini-ca.sh produced an empty fingerprint" >&2
    exit 1
fi

# ---- patched MDS / PS configs ----
sed "s|REPLACE_WITH_PS_FINGERPRINT|$FPR|" "$ROOT_DIR/mds-tls.toml" \
    > "$MDS_CONF"
cp "$ROOT_DIR/ps-tls.toml" "$PS_CONF"

# ---- start MDS, then PS ----
MDS_PID=
PS_PID=
cleanup() {
    [[ -n "$MDS_PID" ]] && kill "$MDS_PID" 2>/dev/null || true
    [[ -n "$PS_PID" ]] && kill "$PS_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

# Helpers: poll the kernel until a port is listening, with a 30s
# cap.  Replaces fixed sleeps so the smoke does not flake under
# load (CI with sanitizers cold-starts slower than dreamer).
wait_for_port() {
    local port=$1 deadline=$((SECONDS + 30))
    while (( SECONDS < deadline )); do
        if ss -ltn 2>/dev/null | grep -q ":$port "; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

"$REFFSD" --config="$MDS_CONF" >"$MDS_LOG" 2>&1 &
MDS_PID=$!

if ! wait_for_port 2049; then
    echo "FAIL: MDS did not bind port 2049 within 30s" >&2
    cat "$MDS_LOG"
    exit 1
fi
if ! kill -0 "$MDS_PID" 2>/dev/null; then
    echo "FAIL: MDS exited during startup" >&2
    cat "$MDS_LOG"
    exit 1
fi

"$REFFSD" --config="$PS_CONF" >"$PS_LOG" 2>&1 &
PS_PID=$!

if ! wait_for_port 4098; then
    echo "FAIL: PS did not bind port 4098 within 30s" >&2
    cat "$PS_LOG"
    exit 1
fi
# Allow a brief window for the PS-MDS PROXY_REGISTRATION compound
# to round-trip after both ports are open: the registration is sent
# from the PS startup loop AFTER the proxy listener binds, so the
# port-binding signal alone does not guarantee the registration log
# line exists yet.
sleep 1

if ! kill -0 "$PS_PID" 2>/dev/null; then
    echo "FAIL: PS exited during startup" >&2
    cat "$PS_LOG"
    exit 1
fi

# ---- assertions ----
"$ROOT_DIR/run-ps-tls-smoke.sh" "$MDS_LOG" "$PS_LOG" "$FPR"
