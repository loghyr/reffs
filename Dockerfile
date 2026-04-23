# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

FROM fedora:43

# Optimize DNF for robustness and speed
RUN echo "max_parallel_downloads=10" >> /etc/dnf/dnf.conf && \
    echo "retries=20" >> /etc/dnf/dnf.conf

# Install dependencies - consolidated and deduplicated
RUN dnf -y install \
    autoconf \
    autoconf-archive \
    automake \
    bash-completion \
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
    iputils \
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
    net-tools \
    ninja-build \
    nslookup \
    krb5-devel \
    krb5-workstation \
    libnfsidmap-devel \
    openssl-devel \
    perf \
    procps-ng \
    tcpdump \
    python3-argcomplete \
    python3-devel \
    python3-pip \
    rpcbind \
    rsync \
    rocksdb-devel \
    scan-build \
    script \
    sslscan \
    strace \
    sudo \
    userspace-rcu-devel \
    wireshark-cli \
    vim \
    xxhash-devel \
    zlib-devel \
    && dnf clean all

# Configure bash completion for the container shell
RUN activate-global-python-argcomplete --user && \
    echo 'eval "$(register-python-argcomplete reffs-probe.py)"' >> /root/.bashrc && \
    echo 'source /etc/profile.d/bash_completion.sh' >> /root/.bashrc

# reply-xdr (xdr-parser) is pulled from HEAD.  Bump REPLY_XDR_REV when
# a newer reply-xdr is required (the value is only used to invalidate
# the Docker layer cache -- the actual revision installed is still
# main of the git repo).  The current value is the date the C-output
# naming convention changed from "<prefix>_xdr.h" to "<prefix>.h";
# older layers built before this date produce the wrong filenames for
# ref_cp_xdr.x and break the lib/xdr/Makefile.am rules.
ARG REPLY_XDR_REV=2026-04-22
RUN echo "reply-xdr cache-bust: ${REPLY_XDR_REV}" && \
    pip3 install --no-cache-dir \
    reply-xdr@git+https://github.com/loghyr/reply.git \
    xdrlib3 \
    xml2rfc

# Test users for NFSv4 identity tests (matches Dockerfile.ci).
RUN groupadd -g 3300 nfsgroup && \
    useradd -u 3300 -g 3300 -M -s /usr/sbin/nologin nfstest
RUN mkdir -p /etc && printf '[General]\nDomain = reffs.test\n' > /etc/idmapd.conf

# Create workspace directories
RUN mkdir -p /reffs /backend /src /build /logs

WORKDIR /build

# Create a startup script to ensure rpcbind is running
RUN printf '#!/bin/bash\nrpcbind -w\nexec "$@"\n' > /usr/local/bin/entrypoint.sh && chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
