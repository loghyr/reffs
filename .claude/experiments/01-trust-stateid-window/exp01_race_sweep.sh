#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Sweep wrapper: drive exp01_race_harness.sh N times, emit CSV.
#
# CSV columns match the RESULT line fields from the harness:
#   round,layout,codec,k,m,size_mb,delay_ms,a_rc,b_rc,
#   a_stripes_ok,a_first_fail_stripe,b_stripes_ok,final_winner,
#   mixed_diff_a,mixed_diff_b
#
# The deck-relevant invariant is "no MIXED across N rounds".  The
# sweep prints a one-line summary at the end:
#   SUMMARY runs=N a_lost=A b_lost=B mixed=M read_failed=R both_completed=C
# Where A is the count of round outcomes with final_winner=A, etc.
# `mixed != 0` is a falsifying observation for the deck claim.

set -u

usage() {
    cat <<EOF
Usage: $0 [options]

  --runs N              Number of rounds (default: 20)
  --layout v1|v2        Layout type (required)
  --codec C             Codec (default: rs)
  --k K --m M           Codec geometry (default: 4 / 2)
  --size-mb S           Per-client input size (default: 100)
  --delay-ms D          A->B start gap (default: 500)
  --mds HOST[:PORT]     MDS endpoint (default: reffs-mds)
  --ec-demo PATH        ec_demo path
  --tmp DIR             Temp dir
  --csv PATH            Output CSV path (default: stdout only)
  --keep-tmp            Pass through to harness

Defaults match the original v1 manual run that informed report.md.
EOF
}

RUNS=20
LAYOUT=""
CODEC=rs
K=4
M=2
SIZE_MB=100
DELAY_MS=500
MDS=reffs-mds
EC_DEMO=/shared/build/tools/ec_demo
TMPDIR_=/tmp
CSV=""
KEEP_TMP=0

while [ $# -gt 0 ]; do
    case "$1" in
        --runs) RUNS=$2; shift 2 ;;
        --layout) LAYOUT=$2; shift 2 ;;
        --codec) CODEC=$2; shift 2 ;;
        --k) K=$2; shift 2 ;;
        --m) M=$2; shift 2 ;;
        --size-mb) SIZE_MB=$2; shift 2 ;;
        --delay-ms) DELAY_MS=$2; shift 2 ;;
        --mds) MDS=$2; shift 2 ;;
        --ec-demo) EC_DEMO=$2; shift 2 ;;
        --tmp) TMPDIR_=$2; shift 2 ;;
        --csv) CSV=$2; shift 2 ;;
        --keep-tmp) KEEP_TMP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown arg: $1" >&2; usage; exit 2 ;;
    esac
done

if [ -z "$LAYOUT" ]; then
    echo "--layout v1|v2 is required" >&2
    usage
    exit 2
fi

HERE=$(dirname "$0")
HARNESS="$HERE/exp01_race_harness.sh"
if [ ! -x "$HARNESS" ]; then
    echo "harness not executable: $HARNESS" >&2
    exit 2
fi

HEADER="round,layout,codec,k,m,size_mb,delay_ms,a_rc,b_rc,a_stripes_ok,a_first_fail_stripe,b_stripes_ok,final_winner,mixed_diff_a,mixed_diff_b"

emit_csv_line() {
    if [ -n "$CSV" ]; then
        echo "$1" >>"$CSV"
    fi
    echo "$1"
}

if [ -n "$CSV" ]; then
    : >"$CSV"
fi
emit_csv_line "$HEADER"

a_won=0
b_won=0
mixed=0
read_failed=0
both_done=0

KEEP_FLAG=""
[ "$KEEP_TMP" = "1" ] && KEEP_FLAG="--keep-tmp"

for r in $(seq 1 "$RUNS"); do
    LINE=$("$HARNESS" \
        --layout "$LAYOUT" --codec "$CODEC" --k "$K" --m "$M" \
        --size-mb "$SIZE_MB" --delay-ms "$DELAY_MS" \
        --round "$r" --mds "$MDS" --ec-demo "$EC_DEMO" \
        --tmp "$TMPDIR_" $KEEP_FLAG \
        2>/dev/null | grep '^RESULT')

    if [ -z "$LINE" ]; then
        echo "round $r: no RESULT line" >&2
        emit_csv_line "$r,$LAYOUT,$CODEC,$K,$M,$SIZE_MB,$DELAY_MS,,,,NA,,HARNESS_FAIL,NA,NA"
        continue
    fi

    # Parse RESULT key=value pairs into shell vars.
    eval "$(echo "$LINE" \
        | sed 's/^RESULT //' \
        | tr ' ' '\n' \
        | sed 's/=\(.*\)$/="\1"/' \
        | sed 's/^/V_/')"

    emit_csv_line "$V_round,$V_layout,$V_codec,$V_k,$V_m,$V_size_mb,$V_delay_ms,$V_a_rc,$V_b_rc,$V_a_stripes_ok,$V_a_first_fail_stripe,$V_b_stripes_ok,$V_final_winner,$V_mixed_diff_a,$V_mixed_diff_b"

    case "$V_final_winner" in
        A) a_won=$((a_won + 1)) ;;
        B) b_won=$((b_won + 1)) ;;
        MIXED) mixed=$((mixed + 1)) ;;
        READ_FAILED) read_failed=$((read_failed + 1)) ;;
    esac
    if [ "$V_a_rc" = "0" ] && [ "$V_b_rc" = "0" ]; then
        both_done=$((both_done + 1))
    fi
done

echo
echo "SUMMARY runs=$RUNS a_won=$a_won b_won=$b_won mixed=$mixed" \
     "read_failed=$read_failed both_completed=$both_done"

if [ "$mixed" -gt 0 ]; then
    echo "FALSIFIED: mixed != 0 means split-brain happened at least once" >&2
    exit 1
fi
