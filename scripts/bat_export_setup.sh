#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# bat_export_setup.sh -- Create per-flavor exports via probe protocol.
#
# The root export (sb_id=1) gets all flavors via TOML config.
# Non-root exports are created via reffs-probe.py (probe is the
# sole authority for export creation per sb-registry-v3).
#
# Usage: bat_export_setup.sh [HOST] [PORT]
#   HOST  Probe server host (default: localhost)
#   PORT  Probe server port (default: 20490)
#
# Prerequisites: reffsd running with root export configured,
#                reffs-probe.py in $PATH.

set -euo pipefail

HOST=${1:-localhost}
PORT=${2:-20490}
PROBE="reffs-probe.py --host $HOST --port $PORT"

die() { echo "FATAL: $*" >&2; exit 1; }
info() { echo "[export] $*"; }

# -----------------------------------------------------------------------
# Verify server is reachable
# -----------------------------------------------------------------------
info "Waiting for probe server at $HOST:$PORT..."
probe_ok=false
for i in $(seq 1 15); do
	if $PROBE sb-list >/dev/null 2>&1; then
		probe_ok=true
		break
	fi
	sleep 2
done
if [ "$probe_ok" != "true" ]; then
	die "Cannot reach probe server after 30s"
fi
info "Probe server ready"

# -----------------------------------------------------------------------
# Create per-flavor exports
# -----------------------------------------------------------------------

create_export() {
	local path=$1
	shift
	local flavors="$*"

	info "Creating export $path (flavors: $flavors)..."

	# Create the export (may fail with EEXIST if already created)
	# Output format: "Created superblock 10: uuid=... path=... ..."
	local output id
	output=$($PROBE sb-create --path "$path" --storage ram 2>&1) || true
	id=$(echo "$output" | grep -o 'superblock [0-9]*' | awk '{print $2}') || true

	if [ -z "$id" ]; then
		info "  sb-create failed or already exists: $output"
		# Try to find existing (exact path match, skip destroyed)
		id=$($PROBE sb-list 2>&1 | awk -v p="$path" '$3 == p && $4 != "destroyed" {print $1; exit}') || true
		if [ -z "$id" ]; then
			die "Cannot create or find export at $path"
		fi
		info "  Found existing export id=$id"
	fi

	# Set flavors
	$PROBE sb-set-flavors --id "$id" --flavors $flavors || \
		info "  WARN: set-flavors failed for $path (id=$id)"

	# Mount
	$PROBE sb-mount --id "$id" --path "$path" 2>/dev/null || \
		info "  (already mounted or mount deferred)"

	info "  Export $path (id=$id) ready"
}

# Per-flavor exports
create_export /sys sys
create_export /krb5 krb5
create_export /krb5i krb5i
create_export /krb5p krb5p
create_export /tls tls

# pNFS exports (need sys + tls for layout I/O)
create_export /ffv1 sys tls
create_export /ffv2 sys tls
create_export /files sys tls

# Enable layout types on pNFS exports
info ""
info "Enabling layout types on pNFS exports..."
FFV1_ID=$($PROBE sb-list 2>&1 | awk '$3 == "/ffv1" && $4 != "destroyed" {print $1; exit}') || true
FFV2_ID=$($PROBE sb-list 2>&1 | awk '$3 == "/ffv2" && $4 != "destroyed" {print $1; exit}') || true
FILES_ID=$($PROBE sb-list 2>&1 | awk '$3 == "/files" && $4 != "destroyed" {print $1; exit}') || true

if [ -n "$FFV1_ID" ]; then
	$PROBE sb-set-layout-types --id "$FFV1_ID" --layout-types ffv1 || \
		info "  WARN: set-layout-types failed for /ffv1"
fi
if [ -n "$FFV2_ID" ]; then
	$PROBE sb-set-layout-types --id "$FFV2_ID" --layout-types ffv2 || \
		info "  WARN: set-layout-types failed for /ffv2"
fi
if [ -n "$FILES_ID" ]; then
	$PROBE sb-set-layout-types --id "$FILES_ID" --layout-types file || \
		info "  WARN: set-layout-types failed for /files"
fi

# Bind dstores to pNFS exports
info "Binding dstores to pNFS exports..."
if [ -n "$FFV1_ID" ]; then
	$PROBE sb-set-dstores --id "$FFV1_ID" --dstores 1 || \
		info "  WARN: set-dstores failed for /ffv1"
fi
if [ -n "$FFV2_ID" ]; then
	$PROBE sb-set-dstores --id "$FFV2_ID" --dstores 1 || \
		info "  WARN: set-dstores failed for /ffv2"
fi
if [ -n "$FILES_ID" ]; then
	$PROBE sb-set-dstores --id "$FILES_ID" --dstores 2 || \
		info "  WARN: set-dstores failed for /files (dstore 2 may not be configured)"
fi

# -----------------------------------------------------------------------
# Verify
# -----------------------------------------------------------------------
info ""
info "=== Export listing ==="
$PROBE sb-list

info ""
info "=== Flavor lint ==="
$PROBE sb-lint-flavors

info ""
info "=== Setup complete ==="
info "Mount examples:"
info "  mount -o vers=4.2,sec=sys  $HOST:/sys  /mnt/sys"
info "  mount -o vers=4.2,sec=krb5 $HOST:/krb5 /mnt/krb5"
info "  mount -o vers=3            $HOST:/ffv1 /mnt/ffv1"
