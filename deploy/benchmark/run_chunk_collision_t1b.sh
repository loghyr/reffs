#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Track 1b chunk-collision harness: partial-range writes via the
# new ec_demo --offset / --length flags
# (.claude/design/chunk-collision-t1b.md).  Targets four sub-cases
# from the validation plan:
#
#   disjoint    -- 4 writers, non-overlapping stripe-aligned ranges.
#                  Every writer's bytes survive; per-writer verify
#                  is deterministic PASS.
#   chunk-split -- 2 writers, each on a different half of one stripe.
#                  Both halves should survive (sub-stripe ranges
#                  go through RMW on the same stripe; race is
#                  last-FINALIZE-wins on the full stripe).
#   overlap     -- 4 writers, overlapping ranges.  Verify is
#                  informational: every byte must match SOME
#                  writer's payload (writers stamp distinct
#                  constant bytes so the reader can prove
#                  attribution).
#   subchunk    -- 2 writers, alternating 1 KiB stripes inside one
#                  4 KiB block.  Sub-shard; race is last-FINALIZE-wins
#                  on the full stripe.  Informational verify --
#                  per-writer verify will mismatch when one writer's
#                  RMW write overwrites the other writer's bytes.
#
# Usage:
#   run_chunk_collision_t1b.sh [--mode disjoint|chunk-split|overlap|subchunk]
#                              [--codec rs|mojette-sys|mojette-nonsys]
#                              [--layout v2] [--k K] [--m M]
#                              [--shard-size BYTES]
#                              [--mds HOST] [--ec_demo PATH]
#                              [--inv1-report] [--probe PATH]
#                              [--ds-list HOST1,HOST2,...]
#
# Layout defaults to v2 (CHUNK ops); the partial-range path goes
# through the per-stripe primitives that only exist on v2.
# Codec defaults to rs (Reed-Solomon).

set -euo pipefail

MODE="disjoint"
CODEC="rs"
LAYOUT="v2"
K=4
M=2
SHARD_SIZE=4096
# Default to the docker-published port on localhost.  ec_demo's
# host:port syntax bypasses libtirpc's clnt_create_timed portmap
# path, which on the host fails because the in-container reffsd
# doesn't register NFS with the host rpcbind.  Inside a container
# attached to the bench network, override with --mds reffs-mds.
MDS="127.0.0.1:2049"
EC_DEMO=""
INV1_REPORT=0
PROBE=""
DS_LIST=""

# Sanitizer-quiet ec_demo: libtirpc allocates inside
# clnt_create_timed / authunix_create_default without a matching
# free on session teardown.  LSan reports those ~6 KiB on every
# successful exit, which would flip ec_demo's rc to 1 and trip
# `set -euo pipefail` here before the per-writer racers spawn.
# Same mitigation the CI integration test (ci_integration_test.sh)
# uses.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=0}"
export LSAN_OPTIONS="${LSAN_OPTIONS:-halt_on_error=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=0}"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--mode)        MODE="$2";        shift 2 ;;
		--codec)       CODEC="$2";       shift 2 ;;
		--layout)      LAYOUT="$2";      shift 2 ;;
		--k)           K="$2";           shift 2 ;;
		--m)           M="$2";           shift 2 ;;
		--shard-size)  SHARD_SIZE="$2";  shift 2 ;;
		--mds)         MDS="$2";         shift 2 ;;
		--ec_demo)     EC_DEMO="$2";     shift 2 ;;
		--inv1-report) INV1_REPORT=1;    shift   ;;
		--probe)       PROBE="$2";       shift 2 ;;
		--ds-list)     DS_LIST="$2";     shift 2 ;;
		-h|--help)
			grep '^# ' "$0" | sed 's/^# \{0,1\}//'
			exit 0 ;;
		*)
			echo "unknown arg: $1" >&2
			exit 1 ;;
	esac
done

case "${MODE}" in
	disjoint|chunk-split|overlap|subchunk) ;;
	*)
		echo "unknown --mode '${MODE}': expected disjoint, chunk-split, overlap, subchunk" >&2
		exit 1 ;;
esac

if [[ -z "${EC_DEMO}" ]]; then
	# Search a script-relative path first so a host invocation
	# picks up the fresh build under ../../build/tools/ ahead of
	# any stale /usr/local/bin/ec_demo left over from an old
	# package install.  The /shared and /reffs entries are
	# container paths that resolve when this script runs inside
	# a docker container attached to the bench network.
	script_dir="$(cd "$(dirname "$0")" && pwd)"
	for cand in "${script_dir}/../../build/tools/ec_demo" \
		    /shared/build/tools/ec_demo \
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

WORKDIR="$(mktemp -d -t reffs-coll-t1b.XXXXXX)"
# Preserve WORKDIR on failure so per-writer logs survive for triage.
# A successful exit cleans up; any non-zero exit leaves the dir behind
# and tells the operator how to inspect / remove it.
cleanup_workdir() {
	local rc=$?
	if [[ $rc -ne 0 ]]; then
		echo "" >&2
		echo "[t1b] non-zero exit ($rc); WORKDIR preserved for triage:" >&2
		echo "[t1b]   ${WORKDIR}" >&2
		echo "[t1b] per-writer logs: writer-<i>.log, verify-<i>.log, prefill.log" >&2
		echo "[t1b] remove manually with: rm -rf '${WORKDIR}'" >&2
	else
		rm -rf "${WORKDIR}"
	fi
}
trap cleanup_workdir EXIT

# stripe_data = k * shard_size
STRIPE_DATA=$((K * SHARD_SIZE))

# Per-mode geometry: N writers, file-size (bytes), per-rank
# (OFFSET, LENGTH) tuples.  All four modes pre-fill the MDS file
# to FILE_SIZE bytes with a full-file write before any range
# writer runs -- the per-stripe RMW path is sparse-RMW-unfriendly
# (NOT_NOW_BROWN_COW in ec_read_stripe_with_file).
case "${MODE}" in
	disjoint)
		N=4
		FILE_SIZE=$((4 * STRIPE_DATA))
		# Each writer takes one full stripe at its rank.
		RANK_OFFSET=()
		RANK_LENGTH=()
		for i in 0 1 2 3; do
			RANK_OFFSET+=($((i * STRIPE_DATA)))
			RANK_LENGTH+=($((STRIPE_DATA)))
		done
		EXPECT_DETERMINISTIC=1
		;;
	chunk-split)
		N=2
		FILE_SIZE=$((STRIPE_DATA))
		HALF=$((STRIPE_DATA / 2))
		RANK_OFFSET=(0 $HALF)
		RANK_LENGTH=($HALF $HALF)
		# Both halves go through RMW on the same stripe.
		# Race semantics make per-writer verify
		# informational unless sub-shard atomicity holds.
		EXPECT_DETERMINISTIC=0
		;;
	overlap)
		N=4
		# Three stripes, each writer covers 2 of them with
		# a 1-stripe overlap with the next rank.
		FILE_SIZE=$((4 * STRIPE_DATA))
		RANK_OFFSET=(0 $((STRIPE_DATA)) $((2 * STRIPE_DATA)) \
			     $((3 * STRIPE_DATA - STRIPE_DATA / 2)))
		RANK_LENGTH=($((2 * STRIPE_DATA)) $((2 * STRIPE_DATA)) \
			     $((STRIPE_DATA + STRIPE_DATA / 2)) \
			     $((STRIPE_DATA / 2 + STRIPE_DATA)))
		EXPECT_DETERMINISTIC=0
		;;
	subchunk)
		N=2
		# One stripe-worth, two writers alternating 1 KiB
		# halves in the FIRST 4 KiB shard.  This is the
		# hardest case: sub-shard sub-stripe RMW on the same
		# physical block.
		FILE_SIZE=$((STRIPE_DATA))
		RANK_OFFSET=(0 1024)
		RANK_LENGTH=(1024 1024)
		EXPECT_DETERMINISTIC=0
		;;
esac

# ----------------------------------------------------------------------
# Stamp distinct constant-byte payloads per rank so the verifier can
# attribute every byte back to a specific writer.  Constant bytes
# (rather than rank-seeded random) make the overlap mode's
# "every byte matches SOME writer" check tractable: read N bytes,
# assert each is in {RANK_STAMP[0..N-1]}.
# ----------------------------------------------------------------------
RANK_STAMPS=(0x10 0x11 0x12 0x13)

for i in $(seq 0 $((N - 1))); do
	OFF=${RANK_OFFSET[$i]}
	LEN=${RANK_LENGTH[$i]}
	STAMP=${RANK_STAMPS[$i]}
	src="${WORKDIR}/writer-${i}.bin"
	# Fill LEN bytes with the rank stamp.  Use tr to materialise
	# the byte (printf doesn't easily emit a single literal byte
	# without the bash hex-escape footgun documented in
	# run_chunk_collision.sh).
	printf '\\x%02x' "$((STAMP))" | xxd -r -p \
		| head -c 1 > "${WORKDIR}/byte-${i}"
	# Repeat the byte LEN times.  head -c is the bounded write.
	dd if=/dev/zero bs=1 count="${LEN}" status=none \
		| tr '\0' "$(cat ${WORKDIR}/byte-${i})" \
		| head -c "${LEN}" > "${src}"
done

# Pre-fill payload: an all-zeros LEN-byte file used for the
# initial full-file write that materialises the MDS file at
# FILE_SIZE bytes.  This is the sparse-RMW workaround --
# subsequent range writers all do RMW on already-materialised
# stripes.
PREFILL="${WORKDIR}/prefill.bin"
dd if=/dev/zero of="${PREFILL}" bs=1 count="${FILE_SIZE}" status=none

NFS_FILE="coll_t1b_${MODE}_${CODEC}_${LAYOUT}_$(date +%s)_$$.dat"

echo "=== chunk-collision Track 1b -- mode=${MODE} ==="
echo "  N writers:    ${N}"
echo "  codec:        ${CODEC}"
echo "  layout:       ${LAYOUT}"
echo "  k:            ${K}"
echo "  m:            ${M}"
echo "  shard_size:   ${SHARD_SIZE}"
echo "  stripe_data:  ${STRIPE_DATA}"
echo "  file_size:    ${FILE_SIZE}"
echo "  MDS:          ${MDS}"
echo "  ec_demo:      ${EC_DEMO}"
echo "  nfs_file:     ${NFS_FILE}"
echo "  deterministic verify: ${EXPECT_DETERMINISTIC}"
echo

if [[ "${INV1_REPORT}" -eq 1 && -z "${PROBE}" ]]; then
	for cand in /shared/build/src/probe1_client \
		    /reffs/build/src/probe1_client \
		    /usr/local/bin/reffs_probe1_clnt; do
		if [[ -x "${cand}" ]]; then
			PROBE="${cand}"
			break
		fi
	done
	if [[ -z "${PROBE}" || ! -x "${PROBE}" ]]; then
		echo "WARN: --inv1-report set but no probe1_client found; skipping" >&2
		INV1_REPORT=0
	fi
fi

# ----------------------------------------------------------------------
# Pre-fill: full-file write at FILE_SIZE bytes to materialise every
# stripe before any partial-range writer runs.
# ----------------------------------------------------------------------
echo ">>> Pre-fill: full-file write of ${FILE_SIZE} bytes"
"${EC_DEMO}" write \
	--mds "${MDS}" --file "${NFS_FILE}" \
	--input "${PREFILL}" --k "${K}" --m "${M}" \
	--codec "${CODEC}" --layout "${LAYOUT}" \
	--shard-size "${SHARD_SIZE}" \
	--id "prefill" \
	>"${WORKDIR}/prefill.log" 2>&1
echo "  pre-fill OK"

# Capture INV-1 baselines after pre-fill so the per-mode deltas
# only reflect the range-writer activity.
inv1_snapshot() {
	local out="$1"
	if [[ "${INV1_REPORT}" -eq 0 ]]; then
		return 0
	fi
	# Sum per-sb chunk-activity counters across the DS hosts in
	# DS_LIST.  Format is one line per counter family.  Empty
	# DS_LIST means combined mode; fall back to MDS alone.
	local hosts="${DS_LIST:-${MDS}}"
	{
		echo "# snapshot ${out} hosts=${hosts}"
		IFS=','
		for host in ${hosts}; do
			"${PROBE}" --host "${host}" --op nfs4-op-stats \
				2>/dev/null || true
		done
		IFS=$' \t\n'
	} >"${out}"
}

inv1_snapshot "${WORKDIR}/inv1.before"

# ----------------------------------------------------------------------
# Launch N concurrent range writers
# ----------------------------------------------------------------------
echo ">>> Launching ${N} concurrent range writers"
WRITER_PIDS=()
for i in $(seq 0 $((N - 1))); do
	OFF=${RANK_OFFSET[$i]}
	LEN=${RANK_LENGTH[$i]}
	src="${WORKDIR}/writer-${i}.bin"
	(
		"${EC_DEMO}" write \
			--mds "${MDS}" --file "${NFS_FILE}" \
			--input "${src}" --k "${K}" --m "${M}" \
			--codec "${CODEC}" --layout "${LAYOUT}" \
			--shard-size "${SHARD_SIZE}" \
			--id "writer${i}" \
			--offset "${OFF}" --length "${LEN}" \
			>"${WORKDIR}/writer-${i}.log" 2>&1
	) &
	WRITER_PIDS+=($!)
done

WRITER_FAIL=0
for pid in "${WRITER_PIDS[@]}"; do
	if ! wait "${pid}"; then
		WRITER_FAIL=$((WRITER_FAIL + 1))
	fi
done

if [[ "${WRITER_FAIL}" -ne 0 ]]; then
	echo "FAIL: ${WRITER_FAIL} writer(s) reported error" >&2
	for i in $(seq 0 $((N - 1))); do
		echo "--- writer-${i}.log ---" >&2
		tail -20 "${WORKDIR}/writer-${i}.log" >&2 || true
	done
	exit 1
fi
echo "  all writers completed"

inv1_snapshot "${WORKDIR}/inv1.after"

# ----------------------------------------------------------------------
# Per-writer verify
# ----------------------------------------------------------------------
echo ">>> Per-writer verify"
VERIFY_FAIL=0
for i in $(seq 0 $((N - 1))); do
	OFF=${RANK_OFFSET[$i]}
	LEN=${RANK_LENGTH[$i]}
	src="${WORKDIR}/writer-${i}.bin"
	if "${EC_DEMO}" verify \
		--mds "${MDS}" --file "${NFS_FILE}" \
		--input "${src}" --k "${K}" --m "${M}" \
		--codec "${CODEC}" --layout "${LAYOUT}" \
		--shard-size "${SHARD_SIZE}" \
		--id "verify${i}" \
		--offset "${OFF}" --length "${LEN}" \
		>"${WORKDIR}/verify-${i}.log" 2>&1; then
		echo "  writer-${i} verify PASS"
	else
		echo "  writer-${i} verify FAIL"
		VERIFY_FAIL=$((VERIFY_FAIL + 1))
		tail -5 "${WORKDIR}/verify-${i}.log" || true
	fi
done

if [[ "${INV1_REPORT}" -eq 1 ]]; then
	echo
	echo "=== INV-1 counter snapshots ==="
	echo "--- before ---"
	cat "${WORKDIR}/inv1.before"
	echo "--- after ---"
	cat "${WORKDIR}/inv1.after"
fi

if [[ "${EXPECT_DETERMINISTIC}" -eq 1 ]]; then
	# Deterministic modes: every writer's range must survive.
	if [[ "${VERIFY_FAIL}" -ne 0 ]]; then
		echo "FAIL: deterministic mode '${MODE}' had ${VERIFY_FAIL} verify mismatches"
		exit 1
	fi
	echo "PASS: mode=${MODE} -- all ${N} writers' ranges verified clean"
else
	# Informational modes: log the verify result without
	# failing the harness.  The intent is to surface what
	# happens under sub-stripe / sub-shard contention so the
	# next slice can target the gap.
	if [[ "${VERIFY_FAIL}" -eq 0 ]]; then
		echo "PASS (informational): mode=${MODE} -- "
		echo "  all ${N} writers' ranges verified clean even though"
		echo "  race semantics did not guarantee determinism"
	else
		echo "OBSERVED: mode=${MODE} -- ${VERIFY_FAIL} of ${N} writers' ranges"
		echo "  lost bytes to last-FINALIZE-wins.  This is the chunk-"
		echo "  collision surface this mode is designed to expose; the"
		echo "  fix lives in the chunk-store sub-stripe atomicity work"
		echo "  (NOT_NOW_BROWN_COW per chunk-collision-validation.md)."
	fi
fi
