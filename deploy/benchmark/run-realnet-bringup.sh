#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# 3-host bringup for the PS-encoder 4-variant real-network bench
# (.claude/design/ps-encoder-bench-4variant-realnet.md prereq #3,
#  .claude/design/multi-host-bench-bringup.md for the slice plan).
#
# Topology (defaults, all overridable via env):
#
#   client host : dreamer                    -- DREAMER_HOST
#   PS     host : adept                      -- ADEPT_HOST
#   DS+MDS host : shadow                     -- SHADOW_HOST
#
# What this script does, in order:
#   1. Pre-flight  : SSH ping shadow and adept.  Discover shadow's
#                    LAN IP via `ip -4 addr`.
#   2. Mint mTLS   : mini-CA + one PS cert locally under
#                    $LOCAL_WORK/tls.  Extract SHA-256 fingerprint.
#   3. Stamp toml  : substitute SHADOW_LAN_IP and the PS fingerprint
#                    into the realnet tomls under $LOCAL_WORK/cfg.
#   4. Push to shadow : git worktree at /tmp/reffs_realnet_repo
#                       from ~/reffs-main (avoids the nightly's
#                       worktree pollution per feedback_lab_use_
#                       worktrees_not_rsync).  Rsync mTLS materials
#                       + stamped tomls + the realnet compose file
#                       to /tmp/reffs_realnet/.  Bring up the
#                       docker-compose stack + MDS container.
#   5. Push to adept  : same git worktree pattern at
#                       /tmp/reffs_realnet_repo.  Rsync TLS + stamped
#                       PS toml.  Build reffsd if no fresh binary.
#                       Launch reffsd standalone w/ ps-realnet.toml.
#   6. Verify      : grep shadow MDS logs for one PROXY_REGISTRATION
#                    grant.  grep adept PS logs for `reffsd ready`.
#                    Fail loudly on either side.
#
# After success the topology is:
#
#   variant a/b/c clients  -> shadow LAN IP, port 2049 (MDS direct)
#   variant d clients      -> adept  LAN IP, port 4098 (PS-encoded)
#
# Run-once-per-bench-session.  Re-running cleans up the prior
# bringup (idempotent).  Companion teardown is left as a follow-up
# -- this script also rebuilds whatever it tore down on re-run.

set -euo pipefail

# -- config -----------------------------------------------------------
SHADOW_HOST="${SHADOW_HOST:-shadow}"
ADEPT_HOST="${ADEPT_HOST:-adept}"
DREAMER_HOST="${DREAMER_HOST:-dreamer}"

# Remote canonical reffs clone path.  On shadow + adept the user
# uses ~/reffs as the canonical clone; the ~/reffs-main convention
# (feedback_lab_use_worktrees_not_rsync.md) is garbo-only because
# that's where the nightly's worktree lives.  Either way, we
# `git worktree add` out to /tmp so we never pollute the source.
REMOTE_CANONICAL="${REMOTE_CANONICAL:-/home/loghyr/reffs}"
REMOTE_WORKTREE="${REMOTE_WORKTREE:-/tmp/reffs_realnet_repo}"
REMOTE_WORK="${REMOTE_WORK:-/tmp/reffs_realnet}"
REMOTE_TLS="${REMOTE_TLS:-/tmp/reffs_ps_tls}"

# Git ref the lab worktrees check out.  Defaults to origin/main
# so a green bringup means "what main says works".  Override with
# REMOTE_REF=origin/<topic> when iterating on the bringup itself
# before the topic branch lands on main.
REMOTE_REF="${REMOTE_REF:-origin/main}"

LOCAL_WORK=$(mktemp -d)
trap 'rm -rf "$LOCAL_WORK"' EXIT

HERE=$(cd "$(dirname "$0")" && pwd)

echo "[bringup] 3-host realnet bench:"
echo "          client = $DREAMER_HOST"
echo "          PS     = $ADEPT_HOST"
echo "          DS+MDS = $SHADOW_HOST"

# -- step 1: pre-flight ----------------------------------------------
echo "[bringup] 1/6 pre-flight..."
for h in "$SHADOW_HOST" "$ADEPT_HOST"; do
    if ! ssh -o BatchMode=yes -o ConnectTimeout=5 \
            "$h" 'exit 0' 2>/dev/null; then
        echo "FAIL: cannot SSH to $h.  Check ~/.ssh/config." >&2
        exit 1
    fi
done

# shadow's LAN IP is what both the PS-on-adept and the client-on-
# dreamer dial.  Prefer the same 192.168.x address space the lab
# uses (per reference_host_roles.md / reference_tailscale_setup.md
# -- tailnet 100.x is an alt path; lab LAN is 192.168.x).
SHADOW_LAN_IP=$(ssh "$SHADOW_HOST" \
    "ip -4 addr show 2>/dev/null \
        | awk '/inet 192\\./ {print \$2; exit}' \
        | cut -d/ -f1")
if [ -z "$SHADOW_LAN_IP" ]; then
    echo "FAIL: no 192.168.x address on $SHADOW_HOST" >&2
    echo "      Confirm shadow is on the lab LAN, not just tailnet." >&2
    exit 1
fi
echo "[bringup]    shadow LAN IP = $SHADOW_LAN_IP"

# -- step 2: mint mini-CA + PS cert ----------------------------------
echo "[bringup] 2/6 minting mini-CA + 1 PS cert..."
mkdir -p "$LOCAL_WORK/tls"
bash "$HERE/../sanity/setup-mini-ca.sh" "$LOCAL_WORK/tls" >/dev/null
# Rename ps.* to ps-0.* for consistency with the indexed naming
# pattern in run-ps-bench-bringup.sh, even though this bringup
# only ever has one PS.
mv "$LOCAL_WORK/tls/ps.crt" "$LOCAL_WORK/tls/ps-0.crt"
mv "$LOCAL_WORK/tls/ps.key" "$LOCAL_WORK/tls/ps-0.key"
mv "$LOCAL_WORK/tls/ps.fpr" "$LOCAL_WORK/tls/ps-0.fpr"
PS_FPR=$(cat "$LOCAL_WORK/tls/ps-0.fpr")
echo "[bringup]    PS fingerprint = $PS_FPR"

# -- step 3: stamp tomls --------------------------------------------
echo "[bringup] 3/6 stamping tomls with shadow IP + PS fingerprint..."
mkdir -p "$LOCAL_WORK/cfg"

sed -e "s|REPLACE_WITH_SHADOW_LAN_IP|$SHADOW_LAN_IP|g" \
    -e "s|REPLACE_WITH_PS_FINGERPRINT|$PS_FPR|g" \
    "$HERE/mds-realnet.toml" \
    > "$LOCAL_WORK/cfg/mds-realnet.toml"

sed -e "s|REPLACE_WITH_PS_CERT|$REMOTE_TLS/ps-0.crt|g" \
    -e "s|REPLACE_WITH_PS_KEY|$REMOTE_TLS/ps-0.key|g" \
    -e "s|REPLACE_WITH_PS_CA|$REMOTE_TLS/ca.crt|g" \
    -e "s|REPLACE_WITH_MDS_HOST|$SHADOW_LAN_IP|g" \
    "$HERE/ps-realnet.toml" \
    > "$LOCAL_WORK/cfg/ps-realnet.toml"

# -- step 4: push to shadow + bring up MDS+DS stack ------------------
echo "[bringup] 4/6 pushing to $SHADOW_HOST + bringing up DS+MDS..."

# Fresh worktree on shadow.  Idempotent: remove + re-add.
ssh "$SHADOW_HOST" bash -s <<EOF
set -euo pipefail
if git -C $REMOTE_WORKTREE rev-parse --git-dir >/dev/null 2>&1; then
    cd $REMOTE_CANONICAL
    git worktree remove --force $REMOTE_WORKTREE 2>/dev/null || true
fi
rm -rf $REMOTE_WORKTREE
cd $REMOTE_CANONICAL && git fetch origin
git -C $REMOTE_CANONICAL worktree add --force \
    $REMOTE_WORKTREE $REMOTE_REF
mkdir -p $REMOTE_WORK $REMOTE_TLS
EOF

# Rsync TLS materials + stamped tomls + the realnet compose yaml.
# Compose yaml is fetched from the worktree on shadow, but the
# stamped tomls live under $REMOTE_WORK because the worktree is
# pristine (we don't modify it on shadow).
rsync -a "$LOCAL_WORK/tls/" "$SHADOW_HOST:$REMOTE_TLS/"
rsync -a "$LOCAL_WORK/cfg/mds-realnet.toml" \
    "$SHADOW_HOST:$REMOTE_WORK/mds-realnet.toml"

# Tear down prior stack, then bring up DSes via docker-compose.
# MDS is brought up out-of-band so we can mount the stamped toml.
echo "[bringup]    tearing down any prior bench/realnet stack..."
ssh "$SHADOW_HOST" bash -s <<EOF
set -euo pipefail
cd $REMOTE_WORKTREE
sudo docker rm -f reffs-bench-mds 2>/dev/null || true
sudo docker compose -f deploy/benchmark/docker-compose-realnet.yml \
    down -v 2>/dev/null || true
# Also tear down the non-realnet stack if present, since it competes
# on port 2049.
sudo docker compose -f deploy/benchmark/docker-compose.yml \
    down -v 2>/dev/null || true
EOF

# Open the per-DS publish ports on shadow's firewall.  shadow's
# default Fedora firewalld config only opens the standard NFS
# service (2049) + rpc-bind (111); the 22049..22058 host ports
# that docker-compose publishes for each DS are blocked until we
# add an explicit allow.  Transient (no --permanent) so this
# bringup does not leave host firewall state behind across reboots.
echo "[bringup]    opening DS publish ports 22049-22058 on $SHADOW_HOST firewall..."
ssh "$SHADOW_HOST" bash -s <<EOF
set -euo pipefail
if sudo systemctl is-active firewalld >/dev/null 2>&1; then
    sudo firewall-cmd --add-port=22049-22058/tcp >/dev/null
    sudo firewall-cmd --add-port=22049-22058/udp >/dev/null
fi
EOF

echo "[bringup]    running docker-compose builder (slow first time)..."
ssh "$SHADOW_HOST" bash -s <<EOF
set -euo pipefail
cd $REMOTE_WORKTREE
sudo docker compose -f deploy/benchmark/docker-compose-realnet.yml \
    run --rm builder 2>&1 | tail -3
EOF

echo "[bringup]    starting DSes (10 containers) on shadow..."
ssh "$SHADOW_HOST" bash -s <<EOF
set -euo pipefail
cd $REMOTE_WORKTREE
sudo docker compose -f deploy/benchmark/docker-compose-realnet.yml \
    up -d --wait --no-recreate \
    ds0 ds1 ds2 ds3 ds4 ds5 ds6 ds7 ds8 ds9 2>&1 | tail -3
EOF

echo "[bringup]    launching MDS container with stamped toml..."
ssh "$SHADOW_HOST" bash -s <<EOF
set -euo pipefail
sudo docker rm -f reffs-bench-mds 2>/dev/null || true
sudo docker run -d --name reffs-bench-mds \
    --network benchmark_bench \
    --network-alias reffs-mds \
    --privileged \
    -p 2049:2049 \
    -v benchmark_build-vol:/shared:ro,z \
    -v $REMOTE_WORK/mds-realnet.toml:/etc/mds.toml:ro,z \
    -v $REMOTE_TLS:$REMOTE_TLS:ro,z \
    reffs-dev:latest \
    /bin/bash -c "rm -rf /tmp/reffs_mds_data /tmp/reffs_mds_state && \
                  mkdir -p /tmp/reffs_mds_data /tmp/reffs_mds_state && \
                  exec /shared/build/src/reffsd --config=/etc/mds.toml" \
    >/dev/null
EOF

echo "[bringup]    waiting for MDS ready..."
ready=0
for i in $(seq 1 120); do
    if ssh "$SHADOW_HOST" \
            "sudo docker logs reffs-bench-mds 2>&1 \
                | grep -q 'reffsd ready'"; then
        echo "[bringup]    MDS ready after ~${i}s"
        ready=1
        break
    fi
    sleep 1
done
if [ "$ready" != "1" ]; then
    echo "FAIL: MDS never logged 'reffsd ready' after 120s" >&2
    ssh "$SHADOW_HOST" \
        "sudo docker logs reffs-bench-mds 2>&1 | tail -20" >&2
    exit 1
fi

# -- step 5: push to adept + launch PS -------------------------------
echo "[bringup] 5/6 pushing to $ADEPT_HOST + launching PS..."

# Discover adept's LAN IP for the success summary.  Same 192.168.x
# heuristic as shadow.
ADEPT_LAN_IP=$(ssh "$ADEPT_HOST" \
    "ip -4 addr show 2>/dev/null \
        | awk '/inet 192\\./ {print \$2; exit}' \
        | cut -d/ -f1")
if [ -z "$ADEPT_LAN_IP" ]; then
    echo "FAIL: no 192.168.x address on $ADEPT_HOST" >&2
    exit 1
fi

# Fresh worktree on adept + rebuild reffsd if no recent binary.
ssh "$ADEPT_HOST" bash -s <<EOF
set -euo pipefail
if git -C $REMOTE_WORKTREE rev-parse --git-dir >/dev/null 2>&1; then
    cd $REMOTE_CANONICAL
    git worktree remove --force $REMOTE_WORKTREE 2>/dev/null || true
fi
rm -rf $REMOTE_WORKTREE
cd $REMOTE_CANONICAL && git fetch origin
git -C $REMOTE_CANONICAL worktree add --force \
    $REMOTE_WORKTREE $REMOTE_REF
mkdir -p $REMOTE_WORK $REMOTE_TLS

# Reuse an existing fresh build under ~/reffs-main if the binary
# already matches origin/main; otherwise rebuild under the new
# worktree.  The lab convention is `~/reffs-main/build/src/reffsd`
# is the canonical recent build; trust it if mtime is within 24h
# of HEAD.  Otherwise build clean under the temp worktree.
if [ -x $REMOTE_CANONICAL/build/src/reffsd ] && \
   [ \$(find $REMOTE_CANONICAL/build/src/reffsd -mtime -1 | wc -l) -gt 0 ]; then
    echo "[bringup]    using existing reffsd under $REMOTE_CANONICAL/build"
    ln -sfn $REMOTE_CANONICAL/build $REMOTE_WORKTREE/build
else
    echo "[bringup]    building reffsd under $REMOTE_WORKTREE..."
    cd $REMOTE_WORKTREE
    mkdir -p m4 && autoreconf -fi
    mkdir -p build && cd build
    ../configure
    make -j\$(nproc)
fi
EOF

rsync -a "$LOCAL_WORK/tls/" "$ADEPT_HOST:$REMOTE_TLS/"
rsync -a "$LOCAL_WORK/cfg/ps-realnet.toml" \
    "$ADEPT_HOST:$REMOTE_WORK/ps-realnet.toml"

# Kill any prior PS reffsd; clean state dirs.
ssh "$ADEPT_HOST" bash -s <<EOF
set -euo pipefail
pkill -u loghyr -f 'reffsd.*ps-realnet' 2>/dev/null || true
sleep 1
rm -rf /tmp/ps_realnet_data /tmp/ps_realnet_state
mkdir -p /tmp/ps_realnet_data /tmp/ps_realnet_state
EOF

# Launch reffsd PS.  Use nohup-via-env so ASAN_OPTIONS reaches the
# child env (feedback in the prior session: `nohup ASAN_OPTIONS=...`
# fails with command-not-found because nohup eats the assignment).
ssh "$ADEPT_HOST" bash -s <<EOF
set -euo pipefail
LOG=/tmp/ps_realnet_state/reffsd.log
env ASAN_OPTIONS=detect_leaks=0:halt_on_error=0 \\
    UBSAN_OPTIONS=halt_on_error=0 \\
    nohup $REMOTE_WORKTREE/build/src/reffsd \\
        --config=$REMOTE_WORK/ps-realnet.toml \\
        > \$LOG 2>&1 < /dev/null &
disown || true
EOF

echo "[bringup]    waiting for PS ready..."
ready=0
for i in $(seq 1 60); do
    if ssh "$ADEPT_HOST" \
            "grep -q 'reffsd ready' /tmp/ps_realnet_state/reffsd.log 2>/dev/null"; then
        echo "[bringup]    PS ready after ~${i}s"
        ready=1
        break
    fi
    sleep 1
done
if [ "$ready" != "1" ]; then
    echo "FAIL: PS never logged 'reffsd ready' after 60s" >&2
    ssh "$ADEPT_HOST" \
        "tail -30 /tmp/ps_realnet_state/reffsd.log" >&2
    exit 1
fi

# -- step 6: verify PROXY_REGISTRATION grant -------------------------
echo "[bringup] 6/6 verifying PROXY_REGISTRATION..."

# Give the PS a moment to send PROXY_REGISTRATION after its
# `reffsd ready` line (the registration happens on the upstream
# session, which the PS opens lazily).
sleep 4

# The MDS grant count is the authoritative check.  PS-side log
# may show a follow-up PROXY_REGISTRATION rejected by the squat
# guard with NFS4ERR_DELAY (the PS occasionally re-registers with
# a fresh registration_id under contention) -- that is a benign
# retry race, not a topology failure, as long as the MDS already
# recorded a successful grant.
mds_grants=$(ssh "$SHADOW_HOST" \
    "sudo docker logs reffs-bench-mds 2>&1 \
        | grep -c 'PROXY_REGISTRATION: client granted PS privilege' \
        || true")
echo "[bringup]    MDS granted PS privilege $mds_grants time(s) (expect >= 1)"
if [ "$mds_grants" = "0" ]; then
    echo "FAIL: MDS recorded no PROXY_REGISTRATION grants" >&2
    ps_log=$(ssh "$ADEPT_HOST" \
        "cat /tmp/ps_realnet_state/reffsd.log 2>&1 || true")
    if echo "$ps_log" | grep -q "listener stays dark"; then
        echo "      cause: PS upstream session never came up:" >&2
        echo "$ps_log" | grep -iE "listener stays dark|create_tls failed" \
            | head -5 >&2
    else
        echo "      MDS-side log:" >&2
        ssh "$SHADOW_HOST" \
            "sudo docker logs reffs-bench-mds 2>&1 \
                | grep -iE 'PROXY_REG|tls_finger' | head -10" >&2
        echo "      PS-side log:" >&2
        echo "$ps_log" | grep -iE "registration|tls" | head -10 >&2
    fi
    exit 1
fi

# -- DONE ------------------------------------------------------------
cat <<EOF

[bringup] DONE.  3-host realnet topology up:

    variant a/b/c clients -> $SHADOW_LAN_IP:2049  (direct MDS)
    variant d   clients   -> $ADEPT_LAN_IP:4098   (PS-encoded)

    Mini-CA + PS cert    : $REMOTE_TLS/ on shadow + adept
    Stamped MDS toml     : $REMOTE_WORK/mds-realnet.toml on shadow
    Stamped PS toml      : $REMOTE_WORK/ps-realnet.toml on adept

To smoke variant c from $DREAMER_HOST:

    ssh $DREAMER_HOST '/path/to/ec_demo --layout v2 --codec rs \\
        --k 4 --m 2 --mds $SHADOW_LAN_IP --size 1MB ...'

To smoke variant d from $DREAMER_HOST:

    ssh $DREAMER_HOST 'sudo mount -t nfs4 -o sec=sys,vers=4.2 \\
        $ADEPT_LAN_IP:/ /mnt/realnet'

The bench harness (prereq #4) will wrap both of these.
EOF
