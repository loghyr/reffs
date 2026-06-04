#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# 4-variant realnet harness for the PS-encoder bench
# (.claude/design/realnet-harness.md is the slice plan;
#  .claude/design/ps-encoder-bench-4variant-realnet.md is the
#  parent plan this slice unblocks at prereq #4).
#
# Drives the 3-host realnet topology (1 MDS on shadow, 1 PS on
# adept, 1 client on dreamer) through the 4-variant cell matrix
# and writes CSV ready for downstream analysis.
#
# Initial scope: variant d ONLY (kernel client -> PS -> MDS+DSes).
# Variants a/b/c emit verify=SKIP rows until the plain-MDS
# bringup follow-up lands (the realnet bringup's MDS is
# tls=true and does not accept plain TCP).
#
# Pre-conditions:
#   1. realnet topology up (run-realnet-bringup.sh succeeded).
#   2. Local laptop can SSH to the client host (default: dreamer).
#   3. Client host has reffs-probe.py installed (for the
#      sb-set-default-coding probe RPCs to the MDS).
#
# Usage example:
#   ./run-realnet-harness.sh \
#       --codecs rs,mojette-sys \
#       --geometries 4+2,8+2 \
#       --sizes 4096,65536,1048576 \
#       --iters 3 \
#       --variants d
#
# Output CSV is appended-friendly; re-runs accumulate into the
# default results/realnet/ directory or a --out PATH.

set -euo pipefail

# -- defaults ----------------------------------------------------------
CODECS="rs"
GEOMETRIES="4+2"
SIZES="4096,65536,1048576"
ITERS=3
VARIANTS="d"
CLIENT_HOST="${CLIENT_HOST:-dreamer}"
PS_HOST="${PS_HOST:-adept}"
DS_MDS_HOST="${DS_MDS_HOST:-shadow}"
MOUNT_POINT="${MOUNT_POINT:-/mnt/realnet-bench}"
OUT=""

HERE=$(cd "$(dirname "$0")" && pwd)

# -- arg parse --------------------------------------------------------
usage() {
    sed -n '/^# Usage example:/,/^# Output/p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --codecs)        CODECS="$2";        shift 2 ;;
    --geometries)    GEOMETRIES="$2";    shift 2 ;;
    --sizes)         SIZES="$2";         shift 2 ;;
    --iters)         ITERS="$2";         shift 2 ;;
    --variants)      VARIANTS="$2";      shift 2 ;;
    --client-host)   CLIENT_HOST="$2";   shift 2 ;;
    --ps-host)       PS_HOST="$2";       shift 2 ;;
    --ds-mds-host)   DS_MDS_HOST="$2";   shift 2 ;;
    --mount-point)   MOUNT_POINT="$2";   shift 2 ;;
    --out)           OUT="$2";           shift 2 ;;
    -h|--help)       usage; exit 0 ;;
    *) echo "FAIL: unknown arg '$1'" >&2; exit 1 ;;
    esac
done

# -- discover lab host IPs --------------------------------------------
# The CSV captures both hostname and resolved LAN IP so analysis
# can distinguish "ps_host" reassignments without losing the IP
# trail.  ip-discover on each host to keep this faithful to what
# the bringup script actually wired up.
echo "[harness] discovering host LAN IPs..."
PS_LAN_IP=$(ssh "$PS_HOST" \
    "ip -4 addr show 2>/dev/null \
        | awk '/inet 192\\./ {print \$2; exit}' \
        | cut -d/ -f1" 2>/dev/null || true)
DS_MDS_LAN_IP=$(ssh "$DS_MDS_HOST" \
    "ip -4 addr show 2>/dev/null \
        | awk '/inet 192\\./ {print \$2; exit}' \
        | cut -d/ -f1" 2>/dev/null || true)

if [ -z "$PS_LAN_IP" ] || [ -z "$DS_MDS_LAN_IP" ]; then
    echo "FAIL: could not discover LAN IPs (PS=$PS_LAN_IP, DS_MDS=$DS_MDS_LAN_IP)" >&2
    exit 1
fi
echo "[harness]    PS at $PS_HOST ($PS_LAN_IP:4098)"
echo "[harness]    DS+MDS at $DS_MDS_HOST ($DS_MDS_LAN_IP:2049)"

# -- pre-flight: topology reachable? ----------------------------------
echo "[harness] pre-flight: verify topology is up..."
if ! ssh "$CLIENT_HOST" "nc -zv -w 3 $PS_LAN_IP 4098 2>&1 | grep -q 'Connected'"; then
    echo "FAIL: $CLIENT_HOST cannot reach PS at $PS_LAN_IP:4098" >&2
    echo "      Run deploy/benchmark/run-realnet-bringup.sh first." >&2
    exit 1
fi
if ! ssh "$CLIENT_HOST" "nc -zv -w 3 $DS_MDS_LAN_IP 2049 2>&1 | grep -q 'Connected'"; then
    echo "FAIL: $CLIENT_HOST cannot reach MDS at $DS_MDS_LAN_IP:2049" >&2
    exit 1
fi

# -- output path ------------------------------------------------------
if [ -z "$OUT" ]; then
    mkdir -p "$HERE/results/realnet"
    # Date can come from the local laptop (the harness driver) --
    # all cells go to the same file regardless of run length so
    # timezone is unambiguous.  UTC keeps it stable across
    # operator timezones.
    ts=$(date -u +%Y%m%d-%H%M%S)
    OUT="$HERE/results/realnet/realnet-${VARIANTS//,/}-${DS_MDS_HOST}-${ts}.csv"
fi

# -- CSV header -------------------------------------------------------
# Same first 9 columns as run_ps_vs_client_bench.sh's CSV so
# gen_benchmark_report.py keeps working unmodified.  Realnet
# extensions are appended (column order is contract for CSV
# tooling).
if [ ! -f "$OUT" ]; then
    echo "variant,codec,geometry,size_bytes,iter,write_ms,read_ms,verify,note,topology,client_host,ps_host,ds_mds_host,timestamp,bytes_per_sec_write,bytes_per_sec_read" \
        > "$OUT"
fi
echo "[harness] writing CSV to $OUT"

# -- helpers ----------------------------------------------------------

# Emit one CSV row.  All fields shell-quoted to survive commas in
# notes; the note column is the only one that might legitimately
# contain commas in error messages, so we sanitise it.
emit_row() {
    local variant=$1 codec=$2 geom=$3 size=$4 iter=$5
    local write_ms=$6 read_ms=$7 verify=$8 note=$9 ts=${10}
    local bps_w=0 bps_r=0

    # Belt-and-suspenders: an empty / non-numeric timing comes
    # from a stripped result line (run_cell_d exited mid-stream)
    # and would crash the arithmetic compare below under `set -u`.
    # Treat it as 0 + add a parse_failed note so the CSV row still
    # emits cleanly.
    if ! [[ "$write_ms" =~ ^[0-9]+$ ]]; then
        write_ms=0
        verify=FAIL
        note="${note:+$note;}parse_failed"
    fi
    if ! [[ "$read_ms" =~ ^[0-9]+$ ]]; then
        read_ms=0
        verify=FAIL
        note="${note:+$note;}parse_failed"
    fi

    # Avoid div-by-zero; report 0 when timing is 0 (failed cell).
    if [ "$write_ms" -gt 0 ]; then
        bps_w=$(( size * 1000 / write_ms ))
    fi
    if [ "$read_ms" -gt 0 ]; then
        bps_r=$(( size * 1000 / read_ms ))
    fi

    # Sanitise commas in note (we use semicolons in error messages
    # already; this is belt-and-suspenders).
    note="${note//,/;}"

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$variant" "$codec" "$geom" "$size" "$iter" \
        "$write_ms" "$read_ms" "$verify" "$note" \
        "realnet-3host" "$CLIENT_HOST" "$PS_HOST" "$DS_MDS_HOST" \
        "$ts" "$bps_w" "$bps_r" \
        >> "$OUT"
}

# Set the MDS's per-export default coding via probe RPC.  Run
# inside the bench MDS container -- the probe binary lives there
# alongside python3/reply-xdr.  Reffs-probe.py takes the spec
# as a single "codec:K+M" string (.claude/design/per-export-
# default-coding.md step 9 grammar).
PROBE_CMD="sudo docker exec reffs-bench-mds \
    /shared/build/scripts/reffs-probe.py \
    --host 127.0.0.1 --port 20490"

set_default_coding() {
    local codec=$1 k=$2 m=$3
    local spec="${codec}:${k}+${m}"

    ssh "$DS_MDS_HOST" "$PROBE_CMD sb-set-default-coding \
        --id 1 --default-coding $spec" >/dev/null 2>&1
}

# Ensure the client has the realnet mount.  Idempotent.  Variant d
# only -- this is the kernel mount of the PS listener.
mount_variant_d() {
    ssh "$CLIENT_HOST" bash -s <<EOF
set -euo pipefail
if mount | grep -q "$MOUNT_POINT"; then
    exit 0
fi
sudo mkdir -p "$MOUNT_POINT"
sudo mount -t nfs4 -o sec=sys,vers=4.2,port=4098 \
    $PS_LAN_IP:/ $MOUNT_POINT
EOF
}

# Cleanup mount on harness exit so re-runs start clean.  Best
# effort -- if umount fails (e.g., busy), leave the mount and
# rely on the next bringup teardown to handle it.
cleanup_mount() {
    ssh "$CLIENT_HOST" \
        "sudo umount $MOUNT_POINT 2>/dev/null || true" >/dev/null 2>&1
}
trap cleanup_mount EXIT

# Per-cell variant d execution.  Echoes "write_ms read_ms verify note"
# on stdout for emit_row.  Run on the client host so all I/O is
# local to the mount.
#
# We use `dd conv=fsync` for the write so the COMMIT round-trip is
# part of the timing (matches the single-host harness's
# --end_fsync=1 on fio).  We drop the page cache between write and
# read so the read measures the actual PS-decode round-trip, not
# cached pages.
run_cell_d() {
    local size=$1 iter=$2 cell_id=$3

    # The remote runs as one ssh round per cell -- everything inside
    # is local on the client.  Timing comes from `date +%s%N` around
    # the dd calls so we measure end-to-end wall clock (not dd's own
    # internal rate which excludes fsync).
    # set +e on dd / cmp -- we want to emit a FAIL row per failed
    # cell rather than abort the whole sweep on the first error.
    # `set -u` stays on (catches typos).  pipefail is harmless --
    # this body has no pipelines today, but the option is kept on
    # so any future pipeline addition does not silently swallow
    # intermediate failures.
    ssh "$CLIENT_HOST" bash -s <<EOF
set -uo pipefail
in=/tmp/realnet_in_${size}.bin
out=/tmp/realnet_out_${cell_id}.bin
target=$MOUNT_POINT/${cell_id}.bin

# Pre-cache input per size (re-used across iters).
if [ ! -f "\$in" ] || [ "\$(stat -c%s "\$in" 2>/dev/null)" != "${size}" ]; then
    if ! sudo dd if=/dev/urandom of="\$in" bs=${size} count=1 status=none; then
        rc=\$?
        echo "0 0 FAIL input_gen_failed_\$rc"
        exit 0
    fi
fi
sudo rm -f "\$target" "\$out"

t0=\$(date +%s%N)
if ! sudo dd if="\$in" of="\$target" bs=${size} count=1 \
        conv=fsync status=none; then
    rc=\$?
    echo "0 0 FAIL dd_write_failed_\$rc"
    exit 0
fi
t1=\$(date +%s%N)
write_ms=\$(( (t1 - t0) / 1000000 ))

# Drop client page cache so the read is not served from RAM.  If
# the drop fails (typically permission), warn via the note column
# but continue: a stale-cache read timing is better than no read
# timing at all, and the note column lets the operator discount
# the read value when relevant.  Matches the design doc's
# documented "warn once and continue" contract.
drop_note=
if ! echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1; then
    drop_note=drop_caches_failed
fi

t0=\$(date +%s%N)
if ! sudo dd if="\$target" of="\$out" bs=${size} status=none; then
    rc=\$?
    echo "\$write_ms 0 FAIL dd_read_failed_\$rc"
    exit 0
fi
t1=\$(date +%s%N)
read_ms=\$(( (t1 - t0) / 1000000 ))

verify=OK
note=\$drop_note
if ! sudo cmp -s "\$in" "\$out"; then
    verify=FAIL
    if [ -n "\$note" ]; then
        note="\${note};bytes_mismatch"
    else
        note=bytes_mismatch
    fi
fi
sudo rm -f "\$out"
echo "\$write_ms \$read_ms \$verify \$note"
EOF
}

# Per-cell variant a/b/c stub.  Always emits SKIP until the
# plain-MDS bringup slice lands.
run_cell_stub() {
    echo "0 0 SKIP needs_plain_mds_bringup"
}

# -- main loop --------------------------------------------------------
total_cells=0
ok_cells=0
fail_cells=0
skip_cells=0

# Variant d setup: mount the PS once (idempotent).
case "$VARIANTS" in
*d*) mount_variant_d ;;
esac

# Iteration order: variants outer, then codecs, then geoms, then
# sizes, then iters.  Switching codec/geom between cells (the
# per-cell set_default_coding) is the slow axis (probe round-trip);
# iterating sizes within a (codec, geom) amortizes the switch.
IFS=',' read -ra VARIANT_ARR <<<"$VARIANTS"
IFS=',' read -ra CODEC_ARR <<<"$CODECS"
IFS=',' read -ra GEOM_ARR <<<"$GEOMETRIES"
IFS=',' read -ra SIZE_ARR <<<"$SIZES"

for variant in "${VARIANT_ARR[@]}"; do
    for codec in "${CODEC_ARR[@]}"; do
        for geom in "${GEOM_ARR[@]}"; do
            # Parse "K+M" into K and M.
            k=${geom%+*}
            m=${geom#*+}

            # Switch MDS default coding once per (codec, geom).
            # Only variant d uses the MDS path through the PS,
            # so skip the probe when running stub variants.  If
            # the probe fails (MDS gone, container died, probe
            # CLI grammar drift) every subsequent variant-d cell
            # in this (codec, geom) iteration would otherwise be
            # labelled with the *requested* codec/geom while the
            # MDS continues issuing layouts under whatever coding
            # it had before -- silent data corruption in the CSV.
            # Skip the cells with verify=FAIL note=
            # set_default_coding_failed so downstream analysis
            # sees the gap explicitly.
            coding_ok=1
            if [ "$variant" = "d" ]; then
                if ! set_default_coding "$codec" "$k" "$m"; then
                    echo "WARN: set_default_coding $codec/$k+$m failed -- skipping cells" >&2
                    coding_ok=0
                fi
            fi

            for size in "${SIZE_ARR[@]}"; do
                for iter in $(seq 1 "$ITERS"); do
                    total_cells=$((total_cells + 1))
                    cell_id="${variant}_${codec}_${k}_${m}_${size}_${iter}"
                    ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)

                    if [ "$coding_ok" = "0" ]; then
                        result="0 0 FAIL set_default_coding_failed"
                    else
                        case "$variant" in
                        d)
                            result=$(run_cell_d "$size" "$iter" "$cell_id")
                            ;;
                        a|b|c)
                            result=$(run_cell_stub)
                            ;;
                        *)
                            echo "FAIL: unknown variant '$variant'" >&2
                            exit 1
                            ;;
                        esac
                    fi

                    # Parse "write_ms read_ms verify note".  Note
                    # may be empty.
                    read -r write_ms read_ms verify note <<<"$result"
                    note=${note:-}

                    emit_row "$variant" "$codec" "$geom" "$size" \
                        "$iter" "$write_ms" "$read_ms" \
                        "$verify" "$note" "$ts"

                    case "$verify" in
                    OK)   ok_cells=$((ok_cells + 1)) ;;
                    FAIL) fail_cells=$((fail_cells + 1)) ;;
                    SKIP) skip_cells=$((skip_cells + 1)) ;;
                    esac

                    # Brief progress line per cell.
                    printf '[harness]  %-30s %s write=%sms read=%sms\n' \
                        "$cell_id" "$verify" "$write_ms" "$read_ms"
                done
            done
        done
    done
done

# -- summary ----------------------------------------------------------
echo ""
echo "[harness] DONE.  $total_cells cells (ok=$ok_cells fail=$fail_cells skip=$skip_cells)"
echo "[harness] CSV: $OUT"
if [ "$fail_cells" -gt 0 ]; then
    echo "[harness] $fail_cells FAIL cell(s); inspect CSV for details."
    exit 1
fi
