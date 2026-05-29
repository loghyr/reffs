#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# krb5_ffv1_stress.sh -- N krb5 NFSv4.2 clients against an external
# Flex Files v1 server, via ec_demo.
#
# Distinct from ci_krb5_multiclient.sh: that one stands up a reffsd
# and an embedded KDC for self-contained CI.  This one assumes the
# NFSv4.2 server and the KDC already exist (it provisions nothing),
# and drives the FFv1 layout path -- LAYOUTGET + direct DS I/O via
# ec_demo -- because the intended target does not proxy inbound I/O.
#
# See .claude/design/krb5-ffv1-stress.md for the design rationale
# and docs/krb5-multiclient-testing.md for QA-facing instructions.

set -euo pipefail

server=
path_dir=
clients=
principals=
ec_demo=./build/tools/ec_demo
local_input=
size=$((10 * 1024 * 1024))
# Default to a single mirror with the `mirror` codec (k=1, m=0)
# -- matches the common "one DS per share" Anvil configuration
# this stress targets.  The `mirror` codec (lib/ec/mirror.c) does
# "N replicas, no parity transform"; with k=1 the encoder's copy
# loop runs zero times so it degenerates to a plain write to the
# single DS, which is exactly what a 1-DS share can back.  The
# EC codecs (rs, mojette-sys, mojette-nonsys) require m >= 1 per
# ec_demo's validator -- m=0 is degenerate for erasure coding
# since "no parity shards" means there's nothing to recover from.
# RS 4+2 still works (override with --codec rs --k 4 --m 2) but
# needs the share to back the layout with at least 6 DSes; on a
# 1-DS share ec_demo bails with "need 6 mirrors, got 1" -- EINVAL.
k=1
m=0
codec=mirror
sec=krb5

usage() {
	cat >&2 <<'EOF'
Usage: krb5_ffv1_stress.sh --server <host[:port]>
                           --path <dir-on-server>
                           --clients <N>
                           --principals <file>
                           [--ec-demo <path>]   (default ./build/tools/ec_demo)
                           [--input <file>]    (else generate --size of urandom)
                           [--size <bytes>]    (default 10 MB)
                           [--k <K>] [--m <M>] (default 1+0)
                           [--codec rs|mojette-sys|mojette-nonsys|stripe|mirror]
                                                            (default mirror)
                           [--sec krb5|krb5i|krb5p]   (default krb5)

Drives N krb5-authenticated NFSv4.2 clients at an external FFv1
server.  Each worker writes <path>/krb5stress_<i> with the supplied
input via `ec_demo write --layout v1`, then `ec_demo verify` reads
it back and compares.  Both invocations run against the worker's
per-worker krb5 ccache, exercising the full FFv1 LAYOUTGET + DS I/O
path under one identity.

The script does NOT provision the server, the KDC, or the AD users.
--principals is a file of "<principal> <password>" lines, one per
worker, principals fully qualified (user@REALM); blank lines and
'#' comments are ignored.  The script kinit's each principal into
its own credential cache and points the corresponding worker at it.

The test host must be configured so its sssd/idmap resolves the
realm's users to distinct uids -- otherwise the server side sees
every worker as nobody.  See docs/krb5-multiclient-testing.md.

Exit 0 = every worker PASS; 1 = one or more workers failed (failing
workers' log tails are dumped to stderr and the run directory is
kept for inspection).
EOF
}

die() {
	echo "FATAL: $*" >&2
	exit 1
}

while [ $# -gt 0 ]; do
	case "$1" in
	--server) server=$2; shift 2 ;;
	--path) path_dir=$2; shift 2 ;;
	--clients) clients=$2; shift 2 ;;
	--principals) principals=$2; shift 2 ;;
	--ec-demo) ec_demo=$2; shift 2 ;;
	--input) local_input=$2; shift 2 ;;
	--size) size=$2; shift 2 ;;
	--k) k=$2; shift 2 ;;
	--m) m=$2; shift 2 ;;
	--codec) codec=$2; shift 2 ;;
	--sec) sec=$2; shift 2 ;;
	-h | --help) usage; exit 0 ;;
	*) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
	esac
done

[ -n "$server" ]      || { usage; die "--server is required"; }
[ -n "$path_dir" ]    || { usage; die "--path is required"; }
[ -n "$clients" ]     || { usage; die "--clients is required"; }
[ -n "$principals" ]  || { usage; die "--principals is required"; }
[[ "$clients" =~ ^[0-9]+$ ]] || die "--clients must be a positive integer"
[ "$clients" -ge 1 ]  || die "--clients must be >= 1"
[ -x "$ec_demo" ]     || die "ec_demo not executable: $ec_demo"
[ -r "$principals" ]  || die "principals file not readable: $principals"
command -v kinit >/dev/null 2>&1 || die "kinit not in PATH"

# Default port to 2049 if the caller didn't pass one.  Hammerspace
# Anvil-class MDSes (the intended target for this stress) do not
# register with rpcbind, so ec_demo's portmap-driven clnt_create
# path (lib/nfs4/client/mds_session.c mds_session_clnt_open) fails
# with ECONNREFUSED on port 111 before any NFS-port traffic ever
# leaves the host.  Appending :2049 makes ec_demo take the
# explicit-port branch and connect straight TCP via clnttcp_create.
case "$server" in
	*:*) ;;  # caller already specified host:port
	*)   server="${server}:2049" ;;
esac

# --------------------------------------------------------------------
# Make sure the libtool .libs/ dirs are on LD_LIBRARY_PATH.
#
# When ec_demo runs from an in-tree build, the libtool wrapper at
# build/tools/ec_demo is supposed to set LD_LIBRARY_PATH before
# exec'ing the inner ELF in build/tools/.libs/.  Two ways that
# falls over:
#   - the wrapper got replaced by a `make install` relink (now a
#     stub that just execs without env-fixing), and
#   - the script's `exec "$ec_demo"` for the verify pass inherits
#     whatever LD_LIBRARY_PATH the parent shell had, not the
#     wrapper-set one.
# Cover both by inferring the build root from $ec_demo and prefixing
# every .libs/ dir.  No-op if user already exported LD_LIBRARY_PATH.
build_root=$(cd "$(dirname "$ec_demo")/.." 2>/dev/null && pwd)
if [ -n "$build_root" ] && [ -d "$build_root/lib" ]; then
	extra=$(find "$build_root" -type d -name '.libs' 2>/dev/null | paste -sd:)
	if [ -n "$extra" ]; then
		export LD_LIBRARY_PATH="$extra${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
	fi
fi

# --------------------------------------------------------------------
# Read the principals file.

declare -a princ_names princ_pws
while read -r name pw rest; do
	case "$name" in '' | '#'*) continue ;; esac
	[ -n "$pw" ] || die "malformed principals line (no password): $name"
	princ_names+=("$name")
	princ_pws+=("$pw")
done <"$principals"

[ "${#princ_names[@]}" -ge "$clients" ] ||
	die "principals file has ${#princ_names[@]} identities, need $clients"

# --------------------------------------------------------------------
# Per-run private directory (ccaches + per-worker logs + local input).

run_dir=$(mktemp -d /tmp/krb5_ffv1_stress.XXXXXX)
keep_dir=0
cleanup() {
	if [ "$keep_dir" -eq 0 ]; then
		rm -rf "$run_dir"
	else
		echo "krb5_ffv1_stress: run dir kept: $run_dir" >&2
	fi
}
trap cleanup EXIT

# Local input: --input given, or generate --size bytes from urandom.
if [ -n "$local_input" ]; then
	[ -r "$local_input" ] || die "--input not readable: $local_input"
else
	local_input=$run_dir/input.bin
	# head -c is portable across Linux and macOS.
	head -c "$size" /dev/urandom >"$local_input" ||
		die "could not generate $size bytes of input"
fi

# --------------------------------------------------------------------
# kinit each principal into its own credential cache.

cc_dir=$run_dir/ccaches
mkdir -p "$cc_dir"
for ((i = 0; i < clients; i++)); do
	cc=$cc_dir/cc_$i
	# Password on stdin, not on the command line (so it doesn't show
	# in ps).  printf '%s' avoids a trailing newline.
	if ! printf '%s' "${princ_pws[$i]}" |
		kinit -c "$cc" "${princ_names[$i]}" 2>/dev/null; then
		keep_dir=1
		die "kinit failed for ${princ_names[$i]} (cc=$cc)"
	fi
done

# --------------------------------------------------------------------
# Fork N workers.

log_dir=$run_dir/logs
mkdir -p "$log_dir"
path_dir=${path_dir%/}  # strip trailing slash, if any

declare -a pids
# ec_demo's `verify` subcommand only reads and compares -- it does
# not write first.  So each worker WRITES the file, then VERIFIES
# the readback against the same local input.  Both invocations run
# against the per-worker krb5 ccache so the round-trip exercises
# write + read against the FFv1 server under one identity.
for ((i = 0; i < clients; i++)); do
	(
		export KRB5CCNAME=$cc_dir/cc_$i
		set -e
		"$ec_demo" write \
			--mds "$server" \
			--file "$path_dir/krb5stress_$i" \
			--input "$local_input" \
			--sec "$sec" \
			--layout v1 \
			--codec "$codec" \
			--k "$k" \
			--m "$m" \
			--id "krb5stress_$i"
		exec "$ec_demo" verify \
			--mds "$server" \
			--file "$path_dir/krb5stress_$i" \
			--input "$local_input" \
			--sec "$sec" \
			--layout v1 \
			--codec "$codec" \
			--k "$k" \
			--m "$m" \
			--id "krb5stress_$i"
	) >"$log_dir/worker_$i.log" 2>&1 &
	pids+=("$!")
done

# --------------------------------------------------------------------
# Collect.

passed=0
failed=0
failed_idxs=()
for ((i = 0; i < clients; i++)); do
	if wait "${pids[$i]}"; then
		passed=$((passed + 1))
	else
		failed=$((failed + 1))
		failed_idxs+=("$i")
	fi
done

if [ "$failed" -eq 0 ]; then
	echo "PASS $passed/$clients krb5 FFv1 clients (server=$server)"
	exit 0
fi

# Failure: keep the run dir, dump each failing worker's log tail.
keep_dir=1
echo "FAIL $passed/$clients krb5 FFv1 clients ($failed failed)" >&2
for idx in "${failed_idxs[@]}"; do
	echo "----- worker $idx (${princ_names[$idx]}) -----" >&2
	tail -n 40 "$log_dir/worker_$idx.log" >&2
done
exit 1
