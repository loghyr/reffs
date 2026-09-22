<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# BLAKE3 portable C implementation

The files in this directory are from the official BLAKE3 C implementation:

- Upstream: https://github.com/BLAKE3-team/BLAKE3
- Release: 1.8.2
- Commit: `df610ddc3b93841ffc59a87e3da659a15910eb46`
- Selected license: Apache-2.0 (`LICENSES/Apache-2.0`)

Only the portable implementation and runtime dispatcher are built.  The
upstream SIMD and assembly implementations are intentionally omitted to keep
the initial checksum implementation portable across Linux, macOS, and FreeBSD.

## Local changes

- `blake3_dispatch.c`: `get_cpu_features()` is declared
  `__attribute__((unused))`.  Upstream defines it unconditionally but
  calls it only under `IS_X86`; on aarch64 it has no callers, and reffs
  builds with `-Werror`, so `-Wunused-function` broke every arm64 build
  from the day the copy was vendored.  A one-line delta; re-apply when
  refreshing the copy.
