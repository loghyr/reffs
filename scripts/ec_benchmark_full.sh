#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Full EC benchmark -- runs all combinations in a single invocation.
#
# Cross product of three axes:
#   layout: v1 (NFSv3) | v2 (CHUNK ops)
#   simd:   SIMD       | scalar (forced)
#   inverse: peel       | gd (geometry-driven)
#
# 2 * 2 * 2 = 8 phases, each healthy + degraded-1.
#
# All output goes to stdout as a single CSV stream.  The 'layout',
# 'simd', and 'inverse' columns in each row distinguish the phases.
#
# Usage: ec_benchmark_full.sh <ec_demo_path> <mds_host>

set -e

EC_DEMO="${1:-/build/tools/ec_demo}"
MDS="${2:-localhost}"

SCRIPT_DIR="$(dirname "$0")"
BENCH="${SCRIPT_DIR}/ec_benchmark.sh"

# Phase 1: v1 + SIMD + peel + degraded (prints CSV header)
echo "=== Phase 1/8: v1 SIMD peel + degraded ===" >&2
"$BENCH" --degrade 1 "$EC_DEMO" "$MDS"

# Phase 2: v1 + scalar + peel + degraded (skip header)
echo "" >&2
echo "=== Phase 2/8: v1 scalar peel + degraded ===" >&2
"$BENCH" --degrade 1 --force-scalar "$EC_DEMO" "$MDS" | tail -n +2

# Phase 3: v2 + SIMD + peel + degraded (skip header)
echo "" >&2
echo "=== Phase 3/8: v2 SIMD peel + degraded ===" >&2
"$BENCH" --degrade 1 --layout v2 "$EC_DEMO" "$MDS" | tail -n +2

# Phase 4: v2 + scalar + peel + degraded (skip header)
echo "" >&2
echo "=== Phase 4/8: v2 scalar peel + degraded ===" >&2
"$BENCH" --degrade 1 --layout v2 --force-scalar "$EC_DEMO" "$MDS" | tail -n +2

# Phase 5: v1 + SIMD + gd + degraded (skip header)
echo "" >&2
echo "=== Phase 5/8: v1 SIMD gd + degraded ===" >&2
"$BENCH" --degrade 1 --force-gd "$EC_DEMO" "$MDS" | tail -n +2

# Phase 6: v1 + scalar + gd + degraded (skip header)
echo "" >&2
echo "=== Phase 6/8: v1 scalar gd + degraded ===" >&2
"$BENCH" --degrade 1 --force-scalar --force-gd "$EC_DEMO" "$MDS" | tail -n +2

# Phase 7: v2 + SIMD + gd + degraded (skip header)
echo "" >&2
echo "=== Phase 7/8: v2 SIMD gd + degraded ===" >&2
"$BENCH" --degrade 1 --layout v2 --force-gd "$EC_DEMO" "$MDS" | tail -n +2

# Phase 8: v2 + scalar + gd + degraded (skip header)
echo "" >&2
echo "=== Phase 8/8: v2 scalar gd + degraded ===" >&2
"$BENCH" --degrade 1 --layout v2 --force-scalar --force-gd "$EC_DEMO" "$MDS" | tail -n +2
