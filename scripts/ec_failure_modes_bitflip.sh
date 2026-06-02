#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# experiment 14 -- bit-flip injection rows
#
# Closes the two cells the 2026-05-02 DS-down sweep didn't:
#   bit-flip-on-disk : mutate one byte in a DS .dat after CHUNK_WRITE.
#                      Expect: DS log "CHUNK_READ: CRC mismatch block N"
#                      fires; client read returns the (now-corrupted)
#                      bytes with OK -- per-design, the DS doesn't
#                      return NFS4ERR_IO; the LOG line is the operator-
#                      grade signal.
#   bit-flip-on-wire : EC_DEMO_INJECT_WIRE_FLIP=N forces ec_demo's
#                      ds_chunk_read to XOR byte N of the wire payload
#                      before the client-side CRC verify.  Expect:
#                      client CRC verify fires; ec_demo returns -EIO.
#
# Output: a CSV row per cell appended to the file passed in via
# --out, schema:
#   encoding,geometry,loss_pattern,iteration,result_code,
#   read_latency_ms,bytes_match,ds_log_crc_mismatch,wire_eio,note
#
# Bench: existing deploy/benchmark docker compose.  Driver assumes
# the bench is already up and healthy.

set -euo pipefail

# ASAN/LSAN-instrumented ec_demo reports libtirpc allocation
# leaks (~6 KiB total) that are non-actionable -- libtirpc itself
# is the upstream source.  Without these envvars, LSan exits the
# process with rc=1 on every successful write, which `set -e`
# then catches and aborts the harness silently.  Mirror the
# Track 1b harness fix (chunk-collision-validation.md "What we
# found" #4).
export ASAN_OPTIONS="detect_leaks=0:halt_on_error=0"
export UBSAN_OPTIONS="halt_on_error=0"
export LSAN_OPTIONS="halt_on_error=0"

MODE=""            # "disk" or "wire"
MDS="127.0.0.1:2049"
EC_DEMO=""
OUT="bitflip-results.csv"
ITERS=5
SIZE=1048576
SHARD=4096
RUNDIR=""
DRY=0

usage() {
	echo "usage: $0 --mode {disk|wire} --ec_demo PATH [--mds HOST:PORT] [--out CSV] [--iters N] [--size BYTES] [--shard BYTES] [--rundir DIR] [--dry]" >&2
	exit 1
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--mode) MODE="$2"; shift 2;;
	--mds) MDS="$2"; shift 2;;
	--ec_demo) EC_DEMO="$2"; shift 2;;
	--out) OUT="$2"; shift 2;;
	--iters) ITERS="$2"; shift 2;;
	--size) SIZE="$2"; shift 2;;
	--shard) SHARD="$2"; shift 2;;
	--rundir) RUNDIR="$2"; shift 2;;
	--dry) DRY=1; shift;;
	-h|--help) usage;;
	*) echo "unknown arg: $1" >&2; usage;;
	esac
done

[[ -z "${MODE}" || -z "${EC_DEMO}" ]] && usage
[[ "${MODE}" != "disk" && "${MODE}" != "wire" ]] && {
	echo "mode must be 'disk' or 'wire'" >&2; exit 1; }

# Per-run scratch dir.  Caller can supply one to keep artefacts
# across invocations; otherwise mktemp.
if [[ -z "${RUNDIR}" ]]; then
	RUNDIR=$(mktemp -d -t reffs-exp14-XXXXXX)
fi
mkdir -p "${RUNDIR}"

# Write CSV header if file doesn't exist.
if [[ ! -f "${OUT}" ]]; then
	echo "encoding,geometry,loss_pattern,iteration,result_code,read_latency_ms,bytes_match,ds_log_crc_mismatch,wire_eio,note" \
		>"${OUT}"
fi

run_one() {
	local encoding="$1" k="$2" m="$3" iter="$4"
	local geom="${k}:${m}"
	local nfs_file="exp14_${MODE}_${encoding}_${k}_${m}_$$_${iter}.dat"
	local input="${RUNDIR}/exp14_input_${SIZE}.bin"
	local output="${RUNDIR}/exp14_out_${encoding}_${k}_${m}_${iter}.bin"

	# Generate a stable random input file (once per RUNDIR).
	if [[ ! -f "${input}" ]]; then
		dd if=/dev/urandom of="${input}" bs="${SIZE}" count=1 \
			status=none 2>/dev/null
	fi

	# Snapshot DS log line counts BEFORE the read so we can diff
	# after to confirm the LOG fired exactly during this read.
	local crc_lines_before=0
	if [[ "${MODE}" == "disk" ]]; then
		crc_lines_before=$(sudo docker logs reffs-bench-ds0 2>&1 |
			grep -c "CHUNK_READ: CRC mismatch" || true)
	fi

	# WRITE phase.
	if [[ "${DRY}" -eq 0 ]]; then
		"${EC_DEMO}" write \
			--mds "${MDS}" --file "${nfs_file}" \
			--input "${input}" --k "${k}" --m "${m}" \
			--encoding "${encoding}" --layout v2 \
			--shard-size "${SHARD}" \
			>"${RUNDIR}/write.log" 2>&1
	fi

	# INJECT phase.
	local note=""
	if [[ "${MODE}" == "disk" && "${DRY}" -eq 0 ]]; then
		# Mutate one byte in DS0's chunk-store dat file for this
		# file.  ds0 holds shard 0 of stripe 0; the .dat name is
		# derived from the inode number, which we don't know
		# directly.  Find the most-recently-modified .dat under
		# /state in the DS container.
		# DS data lives under /tmp/reffs_ds_data/sb_1/ino_*.dat on
		# this docker image; --backend=ram defaults to /tmp.
		# Newest-mtime .dat = the file we just wrote (the test
		# only writes one file at a time).
		local dat
		dat=$(sudo docker exec reffs-bench-ds0 sh -c \
			'find /tmp -name "ino_*.dat" -printf "%T@ %p\n" 2>/dev/null | sort -nr | head -1 | cut -d" " -f2-' \
			2>&1 | tail -1)
		if [[ -n "${dat}" ]]; then
			# XOR byte 17 with 0xFF -- arbitrary mid-block offset
			# clear of any header bytes.
			sudo docker exec reffs-bench-ds0 sh -c \
				"python3 -c 'import sys; f=open(sys.argv[1],\"r+b\"); f.seek(17); b=f.read(1); f.seek(17); f.write(bytes([b[0]^0xFF])); f.close()' '${dat}'" \
				2>&1 || note="inject_failed"
		else
			note="no_dat_found"
		fi
	fi

	# READ phase.  Wire-flip injection via env var; disk-flip is
	# already on the .dat file at this point.
	local result_code=0
	local read_start_ns read_end_ns
	read_start_ns=$(date +%s%N)
	if [[ "${MODE}" == "wire" ]]; then
		export EC_DEMO_INJECT_WIRE_FLIP=17
	else
		unset EC_DEMO_INJECT_WIRE_FLIP || true
	fi
	if "${EC_DEMO}" read \
		--mds "${MDS}" --file "${nfs_file}" \
		--output "${output}" --k "${k}" --m "${m}" \
		--encoding "${encoding}" --layout v2 \
		--shard-size "${SHARD}" \
		--size "${SIZE}" \
		>"${RUNDIR}/read.log" 2>&1; then
		result_code=0
	else
		result_code=$?
	fi
	read_end_ns=$(date +%s%N)
	unset EC_DEMO_INJECT_WIRE_FLIP || true

	local latency_ms=$(((read_end_ns - read_start_ns) / 1000000))

	# Bytes match?  Skip when read failed.
	local bytes_match="false"
	if [[ "${result_code}" -eq 0 && -f "${output}" ]]; then
		if cmp -s "${input}" "${output}"; then
			bytes_match="true"
		else
			bytes_match="false"
		fi
	fi

	# Did DS log fire (disk mode only)?
	local ds_log_fired="false"
	if [[ "${MODE}" == "disk" ]]; then
		local crc_lines_after
		crc_lines_after=$(sudo docker logs reffs-bench-ds0 2>&1 |
			grep -c "CHUNK_READ: CRC mismatch" || true)
		if [[ "${crc_lines_after}" -gt "${crc_lines_before}" ]]; then
			ds_log_fired="true"
		fi
	fi

	# Did client surface -EIO from the CRC verify?  -EIO = 5;
	# wire-flip mode should produce result_code=1 (ec_demo exits
	# non-zero) with the CRC mismatch line in read.log.
	local wire_eio="false"
	if [[ "${MODE}" == "wire" ]]; then
		if grep -q "ds_chunk_read: CRC mismatch" \
			"${RUNDIR}/read.log" 2>/dev/null; then
			wire_eio="true"
		fi
	fi

	echo "${encoding},${geom},bitflip_${MODE},${iter},${result_code},${latency_ms},${bytes_match},${ds_log_fired},${wire_eio},${note}" \
		>>"${OUT}"
}

# Encoding × geometry matrix.  Start with RS 4+2 only as the
# representative cell; expand once the mechanism is confirmed.
# Bit-flip detection is encoding-independent (CRC32 acts on the byte
# stream, not the encoding output), so one cell per mode validates
# the mechanism; the full 2x2 matrix is nice-to-have.
ENCODINGS=(rs)
GEOMS=("4 2")

for encoding in "${ENCODINGS[@]}"; do
	for geom in "${GEOMS[@]}"; do
		read -r k m <<<"${geom}"
		for iter in $(seq 1 "${ITERS}"); do
			run_one "${encoding}" "${k}" "${m}" "${iter}"
		done
	done
done

echo "exp14 ${MODE}: ${ITERS} iterations × ${#ENCODINGS[@]} encoding(s) × ${#GEOMS[@]} geom(s) done"
echo "results: ${OUT}"
echo "scratch: ${RUNDIR}"
