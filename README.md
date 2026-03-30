<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Reffs: Reference File System

Reffs is a high-performance Reference File System project focused on providing reference implementations for NFSv3, NFSv4.2 (with Flex Files support), and FUSE. It leverages modern Linux kernel features like `io_uring` for asynchronous I/O and `liburcu` for scalable, lock-free synchronization.

## Features

- **Protocol Support:** NFSv3, NFSv4.2 (Flex Files), and FUSE.
- **Asynchronous I/O:** High-performance core using `io_uring`.
- **Lock-free Sync:** Scalable synchronization via `liburcu`.
- **Reference Quality:** Focus on protocol compliance and architectural clarity.

## Getting Started

### Docker (Recommended)

The easiest way to build and run reffs is with Docker. This avoids
host-level conflicts with NFS services (`rpcbind`, `lockd`) and
handles all dependencies automatically.

**Requirements:** Docker and docker-compose.

```bash
# Build the developer image and run the NFS server
make -f Makefile.reffs image
make -f Makefile.reffs run-image

# Or run the full CI pipeline (build + unit tests + integration)
make -f Makefile.reffs ci-check
```

See `make -f Makefile.reffs help` for all available targets.

### Native Build Prerequisites

If you prefer to build natively, install the dependencies for your
distribution:

#### Fedora

```bash
sudo dnf install clang pkg-config userspace-rcu-devel libtirpc-devel \
    check-devel xxhash-devel fuse-devel rpcsvc-proto-devel \
    libuuid-devel zlib-devel libzstd-devel liburing-devel \
    jemalloc-devel autoconf automake libtool \
    python3 python3-pip openssl-devel libhdr_histogram-devel
pip install reply-xdr xdrlib3
```

#### Ubuntu / Debian

```bash
sudo apt-get install clang pkg-config liburcu-dev libtirpc-dev check \
    libxxhash-dev libfuse-dev rpcsvc-proto uuid-dev zlib1g-dev \
    libzstd-dev liburing-dev libjemalloc-dev autoconf automake libtool \
    python3 python3-pip libssl-dev libhdr-histogram-c-dev
pip install reply-xdr xdrlib3
```

#### Python XDR tooling

The `reply-xdr` package provides the `xdr-parser` tool and the Python
RPC client library used by reffs-probe. Install from GitHub for the
latest version:

```bash
pip install reply-xdr@git+https://github.com/loghyr/reply.git
```

### Build & Test

1. **Clone and Configure:**
   ```bash
   autoreconf -fi
   mkdir build && cd build
   ../configure --enable-asan --enable-ubsan
   ```
2. **Build:**
   ```bash
   make -j$(nproc)
   ```
3. **Run Tests:**
   ```bash
   make check
   ```

## Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for details on our development workflow, including our requirement for Developer Certificate of Origin (DCO) sign-offs.

## Security

To report security vulnerabilities, please follow the instructions in [SECURITY.md](SECURITY.md).

## License

This project is licensed under the AGPL-3.0-or-later. See [LICENSE.md](LICENSE.md) and the `LICENSES/` directory for full details.
