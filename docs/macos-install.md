# Building reffs on macOS

Tested on macOS 14 Sonoma, Apple Silicon.  For the minimum
smoke test after installing, see `docs/macos-smoke.md` (pending).

## Summary (TL;DR)

```sh
# 1. Homebrew packaged deps
brew install autoconf automake libtool pkg-config cmake pipx \
    openssl@3 xxhash zstd rocksdb python@3.12

# 2. From-source deps (liburcu, HdrHistogram_c) + pipx install of xdr-parser
cd /path/to/reffs
./scripts/macos-bootstrap.sh

# 3. Build reffs
mkdir -p m4
export PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig:$PKG_CONFIG_PATH"
export PATH="$HOME/.local/bin:$PATH"
autoreconf -fvi
./configure
make -j4
```

If you hit an error, the per-step detail below explains why each
piece is needed and what to do when it breaks.

## 1. Homebrew prerequisites

macOS ships the clang toolchain as part of Xcode Command Line
Tools (`xcode-select --install`).  Everything else is Homebrew:

```sh
brew install autoconf automake libtool pkg-config cmake pipx \
    openssl@3 xxhash zstd rocksdb python@3.12
```

- **autoconf / automake / libtool**: generate `configure` from
  `configure.ac`.  Required to build from a git checkout; source
  tarballs ship a pre-generated `configure`.
- **cmake**: used by the HdrHistogram_c source build in step 2.
- **pipx**: macOS Python is PEP-668 externally-managed, so
  `pip install --user` errors out.  pipx installs CLI tools
  into their own managed venvs.
- **openssl@3**: Homebrew keeps it "keg-only" (not on the default
  include path).  `./configure` finds it via `PKG_CONFIG_PATH`
  in step 3.
- **xxhash, zstd, rocksdb**: reffs deps; all plain Homebrew bottles.
- **python@3.12**: xdr-parser needs Python 3.12+ to resolve;
  Apple's bundled Python in Command Line Tools is 3.9 and too
  old for reply-xdr's dependency resolution.

Not in Homebrew:

- **libuuid**: macOS provides `/usr/include/uuid/uuid.h` via the
  base system (Apple variant).  reffs's `configure.ac` probes for
  it and uses what it finds; no action required.
- **FUSE**: `lib/fs/fuse.c` is gated off on macOS (`!HOST_DARWIN`
  in `lib/fs/Makefile.am`).  macFUSE has its own port story that
  is out of scope.

## 2. From-source dependencies

Two reffs deps have no Homebrew formula.  The bootstrap script
at `scripts/macos-bootstrap.sh` handles both plus the xdr-parser
install:

```sh
cd /path/to/reffs
./scripts/macos-bootstrap.sh
```

### What it builds

**liburcu** (Userspace RCU).  Cloned from
`https://github.com/urcu/userspace-rcu`, installed via
`./configure --prefix=$(brew --prefix) && make && sudo make install`.
Provides `liburcu` + `liburcu-cds` + pkgconfig files.

**HdrHistogram_c**.  Cloned from
`https://github.com/HdrHistogram/HdrHistogram_c`, installed via
`cmake -DCMAKE_INSTALL_PREFIX=$(brew --prefix)`.  Used for latency
telemetry.  If you skip this, reffs builds with histograms
disabled (`#ifdef HAVE_HDR_HISTOGRAM` guards).

**xdr-parser** (from the reply-xdr Python package).  Pipx-installs
from a local clone of `https://github.com/loghyr/reply.git` rather
than PyPI because the PyPI index entry is unreliable on macOS.
Provides the `xdr-parser` binary to `~/.local/bin/` which the
reffs Makefile invokes during `make` to generate XDR headers.

### Re-running

The bootstrap script is idempotent: each step is preceded by a
pkg-config / file-existence check; already-installed deps are
reported and skipped.

## 3. Build reffs

### `mkdir -p m4` before `autoreconf`

macOS's `autoreconf -i` has a quirk where a missing `m4/` subdir
trips `aclocal` with "couldn't open directory 'm4'", which
cascades into a confusing "AC_MSG_ERROR undefined or overquoted"
error at the next autoconf pass.  Pre-create the directory:

```sh
mkdir -p m4
```

(Linux and FreeBSD tolerate the missing directory fine; the
quirk is macOS-specific to the autoconf/aclocal/libtool version
combination Homebrew ships.)

### Set PKG_CONFIG_PATH for openssl

Homebrew's `openssl@3` is keg-only — its pkgconfig files are not
on the default search path:

```sh
export PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig:$PKG_CONFIG_PATH"
```

Without this, `./configure` fails with "Requested 'openssl >=
1.0.2' but version of OpenSSL found is ..." (or similar) because
`PKG_CHECK_MODULES` picks up macOS's base LibreSSL instead.

### Put `~/.local/bin` on PATH

pipx installed `xdr-parser` there:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

### Generate + configure + build

```sh
autoreconf -fvi     # -f force, -v verbose, -i install aux files
./configure
make -j4
```

Expect:

```
  CCLD     reffsd
  CCLD     reffs_probe1_clnt
  CCLD     reffs_registry_tool
```

Verify the kqueue+thread-pool backend is selected:

```sh
grep IO_BACKEND_ config.log | head
# Expect:
#   config.log: IO_BACKEND_DARWIN_TRUE=''
#   config.log: IO_BACKEND_DARWIN_FALSE='#'
#   config.log: IO_BACKEND_KQUEUE_TRUE='#'
#   (FreeBSD's kqueue+aio disabled; Darwin's thread-pool enabled.)
```

## 4. Known gotchas

### "AC_MSG_ERROR undefined or overquoted"

You missed `mkdir -p m4` before `autoreconf`.  Run
`autoreconf -fvi` (force re-regeneration) after creating the
directory.

### "Requested 'openssl >= 1.0.2' but ..."

`PKG_CONFIG_PATH` was not set before `./configure`.

### "xdr-parser: command not found"

`~/.local/bin` isn't on PATH.  Either `export PATH=...` or run
`pipx ensurepath` and open a new terminal.

### "error: externally-managed-environment"

Old `pip3 install` attempt on macOS Python.  Use pipx instead.

### "Could not find a version that satisfies the requirement reply-xdr"

Apple's bundled pip (21.2.4) can't resolve reply-xdr reliably.
Either upgrade pip, use Homebrew Python's pip, or use the
bootstrap script which clones the upstream repo and uses
`pipx install .` directly.

### Homebrew python@3.12 shows as "already installed"

Fine — the bootstrap script doesn't force reinstall.

### `brew install rocksdb` compiles slowly

RocksDB ships as a bottle on most Apple Silicon Macs now, but on
older macOS versions Homebrew may compile it from source (can
take 10+ minutes).  Wait it out or use `--disable-rocksdb` if
your reffs configuration doesn't need RocksDB.

### `pipx install reply-xdr` fails with "No matching distribution"

Use the bootstrap script path (clone + `pipx install .`).  The
bootstrap script does this automatically.

## 5. Running reffsd

See `docs/macos-smoke.md` (pending — commit 10 of the macOS
backend PR).  Short version:

```sh
sudo mkdir -p /var/lib/reffs
sudo chown $USER /var/lib/reffs
./src/reffsd -b ram -S /var/lib/reffs --foreground 2>&1 | tee /tmp/reffs.log
```

Expected log: `io_handler_init: started (kqueue fd=N)`,
`io_backend_init: started`, listeners on port 2049.

## 6. Platform-specific design notes

- **I/O backend**: macOS uses a userspace thread pool (4 workers)
  for file I/O and kqueue `EVFILT_READ`/`EVFILT_WRITE` for
  sockets.  Darwin's POSIX aio is too pathological under
  concurrent load to rely on.  Thread-pool source is in
  `lib/io/backend_darwin.c`.

- **Socket code is shared with FreeBSD** via `lib/io/kqueue_socket.c`.
  FreeBSD diverges only on the file-I/O side (EVFILT_AIO); macOS
  diverges only on the file-I/O side (threadpool).

- **TLS** works via Homebrew OpenSSL.  No kTLS on macOS — if
  you're familiar with the Linux kTLS path, note that
  `BIO_get_ktls_send` returns 0 on macOS and the code takes the
  userspace-BIO path transparently.

- **Known limitations today**: no macFUSE support
  (`reffs_fuse` admin tool not built on macOS).  No built-in
  NFS server auto-disable — if macOS's `nfsd` is running on port
  2049, reffsd's bind fails.  Stop it first:

  ```sh
  sudo launchctl unload /System/Library/LaunchDaemons/com.apple.nfsd.plist 2>/dev/null || true
  ```

- **Live log viewing** (macOS equivalent of `journalctl -f`):

  ```sh
  log stream --predicate 'process == "reffsd"' --level debug
  ```
