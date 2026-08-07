#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# test_encodings_local.sh -- happy-path matrix over every encoding in
# the FFv2 registry.
#
# For each of the seven registered encodings, stands up a combined-mode
# reffsd with k+m local dstores, writes a file with ec_demo, and reads
# it back with byte-compare verify.  Asserts both exit 0.
#
# Local dstores (a [[data_server]] with no `port`) select
# dstore_ops_local, which is mounted immediately and always tight
# coupled.  That is deliberate: it means every encoding is exercised
# with the trust table consulted and attribute 90 settled, not just
# the data path.
#
# This matrix earned its place: on 2026-08-07 it caught a heap
# overrun in the linux-md-raid path (a parity array sized from the
# caller's m while the encoder writes its own fixed m=2).
#
# Usage: test_encodings_local.sh [REFFSD_BIN]
#   REFFSD    Path to reffsd (default: <repo>/build/src/reffsd).
#   SIZE      Input bytes (default: 262144).
#   ONLY      Space-separated ec_demo encoding names to run.

set -u

REFFSD=${1:-${REFFSD:-$PWD/build/src/reffsd}}
BUILD=$(dirname "$(dirname "$REFFSD")")
EC=${EC_DEMO:-$BUILD/tools/ec_demo}
NFS_PORT=${NFS_PORT:-12049}
SIZE=${SIZE:-262144}
ONLY=${ONLY:-}

die() { echo "FATAL: $*" >&2; exit 1; }

[ -x "$REFFSD" ] || die "reffsd not executable: $REFFSD"
[ -x "$EC" ] || die "ec_demo not executable: $EC"

# ec_demo name : reffs default_coding : k : m
#
# Geometries are pinned, not free parameters.  linux-md-raid is
# md/raid6 and its m is fixed at 2 by the draft and by
# ec_linux_md_create(); xor-parity produces exactly one parity shard;
# passthrough and mirrored carry none.
#
# All seven run on v2 layouts.  PASSTHROUGH is an FFv2 encoding value
# that happens to use plain READ/WRITE rather than CHUNK operations,
# so it belongs here, on a v2 layout, not on a v1 one.
CASES="
stripe:passthrough:4:0
mojette-sys:mojette-sys:4:2
mojette-nonsys:mojette-nonsys:4:2
rs:rs:4:2
mirror:mirrored:3:0
xor:xor-parity:4:1
linux-md:linux-md-raid:4:2
"

# Make libtool's in-tree .libs/ visible to the inner ELF; see
# krb5_ffv1_stress.sh for the full rationale.
_extra=$(find "$BUILD" -type d -name '.libs' 2>/dev/null | paste -sd:)
[ -n "$_extra" ] && export LD_LIBRARY_PATH="$_extra${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

failures=0
ran=0

printf '%-16s %-18s %-7s %-9s %s\n' ENCODING CODING GEOM WRITE VERIFY
printf '%-16s %-18s %-7s %-9s %s\n' -------- ------ ---- ----- ------

for c in $CASES; do
	enc=${c%%:*}; rest=${c#*:}
	coding=${rest%%:*}; rest=${rest#*:}
	k=${rest%%:*}; m=${rest##*:}
	nds=$((k + m))
	[ "$nds" -lt 2 ] && nds=2

	if [ -n "$ONLY" ]; then
		case " $ONLY " in
		*" $enc "*) ;;
		*) continue ;;
		esac
	fi
	ran=$((ran + 1))

	run=$(mktemp -d /tmp/test_enc.XXXXXX)
	mkdir -p "$run/data" "$run/state" "$run/ds"

	{
		cat <<-EOF
		[server]
		port            = $NFS_PORT
		bind            = "*"
		role            = "combined"
		minor_versions  = [1, 2]
		workers         = 4
		log_level       = "info"

		[backend]
		type            = "posix"
		path            = "$run/data"
		state_file      = "$run/state"
		ds_path         = "$run/ds"

		[[export]]
		path           = "/"
		layout_types   = ["ffv2"]
		dstores        = [$(seq -s, 1 $nds)]
		default_coding = "$coding:${k}+${m}"

		    [[export.clients]]
		    match       = "*"
		    access      = "rw"
		    root_squash = false
		    flavors     = ["sys"]

		EOF
		for i in $(seq 1 $nds); do
			printf '[[data_server]]\nid      = %d\naddress = "127.0.0.1"\npath    = "/"\n\n' "$i"
		done
	} > "$run/reffsd.toml"

	ASAN_OPTIONS=detect_leaks=0:halt_on_error=0 \
	UBSAN_OPTIONS=halt_on_error=0 \
		"$REFFSD" -C "$run/reffsd.toml" -f "$run/reffsd.trc" \
		> "$run/reffsd.log" 2>&1 &
	pid=$!

	ready=no
	for _ in $(seq 1 60); do
		grep -q "reffsd ready" "$run/reffsd.log" 2>/dev/null && { ready=yes; break; }
		sleep 1
	done
	# Local dstores mount immediately, but their runway is built by
	# the renewal thread on a later tick -- wait rather than race.
	for _ in $(seq 1 60); do
		[ "$(grep -c 'runway built' "$run/reffsd.log" 2>/dev/null)" -ge 1 ] && break
		sleep 1
	done

	if [ "$ready" != yes ]; then
		printf '%-16s %-18s %-7s %-9s %s\n' "$enc" "$coding" "${k}+${m}" NOSTART -
		echo "    reffsd did not start; logs: $run" >&2
		failures=$((failures + 1))
		kill $pid 2>/dev/null; wait $pid 2>/dev/null; sleep 2
		continue
	fi

	head -c "$SIZE" /dev/urandom > "$run/in.bin"

	"$EC" write --mds "127.0.0.1:$NFS_PORT" --file "/f_$enc" \
		--input "$run/in.bin" --encoding "$enc" \
		--k "$k" --m "$m" --layout v2 --id "enc_$enc" \
		> "$run/write.log" 2>&1
	wrc=$?

	"$EC" verify --mds "127.0.0.1:$NFS_PORT" --file "/f_$enc" \
		--input "$run/in.bin" --encoding "$enc" \
		--k "$k" --m "$m" --layout v2 --id "enc_$enc" \
		> "$run/verify.log" 2>&1
	vrc=$?

	w=$([ $wrc -eq 0 ] && echo OK || echo "FAIL($wrc)")
	v=$([ $vrc -eq 0 ] && echo OK || echo "FAIL($vrc)")
	printf '%-16s %-18s %-7s %-9s %s\n' "$enc" "$coding" "${k}+${m}" "$w" "$v"

	kill $pid 2>/dev/null; wait $pid 2>/dev/null; sleep 2

	if [ $wrc -ne 0 ] || [ $vrc -ne 0 ]; then
		failures=$((failures + 1))
		echo "    logs kept: $run" >&2
	else
		rm -rf "$run"
	fi
done

echo
if [ "$failures" -eq 0 ]; then
	echo "PASS: $ran/$ran encodings wrote and verified"
	exit 0
fi
echo "FAIL: $failures of $ran encodings failed" >&2
exit 1
