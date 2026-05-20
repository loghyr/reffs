#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Track 1 chunk-collision harness: launch N concurrent ec_demo
# `write` instances against the same MDS file, then verify every
# writer's payload survives a `verify`.
#
# Uses ec_demo write/verify (not put/check) because put/check are
# the plain-mirrored subcommands and silently ignore --codec; this
# slice has to actually drive the RS / Mojette CHUNK paths to be
# diagnostic of bug surfaces 2 and 7 in
# .claude/design/chunk-collision-validation.md (CRC vs payload
# divergence, codec divergence under contention).
#
# Per .claude/design/chunk-collision-validation.md.  Runs against
# the deploy/benchmark/ topology (1 MDS + 10 DSes).  Each ec_demo
# instance gets a unique --id so the MDS sees N distinct
# clientids contending on the same file.
#
# Usage:
#   run_chunk_collision.sh [--n N] [--codec rs|mojette-sys|mojette-nonsys]
#                          [--layout v1|v2] [--size BYTES]
#                          [--iterations N] [--mds HOST]
#                          [--ec_demo PATH] [--inv1-report] [--probe PATH]
#                          [--ds-list HOST1,HOST2,...]
#
# --inv1-report: after the workload, query the per-sb chunk-activity
# counters and print a quotable summary of the partial-stripe write
# pattern the DSes saw.  Backs the INV-1 measurement promised on the
# IETF nfsv4 WG list (Hellwig msg 5 + msg 9).  See
# .claude/design/inv1-ds-instrumentation.md.
#
# --ds-list: comma-separated probe hosts to sum INV-1 counters over.
# CHUNK_WRITE lands on the DSes (not the MDS), so the MDS alone reads
# as zero in any non-combined topology.  Pass the DS containers /
# hosts here, e.g. --ds-list reffs-bench-ds0,reffs-bench-ds1,...
# Combined-mode runs can omit (MDS and DS share the sb).

set -euo pipefail

N=4
CODEC="rs"
LAYOUT="v2"
SIZE=$((4 * 1024 * 1024))   # 4 MiB
ITER=20
MDS="reffs-mds"
EC_DEMO=""
INV1_REPORT=0
PROBE=""
DS_LIST=""

while [[ $# -gt 0 ]]; do
	case "$1" in
		--n)          N="$2";          shift 2 ;;
		--codec)      CODEC="$2";      shift 2 ;;
		--layout)     LAYOUT="$2";     shift 2 ;;
		--size)       SIZE="$2";       shift 2 ;;
		--iterations) ITER="$2";       shift 2 ;;
		--mds)        MDS="$2";        shift 2 ;;
		--ec_demo)    EC_DEMO="$2";    shift 2 ;;
		--inv1-report) INV1_REPORT=1;  shift   ;;
		--probe)      PROBE="$2";      shift 2 ;;
		--ds-list)    DS_LIST="$2";    shift 2 ;;
		-h|--help)
			grep '^# ' "$0" | sed 's/^# \{0,1\}//'
			exit 0 ;;
		*)
			echo "unknown arg: $1" >&2
			exit 1 ;;
	esac
done

# --- Locate ec_demo ---
if [[ -z "${EC_DEMO}" ]]; then
	for cand in /shared/build/tools/ec_demo \
		    /reffs/build/tools/ec_demo \
		    /usr/local/bin/ec_demo; do
		if [[ -x "${cand}" ]]; then
			EC_DEMO="${cand}"
			break
		fi
	done
fi
if [[ -z "${EC_DEMO}" || ! -x "${EC_DEMO}" ]]; then
	echo "ec_demo not found.  Pass --ec_demo PATH" >&2
	exit 1
fi

WORKDIR="$(mktemp -d -t reffs-coll.XXXXXX)"
trap 'rm -rf "${WORKDIR}"' EXIT

LOCAL_INPUTS=()
for i in $(seq 0 $((N-1))); do
	src="${WORKDIR}/writer-${i}.bin"
	# Each writer gets a deterministic-but-distinct payload so that
	# `verify` against any writer's input identifies which one won the
	# race.  The first byte encodes the rank; the rest is rank-seeded
	# random so off-by-one block boundaries are visible.
	#
	# Generate the rank byte via printf-then-xxd: '\\x%02x' formats
	# the rank as the literal text "\xNN" (two-char escape + two hex
	# digits), and xxd -r -p decodes that into a single binary byte.
	# Avoids the bash printf '\x%02x' footgun where bash tries to
	# evaluate \x as a hex escape and complains about the missing
	# digits.
	(
		printf '\\x%02x' "$i" | xxd -r -p
		head -c $((SIZE - 1)) /dev/urandom
	) >"${src}"
	LOCAL_INPUTS[$i]="${src}"
done

# Include $$ in the filename to disambiguate parallel-run-in-same-second.
NFS_FILE="coll_${CODEC}_${LAYOUT}_$(date +%s)_$$.dat"

echo "=== chunk-collision Track 1 ==="
echo "  N writers: ${N}"
echo "  codec:     ${CODEC}"
echo "  layout:    ${LAYOUT}"
echo "  size:      ${SIZE} bytes"
echo "  iters:     ${ITER}"
echo "  MDS:       ${MDS}"
echo "  workdir:   ${WORKDIR}"
echo "  NFS file:  ${NFS_FILE}"
echo ""

corrupt=0
for it in $(seq 1 "${ITER}"); do
	echo "--- iteration ${it}/${ITER} ---"

	# Phase 1: N concurrent put.  Each writer picks its own client
	# identity so the MDS sees N distinct clientids contending.
	PIDS=()
	for i in $(seq 0 $((N-1))); do
		ID="coll-rank${i}-pid$$-it${it}"
		"${EC_DEMO}" write \
			--mds   "${MDS}" \
			--file  "${NFS_FILE}" \
			--input "${LOCAL_INPUTS[$i]}" \
			--codec "${CODEC}" \
			--layout "${LAYOUT}" \
			--id    "${ID}" \
			>"${WORKDIR}/write-it${it}-r${i}.log" 2>&1 &
		PIDS+=($!)
	done

	# Wait for all writers; record exit codes.
	any_write_failed=0
	for pid in "${PIDS[@]}"; do
		if ! wait "${pid}"; then
			any_write_failed=1
		fi
	done
	if (( any_write_failed )); then
		echo "  one or more write failed (see ${WORKDIR}/write-it${it}-*.log)"
		# Not necessarily a corruption; could be expected serialization.
		# Continue to verification.
	fi

	# Phase 2: settle.  Brief pause to let any in-flight LAYOUTRETURN /
	# reflected GETATTR / fence rotation drain on the MDS.
	sleep 1

	# Phase 3: verify the surviving file matches *some* writer's input.
	# `verify` decodes the EC-encoded file and byte-compares to the
	# input.  Passes if the persisted file matches any input file
	# byte-for-byte; that is the correct post-condition for whole-file
	# rewrite races (last-FINALIZE-wins per stripe).
	#
	# A frankenstein post-state -- chunks 0..K from writer A, K+1..N
	# from writer B -- fails every writer's verify and registers as
	# CORRUPTION below.  This is sound because cmd_verify in
	# tools/ec_demo.c does whole-file decode + byte-compare, not
	# per-stripe matching.
	survivor=-1
	for i in $(seq 0 $((N-1))); do
		if "${EC_DEMO}" verify \
			--mds   "${MDS}" \
			--file  "${NFS_FILE}" \
			--input "${LOCAL_INPUTS[$i]}" \
			--codec "${CODEC}" \
			--layout "${LAYOUT}" \
			--id    "coll-verify-it${it}" \
			>"${WORKDIR}/verify-it${it}-r${i}.log" 2>&1; then
			survivor="${i}"
			break
		fi
	done

	if (( survivor < 0 )); then
		echo "  CORRUPTION: no writer's payload survived intact"
		echo "  preserved logs: ${WORKDIR}"
		(( corrupt++ )) || true
		# Do not break -- keep going to surface multi-iteration patterns.
		# Operator can ^C if needed.
	else
		echo "  survivor: writer ${survivor}"
	fi
done

echo ""
echo "=== summary ==="
echo "  iterations:    ${ITER}"
echo "  corruptions:   ${corrupt}"

# --- INV-1 report (.claude/design/inv1-ds-instrumentation.md) ---
# Quotable two-block summary of what the DSes actually saw during the
# workload.  Answers Hellwig's partial-stripe-write question with
# numbers pulled from the per-sb chunk-activity counters.
if (( INV1_REPORT )); then
	if [[ -z "${PROBE}" ]]; then
		for cand in /shared/build/scripts/reffs-probe.py \
			    /reffs/build/scripts/reffs-probe.py \
			    /usr/local/bin/reffs-probe.py \
			    reffs-probe.py; do
			if command -v "${cand}" >/dev/null 2>&1; then
				PROBE="${cand}"
				break
			fi
		done
	fi
	if [[ -z "${PROBE}" ]] || ! command -v "${PROBE}" >/dev/null 2>&1; then
		echo ""
		echo "  INV-1 report: reffs-probe.py not found (pass --probe PATH)"
	else
		echo ""
		echo "=== INV-1 partial-stripe write pattern (DS view) ==="

		# CHUNK_WRITE lands on each DS the layout selects, not on
		# the MDS.  The MDS only sees LAYOUTGET; its sb_chunk_stats
		# stays at zero.  Build a target list that always includes
		# the MDS (for completeness in combined-mode T1b where the
		# same process holds both roles) and any DSes the operator
		# named via --ds-list.
		TARGETS=("${MDS}")
		if [[ -n "${DS_LIST}" ]]; then
			# shellcheck disable=SC2206
			DS_ARR=(${DS_LIST//,/ })
			TARGETS+=("${DS_ARR[@]}")
		fi

		# Stream sb-list from every target into a single awk pass.
		# The 'Chunks:' block follows each SB's header.  Per-SB
		# counters print only when non-zero (the sb-list idiom), so
		# the absence of a counter line means zero for that SB --
		# the sum stays correct.
		for tgt in "${TARGETS[@]}"; do
			"${PROBE}" --host "${tgt}" sb-list 2>/dev/null \
				| awk -v host="${tgt}" '
					/^  Chunks:/         { in_chunks = 1; next }
					/^[^ ]/              { in_chunks = 0 }
					/^  [^ ]/            { in_chunks = 0 }
					in_chunks { print host, $0 }'
		done | awk '
			$2 == "blocks_full:"        { full       += $3 }
			$2 == "blocks_partial:"     { partial    += $3 }
			$2 == "blocks_first_write:" { first      += $3 }
			$2 == "blocks_overwrite:"   { overwrite  += $3 }
			$2 == "writes_1block:"      { w1         += $3 }
			$2 == "writes_2to7:"        { w27        += $3 }
			$2 == "writes_8to31:"       { w831       += $3 }
			$2 == "writes_32plus:"      { w32        += $3 }
			$2 == "pending_displaced:"  { displaced  += $3 }
			$2 == "writes:"             { writes     += $3 }
			END {
			    printf("  CHUNK_WRITE calls accepted:  %d\n", writes)
			    printf("  full / partial blocks:       %d / %d\n",
			           full, partial)
			    printf("  first-write / overwrite:     %d / %d\n",
			           first, overwrite)
			    printf("  writes by batch (1/2-7/8-31/32+): %d / %d / %d / %d\n",
			           w1, w27, w831, w32)
			    printf("  cs_pending_displaced (collision proof): %d\n",
			           displaced)
			    printf("  (fragmentation_runs: deferred to INODE_CHUNK_STATS probe op)\n")
			    if (writes == 0)
			        printf("\n  NOTE: zero CHUNK_WRITEs observed.  T1b uses ec_demo\n         which drives CHUNK_WRITE end-to-end; verify the MDS\n         + DS hosts are reachable via the probe and that --ds-list\n         covers every layout target.\n")
			}'
	fi
fi

if (( corrupt > 0 )); then
	echo "  workdir kept:  ${WORKDIR}"
	trap - EXIT
	exit 2
fi

echo "  PASS (no corruption detected over ${ITER} iterations)"
exit 0
