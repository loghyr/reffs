/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * proxy_stateid value primitives -- slice 6c-x.1.
 *
 * The proxy_stateid is a new server-issued stateid type the MDS
 * mints when accepting a PROXY_PROGRESS work assignment for a
 * registered PS.  Wire shape reuses NFSv4 stateid4; value space
 * is disjoint from open / lock / layout / delegation by *context*
 * (only PROXY_PROGRESS / PROXY_DONE / PROXY_CANCEL args carry
 * one).  See draft-haynes-nfsv4-flexfiles-v2-data-mover
 * sec-proxy-stateid for the wire-level definition.
 *
 * Layout of the 12-byte `other` field:
 *
 *   bytes 0-1: boot_seq (big-endian uint16_t) -- matches the
 *              reffs sps_boot_seq width
 *   bytes 2-3: reserved (must be zero on emit; ignored on receipt)
 *   bytes 4-11: opaque -- 8 bytes from getrandom(2)
 *
 * The boot_seq prefix lets the lookup path reject stale stateids
 * (minted in a prior MDS boot) cheaply: any stateid whose prefix
 * does not equal the current `server_boot_seq()` returns
 * NFS4ERR_STALE_STATEID without a hash probe.  Same trick as
 * clientid4 partitioning (RFC 8881 S2.4.1).
 *
 * Slice 6c-x.1 scope: just the value primitives (alloc, stale
 * detection).  The lookup table is part of the migration record
 * in slice 6c-x.2 (entries in the table ARE migration records,
 * not separate proxy_stateid entries).
 */

#ifndef _REFFS_NFS4_PROXY_STATEID_H
#define _REFFS_NFS4_PROXY_STATEID_H

#include <stdbool.h>
#include <stdint.h>

#include "nfsv42_xdr.h"

/*
 * Layout offsets within stateid4.other[12].  Exposed for the
 * unit tests and for the migration record's lookup-key
 * construction; production code should use the helpers below.
 */
#define PROXY_STATEID_BOOT_SEQ_OFF 0
#define PROXY_STATEID_BOOT_SEQ_LEN 2
#define PROXY_STATEID_RESERVED_OFF 2
#define PROXY_STATEID_RESERVED_LEN 2
#define PROXY_STATEID_OPAQUE_OFF   4
#define PROXY_STATEID_OPAQUE_LEN   8

/*
 * proxy_stateid_alloc -- mint a fresh proxy_stateid for the given
 * boot_seq.  `seqid` is set to 1 (the initial value); subsequent
 * MDS-side renewals bump it via standard NFSv4 stateid sequence
 * semantics (RFC 8881 S8.2.4).
 *
 * `other[12]` is laid out as
 *   { uint16_t boot_seq | uint16_t reserved=0 | uint64_t opaque }
 * with `opaque` taken from the high 8 bytes of a freshly
 * `uuid_generate_random()`-allocated UUID.  libuuid's random UUID
 * generator draws from the OS entropy pool (getrandom(2) on Linux,
 * arc4random_buf(3) on Darwin / BSD); collision in the 64-bit
 * opaque tail is cryptographically negligible at the volume of
 * expected migrations within a single boot (much less than 2^32).
 *
 * Returns 0 on success.  This function does not currently fail
 * (uuid_generate_random is documented as infallible); the int
 * return is for forward-compat if a future implementation moves
 * to a fallible RNG path.
 *
 * Thread-safe: libuuid's random generator is itself thread-safe
 * and the function does not touch any shared state beyond its
 * arguments.
 */
int proxy_stateid_alloc(uint16_t boot_seq, stateid4 *out);

/*
 * proxy_stateid_extract_boot_seq -- decode the boot_seq prefix
 * from a proxy_stateid's `other[12]`.  Returns the value as
 * host-order uint16_t.  Used by the lookup path to reject stale
 * stateids before any hash probe.
 *
 * No validation: the function is a pure decode of bytes 0-1.
 * Callers MUST first establish via context that the stateid is
 * indeed a proxy_stateid (PROXY_DONE / PROXY_CANCEL handler
 * dispatch), otherwise the result is garbage.
 */
uint16_t proxy_stateid_extract_boot_seq(const stateid4 *stid);

/*
 * proxy_stateid_is_stale -- return true iff this stateid was
 * minted in a prior MDS boot (its boot_seq prefix does not
 * match `current_boot_seq`).  The PROXY_DONE / PROXY_CANCEL
 * handler MUST return NFS4ERR_STALE_STATEID for stale stateids,
 * NOT NFS4ERR_BAD_STATEID -- the distinction is normative per
 * the wire-error contract in
 * draft-haynes-nfsv4-flexfiles-v2-data-mover sec-PROXY_DONE.
 */
bool proxy_stateid_is_stale(const stateid4 *stid, uint16_t current_boot_seq);

/*
 * proxy_stateid_other_eq -- byte-compare two stateid.other
 * fields.  Returns true iff equal.  Helper for the migration
 * record's lookup-by-stateid path; production callers use this
 * via the hash-table match callback.
 */
bool proxy_stateid_other_eq(const stateid4 *a, const stateid4 *b);

#endif /* _REFFS_NFS4_PROXY_STATEID_H */
