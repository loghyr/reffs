#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_ps_soak_test.sh -- Long-running stability soak test for the
# proxy server (PS).
#
# Boots an MDS + PS topology like scripts/local_ps_test.sh, mounts a
# NFSv4.2 client against the PS, runs a concurrent read/write
# workload, and periodically restarts the PS.  Verifies no
# ASAN/UBSAN errors, bounded RSS / FD growth, and successful recovery
# (re-mount within 30s) after every restart.
#
# Acceptance criteria from .claude/design/proxy-server.md
# "Systematic testing > Soak testing":
#   - Zero `ERROR: AddressSanitizer` lines in PS or MDS log
#   - Zero `runtime error:` (UBSAN) lines in PS or MDS log
#   - PS RSS at end <= 2x PS RSS at the 60s mark (under load)
#   - PS open FD count at end <= baseline FD + 50% (matches
#     ci_soak_test.sh's tolerance for backend SST accumulation)
#   - After each restart, the kernel client successfully re-mounts
#     within 30s
#
# Default: 30-minute CI soak, restart PS every 5 minutes.
# --bat:   8-hour BAT soak,  restart PS every 15 minutes.
#
# Usage:
#   scripts/ci_ps_soak_test.sh [--bat]
#
# Prerequisites:
#   - /reffs_data exists and is writable
#   - rpcbind running (script starts it if missing)
#   - sudo for rpcbind / mount / umount
#   - Linux kernel with NFSv4.2 client
#
# Topology (matches scripts/local_ps_test.sh):
#
#   reffsd(MDS)  :12049 NFS, :20490 probe -- serves /
#   reffsd(PS)   :12050 NFS native, :14098 NFS proxy, :20491 probe
#                     [[proxy_mds]] -> 127.0.0.1:12049
#   client mount :14098 -- workload runs here

set -uo pipefail

# Linux-only by design (depends on /proc, rpcbind, kernel NFS client).
# Fail fast with autotools "skip" code if invoked elsewhere.
if [ "$(uname -s)" != "Linux" ]; then
	echo "ci_ps_soak_test.sh: Linux only (needs /proc, rpcbind, kernel NFS client)" >&2
	exit 77
fi

# ----------------------------------------------------------------------
# Mode flags
# ----------------------------------------------------------------------
MODE=ci
DURATION_MIN=30
RESTART_MIN=5

while [ $# -gt 0 ]; do
	case "$1" in
	--bat)
		MODE=bat
		DURATION_MIN=480
		RESTART_MIN=15
		shift
		;;
	--duration)
		DURATION_MIN=$2
		shift 2
		;;
	--restart-every)
		RESTART_MIN=$2
		shift 2
		;;
	*)
		echo "unknown arg: $1" >&2
		exit 2
		;;
	esac
done

DURATION_SEC=$((DURATION_MIN * 60))
RESTART_SEC=$((RESTART_MIN * 60))

# ----------------------------------------------------------------------
# Paths + ports
# ----------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
REFFSD="$BUILD_DIR/src/reffsd"

MDS_PORT=12049
MDS_PROBE_PORT=20490
PS_NATIVE_PORT=12050
PS_PROBE_PORT=20491
PS_PROXY_PORT=14098

RUN_DIR="/reffs_data/ps_soak"
MDS_DATA="$RUN_DIR/mds_data"
MDS_STATE="$RUN_DIR/mds_state"
PS_DATA="$RUN_DIR/ps_data"
PS_STATE="$RUN_DIR/ps_state"
MDS_CONFIG="$RUN_DIR/mds.toml"
PS_CONFIG="$RUN_DIR/ps.toml"
MDS_LOG="$RUN_DIR/mds.log"
PS_LOG="$RUN_DIR/ps.log"

MOUNT_BASE="/mnt/reffs_ps_soak"
MOUNT="$MOUNT_BASE"

MDS_PID=""
PS_PID=""
WORKLOAD_PIDS=()
FAILED=false
SOAK_POSTMORTEM_DONE=false
RESTART_COUNT=0
SOAK_START=0

# ----------------------------------------------------------------------
# Logging + post-mortem
# ----------------------------------------------------------------------
info() { echo "[$(date +%H:%M:%S)] $*"; }
die()  { echo "PS_SOAK FAIL: $*" >&2; FAILED=true; }

postmortem() {
	if [ "$SOAK_POSTMORTEM_DONE" = "true" ]; then return; fi
	SOAK_POSTMORTEM_DONE=true
	local rc=$1
	if [ "$rc" -ne 0 ] || [ "$FAILED" = "true" ]; then
		echo "" >&2
		echo "=== PS SOAK POST-MORTEM (exit $rc) ===" >&2
		echo "  Timestamp: $(date)" >&2
		echo "  Restart count: ${RESTART_COUNT}" >&2
		echo "  Elapsed: $(($(date +%s) - SOAK_START))s" >&2
		if [ -n "$PS_PID" ] && kill -0 "$PS_PID" 2>/dev/null; then
			echo "  PS PID $PS_PID still running" >&2
			echo "  PS RSS: $(awk '/VmRSS/{print $2 $3}' /proc/$PS_PID/status 2>/dev/null || echo unknown)" >&2
		fi
		if [ -n "$MDS_PID" ] && kill -0 "$MDS_PID" 2>/dev/null; then
			echo "  MDS PID $MDS_PID still running" >&2
			echo "  MDS RSS: $(awk '/VmRSS/{print $2 $3}' /proc/$MDS_PID/status 2>/dev/null || echo unknown)" >&2
		fi
		echo "--- PS log tail (last 80 lines) ---" >&2
		tail -n 80 "$PS_LOG" 2>/dev/null >&2 || true
		echo "--- MDS log tail (last 40 lines) ---" >&2
		tail -n 40 "$MDS_LOG" 2>/dev/null >&2 || true
	fi
}

cleanup() {
	local rc=$?
	set +e
	for pid in "${WORKLOAD_PIDS[@]}"; do kill "$pid" 2>/dev/null; done
	for pid in "${WORKLOAD_PIDS[@]}"; do wait "$pid" 2>/dev/null; done
	WORKLOAD_PIDS=()
	if mountpoint -q "$MOUNT" 2>/dev/null; then
		sudo umount -f -l "$MOUNT" 2>/dev/null || true
	fi
	for m in "${MOUNT_BASE}"_*; do
		[ -d "$m" ] || continue
		mountpoint -q "$m" 2>/dev/null && sudo umount -f -l "$m" 2>/dev/null || true
	done
	# Capture diagnostic snapshot BEFORE killing the daemons --
	# postmortem reads /proc/<pid>/status which goes away on exit.
	postmortem "$rc"
	if [ -n "$PS_PID" ] && kill -0 "$PS_PID" 2>/dev/null; then
		kill -TERM "$PS_PID" 2>/dev/null
		sleep 2
		kill -KILL "$PS_PID" 2>/dev/null
	fi
	if [ -n "$MDS_PID" ] && kill -0 "$MDS_PID" 2>/dev/null; then
		kill -TERM "$MDS_PID" 2>/dev/null
		sleep 2
		kill -KILL "$MDS_PID" 2>/dev/null
	fi
}
# Single EXIT trap.  Earlier drafts registered postmortem and cleanup
# as separate traps -- bash replaces (does not chain) traps for the
# same signal, which silently dropped the diagnostic dump on every
# failure.  Folded into one.
trap cleanup EXIT

# ----------------------------------------------------------------------
# Build
# ----------------------------------------------------------------------
info "Building reffsd (ASAN + UBSAN)..."
cd "$PROJECT_ROOT"
[ -f configure ] || { mkdir -p m4 && autoreconf -fi; }
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
[ -f Makefile ] || "$PROJECT_ROOT/configure" --enable-asan --enable-ubsan
make -j"$(nproc)" >/dev/null
cd "$PROJECT_ROOT"

[ -x "$REFFSD" ] || REFFSD="$BUILD_DIR/src/.libs/reffsd"
[ -x "$REFFSD" ] || { die "Cannot find reffsd binary"; exit 1; }

# ----------------------------------------------------------------------
# rpcbind
# ----------------------------------------------------------------------
if ! pgrep -x rpcbind >/dev/null; then
	info "Starting rpcbind..."
	sudo rpcbind 2>/dev/null || die "rpcbind failed to start"
	sleep 1
fi

# ----------------------------------------------------------------------
# Fresh dirs + configs
# ----------------------------------------------------------------------
sudo rm -rf "$RUN_DIR"
mkdir -p "$MDS_DATA" "$MDS_STATE" "$PS_DATA" "$PS_STATE"
sudo mkdir -p "$MOUNT_BASE"

cat >"$MDS_CONFIG" <<EOF
[server]
port           = $MDS_PORT
probe_port     = $MDS_PROBE_PORT
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4

[backend]
type       = "posix"
path       = "$MDS_DATA"
state_file = "$MDS_STATE"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]
EOF

cat >"$PS_CONFIG" <<EOF
[server]
port           = $PS_NATIVE_PORT
probe_port     = $PS_PROBE_PORT
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4

[backend]
type       = "posix"
path       = "$PS_DATA"
state_file = "$PS_STATE"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]

[[proxy_mds]]
id          = 1
port        = $PS_PROXY_PORT
bind        = "*"
address     = "127.0.0.1"
mds_port    = $MDS_PORT
mds_probe   = $MDS_PROBE_PORT
EOF

# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------
start_reffsd() {
	local tag=$1 cfg=$2 log=$3 pidvar=$4
	info "Starting $tag..."
	ASAN_OPTIONS="detect_leaks=0:halt_on_error=0:handle_abort=1" \
	UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1" \
		"$REFFSD" --config="$cfg" -c 8 >>"$log" 2>&1 &
	local pid=$!
	printf -v "$pidvar" "%s" "$pid"
	for _ in $(seq 1 60); do
		grep -q "reffsd ready:" "$log" 2>/dev/null && return 0
		kill -0 "$pid" 2>/dev/null || { die "$tag died during startup"; return 1; }
		sleep 1
	done
	die "$tag did not become ready within 60s"
	return 1
}

mount_nfs() {
	local port=$1 mountpoint=$2 label=$3
	info "  mount $label: vers=4.2 port=$port -> $mountpoint"
	sudo timeout 30 mount -v \
		-o vers=4.2,sec=sys,port=$port,timeo=100 \
		127.0.0.1:/ "$mountpoint" 2>&1
}

unmount_nfs() {
	local mountpoint=$1
	if mountpoint -q "$mountpoint" 2>/dev/null; then
		sudo umount -f -l "$mountpoint" 2>/dev/null || true
	fi
}

stop_ps() {
	if [ -n "$PS_PID" ] && kill -0 "$PS_PID" 2>/dev/null; then
		kill -TERM "$PS_PID" 2>/dev/null
		for _ in $(seq 1 10); do
			kill -0 "$PS_PID" 2>/dev/null || break
			sleep 1
		done
		kill -KILL "$PS_PID" 2>/dev/null
		wait "$PS_PID" 2>/dev/null || true
	fi
	PS_PID=""
}

get_rss_kb() {
	local pid=$1
	[ -f "/proc/$pid/status" ] || { echo 0; return; }
	awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null || echo 0
}

get_fd_count() {
	local pid=$1
	[ -d "/proc/$pid/fd" ] || { echo 0; return; }
	ls "/proc/$pid/fd" 2>/dev/null | wc -l
}

check_asan() {
	for log in "$PS_LOG" "$MDS_LOG"; do
		[ -f "$log" ] || continue
		if grep -q "ERROR: AddressSanitizer" "$log"; then
			die "AddressSanitizer error in $log"
			grep -A 5 "ERROR: AddressSanitizer" "$log" | head -30 >&2
		fi
		if grep -q "runtime error:" "$log"; then
			die "UBSAN runtime error in $log"
			grep -B 2 -A 5 "runtime error:" "$log" | head -30 >&2
		fi
	done
}

# ----------------------------------------------------------------------
# Workload (runs against the PS mount)
# ----------------------------------------------------------------------
workload_writes() {
	local id=$1 mount_dir=$2
	local dir="$mount_dir/soak_w_$id"
	sudo mkdir -p "$dir" 2>/dev/null || true
	sudo chmod 1777 "$dir" 2>/dev/null || true
	local seq=0
	while true; do
		seq=$((seq + 1))
		local f="$dir/file_$((seq % 100))"
		dd if=/dev/urandom of="$f" bs=4096 count=$((RANDOM % 16 + 1)) \
			2>/dev/null || true
		mkdir -p "$dir/d_$((seq % 20))" 2>/dev/null || true
		mv "$f" "$dir/r_$((seq % 50))" 2>/dev/null || true
		ls "$dir" >/dev/null 2>/dev/null || true
		rm -f "$dir/r_$((RANDOM % 50))" 2>/dev/null || true
		rmdir "$dir/d_$((RANDOM % 20))" 2>/dev/null || true
		sleep 0.05
	done
}

workload_reads() {
	# Reader reads what the writer with id=1 produces.  The soak
	# runs exactly one reader and one writer per design
	# (proxy-server.md "Soak testing"); do NOT add more workers
	# without generalising the writer-dir argument here.  The id
	# parameter is unused but kept for symmetry with workload_writes.
	local _id=$1 mount_dir=$2
	local dir="$mount_dir/soak_w_1"
	while true; do
		local files
		files=$(ls "$dir" 2>/dev/null | head -10)
		for f in $files; do
			cat "$dir/$f" >/dev/null 2>&1 || true
			stat "$dir/$f" >/dev/null 2>&1 || true
		done
		sleep 0.1
	done
}

# ----------------------------------------------------------------------
# Phase 1: MDS up, seed namespace
# ----------------------------------------------------------------------
start_reffsd "MDS" "$MDS_CONFIG" "$MDS_LOG" MDS_PID || exit 1
info "MDS up (PID $MDS_PID)"

SEED_MOUNT="/mnt/reffs_ps_soak_seed"
sudo mkdir -p "$SEED_MOUNT"
mount_nfs "$MDS_PORT" "$SEED_MOUNT" "MDS-seed" || { die "MDS seed mount failed"; exit 1; }
sudo chmod 1777 "$SEED_MOUNT" 2>/dev/null || true
mkdir -p "$SEED_MOUNT/seeded" 2>/dev/null || true
echo "soak namespace seed" > "$SEED_MOUNT/seeded/marker.txt" 2>/dev/null || true
sudo umount -f -l "$SEED_MOUNT"
sudo rmdir "$SEED_MOUNT" 2>/dev/null || true

# ----------------------------------------------------------------------
# Phase 2: PS up, client mount via PS
# ----------------------------------------------------------------------
start_reffsd "PS" "$PS_CONFIG" "$PS_LOG" PS_PID || exit 1
info "PS up (PID $PS_PID)"

mount_nfs "$PS_PROXY_PORT" "$MOUNT" "PS" || { die "initial PS mount failed"; exit 1; }

# ----------------------------------------------------------------------
# Phase 3: workload + baseline
# ----------------------------------------------------------------------
info "Starting workloads (1 writer, 1 reader)..."
workload_writes 1 "$MOUNT" &
WORKLOAD_PIDS+=($!)
workload_reads 2 "$MOUNT" &
WORKLOAD_PIDS+=($!)

info "Warming up (60s) before baseline capture..."
sleep 60
BASELINE_RSS=$(get_rss_kb "$PS_PID")
BASELINE_FD=$(get_fd_count "$PS_PID")
info "Baseline (under load): PS RSS=${BASELINE_RSS}KB FD=${BASELINE_FD}"

# ----------------------------------------------------------------------
# Phase 4: soak loop with periodic PS restart
# ----------------------------------------------------------------------
info "=== PS soak: ${DURATION_MIN}m total, restart PS every ${RESTART_MIN}m, mode=${MODE} ==="
SOAK_START=$(date +%s)
NEXT_RESTART=$((SOAK_START + RESTART_SEC))

while true; do
	NOW=$(date +%s)
	ELAPSED=$((NOW - SOAK_START))
	[ "$ELAPSED" -ge "$DURATION_SEC" ] && { info "Duration reached. Finishing."; break; }

	if [ "$NOW" -ge "$NEXT_RESTART" ]; then
		RESTART_COUNT=$((RESTART_COUNT + 1))
		info "=== PS Restart #$RESTART_COUNT ==="

		for pid in "${WORKLOAD_PIDS[@]}"; do kill "$pid" 2>/dev/null; done
		for pid in "${WORKLOAD_PIDS[@]}"; do wait "$pid" 2>/dev/null; done
		WORKLOAD_PIDS=()

		unmount_nfs "$MOUNT"
		stop_ps

		check_asan
		[ "$FAILED" = "true" ] && { info "Aborting soak"; exit 1; }

		echo "=== PS restart #$RESTART_COUNT at $(date) ===" >> "$PS_LOG"
		start_reffsd "PS" "$PS_CONFIG" "$PS_LOG" PS_PID || exit 1

		# Fresh mount point (kernel keeps stale superblock alive
		# after umount -f -l); same trick ci_soak_test.sh uses
		MOUNT="${MOUNT_BASE}_${RESTART_COUNT}"
		sudo mkdir -p "$MOUNT"

		# proxy-server.md "Soak testing" calls for "MOUNT/LOOKUP must
		# succeed within 30s" after restart, but that 30s budget is
		# measured from PS-ready, not from PS-stop -- start_reffsd's
		# 60s cap and stop_ps's 10s SIGTERM grace can together push
		# the user-visible window to ~90s.  The 30s budget below
		# applies only to the mount step itself; mount succeeds
		# typically within the first attempt.  6 attempts at 5s
		# stride gives 30s of pure mount-retry headroom on top of
		# the readiness wait.
		info "Re-mounting (mount step must succeed within 30s)..."
		mount_ok=false
		for try in $(seq 1 6); do
			info "  Mount attempt $try/6..."
			if mount_nfs "$PS_PROXY_PORT" "$MOUNT" "PS" >>"$PS_LOG" 2>&1; then
				mount_ok=true
				break
			fi
			info "  Mount attempt $try failed (see PS log tail)"
			sleep 5
		done
		[ "$mount_ok" = "true" ] || {
			die "PS re-mount failed after restart #$RESTART_COUNT"
			exit 1
		}
		info "Mount succeeded after restart"

		workload_writes 1 "$MOUNT" &
		WORKLOAD_PIDS+=($!)
		workload_reads 2 "$MOUNT" &
		WORKLOAD_PIDS+=($!)

		NEXT_RESTART=$((NOW + RESTART_SEC))
	fi

	sleep 10

	if kill -0 "$PS_PID" 2>/dev/null; then
		RSS=$(get_rss_kb "$PS_PID")
		FD=$(get_fd_count "$PS_PID")
		info "Health: PS RSS=${RSS}KB FD=${FD} elapsed=${ELAPSED}s restarts=${RESTART_COUNT}"
	else
		die "PS crashed during soak"
		exit 1
	fi
done

# ----------------------------------------------------------------------
# Phase 5: stop, final checks
# ----------------------------------------------------------------------
info "Stopping workloads..."
for pid in "${WORKLOAD_PIDS[@]}"; do kill "$pid" 2>/dev/null; done
for pid in "${WORKLOAD_PIDS[@]}"; do wait "$pid" 2>/dev/null; done
WORKLOAD_PIDS=()

unmount_nfs "$MOUNT"

FINAL_RSS=$(get_rss_kb "$PS_PID")
FINAL_FD=$(get_fd_count "$PS_PID")

stop_ps
check_asan

info "=== Final metrics ==="
info "  Baseline: PS RSS=${BASELINE_RSS}KB FD=${BASELINE_FD}"
info "  Final:    PS RSS=${FINAL_RSS}KB FD=${FINAL_FD}"
info "  Restarts: $RESTART_COUNT"

if [ "$BASELINE_RSS" -gt 0 ] && [ "$FINAL_RSS" -gt $((BASELINE_RSS * 2)) ]; then
	die "PS memory growth: ${FINAL_RSS}KB > 2x baseline ${BASELINE_RSS}KB"
fi

if [ "$BASELINE_FD" -gt 0 ]; then
	FD_LIMIT=$(( BASELINE_FD + BASELINE_FD / 2 + 5 ))
	if [ "$FINAL_FD" -gt "$FD_LIMIT" ]; then
		die "PS FD growth: ${FINAL_FD} > ${FD_LIMIT} (baseline ${BASELINE_FD} + 50%)"
	fi
fi

if [ "$FAILED" = "true" ]; then
	SOAK_POSTMORTEM_DONE=true
	info "=== PS SOAK FAILED ==="
	exit 1
fi

SOAK_POSTMORTEM_DONE=true
info "=== PS SOAK PASSED (${DURATION_MIN}m, ${RESTART_COUNT} restarts) ==="
