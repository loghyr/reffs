<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Flex Files v2 checksum algorithms

## Scope

Implement every initial `checksum_algorithm4` registration in the current
Flex Files v2 draft: NONE, CRC32, CRC32C, Fletcher4, SHA-256, SHA-512, and
BLAKE3.  Use one common implementation for the prototype client and data
server so packing, validation, read verification, and at-rest verification
cannot drift.  Keep the existing on-disk checksum slot and XDR unchanged.

The existing implementation computes CRC32 over chunk payload bytes.  This
change applies every algorithm to that same byte stream.  The draft's broader
"conceptual header plus payload" coverage cannot yet be implemented
interoperably: CHUNK_WRITE asks the client for a checksum before the data
server chooses the accepted `chunk_guard4`.  That protocol issue is recorded
but is not papered over with a private serialization.

## Tests first

1. Add a focused common-checksum unit test using published known-answer
   vectors for CRC32, CRC32C, SHA-256, SHA-512, and BLAKE3.
2. Verify Fletcher4's four little-endian 32-bit accumulators and big-endian
   64-bit wire output; the implementation uses unsigned 64-bit accumulators
   so the draft-required wrap behavior follows C's defined arithmetic.
3. Cover NONE, unknown algorithms, malformed lengths, Fletcher4 alignment,
   packing, verification success, and one-bit mismatch failure.
4. Update layout capability tests: all seven registered values are accepted;
   unknown values remain rejected.
5. Exercise every data-bearing algorithm through the production CHUNK_WRITE
   validator and stored metadata.  Retain the existing read-path corruption
   tests while the common verifier supplies algorithm-specific coverage.

Existing tests must continue to pass unchanged except tests whose explicit
contract was that the five algorithms were unsupported.

## Implementation

1. Replace CRC32-only inline computation with a common dispatcher that
   returns the registered output length, computes a digest, packs `checksum4`,
   and verifies a received value with a length check before comparison.
2. Use zlib for CRC32, a portable Castagnoli implementation for CRC32C, the
   draft's little-endian-word Fletcher4 definition, and the existing OpenSSL
   dependency for SHA-256/SHA-512.
3. Vendor the official portable BLAKE3 C implementation at a pinned release,
   under its Apache-2.0 option, with provenance and license documentation.
4. Pass each mirror's layout-selected algorithm into CHUNK_WRITE and
   CHUNK_WRITE_REPAIR.  On CHUNK_READ, dispatch from the returned algorithm
   and fail closed on malformed, unknown, or mismatched checksums.
5. Make server CHUNK_WRITE validation and CHUNK_READ at-rest checking use the
   same dispatcher.  Preserve the stored checksum on a corrupt read so the
   client observes the failure rather than laundering corrupted bytes.

## Validation

- Focused known-answer and CHUNK tests.
- Out-of-tree ASAN+UBSAN build and `make check`.
- `make -f Makefile.reffs fix-style`, `license`, `style`, and `check`.
- `make -f Makefile.ci check` and its integration-only `test` target because
  this changes RPC data-path behavior.
- Review allocation cleanup, algorithm/length validation, exact comparison,
  portability, and existing async ownership paths.

## Deferred

- Resolving the draft's checksum-header/accepted-guard ordering contradiction.
- SIMD CRC32C or BLAKE3 optimization; correctness uses portable code first.
- New checksum registry values or an on-disk format revision.

## Validation results

- Focused ASAN+UBSAN checksum, layout-policy, and CHUNK tests pass.  The CHUNK
  suite reports 71/71 after consolidating the algorithm loop into one server
  fixture.
- The full ASAN+UBSAN suite passes with leak detection disabled.  With leak
  detection enabled, the pre-existing `grace_test` leaks allocations rooted
  in `server_state_init`; no checksum test fails.
- The normal repository `check`, formatting, and license gates pass.
- Containerized CI unit tests pass, but the wrapper rejects four existing
  tests for exceeding its two-second timing policy on this host:
  `linux_md_test`, `fs_test_lru`, `chunk_test`, and `nfs4_client_persist`.
- The separately run container integration target passes NFSv4.2 and NFSv3
  byte validation, both build-on-NFS stages, TLS, Kerberos, and its ASAN scan.
- Focused Clang static analysis reports no finding in the common checksum
  implementation.  The full scan stops on pre-existing warning-as-error
  diagnostics in `lib/backends`; its five reports are confined to tomlc99 and
  `lib/trace/common.c`.
- `make dist` remains blocked by the pre-existing `Makefile.am` reference to
  missing `CONTRIBUTING` (the repository contains `CONTRIBUTING.md`).
