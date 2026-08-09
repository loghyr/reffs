#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# install_deps_rocky95.sh -- Install all build / test dependencies
# for reffs on Rocky Linux 9.5 (EL9).
#
# Counterpart to install_deps_fedora43.sh.  Fedora is the source of
# truth for the dependency list (Dockerfile); this script maps that
# list onto EL9's narrower base + EPEL + CRB universe and builds
# from source the two packages that aren't packaged anywhere:
# HdrHistogram_c-devel and (optionally) rocksdb.
#
# Usage:
#   scripts/install_deps_rocky95.sh
#
# Knobs:
#   INSTALL_ROCKSDB=1       build + install rocksdb from source
#                           (off by default -- 20-30 minute compile;
#                           reffs HAVE_ROCKSDB-gates the backend so
#                           the build is fine without it)
#   REFFS_PIP_DEST=user|venv|system
#                           where pip installs reply-xdr et al
#                           (default: user)
#
# QA: this targets Rocky 9.5 specifically.  Should work on AlmaLinux
# 9.5 and RHEL 9.5 unchanged (same package universe).  Older 9.x
# minor releases may be missing ktls-utils (added late in the 9.y
# cycle) -- if dnf complains, drop it and reffs will still build,
# just without ktls-utils-driven TLS handshake tests.

set -euo pipefail

# ---------------------------------------------------------------
# 0.  Sanity
# ---------------------------------------------------------------
if [ -r /etc/rocky-release ]; then
	REL="$(cat /etc/rocky-release)"
elif [ -r /etc/redhat-release ]; then
	REL="$(cat /etc/redhat-release)"
else
	REL=""
fi
case "$REL" in
	*"release 9."*) ;;
	"")
		echo "warning: no /etc/rocky-release or /etc/redhat-release;"
		echo "         this script targets Rocky / Alma / RHEL 9.5."
		read -r -p "continue anyway? [y/N] " ans
		[ "${ans,,}" = "y" ] || exit 1
		;;
	*)
		echo "warning: detected '$REL', not 9.x.  This script targets"
		echo "         Rocky / Alma / RHEL 9.5.  Other majors will"
		echo "         likely need a different package map."
		read -r -p "continue anyway? [y/N] " ans
		[ "${ans,,}" = "y" ] || exit 1
		;;
esac

# ---------------------------------------------------------------
# 1.  Enable EPEL + CRB
#
# EL9 base is intentionally small.  Most of the -devel packages
# we need live in CRB (the renamed PowerTools / CodeReady Builder
# channel), and another tranche lives in EPEL.
# ---------------------------------------------------------------
echo "==> enabling CRB + EPEL..."
sudo dnf -y install epel-release dnf-plugins-core
sudo dnf config-manager --set-enabled crb

# ---------------------------------------------------------------
# 2.  System packages via dnf
#
# Annotations show which repo each package comes from:
#   (base) BaseOS / AppStream
#   (crb)  CodeReady Builder
#   (epel) EPEL
# When dnf-installing, none of this matters -- listed only so the
# next person who adds a dep knows where to look if it fails.
# ---------------------------------------------------------------
DNF_PACKAGES=(
	# autotools / build
	autoconf                  # (base)
	autoconf-archive          # (epel)
	automake                  # (base)
	libtool                   # (base)
	make                      # (base)
	cmake                     # (base)
	ninja-build               # (crb)
	ccache                    # (epel)
	bear                      # (epel)

	# compilers + dev shells
	gcc                       # (base)
	gcc-c++                   # (base)
	clang                     # (base)
	clang-tools-extra         # (base) -- includes scan-build
	llvm-devel                # (crb)
	gdb                       # (base)
	lldb                      # (base)
	strace                    # (base)
	perf                      # (base)

	# reffs library deps -- matches PKG_CHECK_MODULES in configure.ac
	check-devel               # (crb) unit-test harness (libcheck)
	fuse-devel                # (crb) optional FUSE backend
	glibc-devel               # (base)
	jemalloc-devel            # (epel)
	krb5-devel                # (base)  GSS / Kerberos
	libev-devel               # (epel)
	libnfsidmap-devel         # (crb)
	libstdc++-devel           # (base)
	libtirpc-devel            # (base)  SunRPC / TIRPC
	liburing-devel            # (base)  io_uring
	libuuid-devel             # (base)
	openssl-devel             # (base)  TLS (RFC 9289), CRC, key utils
	userspace-rcu-devel       # (crb)   liburcu (RCU, lfht)
	xxhash-devel              # (epel)
	zlib-devel                # (base)

	# runtime tools used by the test harnesses + integration scripts
	bash-completion           # (base)
	bind-utils                # (base)  provides nslookup
	git                       # (base)
	htop                      # (epel)
	iproute                   # (base)
	iputils                   # (base)
	krb5-workstation          # (base)
	ktls-utils                # (base, late-9.x; drop if missing)
	net-tools                 # (base)
	nfs-utils                 # (base)
	procps-ng                 # (base)
	rpcbind                   # (base)
	rsync                     # (base)
	sslscan                   # (epel)
	sudo                      # (base)
	sysstat                   # (base)  provides iostat
	tcpdump                   # (base)
	util-linux                # (base)  provides script(1)
	vim-enhanced              # (base)
	wireshark-cli             # (crb)

	# IOR build (HPC chunk-collision Track 2) needs OpenMPI
	openmpi                   # (base)
	openmpi-devel             # (base)

	# Python tooling for probe / xdr-parser / draft generation
	python3-argcomplete       # (base/epel)
	python3-devel             # (base)
	python3-pip               # (base)
)

# ---------------------------------------------------------------
# Packages from Fedora's Dockerfile NOT installed by dnf here,
# with the reason and what we do instead:
#
#   HdrHistogram_c-devel  -- not packaged on EL9; built from
#                            source in step 4 below.
#   rocksdb-devel         -- not packaged on EL9; optional,
#                            built from source in step 5 when
#                            INSTALL_ROCKSDB=1.
#   vim                   -- replaced with vim-enhanced (EL
#                            convention).
#   nslookup              -- replaced with bind-utils.
#   iostat                -- replaced with sysstat.
#   script(1)             -- comes from util-linux.
# ---------------------------------------------------------------

echo "==> installing $(echo "${DNF_PACKAGES[@]}" | wc -w) Rocky packages..."
# Don't pass --skip-broken: we want a hard fail if a package goes
# missing on a future minor.  Document each removal explicitly.
sudo dnf -y install "${DNF_PACKAGES[@]}"

# ---------------------------------------------------------------
# 3.  Python deps (xdr-parser + xml2rfc for the draft)
# ---------------------------------------------------------------
PIP_FLAGS=(--no-cache-dir)
case "${REFFS_PIP_DEST:-user}" in
	user)    PIP_FLAGS+=(--user) ;;
	venv)    : ;;
	system)  PIP_FLAGS+=(--break-system-packages) ;;
	*)
		echo "REFFS_PIP_DEST must be one of: user, venv, system" >&2
		exit 1
		;;
esac

echo "==> installing Python packages (reply-xdr, xdrlib3, xml2rfc)..."
pip3 install "${PIP_FLAGS[@]}" \
	'reply-xdr@git+https://github.com/loghyr/reply.git' \
	xdrlib3 \
	xml2rfc

# ---------------------------------------------------------------
# 4.  HdrHistogram_c -- not packaged on EL9; build from a pinned
#     release tag.  Small (~1 minute) and required.
# ---------------------------------------------------------------
if pkg-config --exists HdrHistogram_c 2>/dev/null; then
	echo "==> HdrHistogram_c already installed; skipping."
else
	HDR_TMP="$(mktemp -d)"
	trap 'rm -rf "$HDR_TMP" "${IOR_TMP:-}" "${RDB_TMP:-}"' EXIT

	echo "==> building HdrHistogram_c 0.11.8 from source (in $HDR_TMP)..."
	git clone --depth 1 --branch 0.11.8 \
		https://github.com/HdrHistogram/HdrHistogram_c \
		"$HDR_TMP/HdrHistogram_c"
	(
		cd "$HDR_TMP/HdrHistogram_c"
		mkdir build && cd build
		cmake -DCMAKE_INSTALL_PREFIX=/usr/local \
		      -DCMAKE_INSTALL_LIBDIR=lib64 \
		      -DHDR_HISTOGRAM_BUILD_PROGRAMS=OFF \
		      -DHDR_HISTOGRAM_BUILD_SHARED=ON ..
		make -j"$(nproc)"
		sudo make install
		sudo ldconfig
	)
fi

# ---------------------------------------------------------------
# 5.  rocksdb (optional) -- 20-30 minute compile, gated by
#     INSTALL_ROCKSDB=1.  reffs HAVE_ROCKSDB-detects the lib at
#     configure time and skips the RocksDB backend if absent.
# ---------------------------------------------------------------
if [ "${INSTALL_ROCKSDB:-0}" = "1" ]; then
	if pkg-config --exists rocksdb 2>/dev/null; then
		echo "==> rocksdb already installed; skipping."
	else
		# RocksDB build deps that aren't already pulled in.
		sudo dnf -y install \
			snappy-devel lz4-devel zstd \
			libzstd-devel bzip2-devel gflags-devel

		RDB_TMP="$(mktemp -d)"
		echo "==> building rocksdb 9.6.1 from source (in $RDB_TMP);" \
		     "this takes 20-30 minutes..."
		git clone --depth 1 --branch v9.6.1 \
			https://github.com/facebook/rocksdb \
			"$RDB_TMP/rocksdb"
		(
			cd "$RDB_TMP/rocksdb"
			mkdir build && cd build
			cmake -DCMAKE_BUILD_TYPE=Release \
			      -DCMAKE_INSTALL_PREFIX=/usr/local \
			      -DCMAKE_INSTALL_LIBDIR=lib64 \
			      -DWITH_SNAPPY=ON -DWITH_LZ4=ON \
			      -DWITH_ZSTD=ON -DWITH_ZLIB=ON \
			      -DWITH_BZ2=ON -DWITH_GFLAGS=ON \
			      -DROCKSDB_BUILD_SHARED=ON \
			      -DWITH_TESTS=OFF -DWITH_BENCHMARK_TOOLS=OFF \
			      -DWITH_TOOLS=OFF ..
			make -j"$(nproc)" rocksdb rocksdb-shared
			sudo make install
			sudo ldconfig
		)
	fi
else
	echo "==> skipping rocksdb (set INSTALL_ROCKSDB=1 to build it)."
fi

# ---------------------------------------------------------------
# 6.  IOR 4.0.0 -- not packaged.  Same source build as Fedora.
#     EL9 ships GCC 11, so the -std=gnu23 / implicit-function-decl
#     guards are belt-and-suspenders here, not strictly required.
# ---------------------------------------------------------------
if command -v ior >/dev/null 2>&1; then
	echo "==> IOR already installed at $(command -v ior); skipping."
else
	export PATH="/usr/lib64/openmpi/bin:${PATH}"
	export LD_LIBRARY_PATH="/usr/lib64/openmpi/lib"

	IOR_TMP="$(mktemp -d)"

	echo "==> building IOR 4.0.0 from source (in $IOR_TMP)..."
	git clone --depth 1 --branch 4.0.0 \
		https://github.com/hpc/ior "$IOR_TMP/ior"
	(
		cd "$IOR_TMP/ior"
		./bootstrap
		./configure MPICC=mpicc \
			CFLAGS="-O2 -std=gnu17 \
			        -Wno-error=implicit-function-declaration \
			        -Wno-error=implicit-int \
			        -Wno-error=incompatible-pointer-types"
		make -j"$(nproc)"
		sudo make install
	)
	echo "==> IOR installed.  Add the following to your shell rc"
	echo "    so mpicc/mpirun stay on PATH for the chunk-collision"
	echo "    Track 2 harness:"
	echo
	echo "      export PATH=\"/usr/lib64/openmpi/bin:\$PATH\""
	echo "      export LD_LIBRARY_PATH=\"/usr/lib64/openmpi/lib\""
fi

# ---------------------------------------------------------------
# 7.  Test users for NFSv4 identity tests (parity with Dockerfile)
# ---------------------------------------------------------------
if ! getent group nfsgroup >/dev/null; then
	echo "==> creating nfsgroup (gid 3300) for identity tests..."
	sudo groupadd -g 3300 nfsgroup
fi
if ! id -u nfstest >/dev/null 2>&1; then
	echo "==> creating nfstest user (uid 3300) for identity tests..."
	sudo useradd -u 3300 -g 3300 -M -s /usr/sbin/nologin nfstest
fi
if [ ! -f /etc/idmapd.conf ]; then
	echo "==> seeding /etc/idmapd.conf for the test domain..."
	printf '[General]\nDomain = reffs.test\n' \
		| sudo tee /etc/idmapd.conf >/dev/null
fi

# ---------------------------------------------------------------
# 8.  /usr/local/lib64 on the linker path
#
# HdrHistogram_c (and optionally rocksdb) install into
# /usr/local/lib64, which RHEL-family systems don't include on
# the default linker path.  Drop a .conf so future programs link
# without LD_LIBRARY_PATH gymnastics.
# ---------------------------------------------------------------
if [ ! -f /etc/ld.so.conf.d/reffs-local.conf ]; then
	echo "==> adding /usr/local/lib64 to ld.so.conf.d..."
	printf '/usr/local/lib64\n' \
		| sudo tee /etc/ld.so.conf.d/reffs-local.conf >/dev/null
	sudo ldconfig
fi

# ---------------------------------------------------------------
# 9.  Sanity-check: AX_PTHREAD is reachable to aclocal
#
# configure.ac calls AX_PTHREAD (from autoconf-archive).  If that
# macro isn't on the m4 search path when `autoreconf -fi` runs,
# the macro lands in configure as a literal shell token and
# configure fails with:
#     syntax error near unexpected token 'PTHREAD_CFLAGS=-pthread'
# Catch that here instead of finding out 3000 lines into a
# configure run.
# ---------------------------------------------------------------
if [ ! -f /usr/share/aclocal/ax_pthread.m4 ]; then
	echo "ERROR: /usr/share/aclocal/ax_pthread.m4 is missing." >&2
	echo "       autoconf-archive did not install correctly." >&2
	echo "       Re-run: sudo dnf -y install autoconf-archive" >&2
	exit 1
fi

# ---------------------------------------------------------------
# 10.  Done.  Print a quick-start.
# ---------------------------------------------------------------
cat <<'EOF'

==================================================================
reffs build deps installed on Rocky 9.5.

IMPORTANT: bootstrap from the source root, NOT the build/ dir.
If your tree already has a stale configure (generated on a host
without autoconf-archive), wipe it first or you'll hit:
    syntax error near unexpected token 'PTHREAD_CFLAGS=-pthread'

Quick-start (from the reffs source root):

    # Wipe stale autotools state (only needed once, after fresh
    # clone or if you've ever hit the AX_PTHREAD error)
    rm -rf m4 autom4te.cache aclocal.m4 configure
    find . -name 'Makefile.in' -delete

    # Regenerate configure with the archive macros visible
    mkdir -p m4 && autoreconf -fi

    # Out-of-tree build
    mkdir build && cd build
    ../configure --enable-asan --enable-ubsan
    make -j$(nproc)
    make -f Makefile.reffs check          # unit tests
    make -f Makefile.reffs ci-check       # full CI (Docker)

If you opted out of rocksdb (the default), the RocksDB backend is
disabled at configure time.  Re-run this script with
INSTALL_ROCKSDB=1 to enable it.

See CLAUDE.md + .claude/standards.md for the full developer rules.
==================================================================
EOF
