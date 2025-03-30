#SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
#SPDX-License-Identifier: GPL-2.0+

FROM fedora:latest

RUN dnf -y update && dnf -y install gcc make fuse-devel autoreconf automake libtool userspace-rcu userspace-rcu-devel libtirpc-devel check-devel libuuid-devel libev-devel xxhash-devel clang git autoconf-archive llvm llvm-devel vim gdb wireshark liburing liburing-devel rocksdb-devel check libev libev-devel xxhash fuse uuid uuid-devel clang clang-tools-extra llvm-devel glibc-devel libstdc++-devel lldb cmake ninja-build scan-build bear rpcgen ping nslookup traceroute iproute route python3-devel pip sudo

RUN useradd -ms /bin/bash loghyr -u 1066 -g 10
USER loghyr

RUN pip3 install ply

COPY . /reffs

# Build the project
WORKDIR /reffs
RUN ls -la
RUN make check

