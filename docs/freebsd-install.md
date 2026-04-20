<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Building reffs on FreeBSD

Tested on FreeBSD 15.0-RELEASE.  For the minimum smoke test after
installing, see `docs/freebsd-smoke.md`.

## 1. System packages via pkg

Install the FreeBSD-packaged dependencies:

```
sudo pkg install -y \
    autoconf automake libtool pkgconf \
    openssl \
    liburcu \
    libxxhash \
    fusefs-libs3 \
    rocksdb \
    py311-pip \
    gmake bash gdb
```

Notes:

- **bash** is needed because `scripts/timed-test.sh` (and a few other
  test-harness scripts) use `#!/usr/bin/env bash`.  FreeBSD's base
  `/bin/sh` is not bash.
- **gmake** is needed because the Automake-generated Makefiles use
  GNU-make features.  Run `gmake` (or `MAKE=gmake ./configure`) in
  place of `make` throughout.
- **gdb** is optional but useful for debugging core dumps during the
  port work (e.g., if a backend regression crashes reffsd).

## 2. Python test harness

The RPC-wire tests under `lib/rpc/tests/` use the `reply-xdr` Python
package.  Install via pip:

```
pip install --user reply-xdr
```

This puts the `xdr-parser` script in `$HOME/.local/bin`.  Add that
directory to your shell's `PATH` or pass `PATH=$HOME/.local/bin:$PATH`
to `./configure` and `gmake`.

## 3. HdrHistogram_c (build from source)

`HdrHistogram_c` is required for reffs's latency telemetry.  **No
FreeBSD port exists** (as of FreeBSD 15.0), so build from source:

```
cd /tmp
git clone --depth 1 https://github.com/HdrHistogram/HdrHistogram_c
cd HdrHistogram_c
mkdir -p build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DHDR_HISTOGRAM_BUILD_PROGRAMS=OFF \
      -DHDR_HISTOGRAM_BUILD_SHARED=ON \
      ..
gmake -j4
sudo gmake install
```

You should see:

```
-- Installing: /usr/local/include/hdr/hdr_histogram.h
-- Installing: /usr/local/lib/libhdr_histogram.so.6.2.3
...
```

Build dependencies (normally already present): `cmake`, `zlib` (both
in FreeBSD base).  No extra pkgs needed for HdrHistogram itself.

### Why build from source

HdrHistogram_c is a small MIT/BSD-licensed C library (~3 kLOC,
depends only on zlib + libm + pthreads).  No technical obstacle
prevents a FreeBSD port — just no one has filed one yet.  If you
want to contribute a port it would be useful; meanwhile the
one-time source build is a 30-second operation.

### Verifying detection

After install, `./configure` should detect it:

```
$ grep -i hdr config.log | head
configure:checking for hdr/hdr_histogram.h... yes
| #define HAVE_HDR_HISTOGRAM 1
HDR_HISTOGRAM_LIBS='-lhdr_histogram'
```

If you see `hdr_histogram.h not found`, the install directory is not
on the compiler's search path.  `configure` adds `/usr/local/include`
and `/usr/local/lib` automatically on FreeBSD (see the `host_is_freebsd`
branch in `configure.ac`), so a standard cmake install to `/usr/local`
should just work.  If you installed elsewhere, re-export `CPPFLAGS`
and `LDFLAGS` to point at that prefix before `./configure`.

### Running reffsd without HdrHistogram

If you choose to skip the HdrHistogram install, `./configure` prints
a warning and sets `HAVE_HDR_HISTOGRAM` to undefined.  Source files
gate histogram calls behind that macro, so reffsd builds and runs,
but latency histograms are disabled.  For production FreeBSD
deployments, install HdrHistogram; for a quick smoke test, skipping
is fine.

## 4. Build reffs

```
cd reffs
autoreconf -i
PATH=$HOME/.local/bin:$PATH MAKE=gmake ./configure
PATH=$HOME/.local/bin:$PATH gmake -j4
```

Expect:

```
  CCLD     reffsd
  CCLD     reffs_probe1_clnt
  CCLD     reffs_registry_tool
```

and no linker errors.  Verify the binary loads HdrHistogram:

```
$ ldd src/.libs/reffsd | grep hdr
	libhdr_histogram.so.6 => /usr/local/lib/libhdr_histogram.so.6 (0x...)
```

If that line is missing, reffsd still works but histograms are off.

## 5. First run (smoke)

Continue with `docs/freebsd-smoke.md`.

## Known non-obvious FreeBSD gotchas

These are documented in more depth in the relevant configure.ac
comments, listed here for discoverability.

- **`EREMOTEIO` and `ENODATA`**: not defined in FreeBSD `<errno.h>`.
  `configure.ac` defines them to reffs-internal sentinel values
  (198, 199) outside the FreeBSD errno range.  Keep these internal
  — do not return them to callers across the reffs boundary.
- **`libtirpc`**: FreeBSD's base libc provides SunRPC natively, so
  `libtirpc` is not used on FreeBSD (it is a Linux-only dep).
- **`rpc/auth_gss.h`**: FreeBSD's SunRPC does not ship GSS-RPC
  headers.  GSS-related code is gated on `HAVE_RPC_AUTH_GSS_H` +
  `HAVE_GSSAPI_KRB5` and skipped on FreeBSD unless those are
  present.
- **`backtrace(3)`**: in glibc on Linux, but in `libexecinfo` on
  FreeBSD.  `configure.ac` adds `-lexecinfo` automatically.
- **liburcu headers + `-Wpedantic`**: liburcu's headers use
  named-variadic-macro-arguments which trigger warnings on FreeBSD
  clang.  `configure.ac` adds `-Wno-gnu` to suppress these during
  reffs builds (does not leak to installed headers).
- **rocksdb's pkg-config `.pc`**: ships with a broken
  `-Wl,-rpath -Wl,` fragment that libtool rejects.  `configure.ac`
  strips it with a sed post-pass.
- **FUSE**: `fusefs-libs3` is the FreeBSD equivalent of
  `libfuse3-dev`, but `lib/fs/fuse.c` is skipped entirely on FreeBSD
  (Makefile.am `!HOST_FREEBSD` conditional).  reffs's FUSE-backed
  admin tool is currently Linux-only.
- **rpcbind**: start it before reffsd if you want NFSv2/NFSv3
  discovery: `sudo service rpcbind onestart`.  NFSv4 does not
  require rpcbind (uses port 2049 directly) but reffsd logs
  `Failed to register ...` warnings for v2/v3 listeners if rpcbind
  is absent.  Harmless; they do not stop the server.
