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
for i in $(seq 1 45); do
	if $PROBE sb-list >/dev/null 2>&1; then
		probe_ok=true
		break
	fi
	sleep 2
done
if [ "$probe_ok" != "true" ]; then
	die "Cannot reach probe server after 90s"
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
	output=$($PROBE sb-create --path "$path" --storage posix 2>&1) || true
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
	echo "$id"
}

set_client_rules() {
	local id=$1
	shift
	# Each remaining argument is a --rule token
	$PROBE sb-set-client-rules --id "$id" "$@" || \
		info "  WARN: set-client-rules failed for id=$id"
}

# Per-flavor exports
SYS_ID=$(create_export /sys sys)
KRB5_ID=$(create_export /krb5 krb5)
KRB5I_ID=$(create_export /krb5i krb5i)
KRB5P_ID=$(create_export /krb5p krb5p)
TLS_ID=$(create_export /tls tls)

# pNFS exports (need sys + tls for layout I/O)
FFV1_ID_NEW=$(create_export /ffv1 sys tls)
FFV2_ID_NEW=$(create_export /ffv2 sys tls)
FILES_ID_NEW=$(create_export /files sys tls)
# /striped: FFv1 striped layout bound to DSes 1 and 2 (128 KB stripe unit).
# To add more DSes, append them to the sb-set-dstores call below and
# rerun this script.
STRIPED_ID_NEW=$(create_export /striped sys tls)

# Set per-client rules: single wildcard rule matching all clients.
# root_squash=false so MDS control-plane (uid=0) can operate.
info ""
info "Setting per-client export rules..."
if [ -n "$SYS_ID" ]; then
	set_client_rules "$SYS_ID" \
		--rule "match=*,access=rw,root_squash=false,flavors=sys"
fi
if [ -n "$KRB5_ID" ]; then
	set_client_rules "$KRB5_ID" \
		--rule "match=*,access=rw,root_squash=false,flavors=krb5"
fi
if [ -n "$KRB5I_ID" ]; then
	set_client_rules "$KRB5I_ID" \
		--rule "match=*,access=rw,root_squash=false,flavors=krb5i"
fi
if [ -n "$KRB5P_ID" ]; then
	set_client_rules "$KRB5P_ID" \
		--rule "match=*,access=rw,root_squash=false,flavors=krb5p"
fi
if [ -n "$TLS_ID" ]; then
	set_client_rules "$TLS_ID" \
		--rule "match=*,access=rw,root_squash=false,flavors=tls"
fi
# pNFS exports: MDS needs root access (root_squash=false)
if [ -n "$FFV1_ID_NEW" ]; then
	set_client_rules "$FFV1_ID_NEW" \
		--rule "match=*,access=rw,root_squash=false,flavors=sys:tls"
fi
if [ -n "$FFV2_ID_NEW" ]; then
	set_client_rules "$FFV2_ID_NEW" \
		--rule "match=*,access=rw,root_squash=false,flavors=sys:tls"
fi
if [ -n "$FILES_ID_NEW" ]; then
	set_client_rules "$FILES_ID_NEW" \
		--rule "match=*,access=rw,root_squash=false,flavors=sys:tls"
fi
if [ -n "$STRIPED_ID_NEW" ]; then
	set_client_rules "$STRIPED_ID_NEW" \
		--rule "match=*,access=rw,root_squash=false,flavors=sys:tls"
fi

# Enable layout types on pNFS exports
info ""
info "Enabling layout types on pNFS exports..."
FFV1_ID=$($PROBE sb-list 2>&1 | awk '$3 == "/ffv1" && $4 != "destroyed" {print $1; exit}') || true
FFV2_ID=$($PROBE sb-list 2>&1 | awk '$3 == "/ffv2" && $4 != "destroyed" {print $1; exit}') || true
FILES_ID=$($PROBE sb-list 2>&1 | awk '$3 == "/files" && $4 != "destroyed" {print $1; exit}') || true
STRIPED_ID=$($PROBE sb-list 2>&1 | awk '$3 == "/striped" && $4 != "destroyed" {print $1; exit}') || true

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
if [ -n "$STRIPED_ID" ]; then
	$PROBE sb-set-layout-types --id "$STRIPED_ID" --layout-types ffv1 || \
		info "  WARN: set-layout-types failed for /striped"
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
# Striped export: bind all available DSes (dstores 1 and 2).
# Add more dstore IDs here as DSes are configured.
if [ -n "$STRIPED_ID" ]; then
	$PROBE sb-set-dstores --id "$STRIPED_ID" --dstores 1 2 || \
		info "  WARN: set-dstores failed for /striped (dstores 1 and 2 must be configured)"
fi

# Set stripe unit on the striped export (128 KB per stripe)
info "Configuring stripe unit on /striped..."
if [ -n "$STRIPED_ID" ]; then
	$PROBE sb-set-stripe-unit --id "$STRIPED_ID" --stripe-unit 131072 || \
		info "  WARN: set-stripe-unit failed for /striped"
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
info "  mount -o vers=3            $HOST:/striped /mnt/striped"
