#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# PS-encoder vs client-encoder benchmark scaffold.
#
# Produces head-to-head numbers for three workload variants
# against the same MDS + DSes topology:
#
#   A -- client EC (today's ec_demo write/read against MDS)
#   B -- PS EC    (kernel NFSv4.2 mount of PS :4098, PS encodes)
#   C -- MDS inband (kernel NFSv4.2 mount of MDS :2049, no EC)
#
# This is the data side of bucket 2 in
# ~/Documents/reffs-docs/ietf126-plan.md (the load-bearing
# argument for the WG-relevant "where does encoding belong"
# question).  Design lives in .claude/design/ps-encoder-bench.md.
#
# Topology: reuses run-ps-bench-bringup.sh with NPS=1.  Linux
# host required (host networking + privileged containers); will
# refuse to run on macOS the same way the bringup does.
#
# Status: SCAFFOLD.  The driver functions for variants A, B, C
# are sketched.  Actual execution against the bench topology has
# not been smoked yet (per design doc Implementation Order
# step 3).  Run --dry-run to see what each variant would do.
#
# Usage:
#   run_ps_vs_client_bench.sh [--n N] [--sizes "4k,16k,64k,256k,1m"]
#                             [--codec rs] [--k K] [--m M]
#                             [--shard-size BYTES]
#                             [--out-csv PATH] [--dry-run]
#                             [--variants "A,B,C"]
#
# CSV output columns:
#   variant,size,op,iter,ms,bytes,k,m,codec,host,timestamp

set -euo pipefail

# -- defaults ---------------------------------------------------------
N_ITERS=5
SIZES="4k,16k,64k,256k,1m"
CODEC="rs"
K=4
M=2
SHARD_SIZE=4096
OUT_CSV="ps_vs_client_$(date +%Y%m%d_%H%M%S).csv"
DRY_RUN=0
VARIANTS="A,B,C"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--n)          N_ITERS="$2";    shift 2 ;;
		--sizes)      SIZES="$2";      shift 2 ;;
		--codec)      CODEC="$2";      shift 2 ;;
		--k)          K="$2";          shift 2 ;;
		--m)          M="$2";          shift 2 ;;
		--shard-size) SHARD_SIZE="$2"; shift 2 ;;
		--out-csv)    OUT_CSV="$2";    shift 2 ;;
		--dry-run)    DRY_RUN=1;       shift   ;;
		--variants)   VARIANTS="$2";   shift 2 ;;
		-h|--help)
			grep '^# ' "$0" | sed 's/^# \{0,1\}//'
			exit 0 ;;
		*)
			echo "unknown arg: $1" >&2
			exit 1 ;;
	esac
done

# -- platform sanity --------------------------------------------------
# Same check as run-ps-bench-bringup.sh -- this is Linux infra.
if [[ "$DRY_RUN" -eq 0 ]] && ! command -v ip >/dev/null 2>&1; then
	echo "FAIL: this harness requires a Linux host (iproute2)." >&2
	echo "Run on dreamer / garbo / similar; --dry-run works anywhere." >&2
	exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)
HOSTNAME_SHORT=$(hostname -s 2>/dev/null || echo "unknown")
TIMESTAMP=$(date -u +%Y-%m-%dT%H:%M:%SZ)

# -- write CSV header -------------------------------------------------
echo "variant,size,op,iter,ms,bytes,k,m,codec,host,timestamp" > "$OUT_CSV"
echo "[ps-bench] writing CSV to $OUT_CSV"

# -- topology bringup -------------------------------------------------
# NPS=1 is the bucket-2 shape: one PS, one client.  Multi-PS is
# chunk-collision Track 2's concern, not throughput.
if [[ "$DRY_RUN" -eq 0 ]]; then
	echo "[ps-bench] bringing up topology (NPS=1)..."
	NPS=1 bash "${HERE}/run-ps-bench-bringup.sh"
else
	echo "[ps-bench] DRY RUN: would run NPS=1 ${HERE}/run-ps-bench-bringup.sh"
fi

# -- size parsing -----------------------------------------------------
# Convert "4k" / "1m" suffixes to bytes for dd / fio bs= args.
size_to_bytes() {
	local v="$1"
	case "${v: -1}" in
		k|K) echo $(( ${v%[kK]} * 1024 )) ;;
		m|M) echo $(( ${v%[mM]} * 1024 * 1024 )) ;;
		g|G) echo $(( ${v%[gG]} * 1024 * 1024 * 1024 )) ;;
		*)   echo "$v" ;;
	esac
}

# -- CSV emit helper --------------------------------------------------
emit_row() {
	local variant="$1" size="$2" op="$3" iter="$4" ms="$5" bytes="$6"
	echo "${variant},${size},${op},${iter},${ms},${bytes},${K},${M},${CODEC},${HOSTNAME_SHORT},${TIMESTAMP}" \
		>> "$OUT_CSV"
}

# -- variant A: ec_demo client-direct EC -----------------------------
# Re-uses scripts/ec_benchmark.sh shape.  For the scaffold we
# only sketch the command; the actual invocation drops into the
# bench container that already has ec_demo on PATH.
run_variant_a() {
	local size="$1" iter="$2"
	local bytes=$(size_to_bytes "$size")
	local file="A-${size}-i${iter}.dat"

	if [[ "$DRY_RUN" -eq 1 ]]; then
		echo "[A] would: ec_demo write --mds reffs-mds --file ${file}"
		echo "           --input /tmp/payload-${size} --k ${K} --m ${M}"
		echo "           --codec ${CODEC} --layout v2"
		echo "           --shard-size ${SHARD_SIZE}"
		emit_row "A" "$size" "write" "$iter" "DRY" "$bytes"
		emit_row "A" "$size" "read"  "$iter" "DRY" "$bytes"
		return 0
	fi

	# TODO(scaffold): drop into the bench client container,
	# generate /tmp/payload-${size}, time the ec_demo write
	# and the subsequent ec_demo read (no --skip-ds).
	echo "[A] $size iter=$iter -- NOT YET IMPLEMENTED" >&2
}

# -- variant B: kernel NFS mount of PS, PS encodes -------------------
# PS listens on host port 4098; mount and dd against it.
run_variant_b() {
	local size="$1" iter="$2"
	local bytes=$(size_to_bytes "$size")
	local file="B-${size}-i${iter}.dat"

	if [[ "$DRY_RUN" -eq 1 ]]; then
		echo "[B] would: mount -t nfs4 -o port=4098,vers=4.2,sec=sys,nolock"
		echo "           127.0.0.1:/ /mnt/ps0"
		echo "           dd if=/dev/urandom of=/mnt/ps0/${file}"
		echo "           bs=${bytes} count=1 conv=fsync"
		echo "           dd if=/mnt/ps0/${file} of=/dev/null bs=${bytes}"
		emit_row "B" "$size" "write" "$iter" "DRY" "$bytes"
		emit_row "B" "$size" "read"  "$iter" "DRY" "$bytes"
		return 0
	fi

	# TODO(scaffold): mount once outside the loop, drop caches
	# between iterations, time dd, append to CSV.  The mount
	# survives across iterations within the same (size, op) cell.
	echo "[B] $size iter=$iter -- NOT YET IMPLEMENTED" >&2
}

# -- variant C: kernel NFS mount of MDS, no EC (inband) --------------
# Mounts an export with sb_ndstores=0 so LAYOUTGET returns
# NFS4ERR_LAYOUTUNAVAILABLE and the kernel client falls back to
# inband MDS I/O.  The "/no-ec" path is configured at topology
# bringup -- see the design doc Risk 2.
run_variant_c() {
	local size="$1" iter="$2"
	local bytes=$(size_to_bytes "$size")
	local file="C-${size}-i${iter}.dat"

	if [[ "$DRY_RUN" -eq 1 ]]; then
		echo "[C] would: mount -t nfs4 -o port=2049,vers=4.2,sec=sys,nolock"
		echo "           127.0.0.1:/no-ec /mnt/mds-noec"
		echo "           dd if=/dev/urandom of=/mnt/mds-noec/${file}"
		echo "           bs=${bytes} count=1 conv=fsync"
		echo "           dd if=/mnt/mds-noec/${file} of=/dev/null bs=${bytes}"
		emit_row "C" "$size" "write" "$iter" "DRY" "$bytes"
		emit_row "C" "$size" "read"  "$iter" "DRY" "$bytes"
		return 0
	fi

	# TODO(scaffold): verify /no-ec export exists at bringup
	# time (probe op + assert LAYOUTGET returns
	# NFS4ERR_LAYOUTUNAVAILABLE), mount, drop caches, dd, time.
	echo "[C] $size iter=$iter -- NOT YET IMPLEMENTED" >&2
}

# -- INV-1 counter snapshot helper ------------------------------------
# Captures the chunk-activity counters at run boundaries so the
# bench operator can audit "did variant B do the same number of
# CHUNK_WRITEs as variant A, and did variant C do zero?"  See
# .claude/design/chunk-collision-validation.md BLOCKER 2.
snapshot_inv1() {
	local label="$1"
	echo "--- INV-1 counters (${label}) ---" >&2
	if [[ "$DRY_RUN" -eq 1 ]]; then
		echo "[snapshot] DRY: would query reffs-bench-mds for sb-list" >&2
		return 0
	fi
	# TODO(scaffold): match the run_chunk_collision_track2.sh
	# pattern for probe-client discovery + sb-list / nfs4-op-stats.
	echo "[snapshot] NOT YET IMPLEMENTED" >&2
}

# -- main loop --------------------------------------------------------
snapshot_inv1 "pre-run"

IFS=',' read -r -a SIZE_ARRAY <<< "$SIZES"
IFS=',' read -r -a VARIANT_ARRAY <<< "$VARIANTS"

for variant in "${VARIANT_ARRAY[@]}"; do
	for size in "${SIZE_ARRAY[@]}"; do
		for iter in $(seq 1 "$N_ITERS"); do
			case "$variant" in
				A) run_variant_a "$size" "$iter" ;;
				B) run_variant_b "$size" "$iter" ;;
				C) run_variant_c "$size" "$iter" ;;
				*) echo "unknown variant: $variant" >&2 ;;
			esac
		done
	done
done

snapshot_inv1 "post-run"

echo ""
echo "[ps-bench] done.  CSV: $OUT_CSV"
if [[ "$DRY_RUN" -eq 1 ]]; then
	echo "[ps-bench] DRY RUN -- no real measurements taken."
	echo "[ps-bench] CSV contains 'DRY' placeholder ms values."
fi
