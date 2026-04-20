#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# One-time bootstrap of reffs's non-Homebrew dependencies on macOS.
# Builds liburcu and HdrHistogram_c from source, installs the
# xdr-parser Python CLI via pipx.  Run once per machine.
#
# Prerequisites (run separately, this script does NOT install them
# because the user may want to pick specific versions):
#
#   brew install autoconf autoconf-archive automake libtool \
#       pkg-config cmake pipx openssl@3 xxhash zstd rocksdb \
#       check python@3.12
#
# Why pipx rather than pip?  macOS Python is PEP-668 externally-
# managed, so `pip install --user reply-xdr` errors out.  pipx
# installs the CLI into its own managed venv and symlinks the
# binary into ~/.local/bin.
#
# Why clone reply locally rather than `pipx install reply-xdr`?
# The PyPI index entry is stale / mirror-dependent and often fails
# dependency resolution on macOS.  Cloning the upstream repo and
# running `pipx install .` is reliable.
#
# After this script succeeds, cd into the reffs source tree and:
#
#   mkdir -p m4                  # avoids the aclocal-m4-missing trap
#   export PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig:$PKG_CONFIG_PATH"
#   export PATH="$HOME/.local/bin:$PATH"
#   autoreconf -fvi              # -f force, -v verbose, -i install aux
#   ./configure
#   make -j4
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
# xdr-parser (from the reply-xdr package).  Clone upstream reply
# repo and pipx-install it; the PyPI distribution is unreliable on
# macOS due to the pip version shipped with Xcode CLT.
# ---------------------------------------------------------------------
if command -v xdr-parser >/dev/null 2>&1; then
    echo "==> xdr-parser already on PATH"
else
    if ! command -v pipx >/dev/null 2>&1; then
        echo "ERROR: pipx not found.  brew install pipx" >&2
        exit 1
    fi
    echo "==> Cloning reply repo and installing xdr-parser via pipx"
    cd "$BUILD_DIR"
    if [ ! -d reply ]; then
        git clone --depth 1 https://github.com/loghyr/reply.git
    fi
    cd reply
    pipx install .
    echo "==> xdr-parser installed to \$HOME/.local/bin"
    echo "    Add that directory to PATH if not already:"
    echo "      pipx ensurepath"
fi

echo
echo "==> Bootstrap complete."
echo
echo "Next steps:"
echo "  cd /path/to/reffs"
echo "  mkdir -p m4                   # avoid aclocal-m4-missing on first run"
echo "  export ACLOCAL_PATH=\"/opt/homebrew/share/aclocal:\$ACLOCAL_PATH\""
echo "  export PKG_CONFIG_PATH=\"\$(brew --prefix openssl@3)/lib/pkgconfig:\$PKG_CONFIG_PATH\""
echo "  export PATH=\"\$HOME/.local/bin:\$PATH\""
echo "  autoreconf -fvi               # -f force, -v verbose, -i install aux files"
echo "  ./configure"
echo "  make -j4"
echo
echo "ACLOCAL_PATH is required on macOS: Homebrew automake's aclocal"
echo "looks in its own keg dir by default, so pkg.m4 (installed by"
echo "pkg-config) isn't found without this."
