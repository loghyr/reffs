#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# nightly_runner.sh -- Cron bootstrap for nightly CI.
#
# Usage (cron):
#   0 2 * * * /home/loghyr/reffs/scripts/nightly_runner.sh 2>&1
#
# Pulls the latest code from origin/main, then exec's nightly_ci.sh
# so that any changes pushed that day take effect tonight rather than
# the following night.
#
# This file should never need changing.  All CI logic lives in
# nightly_ci.sh, which is updated by the pull below.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PULL_LOG="/reffs_data/nightly_pull.log"

cd "$REPO"

echo "[$(date +%Y%m%d-%H%M%S)] nightly_runner: checkout main" >> "$PULL_LOG"
if git checkout main >> "$PULL_LOG" 2>&1; then
    echo "[$(date +%Y%m%d-%H%M%S)] nightly_runner: checkout ok" >> "$PULL_LOG"
else
    echo "[$(date +%Y%m%d-%H%M%S)] nightly_runner: checkout failed, aborting" >> "$PULL_LOG"
    exit 1
fi

echo "[$(date +%Y%m%d-%H%M%S)] nightly_runner: git pull" >> "$PULL_LOG"
if git pull --ff-only origin main >> "$PULL_LOG" 2>&1; then
    echo "[$(date +%Y%m%d-%H%M%S)] nightly_runner: pull ok" >> "$PULL_LOG"
else
    echo "[$(date +%Y%m%d-%H%M%S)] nightly_runner: pull failed, running stale" >> "$PULL_LOG"
fi

exec "$REPO/scripts/nightly_ci.sh" "$@"
