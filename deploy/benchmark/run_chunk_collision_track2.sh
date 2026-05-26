#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Chunk-collision Track 2 harness: drive fio (shared-file,
# write-then-verify, no MPI) through N Proxy Server containers.
# Each PS holds its own MDS-facing session, so N PSes = N distinct
# clientid4s contending on one shared MDS file -- the multi-writer
# contention surface, exercised through the client-visible POSIX
# path (kernel NFS mount -> PS :4098+ listener -> REFFS_DATA_PROXY
# backend -> CHUNK ops) rather than ec_demo driving CHUNK ops
# directly.
#
# This is a CORRECTNESS validation, not a benchmark.  Container
# throughput numbers do not transfer (see chunk-collision-
# validation.md "Cost model").
#
# Why fio and not IOR/mpirun?  The Fedora 43 image's OpenMPI 5.0.8
# stack wedges in two distinct collectives back-to-back:
#   1. MPI_Init -> mca_btl_base_select -> libfabric SIGBUS
#      (worked around once by --mca pml ob1 --mca btl self,tcp)
#   2. MPI_Comm_split_type -> ompi_comm_nextcid -> opal_progress
#      busy-loop, never makes forward progress with the same
#      MCA workaround
# fio runs N independent processes -- no MPI, no PMIx, no UCX, no
# libfabric.  Pattern-based verify (verify_pattern keyed on rank)
# detects both byte corruption and cross-PS misrouting: a chunk-
# collision bug that lands rank A's writes in rank B's offsets
# shows up as B's verify pass reading A's pattern.
#
# See .claude/design/chunk-collision-track2.md.
#
# Usage:
#   run_chunk_collision_track2.sh [--n N] [--reorder]
#       [--transfer SZ] [--block SZ] [--segments N] [--iterations N]
#
#   --n N          PS count == fio writer count (default 4)
#   --reorder      rank r verifies rank ((r+1)%N)'s stripe through
#                  rank r's PS mount -- cross-PS readback path
#   --transfer SZ  fio --bs (block size)         (default 64k)
#   --block SZ     per-rank per-segment size     (default 4m)
#   --segments N   segments per rank             (default 4)
#   --iterations N write+verify repeat count     (default 3)
#
# Pre-conditions: see deploy/benchmark/run-ps-bench-bringup.sh.

set -euo pipefail

# -- args -------------------------------------------------------------
N=4
REORDER=0
XFER="64k"
BLOCK="4m"
SEGS=4
ITERS=3

while [[ $# -gt 0 ]]; do
	case "$1" in
		--n)          N="$2";       shift 2 ;;
		--reorder)    REORDER=1;    shift   ;;
		--transfer)   XFER="$2";    shift 2 ;;
		--block)      BLOCK="$2";   shift 2 ;;
		--segments)   SEGS="$2";    shift 2 ;;
		--iterations) ITERS="$2";   shift 2 ;;
		-h|--help)
			grep '^# ' "$0" | sed 's/^# \{0,1\}//'
			exit 0 ;;
		*)
			echo "unknown arg: $1" >&2
			exit 1 ;;
	esac
done

if ! [[ "$N" =~ ^[0-9]+$ ]] || [ "$N" -lt 2 ] || [ "$N" -gt 32 ]; then
	echo "FAIL: --n must be an integer in [2, 32], got '$N'"
	exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)
IOR_CONTAINER=reffs-ior-client
IMAGE=reffs-dev:latest

echo "=== chunk-collision Track 2 ==="
echo "  PSes / ranks:   ${N}"
echo "  driver:         fio (no MPI; pattern verify, do_verify=1)"
echo "  per-rank stripe: ${BLOCK} * ${SEGS} segments"
echo "  transfer:       ${XFER}"
echo "  iterations:     ${ITERS}"
echo "  reorder mode:   $([ "$REORDER" -eq 1 ] && echo "ON (cross-PS verify)" || echo "off (same-PS verify)")"
echo ""

# -- step 1: bring up MDS + 10 DSes + N PSes --------------------------
echo "[t2] bringing up topology (NPS=${N})..."
NPS="${N}" bash "${HERE}/run-ps-bench-bringup.sh"

# -- step 2: pre-run chunk-counter snapshot (best-effort) -------------
# The per-sb chunk counters (sb_chunk_writes, sb_chunk_pending_
# displaced, sb_chunk_finalize_crc_fail, ...) shipped with the
# chunk-collision BLOCKER 2 work.  Snapshot them so the operator
# can read the deltas; this is diagnostic -- the hard gate is the
# fio verify result and the sanitizer scan below.
snapshot_counters() {
	local label="$1"
	echo "--- chunk counters (${label}) ---"
	# Try the C probe client inside the MDS container; the probe
	# server listens on the MDS's own probe port (127.0.0.1).
	local clnt
	clnt=$(sudo docker exec reffs-bench-mds \
	           sh -c 'command -v reffs_probe1_clnt \
	                  || find /shared/build -name reffs_probe1_clnt -type f \
	                       2>/dev/null | head -1' 2>/dev/null || true)
	if [ -n "$clnt" ]; then
		sudo docker exec reffs-bench-mds "$clnt" --op sb-list \
			2>/dev/null || echo "(probe sb-list failed)"
	else
		echo "(probe client not found -- counter snapshot skipped;"
		echo " fio verify + sanitizer scan remain the hard gate)"
	fi
}
snapshot_counters "pre-run"

# -- step 3: run fio via N PSes ---------------------------------------
# One privileged, host-networked container kernel-mounts each PS
# listener and launches N parallel fio processes (one per PS).
# Rank r writes a deterministic 4-byte pattern (0xRRRRRRRR, where
# RR is r's byte) into its stripe; the verify pass re-reads and
# compares against the same pattern.  With --reorder, rank r
# verifies rank ((r+1)%N)'s stripe through rank r's PS mount, so
# a chunk-collision bug that misroutes blocks between PSes shows
# up as a pattern mismatch.
echo ""
echo "[t2] launching fio client container..."
sudo docker rm -f "${IOR_CONTAINER}" 2>/dev/null || true

# Build the in-container driver script in a tempfile.  Writing the
# heredoc directly to a file (rather than capturing via
# FIO_DRIVER=$(cat <<EOF ...)) avoids a bash static-parser issue:
# `case` arms inside an inline command-substitution heredoc trip
# the paren-balance tracker and bash -n rejects the whole script.
# The body is single-quoted (<<'EOF') so it stays literal until the
# sed pass below substitutes the placeholders.
FIO_DRIVER=$(mktemp /tmp/reffs-t2-driver.XXXXXX.sh)
trap "rm -f \"${FIO_DRIVER}\"" EXIT INT TERM
cat > "${FIO_DRIVER}" <<'EOF'
set -eu
N=__N__
XFER=__XFER__
BLOCK=__BLOCK__
SEGS=__SEGS__
ITERS=__ITERS__
REORDER=__REORDER__

# fio is not in reffs-dev:latest yet; install on first run.
# NOT_NOW_BROWN_COW: fold fio into Dockerfile in a follow-up.
if ! command -v fio >/dev/null 2>&1; then
	echo "[t2] installing fio in container..."
	dnf -y -q install fio
fi

# Mount every PS listener.  PS r serves the proxy namespace on
# host port 4098+r; mount its root at /mnt/ps-r.
for r in $(seq 0 $((N - 1))); do
	mkdir -p /mnt/ps-$r
	mount -t nfs4 -o port=$((4098 + r)),vers=4.2,sec=sys,nolock \
	      127.0.0.1:/ /mnt/ps-$r
done

# Convert size suffixes (k/K/m/M/g/G) to bytes for offset math.
to_bytes() {
	local v="$1"
	case "${v: -1}" in
		k|K) echo $(( ${v%[kK]} * 1024 )) ;;
		m|M) echo $(( ${v%[mM]} * 1024 * 1024 )) ;;
		g|G) echo $(( ${v%[gG]} * 1024 * 1024 * 1024 )) ;;
		*)   echo "$v" ;;
	esac
}
BLOCK_BYTES=$(to_bytes "$BLOCK")
STRIPE_BYTES=$(( BLOCK_BYTES * SEGS ))
TOTAL_BYTES=$(( STRIPE_BYTES * N ))
echo "[t2] stripe=$STRIPE_BYTES bytes/rank, file size=$TOTAL_BYTES bytes"

# Rank-keyed 4-byte pattern: 0xRRRRRRRR.
pat() { printf '0x%02x%02x%02x%02x' "$1" "$1" "$1" "$1"; }

rc_total=0
for iter in $(seq 1 $ITERS); do
	echo ""
	echo "=== iter $iter / $ITERS ==="

	# --- write phase: N parallel fio writers -----------------
	pids=()
	for r in $(seq 0 $((N - 1))); do
		off=$(( r * STRIPE_BYTES ))
		fio --name=w$r \
		    --filename=/mnt/ps-$r/shared.dat \
		    --rw=write --bs=$XFER \
		    --offset=$off --size=$STRIPE_BYTES \
		    --verify=pattern --verify_pattern=$(pat $r) \
		    --do_verify=0 \
		    --create_on_open=1 --allow_file_create=1 \
		    --thread=0 --ioengine=psync \
		    > /tmp/fio-w-r$r-i$iter.log 2>&1 &
		pids+=($!)
	done
	rc_write=0
	for p in "${pids[@]}"; do
		wait $p || rc_write=1
	done
	if [ $rc_write -ne 0 ]; then
		echo "iter $iter: WRITE phase failed"
		for r in $(seq 0 $((N - 1))); do
			echo "--- rank $r write log ---"
			tail -20 /tmp/fio-w-r$r-i$iter.log
		done
		rc_total=1
		break
	fi

	# --- verify phase: N parallel fio readers ----------------
	# Same-PS verify: rank r reads its own stripe through its
	# own mount.  Cross-PS verify (REORDER): rank r reads rank
	# ((r+1) % N)'s stripe through rank r's mount.
	pids=()
	for r in $(seq 0 $((N - 1))); do
		if [ "$REORDER" -eq 1 ]; then
			src=$(( (r + 1) % N ))
		else
			src=$r
		fi
		off=$(( src * STRIPE_BYTES ))
		fio --name=v$r \
		    --filename=/mnt/ps-$r/shared.dat \
		    --rw=read --bs=$XFER \
		    --offset=$off --size=$STRIPE_BYTES \
		    --verify=pattern --verify_pattern=$(pat $src) \
		    --do_verify=1 --verify_only=1 \
		    --thread=0 --ioengine=psync \
		    > /tmp/fio-v-r$r-i$iter.log 2>&1 &
		pids+=($!)
	done
	rc_verify=0
	for p in "${pids[@]}"; do
		wait $p || rc_verify=1
	done
	if [ $rc_verify -ne 0 ]; then
		echo "iter $iter: VERIFY phase failed"
		for r in $(seq 0 $((N - 1))); do
			echo "--- rank $r verify log ---"
			tail -20 /tmp/fio-v-r$r-i$iter.log
		done
		rc_total=1
		break
	fi

	echo "iter $iter: PASS"
done

# Unmount (don't fail teardown on a wedged unmount).
for r in $(seq 0 $((N - 1))); do
	umount /mnt/ps-$r 2>/dev/null || true
done
exit $rc_total
EOF

# Substitute outer-script values into the literal heredoc.
sed -i \
	-e "s|__N__|${N}|g" \
	-e "s|__XFER__|${XFER}|g" \
	-e "s|__BLOCK__|${BLOCK}|g" \
	-e "s|__SEGS__|${SEGS}|g" \
	-e "s|__ITERS__|${ITERS}|g" \
	-e "s|__REORDER__|${REORDER}|g" \
	"${FIO_DRIVER}"

set +e
sudo docker run --rm -i --name "${IOR_CONTAINER}" \
	--network=host \
	--privileged \
	-v benchmark_build-vol:/shared:ro,z \
	"${IMAGE}" \
	bash -s < "${FIO_DRIVER}" \
	2>&1 | tee /tmp/reffs-t2-fio.log
fio_rc=${PIPESTATUS[0]}
set -e

# -- step 4: post-run counter snapshot --------------------------------
echo ""
snapshot_counters "post-run"

# -- step 5: pass criteria --------------------------------------------
echo ""
echo "=== Track 2 result ==="
fail=0

# Criterion 1: fio reports zero verify mismatches.
# fio's verify-failure phrasing varies by version: "verify: bad",
# "verify failed", "bad pattern", "verify_dump", and the per-rank
# "PASS" lines from our driver.  Grep broadly.
if [ "${fio_rc}" -ne 0 ]; then
	echo "FAIL: fio driver exited non-zero (${fio_rc})"
	fail=$((fail + 1))
elif grep -qiE 'verify: bad|verify failed|verification failed|bad pattern|data check error' \
	     /tmp/reffs-t2-fio.log; then
	echo "FAIL: fio reported verify mismatches"
	grep -iE 'verify|pattern|data check' /tmp/reffs-t2-fio.log | head -10
	fail=$((fail + 1))
else
	echo "PASS: fio write+verify clean (rc=0, no mismatches)"
fi

# Criterion 3: zero ASAN / UBSAN errors in any MDS / DS / PS log.
san_hits=0
for c in reffs-bench-mds $(sudo docker ps -a --format '{{.Names}}' \
                               | grep -E '^reffs-bench-ds|^reffs-ps-'); do
	if sudo docker logs "$c" 2>&1 \
	     | grep -qE 'ERROR: AddressSanitizer|runtime error:'; then
		echo "FAIL: sanitizer hit in ${c}:"
		sudo docker logs "$c" 2>&1 \
			| grep -E 'ERROR: AddressSanitizer|runtime error:' | head -3
		san_hits=$((san_hits + 1))
	fi
done
if [ "${san_hits}" -eq 0 ]; then
	echo "PASS: no ASAN / UBSAN errors across MDS / DS / PS logs"
else
	fail=$((fail + san_hits))
fi

# Criterion 4: zero CONN_CLOSING force-drain warnings.  Per
# .claude/design/conn-info-closing-wedge.md (BLOCKER CLOSED): the
# Slice 1 backstop is a safety net; any "stuck in CLOSING ...
# force-draining" line emitted by lib/io/conn_info.c is the signal
# that a genuine accept-CQE-completion leak (Bug A) has surfaced
# and needs its own follow-up.  Slice 2's listener-exemption fix
# means a clean run is reachable; if this criterion fails, capture
# the warning lines (the "(counts: r=%d w=%d a=%d c=%d, ...)" block
# pins the leaked counter) before tearing down the containers.
drain_hits=0
for c in reffs-bench-mds $(sudo docker ps -a --format '{{.Names}}' \
                               | grep -E '^reffs-bench-ds|^reffs-ps-'); do
	if sudo docker logs "$c" 2>&1 \
	     | grep -qE 'stuck in CLOSING.*force-draining'; then
		echo "FAIL: CONN_CLOSING force-drain warning in ${c}:"
		sudo docker logs "$c" 2>&1 \
			| grep -E 'stuck in CLOSING.*force-draining' | head -5
		drain_hits=$((drain_hits + 1))
	fi
done
if [ "${drain_hits}" -eq 0 ]; then
	echo "PASS: no CONN_CLOSING force-drain warnings across MDS / DS / PS logs"
else
	fail=$((fail + drain_hits))
fi

echo ""
if [ "${fail}" -gt 0 ]; then
	echo "Track 2: FAIL (${fail} issue(s)).  Containers left up for"
	echo "inspection; fio log at /tmp/reffs-t2-fio.log"
	exit 2
fi

echo "Track 2: PASS (${N} PSes, fio clean, sanitizers clean, no force-drain warnings)"
echo "fio log: /tmp/reffs-t2-fio.log"
exit 0
