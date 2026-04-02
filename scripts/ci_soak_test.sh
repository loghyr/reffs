#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_soak_test.sh -- Long-running stability soak test.
#
# Runs reffsd with a concurrent NFS workload and periodic restart
# cycles.  Verifies no ASAN/UBSAN errors, no memory growth, no FD
# leaks, and successful recovery after each restart.
#
# Usage:
#   ci_soak_test.sh REFFSD_BIN SRC_DIR [--bat] [--rocksdb]
#
# Default: 30-minute CI soak, restart every 5 minutes, POSIX backend.
# --bat:     8-hour BAT soak, restart every 15 minutes.
# --rocksdb: use RocksDB metadata backend instead of POSIX.
#
# Prerequisites: rpcbind running, NFS client tools installed.

set -euo pipefail

# -----------------------------------------------------------------------
# Post-mortem: if the script exits unexpectedly, dump diagnostics.
# This catches OOM kills, Docker container eviction, and crashes.
# -----------------------------------------------------------------------
SOAK_POSTMORTEM_DONE=false
postmortem() {
	if [ "$SOAK_POSTMORTEM_DONE" = "true" ]; then
		return
	fi
	SOAK_POSTMORTEM_DONE=true
	local rc=$?
	if [ "$rc" -ne 0 ]; then
		echo "" >&2
		echo "=== SOAK POST-MORTEM (exit code $rc) ===" >&2
		echo "  Timestamp: $(date)" >&2
		echo "  Last restart count: ${RESTART_COUNT:-unknown}" >&2
		echo "  Elapsed: $(($(date +%s) - ${SOAK_START:-0}))s" >&2
		if [ -n "${REFFSD_PID:-}" ] && kill -0 "$REFFSD_PID" 2>/dev/null; then
			echo "  reffsd PID $REFFSD_PID is still running" >&2
			echo "  RSS: $(cat /proc/$REFFSD_PID/status 2>/dev/null | awk '/VmRSS/{print $2 $3}' || echo unknown)" >&2
			echo "  FDs: $(ls /proc/$REFFSD_PID/fd 2>/dev/null | wc -l || echo unknown)" >&2
		else
			echo "  reffsd is NOT running" >&2
		fi
		if [ -f "${LOG:-/dev/null}" ]; then
			echo "  reffsd log (last 20 lines):" >&2
			tail -20 "$LOG" 2>/dev/null >&2 || true
		fi
		echo "  System memory:" >&2
		free -h 2>/dev/null >&2 || cat /proc/meminfo 2>/dev/null | head -5 >&2 || true
		echo "  Possible causes: OOM kill, Docker memory limit, ASAN abort" >&2
		echo "=== END POST-MORTEM ===" >&2
	fi
}
trap postmortem EXIT

REFFSD_BIN=${1:?Usage: ci_soak_test.sh REFFSD_BIN SRC_DIR [--bat] [--rocksdb]}
SRC_DIR=${2:?Usage: ci_soak_test.sh REFFSD_BIN SRC_DIR [--bat] [--rocksdb]}
shift 2

DURATION_MIN=30
RESTART_MIN=5
CLIENTS=2
BACKEND_TYPE="posix"

for arg in "$@"; do
	case "$arg" in
	--bat)
		DURATION_MIN=480  # 8 hours
		RESTART_MIN=15
		CLIENTS=4
		;;
	--rocksdb)
		BACKEND_TYPE="rocksdb"
		;;
	esac
done

DURATION_SEC=$((DURATION_MIN * 60))
RESTART_SEC=$((RESTART_MIN * 60))

REFFSD_PID=
WORKLOAD_PIDS=()
FAILED=false

die() { echo "SOAK FAIL: $*" >&2; FAILED=true; }
info() { echo "[$(date +%H:%M:%S)] $*"; }

# Work directory for data/state/logs.  /tmp is often tmpfs (RAM-backed)
# which causes OOM under soak load.  Use a real filesystem instead.
# Override with REFFS_WORK_DIR env var.
if [ -n "${REFFS_WORK_DIR:-}" ] && [ -d "$REFFS_WORK_DIR" ]; then
	WORK_DIR="$REFFS_WORK_DIR"
elif [ -d /reffs_data ]; then
	WORK_DIR=/reffs_data
elif [ -d /Volumes/reffs_data ]; then
	WORK_DIR=/Volumes/reffs_data/soak
	mkdir -p "$WORK_DIR"
else
	WORK_DIR=/tmp
	info "WARNING: no dedicated work dir found — using /tmp (may OOM on tmpfs)"
	info "  Set REFFS_WORK_DIR or create /reffs_data to use a real filesystem"
fi
info "Work directory: $WORK_DIR"

MOUNT=/mnt/reffs_soak
DATA=$WORK_DIR/reffs_soak_data
STATE=$WORK_DIR/reffs_soak_state
CONFIG=$WORK_DIR/reffs_soak.toml
LOG=$WORK_DIR/reffsd-soak.log
TRACE_FILE=$WORK_DIR/reffs-soak-trace.log

# Clear stale logs from previous runs
rm -f "$LOG" "$TRACE_FILE" /logs/soak.log /logs/soak-trace.log 2>/dev/null

cleanup() {
	set +e
	for pid in "${WORKLOAD_PIDS[@]}"; do
		kill "$pid" 2>/dev/null
		wait "$pid" 2>/dev/null
	done
	umount -f "$MOUNT" 2>/dev/null
	if [ -n "$REFFSD_PID" ]; then
		kill -TERM "$REFFSD_PID" 2>/dev/null
		sleep 2
		kill -KILL "$REFFSD_PID" 2>/dev/null
		wait "$REFFSD_PID" 2>/dev/null
	fi
	postmortem
}
trap cleanup EXIT

# -----------------------------------------------------------------------
# Docker / system resource check
# -----------------------------------------------------------------------

check_resources() {
	local mem_kb=0
	if [ -f /sys/fs/cgroup/memory.max ]; then
		# cgroup v2
		local mem_max
		mem_max=$(cat /sys/fs/cgroup/memory.max 2>/dev/null || echo "max")
		if [ "$mem_max" != "max" ]; then
			mem_kb=$((mem_max / 1024))
		fi
	elif [ -f /sys/fs/cgroup/memory/memory.limit_in_bytes ]; then
		# cgroup v1
		local mem_max
		mem_max=$(cat /sys/fs/cgroup/memory/memory.limit_in_bytes 2>/dev/null || echo 0)
		if [ "$mem_max" -gt 0 ] && [ "$mem_max" -lt 9223372036854771712 ]; then
			mem_kb=$((mem_max / 1024))
		fi
	fi

	if [ "$mem_kb" -gt 0 ]; then
		local mem_gb=$(( mem_kb / 1048576 ))
		info "Container memory limit: ${mem_kb}KB (~${mem_gb}GB)"
		if [ "$mem_kb" -lt 3145728 ]; then  # 3GB
			info "WARNING: Container memory < 3GB — soak may be OOM-killed"
			info "  Increase Docker Desktop memory or reduce soak load"
		fi
	else
		info "Container memory limit: unlimited"
	fi

	local total_mem_kb
	total_mem_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)
	if [ "$total_mem_kb" -gt 0 ]; then
		info "System total memory: $((total_mem_kb / 1024))MB"
	fi
}

# -----------------------------------------------------------------------
# Config
# -----------------------------------------------------------------------

mkdir -p "$DATA" "$MOUNT" "$STATE"

cat >"$CONFIG" <<EOF
[server]
port           = 2049
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4
trace_file     = "$TRACE_FILE"

[backend]
type       = "$BACKEND_TYPE"
path       = "$DATA"
state_file = "$STATE"

[cache]
inode_cache_max  = 16384
dirent_cache_max = 65536

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]
EOF

# -----------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------

start_server() {
	info "Starting reffsd..."
	ulimit -c unlimited 2>/dev/null || true
	local asan_opts="quarantine_size_mb=256:detect_leaks=0:halt_on_error=1:handle_abort=1:print_stats=1"
	# RocksDB's Arena calls malloc_usable_size() on its own allocator
	# pointers, which ASAN rejects.  Disable that specific check.
	if [ "$BACKEND_TYPE" = "rocksdb" ]; then
		asan_opts="${asan_opts}:check_malloc_usable_size=0"
	fi
	ASAN_OPTIONS="$asan_opts" \
	UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	"$REFFSD_BIN" --config="$CONFIG" >"$LOG" 2>&1 &
	REFFSD_PID=$!

	for i in $(seq 1 30); do
		(echo >/dev/tcp/127.0.0.1/2049) 2>/dev/null && break
		kill -0 "$REFFSD_PID" 2>/dev/null || {
			info "reffsd died during startup"
			cat "$LOG"
			die "reffsd startup failure"
			return 1
		}
		sleep 1
	done
	info "reffsd up (PID $REFFSD_PID)"
}

stop_server() {
	info "Stopping reffsd (PID $REFFSD_PID)..."
	kill -TERM "$REFFSD_PID" 2>/dev/null
	for i in $(seq 1 60); do
		kill -0 "$REFFSD_PID" 2>/dev/null || break
		sleep 1
	done
	if kill -0 "$REFFSD_PID" 2>/dev/null; then
		info "reffsd still running after 60s, sending SIGKILL"
		kill -KILL "$REFFSD_PID" 2>/dev/null || true
	fi
	wait "$REFFSD_PID" 2>/dev/null || true
	REFFSD_PID=
}

mount_nfs() {
	mount -o vers=4.2,sec=sys,soft,timeo=30,retrans=3 127.0.0.1:/ "$MOUNT"
}

unmount_nfs() {
	umount -f -l "$MOUNT" 2>/dev/null || true
}

check_asan() {
	if grep -qE "ERROR: (AddressSanitizer|LeakSanitizer)" "$LOG" 2>/dev/null; then
		info "ASAN/LSAN error detected!"
		grep -A5 "ERROR:" "$LOG"
		die "ASAN error in reffsd log"
	fi
	if grep -q "runtime error:" "$LOG" 2>/dev/null; then
		info "UBSAN error detected!"
		grep "runtime error:" "$LOG"
		die "UBSAN error in reffsd log"
	fi
}

get_rss_kb() {
	local pid=$1
	if [ -f "/proc/$pid/status" ]; then
		awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null || echo 0
	else
		echo 0
	fi
}

get_fd_count() {
	local pid=$1
	if [ -d "/proc/$pid/fd" ]; then
		ls "/proc/$pid/fd" 2>/dev/null | wc -l
	else
		echo 0
	fi
}

# -----------------------------------------------------------------------
# Workload: git clone + make loop
# -----------------------------------------------------------------------

workload_build() {
	local id=$1
	local mount_dir=$2

	while true; do
		local work_dir="$mount_dir/soak_build_$id"
		rm -rf "$work_dir" 2>/dev/null
		git clone --quiet "$SRC_DIR" "$work_dir" 2>/dev/null || continue
		(
			cd "$work_dir" || exit
			mkdir -p m4
			autoreconf -fi >/dev/null 2>&1 || exit
			mkdir -p build_soak
			cd build_soak || exit
			../configure --disable-asan --disable-ubsan >/dev/null 2>&1 || exit
			make -j2 >/dev/null 2>&1 || exit
		) 2>/dev/null
		rm -rf "$work_dir" 2>/dev/null
		sleep 1
	done
}

# -----------------------------------------------------------------------
# Workload: random file operations
# -----------------------------------------------------------------------

workload_fileops() {
	local id=$1
	local mount_dir=$2
	local dir="$mount_dir/soak_ops_$id"

	mkdir -p "$dir" 2>/dev/null || true
	local seq=0

	while true; do
		seq=$((seq + 1))
		local f="$dir/file_$((seq % 100))"
		local d="$dir/dir_$((seq % 20))"

		# Create/overwrite a file
		dd if=/dev/urandom of="$f" bs=4096 count=$((RANDOM % 64 + 1)) 2>/dev/null || true

		# mkdir
		mkdir -p "$d" 2>/dev/null || true

		# rename
		local f2="$dir/renamed_$((seq % 50))"
		mv "$f" "$f2" 2>/dev/null || true

		# readdir
		ls "$dir" >/dev/null 2>/dev/null || true

		# remove
		rm -f "$dir/renamed_$((RANDOM % 50))" 2>/dev/null || true
		rmdir "$d" 2>/dev/null || true

		sleep 0
	done
}

# -----------------------------------------------------------------------
# Main soak loop
# -----------------------------------------------------------------------

info "=== Soak test: ${DURATION_MIN}m, restart every ${RESTART_MIN}m, ${CLIENTS} clients, backend=${BACKEND_TYPE} ==="

check_resources

if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
	rpcbind || die "rpcbind failed"
	sleep 1
fi

start_server || exit 1
mount_nfs || { cat "$LOG"; die "initial mount failed"; exit 1; }

# Start workloads BEFORE capturing baseline — the baseline should
# reflect the server under load, not the cold-start empty state.
# A cold-start baseline (52MB) vs under-load final (1.2GB) always
# fails the 2x check.
for i in $(seq 1 "$CLIENTS"); do
	if [ $((i % 2)) -eq 0 ]; then
		workload_fileops "$i" "$MOUNT" &
	else
		workload_build "$i" "$MOUNT" &
	fi
	WORKLOAD_PIDS+=($!)
done
info "Started $CLIENTS workload processes"

# Let workloads warm up, then capture the baseline under load.
info "Warming up (60s)..."
sleep 60
BASELINE_RSS=$(get_rss_kb "$REFFSD_PID")
BASELINE_FD=$(get_fd_count "$REFFSD_PID")
info "Baseline (under load): RSS=${BASELINE_RSS}KB FD=${BASELINE_FD}"

SOAK_START=$(date +%s)
RESTART_COUNT=0
NEXT_RESTART=$((SOAK_START + RESTART_SEC))

while true; do
	NOW=$(date +%s)
	ELAPSED=$((NOW - SOAK_START))

	if [ "$ELAPSED" -ge "$DURATION_SEC" ]; then
		info "Duration reached (${DURATION_MIN}m). Finishing."
		break
	fi

	# Periodic restart
	if [ "$NOW" -ge "$NEXT_RESTART" ]; then
		RESTART_COUNT=$((RESTART_COUNT + 1))
		info "=== Restart #$RESTART_COUNT ==="

		# Stop workloads
		for pid in "${WORKLOAD_PIDS[@]}"; do
			kill "$pid" 2>/dev/null
		done
		for pid in "${WORKLOAD_PIDS[@]}"; do
			wait "$pid" 2>/dev/null || true
		done
		WORKLOAD_PIDS=()

		unmount_nfs
		stop_server

		# Check log from this run
		check_asan
		if [ "$FAILED" = "true" ]; then
			info "Aborting soak due to errors"
			exit 1
		fi

		# Rotate log — keep previous run's output for diagnostics
		echo "=== Restart #$RESTART_COUNT at $(date) ===" >> "$LOG"

		start_server || exit 1

		# Force-unmount any stale NFS mount left by SIGKILL.
		# The previous umount may have failed (busy from workloads)
		# and SIGKILL leaves the kernel NFS superblock stale.
		umount -f -l "$MOUNT" 2>/dev/null || true
		sleep 1

		# Re-mount — must succeed within 30s (recovery check)
		mount_ok=false
		for try in $(seq 1 6); do
			if mount_nfs 2>/dev/null; then
				mount_ok=true
				break
			fi
			sleep 5
		done
		if [ "$mount_ok" != "true" ]; then
			die "mount failed after restart #$RESTART_COUNT"
			exit 1
		fi
		info "Mount succeeded after restart"

		# Restart workloads
		for i in $(seq 1 "$CLIENTS"); do
			if [ $((i % 2)) -eq 0 ]; then
				workload_fileops "$i" "$MOUNT" &
			else
				workload_build "$i" "$MOUNT" &
			fi
			WORKLOAD_PIDS+=($!)
		done

		NEXT_RESTART=$((NOW + RESTART_SEC))
	fi

	sleep 10

	# Periodic health check
	if kill -0 "$REFFSD_PID" 2>/dev/null; then
		RSS=$(get_rss_kb "$REFFSD_PID")
		FD=$(get_fd_count "$REFFSD_PID")
		info "Health: RSS=${RSS}KB FD=${FD} elapsed=${ELAPSED}s restarts=${RESTART_COUNT}"
	else
		die "reffsd crashed during soak"
		cat "$LOG"
		exit 1
	fi
done

# -----------------------------------------------------------------------
# Cleanup and final checks
# -----------------------------------------------------------------------

info "Stopping workloads..."
for pid in "${WORKLOAD_PIDS[@]}"; do
	kill "$pid" 2>/dev/null
done
for pid in "${WORKLOAD_PIDS[@]}"; do
	wait "$pid" 2>/dev/null || true
done
WORKLOAD_PIDS=()

unmount_nfs

# Final resource check
FINAL_RSS=$(get_rss_kb "$REFFSD_PID")
FINAL_FD=$(get_fd_count "$REFFSD_PID")

stop_server
check_asan

info "=== Final metrics ==="
info "  Baseline: RSS=${BASELINE_RSS}KB FD=${BASELINE_FD}"
info "  Final:    RSS=${FINAL_RSS}KB FD=${FINAL_FD}"
info "  Restarts: $RESTART_COUNT"

# RSS check: final within 2x of baseline
if [ "$BASELINE_RSS" -gt 0 ] && [ "$FINAL_RSS" -gt $((BASELINE_RSS * 2)) ]; then
	die "Memory growth: ${FINAL_RSS}KB > 2x baseline ${BASELINE_RSS}KB"
fi

# FD check: final within 10% of baseline
if [ "$BASELINE_FD" -gt 0 ]; then
	FD_LIMIT=$(( BASELINE_FD + BASELINE_FD / 10 + 5 ))
	if [ "$FINAL_FD" -gt "$FD_LIMIT" ]; then
		die "FD growth: ${FINAL_FD} > ${FD_LIMIT} (baseline ${BASELINE_FD} + 10%)"
	fi
fi

if [ "$FAILED" = "true" ]; then
	SOAK_POSTMORTEM_DONE=true  # we already printed diagnostics
	info "=== SOAK TEST FAILED ==="
	cat "$LOG"
	exit 1
fi

SOAK_POSTMORTEM_DONE=true  # clean exit — suppress post-mortem
info "=== SOAK TEST PASSED (${DURATION_MIN}m, ${RESTART_COUNT} restarts, ${BACKEND_TYPE}) ==="
