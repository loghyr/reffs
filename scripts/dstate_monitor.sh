#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# dstate_monitor.sh -- Watch for D-state processes on an NFS mount.
#
# Runs in background during soak tests.  Classifies D-state as:
#   - Baseline (pre-existing at monitor startup): filtered out entirely
#   - Transient (during restart grace period): logged, not a failure
#   - Persistent (after server is back): logged as FAILURE
#
# Usage:
#   scripts/dstate_monitor.sh MOUNT_PATH [LOGFILE]
#
# Signals:
#   USR1  Begin grace period (restart starting, suppress for 120s)
#   TERM  Stop monitoring
#
# Protocol: for each restart cycle, the soak script kills this monitor
# (TERM), allocates a fresh mount path (e.g. /mnt/reffs_local_soak_N),
# mounts the server there, starts a new monitor instance on the fresh
# path, then sends USR1 to begin the startup grace period.  Using a
# fresh path per cycle prevents stale-superblock false positives from
# D-state processes that held the previous mount point.
# Lines starting with "FAILURE:" indicate D-state that persisted after
# the grace period.

set -uo pipefail

MOUNT=${1:?Usage: dstate_monitor.sh MOUNT_PATH [LOGFILE]}
LOGFILE=${2:-/tmp/dstate_monitor.log}
GRACE_SEC=120
POLL_SEC=5

# Grace period state
GRACE_UNTIL=0

log() { echo "[$(date +%H:%M:%S)] $*" >> "$LOGFILE"; }

handle_usr1() {
	GRACE_UNTIL=$(( $(date +%s) + GRACE_SEC ))
	log "GRACE: restart signaled, suppressing D-state alerts for ${GRACE_SEC}s"
}

handle_usr2() {
	# Refresh baseline with any D-state processes that are still alive
	# on the mount after the restart completed.  These are stragglers
	# from the previous cycle that we could not force-exit; adding them
	# to the baseline prevents accumulation from causing false failures.
	local new_pids=()
	while IFS= read -r line; do
		local pid
		pid=$(echo "$line" | awk '{print $1}')
		local found=false
		if readlink /proc/$pid/cwd 2>/dev/null | grep -q "$MOUNT"; then
			found=true
		fi
		if [ "$found" = "false" ]; then
			if readlink /proc/$pid/fd/* 2>/dev/null | grep -q "$MOUNT"; then
				found=true
			fi
		fi
		if [ "$found" = "true" ]; then
			is_baseline_pid "$pid" || new_pids+=("$pid")
		fi
	done < <(ps -eo pid,stat,wchan:32,comm 2>/dev/null | grep ' D ')
	if [ ${#new_pids[@]} -gt 0 ]; then
		for p in "${new_pids[@]}"; do
			BASELINE_PIDS+=("$p")
		done
		log "BASELINE-REFRESH: absorbed ${#new_pids[@]} straggler D-state PIDs: ${new_pids[*]}"
	fi
	GRACE_UNTIL=$(( $(date +%s) + GRACE_SEC ))
	log "GRACE: post-recovery baseline refreshed, grace reset for ${GRACE_SEC}s"
}

handle_term() {
	log "STOP: monitor shutting down"
	exit 0
}

trap handle_usr1 USR1
trap handle_usr2 USR2
trap handle_term TERM

: > "$LOGFILE"
log "START: monitoring $MOUNT (grace=${GRACE_SEC}s, poll=${POLL_SEC}s)"

# Capture pre-existing D-state PIDs before the first poll.
# D-state processes from previous soak runs may never exit without a
# reboot -- they hold the write end of a dead NFS mount that the kernel
# cannot unblock.  Recording them at startup lets us filter them out so
# they do not cause false failures for the current run.
BASELINE_PIDS=()
while IFS= read -r line; do
	pid=$(echo "$line" | awk '{print $1}')
	# Use readlink (not ls -l) to read the symlink target string from the
	# kernel without statting the target.  ls -l stats each fd symlink,
	# which blocks in D-state if the fd points to a dead NFS mount --
	# the monitor then gets stuck in D-state itself.
	if readlink /proc/$pid/cwd 2>/dev/null | grep -q "$MOUNT" || \
	   readlink /proc/$pid/fd/* 2>/dev/null | grep -q "$MOUNT"; then
		BASELINE_PIDS+=("$pid")
	fi
done < <(ps -eo pid,stat,wchan:32,comm 2>/dev/null | grep ' D ')
if [ ${#BASELINE_PIDS[@]} -gt 0 ]; then
	log "BASELINE: ${#BASELINE_PIDS[@]} pre-existing D-state PIDs (filtered): ${BASELINE_PIDS[*]}"
fi

is_baseline_pid() {
	local p=$1
	for b in "${BASELINE_PIDS[@]}"; do
		[ "$b" = "$p" ] && return 0
	done
	return 1
}

while true; do
	sleep "$POLL_SEC" &
	wait $! 2>/dev/null || true  # interruptible sleep for signal handling

	NOW=$(date +%s)
	IN_GRACE=false
	if [ "$NOW" -lt "$GRACE_UNTIL" ]; then
		IN_GRACE=true
	fi

	# Find D-state processes on our mount, excluding pre-existing baseline PIDs
	DSTATE_PIDS=()
	while IFS= read -r line; do
		pid=$(echo "$line" | awk '{print $1}')
		if readlink /proc/$pid/cwd 2>/dev/null | grep -q "$MOUNT" || \
		   readlink /proc/$pid/fd/* 2>/dev/null | grep -q "$MOUNT"; then
			is_baseline_pid "$pid" && continue
			DSTATE_PIDS+=("$pid")
		fi
	done < <(ps -eo pid,stat,wchan:32,comm 2>/dev/null | grep ' D ')

	if [ ${#DSTATE_PIDS[@]} -eq 0 ]; then
		continue
	fi

	# D-state detected on our mount
	if [ "$IN_GRACE" = true ]; then
		log "TRANSIENT: ${#DSTATE_PIDS[@]} D-state processes (in grace period, $(( GRACE_UNTIL - NOW ))s remaining)"
		for pid in "${DSTATE_PIDS[@]}"; do
			comm=$(cat /proc/$pid/comm 2>/dev/null || echo "?")
			log "  PID $pid ($comm)"
		done
	else
		log "FAILURE: ${#DSTATE_PIDS[@]} D-state processes OUTSIDE grace period"
		for pid in "${DSTATE_PIDS[@]}"; do
			comm=$(cat /proc/$pid/comm 2>/dev/null || echo "?")
			log "  PID $pid ($comm)"
			log "  stack:"
			sudo cat /proc/$pid/stack 2>/dev/null >> "$LOGFILE" || \
				log "    (cannot read stack)"
		done
		# Capture NFS-related dmesg
		log "  dmesg (nfs):"
		dmesg 2>/dev/null | grep -i nfs | tail -10 >> "$LOGFILE" || true
		log "  ---"
	fi
done
