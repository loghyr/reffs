#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Bring up the bench MDS + 10 DSes + N PSes (mTLS) on this host.
#
# N is set by the NPS environment variable (default 2).  Designed
# for experiments #5 (cross-PS coherence), #10 (PS-vs-ec_demo
# throughput), #11 (multi-writer disjoint), #13 (adjacent writes),
# and chunk-collision Track 2 (run_chunk_collision_track2.sh,
# which passes NPS=4 or NPS=8).
#
# Pre-conditions:
#   - reffs-dev image built (root Dockerfile, via `make image`).
#   - benchmark_build-vol populated (this script runs the
#     docker-compose `builder` service if not).
#   - Host can run `sudo docker`.
#   - openssl available locally (for the mini-CA setup).
#
# Steps:
#   1. Mint mini-CA + one cert per PS; extract each fingerprint.
#   2. Splice every fingerprint into a working mds-tls.toml as a
#      separate [[allowed_ps]] block.
#   3. Stamp one ps-tls.toml working copy per PS from the template.
#   4. Tear down any existing bench stack.
#   5. Run the docker-compose builder service.
#   6. Bring up MDS + 10 DSes via docker compose.
#   7. Launch PS 0..N-1 as docker run --network=host.
#   8. Assert every PS registered and the MDS granted N times.
#
# PS r (0-indexed) listens for clients on host port 4098+r; its
# own reffsd NFS port is 12058+r and probe port 20598+r.  After
# this script returns success a client can talk to PS r by
# pointing at 127.0.0.1:$((4098+r)).

set -euo pipefail

# -- config -----------------------------------------------------------
NPS="${NPS:-2}"
if ! [[ "$NPS" =~ ^[0-9]+$ ]] || [ "$NPS" -lt 1 ] || [ "$NPS" -gt 32 ]; then
    echo "FAIL: NPS must be an integer in [1, 32], got '$NPS'"
    exit 1
fi

# -- paths ------------------------------------------------------------
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TLS_DIR="${TLS_DIR:-/tmp/reffs_ps_tls}"
MDS_TLS_TOML="$HERE/mds-tls.toml"
PS_TEMPLATE="$HERE/ps-tls.toml"
COMPOSE="$HERE/docker-compose.yml"

echo "[bringup] bringing up MDS + 10 DSes + $NPS PS(es)"

# -- step 0: discover this host's LAN IP ------------------------------
HOST_IP=$(ip -4 addr show 2>/dev/null | awk '/inet 192\./ {print $2; exit}' \
              | cut -d/ -f1)
if [ -z "$HOST_IP" ]; then
    HOST_IP=127.0.0.1
fi
echo "[bringup] host IP: $HOST_IP"

# -- step 1: mini-CA + one cert per PS --------------------------------
# Each PS needs its own cert: the squat-guard in
# proxy_registration.c rejects a second PROXY_REGISTRATION from the
# same identity while the first holds a valid lease, so PSes
# sharing a fingerprint would not all register.
#
# setup-mini-ca.sh mints the CA plus one ps cert; rename that to
# PS 0's slot, then mint the rest against the same CA.
echo "[bringup] minting mini-CA + $NPS PS cert(s) in $TLS_DIR..."
bash "$REPO/deploy/sanity/setup-mini-ca.sh" "$TLS_DIR"
mv "$TLS_DIR/ps.crt" "$TLS_DIR/ps-0.crt"
mv "$TLS_DIR/ps.key" "$TLS_DIR/ps-0.key"
mv "$TLS_DIR/ps.fpr" "$TLS_DIR/ps-0.fpr"

# mint_ps_cert INDEX -- gen key, build CSR with a distinct CN,
# sign with the mini-CA, emit ps-INDEX.{crt,key,fpr}.
mint_ps_cert() {
    local idx="$1"
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 \
        -out "$TLS_DIR/ps-${idx}.key" 2>/dev/null
    local cfg
    cfg=$(mktemp)
    cat > "$cfg" <<EOF
[req]
distinguished_name = dn
prompt = no
[dn]
CN = reffs-test-PS-${idx}
EOF
    openssl req -new -key "$TLS_DIR/ps-${idx}.key" -config "$cfg" \
        -out "$TLS_DIR/ps-${idx}.csr" 2>/dev/null
    rm -f "$cfg"
    openssl x509 -req -in "$TLS_DIR/ps-${idx}.csr" \
        -CA "$TLS_DIR/ca.crt" -CAkey "$TLS_DIR/ca.key" \
        -CAcreateserial -days 7 -out "$TLS_DIR/ps-${idx}.crt" 2>/dev/null
    rm -f "$TLS_DIR/ps-${idx}.csr" "$TLS_DIR/ca.srl"
    openssl x509 -in "$TLS_DIR/ps-${idx}.crt" -noout -fingerprint -sha256 \
        | sed 's/^.*Fingerprint=//' \
        > "$TLS_DIR/ps-${idx}.fpr"
    chmod 0600 "$TLS_DIR/ps-${idx}.key"
}

for r in $(seq 1 $((NPS - 1))); do
    mint_ps_cert "$r"
done

for r in $(seq 0 $((NPS - 1))); do
    echo "[bringup] PS $r fingerprint: $(cat "$TLS_DIR/ps-${r}.fpr")"
done

# -- step 2: working mds-tls.toml with N allowlist blocks -------------
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ALLOWED_BLOCK=""
for r in $(seq 0 $((NPS - 1))); do
    fpr=$(cat "$TLS_DIR/ps-${r}.fpr")
    if [ -n "$ALLOWED_BLOCK" ]; then
        ALLOWED_BLOCK="${ALLOWED_BLOCK}"$'\n\n'
    fi
    ALLOWED_BLOCK="${ALLOWED_BLOCK}[[allowed_ps]]"$'\n'"tls_cert_fingerprint = \"$fpr\""
done

# Replace the single placeholder [[allowed_ps]] block in the
# template with the N blocks built above.
awk -v block="$ALLOWED_BLOCK" '
    /^\[\[allowed_ps\]\]/ { in_block=1 }
    in_block && /^tls_cert_fingerprint/ {
        print block
        in_block=0
        next
    }
    in_block && /^$/ { in_block=0; next }
    in_block { next }
    { print }
' "$MDS_TLS_TOML" > "$WORK/mds-tls.toml"

# -- step 3: stamp one PS config per PS -------------------------------
for r in $(seq 0 $((NPS - 1))); do
    sed -e "s|REPLACE_WITH_PS_NFS_PORT|$((12058 + r))|" \
        -e "s|REPLACE_WITH_PS_PROBE_PORT|$((20598 + r))|" \
        -e "s|REPLACE_WITH_PS_DATA|/tmp/ps_${r}_data|" \
        -e "s|REPLACE_WITH_PS_STATE|/tmp/ps_${r}_state|" \
        -e "s|REPLACE_WITH_PS_ID|$((r + 1))|" \
        -e "s|REPLACE_WITH_PS_PORT|$((4098 + r))|" \
        -e "s|REPLACE_WITH_MDS_HOST|$HOST_IP|" \
        -e "s|REPLACE_WITH_PS_CERT|$TLS_DIR/ps-${r}.crt|" \
        -e "s|REPLACE_WITH_PS_KEY|$TLS_DIR/ps-${r}.key|" \
        "$PS_TEMPLATE" > "$WORK/ps-tls-${r}.toml"
done

# -- step 4: stop prior stack -----------------------------------------
echo "[bringup] tearing down prior bench + PS containers..."
PRIOR_PS=$(sudo docker ps -a --format '{{.Names}}' \
               | grep -E '^reffs-ps-' || true)
if [ -n "$PRIOR_PS" ]; then
    # shellcheck disable=SC2086
    sudo docker rm -f $PRIOR_PS 2>/dev/null || true
fi
( cd "$REPO" && sudo make -f Makefile.reffs stop-benchmark 2>/dev/null || true ) \
    | tail -1 || true

# -- step 5: builder + bench MDS + DSes -------------------------------
echo "[bringup] running docker-compose builder..."
sudo docker compose -f "$COMPOSE" run --rm builder 2>&1 | tail -3

echo "[bringup] starting bench MDS + 10 DSes..."
sudo docker compose -f "$COMPOSE" up -d --wait \
    --no-recreate ds0 ds1 ds2 ds3 ds4 ds5 ds6 ds7 ds8 ds9 \
    2>&1 | tail -3

# Bring MDS up separately so we can mount the TLS config and the
# mini-CA materials.
sudo docker rm -f reffs-bench-mds 2>/dev/null || true
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

# Wait for the MDS to actually be serving compounds.  The kernel
# accepts TCP before reffsd reads the socket, so the authoritative
# signal is the "reffsd ready" log line.
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

# -- step 6: launch PSes ----------------------------------------------
for r in $(seq 0 $((NPS - 1))); do
    cfg="$WORK/ps-tls-${r}.toml"
    name="reffs-ps-${r}"
    statedir="/tmp/ps_${r}_state"
    datadir="/tmp/ps_${r}_data"
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
    echo "[bringup] PS $r started -> $name (listener 127.0.0.1:$((4098 + r)))"
done

sleep 6

# -- step 7: assert PROXY_REGISTRATION succeeded ----------------------
# A PS that gets "listener stays dark" passes a naive no-failure
# check by accident (no registration was attempted because the
# upstream session itself failed).  Check both: PS-side connect,
# and MDS-side privilege grant.
fail=0
for r in $(seq 0 $((NPS - 1))); do
    name="reffs-ps-${r}"
    log=$(sudo docker logs "$name" 2>&1)
    if echo "$log" | grep -q "listener stays dark"; then
        echo "FAIL: PS $r upstream session never came up:"
        echo "$log" | grep -iE "listener stays dark|create_tls failed" | head -3
        fail=$((fail + 1))
    elif echo "$log" | grep -q "PROXY_REGISTRATION failed"; then
        echo "FAIL: PS $r PROXY_REGISTRATION failed:"
        echo "$log" | grep -i "registration\|tls" | head -5
        fail=$((fail + 1))
    elif echo "$log" | grep -q "reffsd ready"; then
        echo "[bringup] PS $r: ready"
    else
        echo "FAIL: PS $r has no 'reffsd ready' line yet"
        fail=$((fail + 1))
    fi
done

# MDS-side grant count is the authoritative check -- one log line
# per successful PROXY_REGISTRATION.  Anything less than N is a
# failure even if the PS-side logs were clean.
mds_grants=$(sudo docker logs reffs-bench-mds 2>&1 \
                 | grep -c "PROXY_REGISTRATION: client granted PS privilege" \
                 || true)
echo "[bringup] MDS granted PS privilege $mds_grants time(s) (expect $NPS)"
if [ "$mds_grants" != "$NPS" ]; then
    echo "FAIL: expected $NPS PROXY_REGISTRATION grants on MDS"
    sudo docker logs reffs-bench-mds 2>&1 | grep -i "PROXY_REG\|tls_finger" | head -10
    fail=$((fail + 1))
fi

if [ $fail -gt 0 ]; then
    echo "[bringup] $fail check(s) failed -- topology not usable"
    exit 1
fi

echo "[bringup] DONE.  $NPS PS(es) up:"
for r in $(seq 0 $((NPS - 1))); do
    echo "          PS $r -> client port 127.0.0.1:$((4098 + r))"
done
echo "          MDS visible at 127.0.0.1:2049 (TLS server cert: $TLS_DIR/ca.crt)"
echo "          To talk to a PS as a client, point at 127.0.0.1:<listener port>"
