# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

FROM fedora:latest

RUN dnf -y update && dnf -y install \
    gcc make fuse-devel autoreconf automake libtool \
    userspace-rcu userspace-rcu-devel libtirpc-devel \
    check-devel libuuid-devel libev-devel xxhash-devel \
    clang git autoconf-archive llvm llvm-devel vim gdb \
    wireshark liburing liburing-devel rocksdb-devel \
    check libev libev-devel xxhash fuse uuid uuid-devel \
    clang clang-tools-extra llvm-devel glibc-devel \
    libstdc++-devel lldb cmake ninja-build scan-build \
    bear rpcgen ping nslookup traceroute iproute route \
    python3-devel python3-pip sudo rpcbind \
    ccache strace perf htop iostat script \
    jemalloc jemalloc-devel HdrHistogram_c HdrHistogram_c-devel \
    ktls-utils sslscan

RUN pip3 install ply xdrlib3 xml2rfc

# Create workspace directories
RUN mkdir -p /reffs /backend

WORKDIR /reffs

# Create a startup script to ensure rpcbind is running
RUN echo '#!/bin/bash\n\
rpcbind -w\n\
exec "$@"' > /usr/local/bin/entrypoint.sh && chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
