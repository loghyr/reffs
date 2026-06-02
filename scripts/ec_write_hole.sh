#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# experiment 12 -- write-hole behavior under client failure
#
# Procedure (post-fix re-run, per ietf126.experiments.md P1 #4):
#   1. Pre-fill the file with content X via ec_demo write
#   2. Launch a second ec_demo write of content Y in background
#   3. Sleep kill_ms milliseconds; SIGKILL the writer
#   4. Wait for lease expiry (lease_time * 1.5 + reaper interval +
#      safety margin) so lease_reaper -> nfs4_client_expire ->
#      chunk_rollback_for_client sweeps the dead writer's
#      PENDING/FINALIZED chunks back to EMPTY
#   5. Read the file via ec_demo and classify the outcome:
#        PRE_WRITE_X    bytes match X (rollback succeeded)
#        POST_WRITE_Y   bytes match Y (writer made it past the
#                       last COMMIT before the kill -- writes
#                       had landed durably)
#        NO_CONTENT     read failed / decode failed (writer died
#                       before any commits -- the file's contents
#                       got rolled back to EMPTY at the killed
#                       offsets; this is also a clean state, no
#                       partial-stripe corruption)
#        MIXED          bytes don't match X or Y but read
#                       succeeded -- the write-hole failure mode
#                       this experiment is designed to detect.
#                       MUST be 0/N for the success criterion.
#
# Output: CSV row per cell appended to --out:
#   codec,geometry,kill_ms,iteration,outcome,read_latency_ms,note

set -uo pipefail

# ec_demo's LSan post-process trips on benign libtirpc leaks;
# match the Track 1b + exp 14 harness convention.
export ASAN_OPTIONS="detect_leaks=0:halt_on_error=0"
export UBSAN_OPTIONS="halt_on_error=0"
export LSAN_OPTIONS="halt_on_error=0"

MDS="${MDS:-127.0.0.1:2049}"
EC_DEMO="${EC_DEMO:-/home/loghyr/reffs/build/tools/ec_demo}"
OUT="${OUT:-/tmp/exp12-write-hole.csv}"
ITERS="${ITERS:-3}"
SIZE="${SIZE:-1048576}"
LEASE_WAIT_SEC="${LEASE_WAIT_SEC:-90}"   # 45s lease * 1.5 + reaper + safety
KILL_MS_LIST="${KILL_MS_LIST:-100 250 500 750 900}"

if [[ ! -f "${OUT}" ]]; then
	echo "codec,geometry,kill_ms,iteration,outcome,read_latency_ms,note" >"${OUT}"
fi

# Stable random payloads X (pre-fill) and Y (mid-write target).
X="/tmp/exp12_X_${SIZE}.bin"
Y="/tmp/exp12_Y_${SIZE}.bin"
if [[ ! -f "${X}" ]]; then
	head -c "${SIZE}" /dev/urandom >"${X}"
fi
if [[ ! -f "${Y}" ]]; then
	head -c "${SIZE}" /dev/urandom >"${Y}"
fi

ms_now() { echo "$(($(date +%s%N) / 1000000))"; }

run_one() {
	local kill_ms="$1" iter="$2"
	local fname="exp12_$$_${kill_ms}_${iter}.dat"
	local output="/tmp/exp12_out_${kill_ms}_${iter}.bin"

	# Pre-fill: write X.  Uses a unique --id so this client's
	# clientid doesn't collide with the killed Y-writer.
	"${EC_DEMO}" write --mds "${MDS}" --file "${fname}" \
		--input "${X}" --k 4 --m 2 --codec rs --layout v2 \
		--shard-size 4096 --id "exp12_prefill_$$_${iter}" \
		>/tmp/exp12_prefill.log 2>&1 || {
			echo "rs,4:2,${kill_ms},${iter},NO_CONTENT,0,prefill_failed" >>"${OUT}"
			return
		}

	# Mid-write kill: launch Y-writer in background; sleep
	# kill_ms; SIGKILL.
	"${EC_DEMO}" write --mds "${MDS}" --file "${fname}" \
		--input "${Y}" --k 4 --m 2 --codec rs --layout v2 \
		--shard-size 4096 --id "exp12_killer_$$_${kill_ms}_${iter}" \
		>/tmp/exp12_killer.log 2>&1 &
	local killer_pid=$!
	sleep "$(awk -v ms="${kill_ms}" 'BEGIN { print ms/1000 }')"
	kill -9 "${killer_pid}" 2>/dev/null || true
	wait "${killer_pid}" 2>/dev/null || true

	# Wait for lease expiry + reaper sweep + chunk_rollback.
	# Default 90s = 45s lease * 1.5 + reaper safety margin.
	sleep "${LEASE_WAIT_SEC}"

	# Reader probe.
	local t0=$(ms_now)
	rm -f "${output}"
	local read_rc=0
	"${EC_DEMO}" read --mds "${MDS}" --file "${fname}" \
		--output "${output}" --k 4 --m 2 --codec rs --layout v2 \
		--shard-size 4096 --size "${SIZE}" \
		--id "exp12_reader_$$_${iter}" \
		>/tmp/exp12_reader.log 2>&1 || read_rc=$?
	local t1=$(ms_now)
	local latency=$((t1 - t0))

	local outcome="MIXED"
	local note=""
	if [[ "${read_rc}" -ne 0 || ! -s "${output}" ]]; then
		outcome="NO_CONTENT"
		note="read_rc=${read_rc}"
	elif cmp -s "${X}" "${output}"; then
		outcome="PRE_WRITE_X"
	elif cmp -s "${Y}" "${output}"; then
		outcome="POST_WRITE_Y"
	else
		# Neither matches.  Could be "first half Y, last half
		# rolled-back-to-X (mixed)", or a frankenstein.  Mark
		# MIXED so the experiment's success criterion catches
		# it; the per-iteration scratch dir keeps the output
		# file for triage.
		outcome="MIXED"
		note="output preserved at ${output}"
	fi

	echo "rs,4:2,${kill_ms},${iter},${outcome},${latency},${note}" >>"${OUT}"
}

for kill_ms in ${KILL_MS_LIST}; do
	for iter in $(seq 1 "${ITERS}"); do
		echo "[exp12] kill_ms=${kill_ms} iter=${iter}/${ITERS}" >&2
		run_one "${kill_ms}" "${iter}"
	done
done

echo "exp12 done; results: ${OUT}"
