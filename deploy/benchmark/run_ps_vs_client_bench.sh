#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# PS-encoder vs client-encoder A/B/C benchmark harness.
# Closes IETF 126 plan Bucket 2: "where does EC encoding belong?"
#
# Three variants against the same MDS + 10 DS topology + 1 PS:
#   A "client EC"  -- ec_demo write/read direct to MDS (today's path)
#   B "PS EC"      -- kernel NFSv4.2 mount of PS:4098 + fio
#   C "MDS inband" -- kernel NFSv4.2 mount of MDS:2049 + fio (no EC)
#
# See .claude/design/ps-encoder-bench.md for the full design.
#
# Usage:
#   sudo ./run_ps_vs_client_bench.sh \
#       [--codec rs|mojette-sys|mojette-nonsys] \
#       [--k K] [--m M] \
#       [--sizes "4096 16384 65536 262144 1048576"] \
#       [--iters N] \
#       [--variants "A B C"] \
#       [--out CSV] \
#       [--bring-up] \
#       [--keep-up]
#
# Defaults: codec=rs, k=4 m=2, all sizes, iters=5, all variants,
# out=results/ps_vs_client/results-<host>-<timestamp>.csv.
#
# --bring-up runs run-ps-bench-bringup.sh NPS=1 first.  Otherwise
# the bench is assumed up already.
# --keep-up leaves the bench up after the run for re-runs.
#
# Codec scoping note (per design MVP decision): operator MUST
# pre-configure the MDS to issue --codec for variant B's test
# files.  Variant A passes --codec directly to ec_demo.  This
# harness does not orchestrate MDS codec config; a probe-protocol
# extension for that is NOT_NOW_BROWN_COW.

set -euo pipefail

# -- defaults ---------------------------------------------------------
CODEC="rs"
K=4
M=2
SIZES="4096 16384 65536 262144 1048576"
ITERS=5
VARIANTS="A B C"
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=""
BRING_UP=0
KEEP_UP=0

# -- arg parse --------------------------------------------------------
while [[ $# -gt 0 ]]; do
	case "$1" in
	--codec)    CODEC="$2";    shift 2 ;;
	--k)        K="$2";        shift 2 ;;
	--m)        M="$2";        shift 2 ;;
	--sizes)    SIZES="$2";    shift 2 ;;
	--iters)    ITERS="$2";    shift 2 ;;
	--variants) VARIANTS="$2"; shift 2 ;;
	--out)      OUT="$2";      shift 2 ;;
	--bring-up) BRING_UP=1;    shift ;;
	--keep-up)  KEEP_UP=1;     shift ;;
	-h|--help)
		sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*) echo "FAIL: unknown arg '$1'" >&2; exit 1 ;;
	esac
done

# -- preconditions ----------------------------------------------------
if ! command -v ip >/dev/null 2>&1; then
	echo "FAIL: this harness requires a Linux host (iproute2)." >&2
	echo "      Run on dreamer / garbo / shadow, not macOS." >&2
	exit 1
fi
if ! command -v sudo >/dev/null 2>&1; then
	echo "FAIL: sudo not found.  This harness needs privileged" >&2
	echo "      docker access for the kernel-mount client." >&2
	exit 1
fi
case "$CODEC" in
rs|mojette-sys|mojette-nonsys) ;;
*) echo "FAIL: --codec must be rs|mojette-sys|mojette-nonsys" >&2; exit 1 ;;
esac

# -- ec_demo discovery -----------------------------------------------
EC_DEMO=""
for cand in "${HERE}/../../build/tools/ec_demo" \
	    /shared/build/tools/ec_demo \
	    /reffs/build/tools/ec_demo \
	    /usr/local/bin/ec_demo; do
	if [ -x "${cand}" ]; then EC_DEMO="${cand}"; break; fi
done
if [ -z "${EC_DEMO}" ]; then
	echo "FAIL: ec_demo not found.  Build reffs first." >&2
	exit 1
fi

# Sanitizer noise filter -- libtirpc allocates inside
# rpcb_set/clnt_create, valgrind/LSan flag these as "reachable"
# but they're library-lifetime allocations, not real leaks.
# Without these envvars LSan exits ec_demo with rc=1 after a
# successful write and trips `set -e`.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=0}"
export LSAN_OPTIONS="${LSAN_OPTIONS:-halt_on_error=0}"

# -- output path ------------------------------------------------------
if [ -z "${OUT}" ]; then
	mkdir -p "${HERE}/results/ps_vs_client"
	ts=$(date +%Y%m%d-%H%M%S)
	host=$(hostname -s)
	OUT="${HERE}/results/ps_vs_client/results-${host}-${ts}.csv"
fi
if [ ! -f "${OUT}" ]; then
	echo "variant,codec,geometry,size_bytes,iter,write_ms,read_ms,verify,note" \
	    >"${OUT}"
fi

# -- bring-up ---------------------------------------------------------
if [ "${BRING_UP}" -eq 1 ]; then
	echo "[ps-bench] bringing up bench (NPS=1)..."
	NPS=1 "${HERE}/run-ps-bench-bringup.sh"
fi

# Verify the bench is up.
if ! sudo docker ps --format '{{.Names}}' | grep -q '^reffs-bench-mds$'; then
	echo "FAIL: reffs-bench-mds not running.  Pass --bring-up or run" >&2
	echo "      run-ps-bench-bringup.sh first." >&2
	exit 1
fi

# -- variant A: client EC via ec_demo --------------------------------
# Directly invokes the local ec_demo binary against MDS:2049.
# Wall-clock is measured by `date +%s%N` around the call so we
# don't depend on ec_demo's internal timing.
run_variant_A() {
	local size="$1" iter="$2" cell_id="A_${CODEC}_${K}_${M}_${size}_${iter}"
	local fname="ps_bench_${cell_id}.dat"
	local input="/tmp/ec_bench_in_${size}.bin"
	local output="/tmp/ec_bench_out_${cell_id}.bin"
	local note="" write_ms=0 read_ms=0 verify="OK"

	# Pre-generated random input per size (cached).
	if [ ! -f "${input}" ] || [ "$(stat -c%s "${input}")" != "${size}" ]; then
		dd if=/dev/urandom of="${input}" bs="${size}" count=1 \
		   status=none 2>/dev/null
	fi
	rm -f "${output}"

	local t0 t1
	t0=$(date +%s%N)
	if ! "${EC_DEMO}" write --mds 127.0.0.1:2049 --file "${fname}" \
	     --input "${input}" --k "${K}" --m "${M}" \
	     --codec "${CODEC}" --layout v2 --shard-size 4096 \
	     --id "psbench_A_${cell_id}" \
	     >/tmp/ps_bench_write.log 2>&1; then
		note="ec_demo_write_failed"; verify="FAIL"
		echo "A,${CODEC},${K}+${M},${size},${iter},0,0,${verify},${note}" \
		    >>"${OUT}"
		return
	fi
	t1=$(date +%s%N)
	write_ms=$(( (t1 - t0) / 1000000 ))

	t0=$(date +%s%N)
	if ! "${EC_DEMO}" read --mds 127.0.0.1:2049 --file "${fname}" \
	     --output "${output}" --k "${K}" --m "${M}" \
	     --codec "${CODEC}" --layout v2 --shard-size 4096 \
	     --size "${size}" \
	     --id "psbench_A_${cell_id}_r" \
	     >/tmp/ps_bench_read.log 2>&1; then
		note="ec_demo_read_failed"; verify="FAIL"
		echo "A,${CODEC},${K}+${M},${size},${iter},${write_ms},0,${verify},${note}" \
		    >>"${OUT}"
		return
	fi
	t1=$(date +%s%N)
	read_ms=$(( (t1 - t0) / 1000000 ))

	if ! cmp -s "${input}" "${output}"; then
		verify="FAIL"; note="bytes_mismatch"
	fi
	rm -f "${output}"

	echo "A,${CODEC},${K}+${M},${size},${iter},${write_ms},${read_ms},${verify},${note}" \
	    >>"${OUT}"
}

# -- variants B + C: kernel NFSv4.2 mount + fio ----------------------
# Both variants share a privileged --network=host fio client
# container.  We mount PS:4098 once for variant B and MDS:2049
# once for variant C; per-cell fio invocations reuse the mounts.
#
# fio's `lat_ns.mean` is the per-IO mean; for single-bs single-
# count cells it equals the wall-clock duration.  We extract it
# from the JSON output.
#
# Variant C has no codec axis (MDS handles the bytes directly
# with no EC); the CSV uses '-' in the codec/geometry columns.

CLIENT_CONTAINER="reffs-ps-bench-client"

start_client_container() {
	sudo docker rm -f "${CLIENT_CONTAINER}" >/dev/null 2>&1 || true
	sudo docker run -d --name "${CLIENT_CONTAINER}" \
	    --network=host --privileged \
	    --cap-add=SYS_ADMIN \
	    -v /tmp:/host-tmp \
	    --entrypoint=/usr/bin/sleep \
	    reffs-dev:latest infinity >/dev/null
	# fio + nfs-utils on first run (matches the track2 idiom).
	sudo docker exec "${CLIENT_CONTAINER}" sh -c \
	    'command -v fio >/dev/null 2>&1 && command -v mount.nfs >/dev/null 2>&1' \
	    || sudo docker exec "${CLIENT_CONTAINER}" \
	        dnf -y -q install fio nfs-utils >/dev/null

	# Inject DS bridge IPs into the client's /etc/hosts so the
	# kernel can resolve `reffs-bench-ds0..9` (the docker-compose
	# service names the MDS puts in FFv1 layout deviceinfo).  The
	# --network=host client container shares the host network
	# namespace and otherwise cannot reach docker-compose DNS; the
	# kernel would fail layout fetch and fall back to MDS-inband.
	# See reference_bench_ds_nfsv3_gap memory note for the full
	# story.  Net effect: variant C becomes a real kernel-FFv1+
	# NFSv3 pNFS measurement instead of inband.
	for ds in $(seq 0 9); do
		ip=$(sudo docker inspect -f \
		    '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' \
		    "reffs-bench-ds${ds}" 2>/dev/null)
		if [ -n "${ip}" ]; then
			sudo docker exec "${CLIENT_CONTAINER}" sh -c \
			    "grep -q 'reffs-bench-ds${ds}$' /etc/hosts || \
			     echo '${ip} reffs-bench-ds${ds}' >>/etc/hosts"
		fi
	done
}

stop_client_container() {
	# Best-effort umount inside container; the rm -f below will
	# handle leaks if umount fails.
	sudo docker exec "${CLIENT_CONTAINER}" sh -c \
	    'umount /mnt/ps 2>/dev/null; umount /mnt/mds 2>/dev/null' \
	    >/dev/null 2>&1 || true
	sudo docker rm -f "${CLIENT_CONTAINER}" >/dev/null 2>&1 || true
}

# Mount inside the client container.  $1 = "ps" or "mds";
# $2 = port (4098 for PS, 2049 for MDS).
mount_in_client() {
	local tag="$1" port="$2"
	sudo docker exec "${CLIENT_CONTAINER}" sh -c \
	    "mkdir -p /mnt/${tag} && \
	     mount -t nfs4 -o port=${port},vers=4.2,sec=sys,nolock \
	           127.0.0.1:/ /mnt/${tag}" \
	    >/tmp/ps_bench_mount_${tag}.log 2>&1 || {
		echo "FAIL: mount ${tag}:${port} in client container" >&2
		cat /tmp/ps_bench_mount_${tag}.log >&2
		return 1
	}
}

# Run one fio cell inside the client container.  Prints
# "write_ms read_ms verify note" on stdout.
#
# Timing model: wall-clock measured by date(1) around the fio
# call, matching how variant A measures wall-clock around
# ec_demo.  fio's internal lat_ns.mean is only the write()
# syscall latency and excludes the close + (for PS Phase 4b)
# the COMMIT round-trip that actually flushes the bytes -- the
# smoke run on shadow 2026-06-02 showed 1 ms write timings via
# that metric, which is meaningless.  --end_fsync=1 forces
# the close to wait for fsync, but we still measure outside
# fio so the timing is fully comparable to variant A.
fio_cell() {
	local mount_tag="$1" size="$2" cell_id="$3"
	local fname="/mnt/${mount_tag}/ps_bench_${cell_id}.dat"
	local write_ms=0 read_ms=0 verify="OK" note=""

	local t0 t1
	t0=$(date +%s%N)
	if ! sudo docker exec "${CLIENT_CONTAINER}" \
	     fio --name=psb-w --filename="${fname}" \
	         --rw=write --bs="${size}" --size="${size}" \
	         --verify=crc32c --do_verify=0 \
	         --create_on_open=1 --allow_file_create=1 \
	         --end_fsync=1 \
	         --thread=0 --ioengine=psync \
	         --minimal \
	     >/tmp/ps_bench_fio_w_stderr.log 2>&1; then
		verify="FAIL"; note="fio_write_failed"
		echo "0 0 ${verify} ${note}"
		return
	fi
	t1=$(date +%s%N)
	write_ms=$(( (t1 - t0) / 1000000 ))

	t0=$(date +%s%N)
	if ! sudo docker exec "${CLIENT_CONTAINER}" \
	     fio --name=psb-r --filename="${fname}" \
	         --rw=read --bs="${size}" --size="${size}" \
	         --verify=crc32c --do_verify=1 \
	         --thread=0 --ioengine=psync \
	         --minimal \
	     >/tmp/ps_bench_fio_r_stderr.log 2>&1; then
		verify="FAIL"; note="fio_read_or_verify_failed"
		echo "${write_ms} 0 ${verify} ${note}"
		return
	fi
	t1=$(date +%s%N)
	read_ms=$(( (t1 - t0) / 1000000 ))

	# Clean up the test file inside the mount so subsequent
	# iters don't accumulate.  Ignore errors; verify rc above
	# is the real signal.
	sudo docker exec "${CLIENT_CONTAINER}" \
	    rm -f "${fname}" >/dev/null 2>&1 || true

	echo "${write_ms} ${read_ms} ${verify} ${note}"
}

run_variant_B() {
	local size="$1" iter="$2"
	local cell_id="B_${CODEC}_${K}_${M}_${size}_${iter}"
	read -r write_ms read_ms verify note < <(fio_cell ps "${size}" "${cell_id}")
	echo "B,${CODEC},${K}+${M},${size},${iter},${write_ms},${read_ms},${verify},${note}" \
	    >>"${OUT}"
}

run_variant_C() {
	local size="$1" iter="$2"
	local cell_id="C_${size}_${iter}"
	read -r write_ms read_ms verify note < <(fio_cell mds "${size}" "${cell_id}")
	echo "C,-,-,${size},${iter},${write_ms},${read_ms},${verify},${note}" \
	    >>"${OUT}"
}

# -- main loop --------------------------------------------------------
echo "[ps-bench] config: codec=${CODEC} k=${K} m=${M}"
echo "[ps-bench]         sizes=${SIZES}"
echo "[ps-bench]         iters=${ITERS}  variants=${VARIANTS}"
echo "[ps-bench]         ec_demo=${EC_DEMO}"
echo "[ps-bench]         out=${OUT}"

# Variants B and C need the client container + mounts up; A does
# not.  Start the container once if either is in the variant set.
needs_client_container=0
for v in ${VARIANTS}; do
	case "$v" in B|C) needs_client_container=1 ;; esac
done
if [ "${needs_client_container}" -eq 1 ]; then
	start_client_container
	trap stop_client_container EXIT INT TERM
	for v in ${VARIANTS}; do
		case "$v" in
		B) mount_in_client ps  4098 ;;
		C) mount_in_client mds 2049 ;;
		esac
	done
fi

for size in ${SIZES}; do
	for iter in $(seq 1 "${ITERS}"); do
		for v in ${VARIANTS}; do
			echo "[ps-bench] variant=${v} size=${size} iter=${iter}/${ITERS}"
			case "$v" in
			A) run_variant_A "${size}" "${iter}" ;;
			B) run_variant_B "${size}" "${iter}" ;;
			C) run_variant_C "${size}" "${iter}" ;;
			*) echo "FAIL: unknown variant '$v'" >&2; exit 1 ;;
			esac
		done
	done
done

# -- summary ----------------------------------------------------------
fail_count=$(awk -F, 'NR>1 && $8=="FAIL"' "${OUT}" | wc -l | tr -d ' ')
total=$(($(wc -l <"${OUT}") - 1))
echo ""
echo "[ps-bench] done: ${total} cells, ${fail_count} FAIL"
echo "[ps-bench] CSV: ${OUT}"

# -- teardown ---------------------------------------------------------
if [ "${KEEP_UP}" -eq 0 ] && [ "${BRING_UP}" -eq 1 ]; then
	# Only tear down what we brought up.
	echo "[ps-bench] tearing down (pass --keep-up to skip)..."
	# The bringup script doesn't ship a teardown helper; the
	# safe move is to docker compose down and kill any host PS
	# containers.  Match the bringup script's naming.
	sudo docker compose -f "${HERE}/docker-compose.yml" down \
	    --remove-orphans >/dev/null 2>&1 || true
	for c in $(sudo docker ps --format '{{.Names}}' | grep -E '^reffs-ps-[0-9]+$' || true); do
		sudo docker rm -f "$c" >/dev/null 2>&1 || true
	done
fi

if [ "${fail_count}" -gt 0 ]; then
	exit 1
fi
