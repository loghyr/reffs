# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

FROM fedora:41

# Optimize DNF for robustness and speed
RUN echo "max_parallel_downloads=10" >> /etc/dnf/dnf.conf && \
    echo "retries=20" >> /etc/dnf/dnf.conf

# Install dependencies - consolidated and deduplicated
RUN dnf -y install \
    autoconf \
    autoconf-archive \
    automake \
    bear \
    ccache \
    check-devel \
    clang \
    clang-tools-extra \
    cmake \
    fuse-devel \
    gcc \
    gcc-c++ \
    gdb \
    git \
    glibc-devel \
    HdrHistogram_c-devel \
    htop \
    iostat \
    iproute \
    jemalloc-devel \
    ktls-utils \
    libev-devel \
    libstdc++-devel \
    libtirpc-devel \
    libtool \
    liburing-devel \
    libuuid-devel \
    lldb \
    llvm-devel \
    make \
    ninja-build \
    nslookup \
    openssl-devel \
    perf \
    python3-devel \
    python3-pip \
    rpcbind \
    rpcgen \
    rocksdb-devel \
    scan-build \
    script \
    sslscan \
    strace \
    sudo \
    userspace-rcu-devel \
    vim \
    xxhash-devel \
    zlib-devel \
    && dnf clean all

RUN pip3 install --no-cache-dir \
    ply \
    xdrlib3 \
    xml2rfc

# Create workspace directories
RUN mkdir -p /reffs /backend

WORKDIR /reffs

# Create a startup script to ensure rpcbind is running
RUN printf '#!/bin/bash\nrpcbind -w\nexec "$@"\n' > /usr/local/bin/entrypoint.sh && chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
