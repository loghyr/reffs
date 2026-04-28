#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# End-to-end PROXY_REGISTRATION-over-mTLS smoke (slice plan-1-tls.c,
# task #139).
#
# Pre-conditions:
#   - mini-CA materials in /tmp/reffs_ps_tls (run setup-mini-ca.sh
#     before this script if they aren't already there).
#   - mds.toml's [[allowed_ps]] block carries the ps.fpr value (see
#     the patched config below).
#   - ps.toml's [[proxy_mds]] block carries tls_cert / tls_key /
#     tls_ca / tls_mode / tls_insecure_no_verify (see below).
#
# What this script asserts:
#
#   1. The PS log carries a "PROXY_REGISTRATION ok" line, proving
#      the MDS handler reached NFS4_OK on the registration -- i.e.,
#      the MDS read c_tls_fingerprint from the per-fd peer cert,
#      matched it against the allowlist, transitioned the PS to
#      registered state.  Pre-#139 this line never appeared (the
#      handler returned NFS4ERR_PERM because the AUTH_SYS+plain-TCP
#      session had neither GSS principal nor TLS fingerprint).
#
#   2. The MDS log carries a "PROXY_REGISTRATION accepted"
#      diagnostic with a tls_fingerprint matching the PS cert
#      we minted in setup-mini-ca.sh.
#
# Failure modes the script surfaces explicitly:
#   - mini-CA materials missing -> bail with a setup hint.
#   - PS started without the [[proxy_mds]] tls_* keys -> the test
#     fails fast with "no TLS upstream attempted" rather than
#     racing the docker logs.
#   - MDS rejection (mismatched fingerprint, expired cert, missing
#     [[allowed_ps]] entry) -> the script prints both logs.
#
# Usage:
#   run-ps-tls-smoke.sh <mds_log> <ps_log> <expected_fpr>
#
# Caller (docker compose ps-demo-tls service) supplies log paths
# extracted via `docker logs reffs-mds > $MDS_LOG`.

set -uo pipefail

MDS_LOG="${1:?missing MDS log path}"
PS_LOG="${2:?missing PS log path}"
EXPECTED_FPR="${3:?missing expected PS cert fingerprint}"

fail() {
    echo "FAIL: $*" >&2
    echo "--- MDS log tail ---"
    tail -n 50 "$MDS_LOG" 2>/dev/null || echo "  (no MDS log)"
    echo "--- PS log tail ---"
    tail -n 50 "$PS_LOG" 2>/dev/null || echo "  (no PS log)"
    exit 1
}

# 1. MDS handler ran the success branch and granted PS privilege.
# This is the line emitted by lib/nfs4/server/proxy_registration.c
# nfs4_op_proxy_registration when the allowlist match succeeds.
if ! grep -q "PROXY_REGISTRATION: client granted PS privilege" \
		"$MDS_LOG"; then
    fail "MDS log has no 'client granted PS privilege' -- registration did not reach NFS4_OK"
fi

# 2. The grant referenced the cert fingerprint we minted, proving
# the per-fd peer-cert path (io_conn_get_peer_cert_fingerprint)
# carried our cert all the way into the allowlist check.
if ! grep -q "tls_fingerprint=$EXPECTED_FPR" "$MDS_LOG"; then
    fail "MDS log lacks the expected tls_fingerprint=$EXPECTED_FPR (allowlist mismatch?)"
fi

# 3. PS log corroborates: no "PROXY_REGISTRATION failed" line means
# the PS-side mds_session_send_proxy_registration accepted the
# reply and continued startup with registered-PS state.  A failed
# registration emits "proxy_mds[%u]: PROXY_REGISTRATION failed: %s
# -- PS will operate without registered-PS privilege" in
# src/reffsd.c (around line 749 at this commit).
if grep -q "PROXY_REGISTRATION failed" "$PS_LOG"; then
    fail "PS log shows 'PROXY_REGISTRATION failed' -- PS did not reach the registered-PS state"
fi

echo "PASS: PS registered successfully via mTLS (fingerprint $EXPECTED_FPR)"
exit 0
