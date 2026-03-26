#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Wrapper for Automake LOG_COMPILER — runs a test and appends its
# wall-clock duration to a timing summary file.
#
# Usage: set in Makefile.am:
#   LOG_COMPILER = $(top_srcdir)/scripts/timed-test.sh

TIMING_FILE="${REFFS_TEST_TIMING:-/tmp/reffs-test-timing.txt}"
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

if [ "$elapsed_ms" -ge 1000 ]; then
    elapsed_s=$(( elapsed_ms / 1000 ))
    elapsed_frac=$(( (elapsed_ms % 1000) / 100 ))
    printf "%s: %s [%d.%ds]\n" "$result" "$test_name" "$elapsed_s" "$elapsed_frac" >> "$TIMING_FILE"
else
    printf "%s: %s [%dms]\n" "$result" "$test_name" "$elapsed_ms" >> "$TIMING_FILE"
fi

exit $rc
