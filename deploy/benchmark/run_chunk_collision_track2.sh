#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Chunk-collision Track 2 harness: drive IOR (shared-file,
# write-then-verify) through N Proxy Server containers.  Each PS
# holds its own MDS-facing session, so N PSes = N distinct
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
# See .claude/design/chunk-collision-track2.md.
#
# Usage:
#   run_chunk_collision_track2.sh [--n N] [--reorder]
#       [--transfer SZ] [--block SZ] [--segments N] [--iterations N]
#
#   --n N          PS count == MPI rank count (default 4)
#   --reorder      add IOR -C (reorderTasks): each rank verifies
#                  another rank's data, cross-PS
#   --transfer SZ  IOR -t transfer size   (default 64k)
#   --block SZ     IOR -b block size      (default 4m)
#   --segments N   IOR -s segment count   (default 4)
#   --iterations N IOR -i repeat count    (default 3)
#
# Pre-conditions: see deploy/benchmark/run-ps-bench-bringup.sh.

set -euo pipefail

# -- args -------------------------------------------------------------
N=4
REORDER=""
XFER="64k"
BLOCK="4m"
SEGS=4
ITERS=3

while [[ $# -gt 0 ]]; do
	case "$1" in
		--n)          N="$2";       shift 2 ;;
		--reorder)    REORDER="-C"; shift   ;;
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
echo "  PSes / ranks: ${N}"
echo "  IOR:          shared file, -w -r -W -R ${REORDER} -e -k"
echo "                -t ${XFER} -b ${BLOCK} -s ${SEGS} -i ${ITERS}"
echo "  shared file:  /ior_shared.dat (one MDS file via N PSes)"
echo ""

# -- step 1: bring up MDS + 10 DSes + N PSes --------------------------
echo "[t2] bringing up topology (NPS=${N})..."
NPS="${N}" bash "${HERE}/run-ps-bench-bringup.sh"

# -- step 2: pre-run chunk-counter snapshot (best-effort) -------------
# The per-sb chunk counters (sb_chunk_writes, sb_chunk_pending_
# displaced, sb_chunk_finalize_crc_fail, ...) shipped with the
# chunk-collision BLOCKER 2 work.  Snapshot them so the operator
# can read the deltas; this is diagnostic -- the hard gate is the
# IOR verify result and the sanitizer scan below.
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
		echo " IOR verify + sanitizer scan remain the hard gate)"
	fi
}
snapshot_counters "pre-run"

# -- step 3: run IOR via N PSes ---------------------------------------
# One privileged, host-networked IOR container kernel-mounts each
# PS listener and runs mpirun -np N locally.  An ior-rank wrapper
# rewrites -o per rank so each rank reaches the SAME MDS file
# through a DIFFERENT PS (= a different clientid on the MDS).
echo ""
echo "[t2] launching IOR client container..."
sudo docker rm -f "${IOR_CONTAINER}" 2>/dev/null || true

# Build the in-container driver script.
IOR_DRIVER=$(cat <<EOF
set -eu
N=${N}
XFER=${XFER}
BLOCK=${BLOCK}
SEGS=${SEGS}
ITERS=${ITERS}
REORDER='${REORDER}'

# Mount every PS listener.  PS r serves the proxy namespace on
# host port 4098+r; mount its root at /mnt/ps-r.
for r in \$(seq 0 \$((N - 1))); do
	mkdir -p /mnt/ps-\$r
	mount -t nfs4 -o port=\$((4098 + r)),vers=4.2,sec=sys,nolock \
	      127.0.0.1:/ /mnt/ps-\$r
done

# ior-rank wrapper: substitute the MPI rank into -o so each rank
# targets the shared file through its own PS mount.
cat > /usr/local/bin/ior-rank.sh <<'WRAP'
#!/bin/bash
exec ior "\$@" -o "/mnt/ps-\${OMPI_COMM_WORLD_RANK}/ior_shared.dat"
WRAP
chmod +x /usr/local/bin/ior-rank.sh

set +e
# IOR shared-file mode is the DEFAULT -- do NOT pass -F (that flag
# is the boolean file-per-process toggle; there is no "-F 0", and
# a stray "0" makes IOR reject the whole command line).
#
# --mca pml ob1 --mca btl self,tcp forces OpenMPI off the OFI /
# libfabric path that SIGBUSes inside MPI_Init on the Fedora 43
# image's OpenMPI 5.0.8 / UCX 1.18.1 / libfabric 2.2.0 stack.
# The failing backtrace is mca_btl_base_select -> libopen-pal ->
# libfabric -> libucs page-fault.  The OB1 PML + self/tcp BTLs
# bypass UCX/libfabric entirely.  Repro confirmed in isolation
# (np=8 IOR write+read): default mpirun SIGBUSes, this flag set
# completes cleanly.  Track 2 is a correctness validation, not
# a benchmark, so the TCP transport between in-container ranks
# is fine.
mpirun --allow-run-as-root --oversubscribe -np \$N \
	--mca pml ob1 --mca btl self,tcp \
	/usr/local/bin/ior-rank.sh \
	-a POSIX -w -r -W -R \$REORDER -e -k \
	-t \$XFER -b \$BLOCK -s \$SEGS -i \$ITERS
rc=\$?
set -e

for r in \$(seq 0 \$((N - 1))); do
	umount /mnt/ps-\$r 2>/dev/null || true
done
exit \$rc
EOF
)

set +e
sudo docker run --rm --name "${IOR_CONTAINER}" \
	--network=host \
	--privileged \
	-v benchmark_build-vol:/shared:ro,z \
	"${IMAGE}" \
	/bin/bash -c "${IOR_DRIVER}" \
	2>&1 | tee /tmp/reffs-t2-ior.log
ior_rc=${PIPESTATUS[0]}
set -e

# -- step 4: post-run counter snapshot --------------------------------
echo ""
snapshot_counters "post-run"

# -- step 5: pass criteria --------------------------------------------
echo ""
echo "=== Track 2 result ==="
fail=0

# Criterion 1: IOR reports zero verify mismatches.
if [ "${ior_rc}" -ne 0 ]; then
	echo "FAIL: IOR exited non-zero (${ior_rc})"
	fail=$((fail + 1))
elif grep -qiE 'errors? *[1-9]|verification failed|data check error' \
	     /tmp/reffs-t2-ior.log; then
	echo "FAIL: IOR reported verify mismatches"
	grep -iE 'error|verif' /tmp/reffs-t2-ior.log | head -10
	fail=$((fail + 1))
else
	echo "PASS: IOR write+verify clean (rc=0, no mismatches)"
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
	echo "inspection; IOR log at /tmp/reffs-t2-ior.log"
	exit 2
fi

echo "Track 2: PASS (${N} PSes, IOR clean, sanitizers clean, no force-drain warnings)"
echo "IOR log: /tmp/reffs-t2-ior.log"
exit 0
