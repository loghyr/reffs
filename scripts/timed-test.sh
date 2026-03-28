#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Wrapper for Automake LOG_COMPILER — runs a test and appends its
# wall-clock duration to a timing summary file.  Tests exceeding
# REFFS_TEST_MAX_MS (default 2000ms) are flagged as SLOW.
#
# Usage: set in Makefile.am:
#   LOG_COMPILER = $(top_srcdir)/scripts/timed-test.sh

TIMING_FILE="${REFFS_TEST_TIMING:-/tmp/reffs-test-timing.txt}"
MAX_MS="${REFFS_TEST_MAX_MS:-2000}"
test_name=$(basename "$1")

start=$(date +%s%N)
"$@"
rc=$?
end=$(date +%s%N)

elapsed_ms=$(( (end - start) / 1000000 ))

if [ $rc -eq 0 ]; then
    result="PASS"
else
    result="FAIL"
fi

slow=""
if [ "$elapsed_ms" -gt "$MAX_MS" ]; then
    slow=" SLOW"
fi

if [ "$elapsed_ms" -ge 1000 ]; then
    elapsed_s=$(( elapsed_ms / 1000 ))
    elapsed_frac=$(( (elapsed_ms % 1000) / 100 ))
    line=$(printf "%s: %s [%d.%ds]%s" "$result" "$test_name" \
        "$elapsed_s" "$elapsed_frac" "$slow")
else
    line=$(printf "%s: %s [%dms]%s" "$result" "$test_name" \
        "$elapsed_ms" "$slow")
fi

echo "$line" >> "$TIMING_FILE"
echo "$line"

# Warn on stderr so it shows up in CI logs
if [ -n "$slow" ]; then
    echo "WARNING: $test_name took ${elapsed_ms}ms (limit ${MAX_MS}ms)" >&2
fi

exit $rc
