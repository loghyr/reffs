#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# tls_stress_test.sh — Run nfs_tls_stress against a local reffsd inside
# the ci-image Docker container.  No kernel NFS mounts needed.
#
# Usage:
#   # Build the ci-image first:
#   make -f Makefile.reffs ci-image
#
#   # Run the test (from the reffs source root):
#   tools/tls_stress_test.sh [--duration SECS]
#
# The script launches everything inside a single Docker container:
# reffsd (standalone NFSv4.2 with TLS) + nfs_tls_stress (all 4 modes).

set -euo pipefail

DURATION=${1:-60}
CI_IMAGE=${CI_IMAGE:-reffs-ci}
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== TLS Stress Test (${DURATION}s) ==="
echo "  Image:  $CI_IMAGE"
echo "  Source: $ROOT_DIR"
echo

docker run --rm --privileged \
    --ulimit nofile=65536:65536 \
    -v "$ROOT_DIR":/reffs:ro,Z \
    "$CI_IMAGE" \
    /bin/bash -c '
set -euo pipefail

DURATION='"$DURATION"'

# ---- Build ----
echo "=== Building ==="
rsync -a --exclude=".git" --exclude="build" /reffs/ /src/
cd /src && find . -name parsetab.py -delete && mkdir -p m4 && autoreconf -fi
rm -rf /build/* && cd /build
SKIP_STYLE=1 ../src/configure --enable-asan --enable-ubsan
make -j$(nproc)

# Build the stress tool (standalone, links lib/tls/tls_client.c)
cc -Wall -Wextra -Wno-unused-function \
    -I/src/lib/include \
    -o /build/nfs_tls_stress \
    /src/tools/nfs_tls_stress.c \
    /src/lib/tls/tls_client.c \
    -lssl -lcrypto
echo "Build complete."

# ---- Generate TLS certs ----
TLS_DIR="/tmp/tls_certs"
mkdir -p "$TLS_DIR"
openssl req -x509 -newkey rsa:2048 \
    -keyout "$TLS_DIR/server.key" \
    -out "$TLS_DIR/server.pem" \
    -days 1 -nodes \
    -subj "/CN=localhost" 2>/dev/null

# ---- Config ----
DATA="/tmp/tls_stress_data"
STATE="/tmp/tls_stress_state"
LOG="/tmp/reffsd_tls.log"
CONFIG="/tmp/reffsd_tls.toml"

mkdir -p "$DATA" "$STATE"

cat >"$CONFIG" <<TOML
[server]
port           = 2049
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 8
nfs4_domain    = "reffs.test"
tls_cert       = "$TLS_DIR/server.pem"
tls_key        = "$TLS_DIR/server.key"
trace_file     = "/tmp/reffsd_tls_trace.log"

[backend]
type       = "posix"
path       = "$DATA"
state_file = "$STATE"

[[export]]
path    = "/"
flavors = ["sys", "tls"]
TOML

# ---- Start reffsd ----
echo "=== Starting reffsd ==="
ASAN_OPTIONS="detect_leaks=0:halt_on_error=0" \
UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1" \
    /build/src/reffsd --config="$CONFIG" >"$LOG" 2>&1 &
REFFSD_PID=$!

# Wait for reffsd to be ready
for i in $(seq 1 30); do
    (echo >/dev/tcp/127.0.0.1/2049) 2>/dev/null && break
    sleep 0.5
done

if ! kill -0 "$REFFSD_PID" 2>/dev/null; then
    echo "FAIL: reffsd failed to start"
    cat "$LOG"
    exit 1
fi
echo "reffsd running (PID $REFFSD_PID)"

# ---- Compute iterations from duration ----
# Each starttls-loop iteration takes ~5-10ms.
# Use duration / 4 modes = per-mode seconds, ~100 iterations per second.
PER_MODE=$((DURATION / 4))
ITERATIONS=$((PER_MODE * 50))
if [ "$ITERATIONS" -lt 10 ]; then
    ITERATIONS=10
fi

# ---- Run stress test ----
echo "=== Running nfs_tls_stress (${DURATION}s, ${ITERATIONS} iterations/mode) ==="
FAILED=0

# Per-mode success criteria:
#   starttls-loop:      99%+ (rare TLS timeout under ASAN is acceptable)
#   rapid-cycle:        99%+ (same)
#   mid-op-disconnect:  90%+ (some reconnect timing failures expected)
#   hot-reconnect:      50%+ (server correctly rejects direct TLS;
#                        success = STARTTLS retry works afterwards)

run_mode() {
    local MODE=$1
    local MIN_PCT=$2

    echo ""
    echo "--- Mode: $MODE (require ${MIN_PCT}%+) ---"

    OUTPUT=$(/build/nfs_tls_stress \
        --host 127.0.0.1 \
        --mode "$MODE" \
        --iterations "$ITERATIONS" \
        --ca "$TLS_DIR/server.pem" \
        --verbose 2>&1)
    echo "$OUTPUT"

    # Extract success percentage from summary
    SUCCESSES=$(echo "$OUTPUT" | grep "Successes:" | awk -F"[(%]" "{print int(\$2)}")
    if [ -z "$SUCCESSES" ]; then
        SUCCESSES=0
    fi

    if [ "$SUCCESSES" -ge "$MIN_PCT" ]; then
        echo "  $MODE: PASS (${SUCCESSES}% >= ${MIN_PCT}%)"
        return 0
    else
        echo "  $MODE: FAIL (${SUCCESSES}% < ${MIN_PCT}%)"
        return 1
    fi
}

run_mode starttls-loop 99       || FAILED=1
run_mode mid-op-disconnect 90   || FAILED=1
run_mode hot-reconnect 50       || FAILED=1
run_mode rapid-cycle 99         || FAILED=1

# ---- Shutdown reffsd ----
echo ""
echo "=== Stopping reffsd ==="
kill -TERM "$REFFSD_PID" 2>/dev/null || true

# Wait up to 30s for clean shutdown
for i in $(seq 1 60); do
    kill -0 "$REFFSD_PID" 2>/dev/null || break
    sleep 0.5
done

if kill -0 "$REFFSD_PID" 2>/dev/null; then
    echo "WARNING: reffsd still running after 30s, sending SIGKILL"
    kill -KILL "$REFFSD_PID" 2>/dev/null || true
fi
wait "$REFFSD_PID" 2>/dev/null || true

# ---- Check for ASAN/UBSAN errors ----
echo ""
echo "=== Checking reffsd log for sanitizer errors ==="
if grep -q "ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error:" "$LOG"; then
    echo "FAIL: Sanitizer errors detected in reffsd"
    grep "ERROR:\|runtime error:" "$LOG" | head -20
    FAILED=1
else
    echo "No sanitizer errors."
fi

# ---- Summary ----
echo ""
if [ "$FAILED" -eq 0 ]; then
    echo "=== TLS STRESS TEST PASSED ==="
else
    echo "=== TLS STRESS TEST FAILED ==="
    echo ""
    echo "=== reffsd log (last 50 lines) ==="
    tail -50 "$LOG"
fi

exit "$FAILED"
'
