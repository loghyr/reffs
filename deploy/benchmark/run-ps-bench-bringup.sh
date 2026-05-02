#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Bring up the bench MDS + 10 DSes + 2 PSes (mTLS) on this host.
# Designed for experiments #5 (cross-PS coherence), #10
# (PS-vs-ec_demo throughput), #11 (multi-writer disjoint),
# #13 (adjacent writes).
#
# Pre-conditions:
#   - reffs-dev image built (deploy/benchmark/Dockerfile, via the
#     normal `make image` target).
#   - benchmark_build-vol populated (run the docker-compose
#     `builder` service if not -- this script does it for you).
#   - Host can run `sudo docker`.
#   - openssl available locally (for the mini-CA setup script).
#
# Steps:
#   1. Mint mini-CA + PS cert + extract fingerprint.
#   2. Splice the fingerprint into deploy/benchmark/mds-tls.toml.
#   3. Splice this host's IP into ps-tls-A.toml + ps-tls-B.toml.
#   4. Tear down any existing bench stack.
#   5. Run the docker-compose builder service.
#   6. Bring up MDS + 10 DSes via docker compose with the
#      mds-tls.toml override.
#   7. Launch PS A + PS B as docker run --network=host.
#   8. Wait for both PSes to log "PROXY_REGISTRATION ok"; fail
#      loudly if either stays "PS will operate without registered-
#      PS privilege".
#
# After this script returns success, the cross-PS topology is
# usable: clients can issue NFSv4.2 ops to either PS via
# 127.0.0.1:4098 (PS A) or :4099 (PS B); writes through one PS
# should be observable through the other after one MDS
# round-trip.

set -euo pipefail

# -- paths ------------------------------------------------------------
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TLS_DIR="${TLS_DIR:-/tmp/reffs_ps_tls}"
MDS_TLS_TOML="$HERE/mds-tls.toml"
PS_A_TOML="$HERE/ps-tls-A.toml"
PS_B_TOML="$HERE/ps-tls-B.toml"
COMPOSE="$HERE/docker-compose.yml"

# -- step 0: discover this host's LAN IP ------------------------------
HOST_IP=$(ip -4 addr show 2>/dev/null | awk '/inet 192\./ {print $2; exit}' \
              | cut -d/ -f1)
if [ -z "$HOST_IP" ]; then
    HOST_IP=127.0.0.1
fi
echo "[bringup] host IP: $HOST_IP"

# -- step 1: mini-CA --------------------------------------------------
echo "[bringup] minting mini-CA in $TLS_DIR..."
bash "$REPO/deploy/sanity/setup-mini-ca.sh" "$TLS_DIR"
PS_FPR=$(cat "$TLS_DIR/ps.fpr")
echo "[bringup] PS cert fingerprint: $PS_FPR"

# -- step 2: splice configs (working copies; never edit originals) ----
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
sed "s|REPLACE_WITH_PS_FINGERPRINT|$PS_FPR|" "$MDS_TLS_TOML" > "$WORK/mds-tls.toml"
sed "s|REPLACE_WITH_MDS_HOST|$HOST_IP|"      "$PS_A_TOML"    > "$WORK/ps-tls-A.toml"
sed "s|REPLACE_WITH_MDS_HOST|$HOST_IP|"      "$PS_B_TOML"    > "$WORK/ps-tls-B.toml"

# -- step 3: stop prior stack -----------------------------------------
echo "[bringup] tearing down prior bench + PS containers..."
sudo docker rm -f reffs-ps-A reffs-ps-B 2>/dev/null || true
( cd "$REPO" && sudo make -f Makefile.reffs stop-benchmark 2>/dev/null || true ) | tail -1 || true

# -- step 4: builder + bench MDS + DSes -------------------------------
echo "[bringup] running docker-compose builder..."
sudo docker compose -f "$COMPOSE" run --rm builder 2>&1 | tail -3

echo "[bringup] starting bench MDS + 10 DSes..."
# Use the TLS-enabled MDS config -- bind-mount over the default
# mds.toml inside the MDS container.
sudo docker compose -f "$COMPOSE" up -d --wait \
    --no-recreate ds0 ds1 ds2 ds3 ds4 ds5 ds6 ds7 ds8 ds9 \
    2>&1 | tail -3

# Bring MDS up separately so we can mount the TLS config and the
# mini-CA materials.  The compose definition mounts mds.toml from
# /shared/src/deploy/benchmark/mds.toml; we override by stopping
# the compose-managed mds and starting our own with a binding
# overlay.
sudo docker rm -f reffs-bench-mds 2>/dev/null || true
# Same prelude as the docker-compose mds service: state + data
# dirs must exist before reffsd reads its config (otherwise
# server_state_init fails on the missing state_file path).
sudo docker run -d --name reffs-bench-mds \
    --network benchmark_bench \
    --network-alias reffs-mds \
    --privileged \
    -p 2049:2049 \
    -v benchmark_build-vol:/shared:ro,z \
    -v "$WORK/mds-tls.toml:/etc/mds.toml:ro,z" \
    -v "$TLS_DIR:$TLS_DIR:ro,z" \
    reffs-dev:latest \
    /bin/bash -c "rm -rf /tmp/reffs_mds_data /tmp/reffs_mds_state && \
                  mkdir -p /tmp/reffs_mds_data /tmp/reffs_mds_state && \
                  exec /shared/build/src/reffsd --config=/etc/mds.toml" \
    >/dev/null

# Wait for MDS to ACTUALLY be serving compounds, not just bound.
# The kernel's listen queue accepts TCP before reffsd reads from
# the socket, so a /dev/tcp probe passes well before the dstore
# mount loop finishes (~22 s on adept for 10 DSes).  The
# authoritative signal is the "reffsd ready" log line.
echo "[bringup] waiting for MDS to finish dstore init..."
for i in $(seq 1 120); do
    if sudo docker logs reffs-bench-mds 2>&1 \
            | grep -q "reffsd ready"; then
        echo "[bringup] MDS ready (took ~${i}s)"
        break
    fi
    sleep 1
    if [ "$i" -eq 120 ]; then
        echo "FAIL: MDS never logged 'reffsd ready' after 120s"
        sudo docker logs reffs-bench-mds 2>&1 | tail -10
        exit 1
    fi
done

# -- step 5: launch PSes ----------------------------------------------
for tag in A B; do
    cfg="$WORK/ps-tls-$tag.toml"
    name=reffs-ps-$tag
    statedir=/tmp/ps_${tag}_state
    datadir=/tmp/ps_${tag}_data
    sudo rm -rf "$statedir" "$datadir"
    sudo mkdir -p "$statedir" "$datadir"
    sudo docker rm -f "$name" 2>/dev/null || true
    sudo docker run -d --name "$name" \
        --network=host \
        --privileged \
        -v benchmark_build-vol:/shared:ro,z \
        -v "$cfg:/etc/ps.toml:ro,z" \
        -v "$TLS_DIR:$TLS_DIR:ro,z" \
        -v "$datadir:$datadir:z" \
        -v "$statedir:$statedir:z" \
        reffs-dev:latest \
        /shared/build/src/reffsd --config=/etc/ps.toml >/dev/null
    echo "[bringup] PS $tag started -> reffs-ps-$tag"
done

sleep 6

# -- step 6: assert PROXY_REGISTRATION succeeded ----------------------
# A PS that gets "listener stays dark" passes the no-PROXY_REGISTRATION-
# failed check by accident (no registration was attempted because the
# upstream session itself failed).  Check both: PS-side connect, and
# MDS-side privilege grant.
fail=0
for tag in A B; do
    name=reffs-ps-$tag
    log=$(sudo docker logs "$name" 2>&1)
    if echo "$log" | grep -q "listener stays dark"; then
        echo "FAIL: PS $tag upstream session never came up:"
        echo "$log" | grep -iE "listener stays dark|create_tls failed" | head -3
        fail=$((fail + 1))
    elif echo "$log" | grep -q "PROXY_REGISTRATION failed"; then
        echo "FAIL: PS $tag PROXY_REGISTRATION failed:"
        echo "$log" | grep -i "registration\|tls" | head -5
        fail=$((fail + 1))
    elif echo "$log" | grep -q "reffsd ready"; then
        echo "[bringup] PS $tag: ready (no listener-dark, no registration failure)"
    else
        echo "FAIL: PS $tag has no 'reffsd ready' line yet"
        fail=$((fail + 1))
    fi
done

# MDS-side grant count is the authoritative check -- one log line per
# successful PROXY_REGISTRATION.  Anything less than 2 is a failure
# even if the PS-side log was clean.
mds_grants=$(sudo docker logs reffs-bench-mds 2>&1 \
                 | grep -c "PROXY_REGISTRATION: client granted PS privilege" \
                 || true)
echo "[bringup] MDS granted PS privilege $mds_grants time(s) (expect 2)"
if [ "$mds_grants" != "2" ]; then
    echo "FAIL: expected 2 PROXY_REGISTRATION grants on MDS"
    sudo docker logs reffs-bench-mds 2>&1 | grep -i "PROXY_REG\|tls_finger" | head -10
    fail=$((fail + 1))
fi

if [ $fail -gt 0 ]; then
    echo "[bringup] $fail check(s) failed -- topology not usable for cross-PS exps"
    exit 1
fi

echo "[bringup] DONE.  PS A on 127.0.0.1:4098, PS B on 127.0.0.1:4099"
echo "          MDS visible at 127.0.0.1:2049 (TLS server cert: $TLS_DIR/ca.crt)"
echo "          To talk to a PS as a client, set --mds 127.0.0.1:4098 (or :4099)"
