# Reffs: Reference File System

Reffs is a high-performance Reference File System project focused on providing reference implementations for NFSv3, NFSv4.2 (with Flex Files support), and FUSE. It leverages modern Linux kernel features like `io_uring` for asynchronous I/O and `liburcu` for scalable, lock-free synchronization.

## Features

- **Protocol Support:** NFSv3, NFSv4.2 (Flex Files), and FUSE.
- **Asynchronous I/O:** High-performance core using `io_uring`.
- **Lock-free Sync:** Scalable synchronization via `liburcu`.
- **Reference Quality:** Focus on protocol compliance and architectural clarity.

## Getting Started

### Prerequisites

You will need the following dependencies installed (Ubuntu/Debian example):

```bash
sudo apt-get install clang pkg-config liburcu-dev libtirpc-dev check \
    libxxhash-dev libfuse-dev rpcgen uuid-dev zlib1g-dev libzstd-dev \
    liburing-dev libjemalloc-dev autoconf automake libtool
```

### Build & Test

1. **Clone and Configure:**
   ```bash
   ./autoreconf -fi
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

### Docker Sandbox (Recommended)

To avoid host-level conflicts with NFS services (`rpcbind`, `lockd`), use the isolated Docker environment:

1. **Build Image:**
   ```bash
   make -f Makefile.reffs image
   ```
2. **Run Server:**
   ```bash
   make -f Makefile.reffs run-image
   ```

## Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for details on our development workflow, including our requirement for Developer Certificate of Origin (DCO) sign-offs.

## Security

To report security vulnerabilities, please follow the instructions in [SECURITY.md](SECURITY.md).

## License

This project is licensed under the AGPL-3.0-or-later. See [LICENSE.md](LICENSE.md) and the `LICENSES/` directory for full details.
