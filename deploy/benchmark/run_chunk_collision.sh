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
#                          [--ec_demo PATH]

set -euo pipefail

N=4
CODEC="rs"
LAYOUT="v2"
SIZE=$((4 * 1024 * 1024))   # 4 MiB
ITER=20
MDS="reffs-mds"
EC_DEMO=""

while [[ $# -gt 0 ]]; do
	case "$1" in
		--n)          N="$2";          shift 2 ;;
		--codec)      CODEC="$2";      shift 2 ;;
		--layout)     LAYOUT="$2";     shift 2 ;;
		--size)       SIZE="$2";       shift 2 ;;
		--iterations) ITER="$2";       shift 2 ;;
		--mds)        MDS="$2";        shift 2 ;;
		--ec_demo)    EC_DEMO="$2";    shift 2 ;;
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
if (( corrupt > 0 )); then
	echo "  workdir kept:  ${WORKDIR}"
	trap - EXIT
	exit 2
fi

echo "  PASS (no corruption detected over ${ITER} iterations)"
exit 0
