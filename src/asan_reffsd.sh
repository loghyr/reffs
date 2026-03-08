#!/bin/sh

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
ASAN_OPTIONS="suppressions=$SCRIPT_DIR/asan-suppressions.txt" $SCRIPT_DIR/reffsd "$@"
