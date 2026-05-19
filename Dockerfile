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
    nfs-utils \
    ninja-build \
    nslookup \
    krb5-devel \
    krb5-workstation \
    libnfsidmap-devel \
    openmpi \
    openmpi-devel \
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

RUN pip3 install --no-cache-dir \
    reply-xdr@git+https://github.com/loghyr/reply.git \
    xdrlib3 \
    xml2rfc

# IOR + OpenMPI for chunk-collision Track 2 (see
# deploy/benchmark/run_chunk_collision_track2.sh and
# .claude/design/chunk-collision-track2.md).  N proxy servers act
# as N distinct clientids contending on one shared MDS file; IOR
# -F 0 -W -R -C is the parallel writer/verifier and mpirun the
# launcher.  OpenMPI is packaged; IOR (github.com/hpc/ior) is not,
# so it is built from a pinned release tag.  Fedora puts the MPI
# wrappers under /usr/lib64/openmpi -- expose them on PATH so both
# the build and the harness find mpicc/mpirun without `module load`.
ENV PATH="/usr/lib64/openmpi/bin:${PATH}"
ENV LD_LIBRARY_PATH="/usr/lib64/openmpi/lib"
# IOR 4.0.0 predates the C23 toolchain default; Fedora 43's GCC 15
# builds -std=gnu23, which turns implicit function declarations and
# related legacy-C constructs into hard errors.  Pin the build to
# gnu17 and downgrade the new-default errors so the release tag
# stays usable on a modern Fedora base.
RUN git clone --depth 1 --branch 4.0.0 \
        https://github.com/hpc/ior /tmp/ior && \
    cd /tmp/ior && ./bootstrap && \
    ./configure MPICC=mpicc \
        CFLAGS="-O2 -std=gnu17 -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=incompatible-pointer-types" && \
    make -j"$(nproc)" && \
    make install && \
    rm -rf /tmp/ior

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
