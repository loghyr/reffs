#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Provision the 5 sanity-test superblocks against a running MDS.
# Per .claude/design/sb-registry-v3.md, [[export]] in TOML is
# root-only since the registry-v3 work; non-root SBs must be
# created via the probe protocol after the MDS is up.
#
# For each SB:
#   - sb-create  --path <path> --storage ram
#   - sb-set-dstores --id <id> --dstores 1 2 3 4 5 6  (all 6 DSes)
#   - sb-mount   --id <id> --path <path>
#
# Idempotent: if a SB already exists at that path, skip it (so the
# script can be re-run on a re-attached test container).
#
# Usage: setup-sbs.sh <mds_host>

set -e

MDS="${1:-reffs-mds}"
PROBE="/shared/build/scripts/reffs-probe.py"
if [ ! -x "$PROBE" ]; then
    PROBE=$(find /shared/build -name reffs-probe.py -type f 2>/dev/null | head -1)
    if [ -z "$PROBE" ]; then
        echo "ERROR: reffs-probe.py not found in /shared/build" >&2
        exit 1
    fi
fi

PROBE_OPTS=(--host "$MDS")

wait_for_probe() {
    echo "Waiting for MDS probe port at $MDS:20490..." >&2
    for i in $(seq 1 60); do
        if "$PROBE" "${PROBE_OPTS[@]}" sb-list >/dev/null 2>&1; then
            echo "MDS probe is up." >&2
            return 0
        fi
        sleep 1
    done
    echo "ERROR: MDS probe did not come up within 60s" >&2
    exit 1
}

# Returns the SB id at the given mount path, or empty string.
# sb-list output is tabular: "ID  UUID  Path  State  Flavors"
# (columns 1, 2, 3, 4, 5).  Header lines start with "ID" or "--".
sb_id_for_path() {
    local path="$1"
    "$PROBE" "${PROBE_OPTS[@]}" sb-list 2>/dev/null \
        | awk -v p="$path" '$1 != "ID" && $1 !~ /^-/ && $3 == p { print $1; exit }'
}

create_sb() {
    local path="$1"
    local existing
    existing=$(sb_id_for_path "$path")
    if [ -n "$existing" ]; then
        echo "  SB '$path' already exists at id=$existing" >&2
        echo "$existing"
        return 0
    fi
    local out
    out=$("$PROBE" "${PROBE_OPTS[@]}" sb-create --path "$path" --storage ram)
    # Output is "Created superblock <id>: uuid=<u> path=<p> ..."
    local id
    id=$(echo "$out" | awk '/^Created superblock/ { sub(":",""); print $3; exit }')
    if [ -z "$id" ]; then
        echo "ERROR: failed to parse SB id from sb-create output: $out" >&2
        exit 1
    fi
    echo "  SB '$path' created at id=$id" >&2
    echo "$id"
}

set_dstores() {
    local id="$1"; shift
    "$PROBE" "${PROBE_OPTS[@]}" sb-set-dstores --id "$id" --dstores "$@" >&2
}

mount_sb() {
    local id="$1"
    local path="$2"
    "$PROBE" "${PROBE_OPTS[@]}" sb-mount --id "$id" --path "$path" >&2 || {
        # Already-mounted is OK
        echo "  (sb-mount returned non-zero; assuming already mounted)" >&2
    }
}

provision() {
    local path="$1"
    local id
    id=$(create_sb "$path")
    set_dstores "$id" 1 2 3 4 5 6
    mount_sb "$id" "$path"
}

main() {
    wait_for_probe

    echo "=== Provisioning sanity SBs (all bound to all 6 DSes) ===" >&2
    for path in /ffv1-csm /ffv1-stripes /ffv2-csm /ffv2-rs /ffv2-mj; do
        provision "$path"
    done

    echo "=== Final SB list ===" >&2
    "$PROBE" "${PROBE_OPTS[@]}" sb-list >&2
}

main "$@"
