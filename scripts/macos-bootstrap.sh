#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# One-time bootstrap of reffs's non-Homebrew dependencies on macOS.
# Builds liburcu and HdrHistogram_c from source, installs pip
# packages needed by configure.  Run once per machine.
#
# Prerequisites (run separately, this script does NOT install them
# because the user may want to pick specific versions):
#
#   brew install autoconf automake libtool pkg-config \
#       openssl@3 xxhash zstd rocksdb python@3.12
#
# Then run this script.  Then cd into the reffs source tree and:
#
#   export PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig:$PKG_CONFIG_PATH"
#   export PATH="$HOME/.local/bin:$PATH"
#   autoreconf -i && ./configure && make -j4
#
# Tested on macOS 14 Sonoma arm64.

set -euo pipefail

HOMEBREW_PREFIX="$(brew --prefix)"
BUILD_DIR="${TMPDIR:-/tmp}/reffs-macos-bootstrap"
mkdir -p "$BUILD_DIR"

echo "==> Bootstrap prefix: $HOMEBREW_PREFIX"
echo "==> Build scratch:    $BUILD_DIR"
echo

# ---------------------------------------------------------------------
# liburcu (Userspace RCU) -- not in Homebrew core, build from source.
# Installs liburcu, liburcu-cds, pkgconfig files into HOMEBREW_PREFIX.
# ---------------------------------------------------------------------
if pkg-config --exists liburcu 2>/dev/null; then
    echo "==> liburcu already installed ($(pkg-config --modversion liburcu))"
else
    echo "==> Building liburcu from source"
    cd "$BUILD_DIR"
    if [ ! -d userspace-rcu ]; then
        git clone --depth 1 https://github.com/urcu/userspace-rcu.git
    fi
    cd userspace-rcu
    ./bootstrap
    ./configure --prefix="$HOMEBREW_PREFIX"
    make -j4
    sudo make install
    echo "==> liburcu installed"
fi

# ---------------------------------------------------------------------
# HdrHistogram_c -- not in Homebrew, build from source.
# ---------------------------------------------------------------------
if [ -f "$HOMEBREW_PREFIX/include/hdr/hdr_histogram.h" ]; then
    echo "==> HdrHistogram_c already installed"
else
    echo "==> Building HdrHistogram_c from source"
    cd "$BUILD_DIR"
    if [ ! -d HdrHistogram_c ]; then
        git clone --depth 1 https://github.com/HdrHistogram/HdrHistogram_c
    fi
    cd HdrHistogram_c
    mkdir -p build && cd build
    cmake -DCMAKE_INSTALL_PREFIX="$HOMEBREW_PREFIX" \
          -DHDR_HISTOGRAM_BUILD_PROGRAMS=OFF \
          -DHDR_HISTOGRAM_BUILD_SHARED=ON \
          ..
    make -j4
    sudo make install
    echo "==> HdrHistogram_c installed"
fi

# ---------------------------------------------------------------------
# reply-xdr (Python) for the XDR parser invoked by Makefile.
# ---------------------------------------------------------------------
if command -v xdr-parser >/dev/null 2>&1; then
    echo "==> xdr-parser already on PATH"
else
    echo "==> Installing reply-xdr via pip"
    pip3 install --user reply-xdr
    echo "==> xdr-parser installed to \$HOME/.local/bin"
    echo "    Add that directory to PATH before running ./configure:"
    echo "      export PATH=\"\$HOME/.local/bin:\$PATH\""
fi

echo
echo "==> Bootstrap complete."
echo
echo "Next steps:"
echo "  cd /path/to/reffs"
echo "  export PKG_CONFIG_PATH=\"\$(brew --prefix openssl@3)/lib/pkgconfig:\$PKG_CONFIG_PATH\""
echo "  export PATH=\"\$HOME/.local/bin:\$PATH\""
echo "  autoreconf -i"
echo "  ./configure"
echo "  make -j4"
