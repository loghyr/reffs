#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# nfs_ctime_compare.sh -- Compare ctime/nlink semantics between reffs and knfsd.
#
# Runs entirely inside a throwaway reffs-ci container.  The host is not
# modified.  nfs-kernel-server is installed at container startup and
# discarded with the container.
#
# Usage:
#   scripts/nfs_ctime_compare.sh
#
# Output: per-test PASS/FAIL for reffs (port 3049) and knfsd (port 2049).

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
CI_IMAGE="reffs-ci:latest"
INNER="/reffs/scripts/_nfs_ctime_compare_inner.sh"

exec sudo docker run --rm --privileged \
	-v "$REPO":/reffs:ro,Z \
	"$CI_IMAGE" \
	/bin/bash "$INNER"
