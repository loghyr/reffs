#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# install_deps_fedora43.sh -- Install all build / test dependencies
# for reffs on Fedora 43.
#
# Lifted verbatim from the Fedora Dockerfile (the source of truth for
# Fedora deps per CLAUDE.md "New library dependencies").  If you add
# a PKG_CHECK_MODULES to configure.ac you MUST add the corresponding
# `-devel` package here AND to Dockerfile.
#
# Usage:
#   scripts/install_deps_fedora43.sh
#
# Run as a normal user with sudo; the script re-invokes itself under
# sudo for the package-install steps and drops back to the original
# user for the pip / IOR pieces that install into ~/.local and prefix.
#
# QA: other distros (RHEL/Rocky, Ubuntu, Arch, ...) need their own
# variants.  Dockerfile.ci has the Ubuntu equivalent if you need to
# port this.

set -euo pipefail

# ---------------------------------------------------------------
# 0.  Sanity
# ---------------------------------------------------------------
if [ ! -r /etc/fedora-release ]; then
	echo "warning: /etc/fedora-release not found; this script targets"
	echo "         Fedora 43.  Proceed only if you know what you're doing."
	read -r -p "continue anyway? [y/N] " ans
	[ "${ans,,}" = "y" ] || exit 1
fi

# ---------------------------------------------------------------
# 1.  System packages via dnf
#
# Keep this list 1:1 with Dockerfile's `dnf -y install` block.
# Anything new there must land here too in the same commit.
# ---------------------------------------------------------------
DNF_PACKAGES=(
	# autotools / build
	autoconf
	autoconf-archive
	automake
	libtool
	make
	cmake
	ninja-build
	ccache
	bear

	# compilers + dev shells
	gcc
	gcc-c++
	clang
	clang-tools-extra
	scan-build
	llvm-devel
	gdb
	lldb
	strace
	perf

	# reffs library deps -- matches PKG_CHECK_MODULES in configure.ac
	check-devel             # unit-test harness (libcheck)
	fuse-devel              # optional FUSE backend
	glibc-devel
	HdrHistogram_c-devel    # latency histograms
	jemalloc-devel
	krb5-devel              # GSS / Kerberos
	libev-devel
	libnfsidmap-devel
	libstdc++-devel
	libtirpc-devel          # SunRPC / TIRPC
	liburing-devel          # io_uring
	libuuid-devel
	openssl-devel           # TLS (RFC 9289), CRC, key utils
	rocksdb-devel           # optional RocksDB backend
	userspace-rcu-devel     # liburcu (RCU, lock-free hash tables)
	xxhash-devel
	zlib-devel

	# runtime tools used by the test harnesses + integration scripts
	bash-completion
	git
	htop
	iostat
	iproute
	iputils
	krb5-workstation
	ktls-utils
	net-tools
	nfs-utils
	nslookup
	procps-ng
	rpcbind
	rsync
	script
	sslscan
	sudo
	tcpdump
	vim
	wireshark-cli

	# IOR build (HPC chunk-collision Track 2) needs OpenMPI
	openmpi
	openmpi-devel

	# Python tooling for probe / xdr-parser / draft generation
	python3-argcomplete
	python3-devel
	python3-pip
)

echo "==> installing $(echo "${DNF_PACKAGES[@]}" | wc -w) Fedora packages..."
sudo dnf -y install "${DNF_PACKAGES[@]}"

# ---------------------------------------------------------------
# 2.  Python deps (xdr-parser + xml2rfc for the draft)
#
# reply-xdr ships from git; xdrlib3 + xml2rfc are PyPI.  Use
# --user so we don't fight the system Python prefix.  If you'd
# rather use a venv, set REFFS_PIP_DEST=venv before running.
# ---------------------------------------------------------------
PIP_FLAGS=(--no-cache-dir)
case "${REFFS_PIP_DEST:-user}" in
	user)  PIP_FLAGS+=(--user) ;;
	venv)  : ;;   # caller already activated a venv
	system)
		# Fedora's PEP 668 mark blocks this without --break-system-packages.
		PIP_FLAGS+=(--break-system-packages)
		;;
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
# 3.  IOR 4.0.0 -- not packaged, build from a pinned release tag.
#
# Same notes as Dockerfile: Fedora 43 ships GCC 15 / -std=gnu23,
# IOR 4.0.0 still uses legacy-C idioms.  Pin -std=gnu17 and
# downgrade the new-default errors.
#
# Skip if `ior` is already on PATH (idempotent re-runs).
# ---------------------------------------------------------------
if command -v ior >/dev/null 2>&1; then
	echo "==> IOR already installed at $(command -v ior); skipping."
else
	# OpenMPI on Fedora puts mpicc / mpirun under /usr/lib64/openmpi/bin
	# (not on the default PATH).  Add it for this shell so the build
	# can find them; remind the operator to do the same when running
	# the IOR-based harness.
	export PATH="/usr/lib64/openmpi/bin:${PATH}"
	export LD_LIBRARY_PATH="/usr/lib64/openmpi/lib"

	IOR_TMP="$(mktemp -d)"
	trap 'rm -rf "$IOR_TMP"' EXIT

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
# 4.  Test users for NFSv4 identity tests
#
# Matches Dockerfile.ci so the identity tests have a stable uid/gid
# to map against.  Skip silently if the user already exists.
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
	printf '[General]\nDomain = reffs.test\n' | sudo tee /etc/idmapd.conf >/dev/null
fi

# ---------------------------------------------------------------
# 5.  Done.  Print a quick-start.
# ---------------------------------------------------------------
cat <<'EOF'

==================================================================
reffs build deps installed.  Quick-start:

    mkdir -p m4 && autoreconf -fi
    mkdir build && cd build
    ../configure --enable-asan --enable-ubsan
    make -j$(nproc)
    make -f Makefile.reffs check          # unit tests
    make -f Makefile.reffs ci-check       # full CI (Docker)

CI gate:
    make -f Makefile.reffs ci-check       # ASAN/LSAN clean required

See CLAUDE.md + .claude/standards.md for the full developer rules.
==================================================================
EOF
