#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Full EC benchmark — runs all combinations in a single invocation.
#
# Phase 1: SIMD enabled, healthy + degraded-1
# Phase 2: scalar (forced), healthy + degraded-1
#
# All output goes to stdout as a single CSV stream.  The 'simd' column
# in each row distinguishes the SIMD path used.
#
# Usage: ec_benchmark_full.sh <ec_demo_path> <mds_host>

set -e

EC_DEMO="${1:-/build/tools/ec_demo}"
MDS="${2:-localhost}"

SCRIPT_DIR="$(dirname "$0")"
BENCH="${SCRIPT_DIR}/ec_benchmark.sh"

# Phase 1: SIMD enabled + degraded
echo "=== Phase 1/2: SIMD + degraded ===" >&2
"$BENCH" --degrade 1 "$EC_DEMO" "$MDS"

# Phase 2: scalar + degraded (append — skip CSV header)
echo "" >&2
echo "=== Phase 2/2: scalar + degraded ===" >&2
"$BENCH" --degrade 1 --force-scalar "$EC_DEMO" "$MDS" | tail -n +2
