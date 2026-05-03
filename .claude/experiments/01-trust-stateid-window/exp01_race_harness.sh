#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Two-writer race harness (single round) for exp 1.
#
# Replaces exp01_race_v1.sh / exp01_race_v2.sh.  Parameterised so
# the sweep wrapper can drive it N times and accumulate CSV.
#
# Emits a single line on stdout of the form:
#   RESULT round=$N layout=$L codec=$C k=$K m=$M size_mb=$S \
#          delay_ms=$D a_rc=$ARC b_rc=$BRC \
#          a_stripes_ok=$AS a_first_fail_stripe=$AFS \
#          b_stripes_ok=$BS final_winner=$FW \
#          mixed_diff_a=$MDA mixed_diff_b=$MDB
#
# See harness.md for the field definitions.

set -u

usage() {
    cat <<EOF
Usage: $0 [options]

  --layout v1|v2        Layout type (required)
  --codec rs|mojette-sys|mojette-nonsys  (default: rs)
  --k K                 Data shards (default: 4)
  --m M                 Parity shards (default: 2)
  --size-mb S           Per-client input size in MiB (default: 100)
  --delay-ms D          Gap between A start and B start in ms (default: 500)
  --round N             Round number for naming/log tags (default: 1)
  --read-delay-s S      Sleep S seconds between writer exit and read-back
                        (default: 0).  Lets the MDS reconcile state before
                        the read; useful for distinguishing transient MIXED
                        observations from durable split-brain.
  --mds HOST[:PORT]     MDS endpoint (default: reffs-mds)
  --ec-demo PATH        ec_demo binary (default: /shared/build/tools/ec_demo)
  --tmp DIR             Temp directory (default: /tmp)
  --keep-tmp            Do not delete /tmp/A_N, /tmp/B_N, /tmp/post_N
EOF
}

LAYOUT=""
CODEC=rs
K=4
M=2
SIZE_MB=100
DELAY_MS=500
ROUND=1
READ_DELAY_S=0
MDS=reffs-mds
EC_DEMO=/shared/build/tools/ec_demo
TMPDIR_=/tmp
KEEP_TMP=0

while [ $# -gt 0 ]; do
    case "$1" in
        --layout) LAYOUT=$2; shift 2 ;;
        --codec) CODEC=$2; shift 2 ;;
        --k) K=$2; shift 2 ;;
        --m) M=$2; shift 2 ;;
        --size-mb) SIZE_MB=$2; shift 2 ;;
        --delay-ms) DELAY_MS=$2; shift 2 ;;
        --round) ROUND=$2; shift 2 ;;
        --read-delay-s) READ_DELAY_S=$2; shift 2 ;;
        --mds) MDS=$2; shift 2 ;;
        --ec-demo) EC_DEMO=$2; shift 2 ;;
        --tmp) TMPDIR_=$2; shift 2 ;;
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
case "$LAYOUT" in
    v1|v2) ;;
    *) echo "--layout must be v1 or v2 (got: $LAYOUT)" >&2; exit 2 ;;
esac

SIZE=$((SIZE_MB * 1024 * 1024))
FILE=race_target_${ROUND}
APATH=$TMPDIR_/A_${ROUND}
BPATH=$TMPDIR_/B_${ROUND}
POSTPATH=$TMPDIR_/post_${ROUND}
AERR=$TMPDIR_/A_${ROUND}.err
BERR=$TMPDIR_/B_${ROUND}.err
RERR=$TMPDIR_/R_${ROUND}.err
AOUT=$TMPDIR_/A_${ROUND}.out
BOUT=$TMPDIR_/B_${ROUND}.out

cleanup() {
    if [ "$KEEP_TMP" = "0" ]; then
        rm -f "$APATH" "$BPATH" "$POSTPATH" \
              "$AERR" "$BERR" "$RERR" "$AOUT" "$BOUT"
    fi
}
trap cleanup EXIT

# Generate two distinct blobs of SIZE bytes each.
dd if=/dev/urandom of="$APATH" bs="$SIZE" count=1 2>/dev/null
dd if=/dev/urandom of="$BPATH" bs="$SIZE" count=1 2>/dev/null

# Start client A.
"$EC_DEMO" write --mds "$MDS" --file "$FILE" --input "$APATH" \
    --k "$K" --m "$M" --codec "$CODEC" --layout "$LAYOUT" \
    --id "clientA_r${ROUND}" \
    >"$AOUT" 2>"$AERR" &
APID=$!

# Convert delay-ms to a fractional second for sleep.
SLEEP_SEC=$(awk -v ms="$DELAY_MS" 'BEGIN{printf "%.3f\n", ms/1000.0}')
sleep "$SLEEP_SEC"

# Start client B.
"$EC_DEMO" write --mds "$MDS" --file "$FILE" --input "$BPATH" \
    --k "$K" --m "$M" --codec "$CODEC" --layout "$LAYOUT" \
    --id "clientB_r${ROUND}" \
    >"$BOUT" 2>"$BERR" &
BPID=$!

wait "$APID"; ARC=$?
wait "$BPID"; BRC=$?

# Optional reconciliation window before the read-back: lets the
# MDS converge any in-flight LAYOUTRETURN / chunk-state changes
# triggered by the writer that lost the race.  See harness.md for
# why this knob exists.
if [ "$READ_DELAY_S" -gt 0 ]; then
    sleep "$READ_DELAY_S"
fi

# Read back to determine the final file state.  Use a fresh reader
# id so it cannot be confused with either writer.
"$EC_DEMO" read --mds "$MDS" --file "$FILE" --output "$POSTPATH" \
    --k "$K" --m "$M" --codec "$CODEC" --size "$SIZE" \
    --layout "$LAYOUT" --id "clientReader_r${ROUND}" \
    2>"$RERR"
RR=$?

# Classify final state.
if [ "$RR" -ne 0 ]; then
    FW=READ_FAILED
    MDA=
    MDB=
elif cmp -s "$APATH" "$POSTPATH"; then
    FW=A
    MDA=
    MDB=
elif cmp -s "$BPATH" "$POSTPATH"; then
    FW=B
    MDA=
    MDB=
else
    FW=MIXED
    MDA=$(cmp -l "$APATH" "$POSTPATH" 2>/dev/null | wc -l | tr -d ' ')
    MDB=$(cmp -l "$BPATH" "$POSTPATH" 2>/dev/null | wc -l | tr -d ' ')
fi

# Count A's successful and failed stripe-shard writes.  ec_demo
# emits lines like:
#   [t.us] ec_write: stripe N data[i] fh_len=24 wsz=4096
#   [t.us] ec_write: data[i] ok
#   [t.us] ec_write: parity[i] FAILED: -5
# `grep -c` exits 1 on no-matches (still printing "0"); $(...)
# captures the "0".  An empty capture means the file is missing.
AS=$(grep -c " ok$" "$AERR" 2>/dev/null)
[ -z "$AS" ] && AS=0
BS=$(grep -c " ok$" "$BERR" 2>/dev/null)
[ -z "$BS" ] && BS=0

# First failed stripe in A's stream.  The "FAILED:" line itself
# does not carry the stripe number; the preceding "stripe N
# parity[i]" line does.  Track the most recent stripe N seen and
# print it when FAILED: first appears.
AFS=$(awk '
    match($0, /stripe [0-9]+/) {
        s = substr($0, RSTART + 7, RLENGTH - 7)
    }
    /FAILED:/ {
        print s
        exit
    }
' "$AERR" 2>/dev/null)
[ -z "$AFS" ] && AFS=NA

echo "RESULT round=$ROUND layout=$LAYOUT codec=$CODEC k=$K m=$M" \
     "size_mb=$SIZE_MB delay_ms=$DELAY_MS" \
     "a_rc=$ARC b_rc=$BRC" \
     "a_stripes_ok=$AS a_first_fail_stripe=$AFS" \
     "b_stripes_ok=$BS" \
     "final_winner=$FW" \
     "mixed_diff_a=${MDA:-NA} mixed_diff_b=${MDB:-NA}"
