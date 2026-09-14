#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# LOG_COMPILER for .py entries in TESTS.
#
# configure's AM_PATH_PYTHON([3.6],, [:]) makes python optional, and
# sets PYTHON to ':' when it finds none.  Running a test through ':'
# exits 0 having executed nothing, so the suite would report a pass for
# a test that never ran.  Fail instead: a check whose result cannot be
# false is not a check.

set -u

if [ -z "${PYTHON:-}" ] || [ "${PYTHON}" = ":" ]; then
	echo "ERROR: no python3 interpreter (AM_PATH_PYTHON fell back to ':')." >&2
	echo "       $* cannot run; refusing to report a pass." >&2
	exit 1
fi

exec "${PYTHON}" "$@"
