#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Full EC benchmark -- runs all combinations in a single invocation.
#
# Phase 1: v1 (NFSv3), SIMD enabled, healthy + degraded-1
# Phase 2: v1 (NFSv3), scalar (forced), healthy + degraded-1
# Phase 3: v2 (CHUNK ops), SIMD enabled, healthy + degraded-1
# Phase 4: v2 (CHUNK ops), scalar (forced), healthy + degraded-1
#
# All output goes to stdout as a single CSV stream.  The 'layout' and
# 'simd' columns in each row distinguish the phases.
#
# Usage: ec_benchmark_full.sh <ec_demo_path> <mds_host>

set -e

EC_DEMO="${1:-/build/tools/ec_demo}"
MDS="${2:-localhost}"

SCRIPT_DIR="$(dirname "$0")"
BENCH="${SCRIPT_DIR}/ec_benchmark.sh"

# Phase 1: v1 + SIMD + degraded (prints CSV header)
echo "=== Phase 1/4: v1 SIMD + degraded ===" >&2
"$BENCH" --degrade 1 "$EC_DEMO" "$MDS"

# Phase 2: v1 + scalar + degraded (skip header)
echo "" >&2
echo "=== Phase 2/4: v1 scalar + degraded ===" >&2
"$BENCH" --degrade 1 --force-scalar "$EC_DEMO" "$MDS" | tail -n +2

# Phase 3: v2 + SIMD + degraded (skip header)
echo "" >&2
echo "=== Phase 3/4: v2 SIMD + degraded ===" >&2
"$BENCH" --degrade 1 --layout v2 "$EC_DEMO" "$MDS" | tail -n +2

# Phase 4: v2 + scalar + degraded (skip header)
echo "" >&2
echo "=== Phase 4/4: v2 scalar + degraded ===" >&2
"$BENCH" --degrade 1 --layout v2 --force-scalar "$EC_DEMO" "$MDS" | tail -n +2
