<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Experimental Linux DS oracle

`lib/d1store` and `lib/d2store` are executable design and recovery oracles for
the experimental Linux NFSv4.2 Flexible Files v2 data server. They define and
test state transitions, fixed-file encoding, write-ahead ordering, restart
reconstruction, and exact request receipts.

The libraries are isolated from the reffs NFS and storage request paths. They
do not provide a production data server, and passing their tests does not
establish kernel, RPC, interoperability, power-cut, or filesystem support.
Unsupported operations remain explicit and fail closed.

The current file-backed oracle targets provisioned fixed files on XFS. Its
bounded follow-up work includes binding damaged-content records to a digest,
broader crash and recovery matrices, and production capacity and lifecycle
policy. Kernel code must implement the accepted behavior independently under
the kernel's license; this AGPL implementation is a behavioral reference.
