#!/bin/sh
# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
ASAN_OPTIONS="suppressions=$SCRIPT_DIR/asan-suppressions.txt" $SCRIPT_DIR/reffsd "$@"
