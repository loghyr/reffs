/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * proxy_stateid value primitives -- slice 6c-x.1.
 *
 * Implements proxy_stateid_alloc, proxy_stateid_extract_boot_seq,
 * proxy_stateid_is_stale, and proxy_stateid_other_eq.  See
 * lib/nfs4/include/nfs4/proxy_stateid.h for the contract and the
 * other[12] layout.
 *
 * The lookup table that resolves proxy_stateid -> migration record
 * is built in slice 6c-x.2 because the entries IN that table ARE
 * migration records; this slice provides only the value primitives
 * those records depend on.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <uuid/uuid.h>

#include "nfsv42_xdr.h"
#include "nfs4/proxy_stateid.h"

/*
 * Layout invariant: the offsets and lengths in proxy_stateid.h must
 * exactly tile NFS4_OTHER_SIZE (12 bytes for stateid4.other).  If the
 * XDR widens stateid4.other or the header offsets drift, this catches
 * it at compile time before silent wire corruption ships.
 */
_Static_assert(PROXY_STATEID_BOOT_SEQ_OFF == 0,
	       "proxy_stateid boot_seq must start at offset 0");
_Static_assert(PROXY_STATEID_BOOT_SEQ_OFF + PROXY_STATEID_BOOT_SEQ_LEN ==
		       PROXY_STATEID_RESERVED_OFF,
	       "proxy_stateid reserved must follow boot_seq");
_Static_assert(PROXY_STATEID_RESERVED_OFF + PROXY_STATEID_RESERVED_LEN ==
		       PROXY_STATEID_OPAQUE_OFF,
	       "proxy_stateid opaque must follow reserved");
_Static_assert(PROXY_STATEID_OPAQUE_OFF + PROXY_STATEID_OPAQUE_LEN ==
		       NFS4_OTHER_SIZE,
	       "proxy_stateid layout must exactly tile NFS4_OTHER_SIZE");

int proxy_stateid_alloc(uint16_t boot_seq, stateid4 *out)
{
	uuid_t u;

	if (!out)
		return -EINVAL;

	memset(out, 0, sizeof(*out));

	/*
	 * Initial seqid: per RFC 8881 S8.2.4, the server bumps the
	 * stateid's seqid on every issuance.  An initial value of 1
	 * matches the convention used by other server-issued stateids
	 * in this codebase (open / lock / layout / delegation -- see
	 * lib/nfs4/server/stateid.c) so the migration-side renewal
	 * path's "bump on each issuance" rule starts from a non-zero
	 * baseline.
	 */
	out->seqid = 1;

	/*
	 * Boot_seq prefix in big-endian byte order.  Endianness chosen
	 * for human-readability when the stateid is dumped in hex (e.g.
	 * `04 27 00 00 ...` for boot_seq 0x0427) and for compatibility
	 * with future widening: extending the prefix to 32 bits would
	 * keep the most-significant byte at offset 0.
	 */
	out->other[PROXY_STATEID_BOOT_SEQ_OFF + 0] =
		(uint8_t)((boot_seq >> 8) & 0xFF);
	out->other[PROXY_STATEID_BOOT_SEQ_OFF + 1] = (uint8_t)(boot_seq & 0xFF);

	/* Reserved bytes 2-3 are already zero from the memset above. */

	/*
	 * Opaque tail: 8 bytes from the high half of a fresh UUID.
	 * libuuid's random generator is the same source the project
	 * uses for sb_uuid (super_block_alloc) and server_state UUID
	 * (server_state_init); using it here keeps the entropy story
	 * consistent across the codebase.  The UUID's lower 8 bytes
	 * are unused; collision in the high 8 bytes is the rare event
	 * we care about and remains cryptographically negligible.
	 */
	uuid_generate_random(u);
	memcpy(&out->other[PROXY_STATEID_OPAQUE_OFF], u,
	       PROXY_STATEID_OPAQUE_LEN);

	return 0;
}

uint16_t proxy_stateid_extract_boot_seq(const stateid4 *stid)
{
	if (!stid)
		return 0;
	/*
	 * stateid4.other is char[12].  C leaves char's signedness up to
	 * the implementation: signed on x86/x86_64, unsigned on aarch64
	 * and many others.  Without the (uint8_t) cast, a byte with the
	 * high bit set (>= 0x80) sign-extends through integer promotion
	 * before the (uint16_t) cast, so e.g. byte 0xAB becomes 0xFFAB
	 * instead of 0x00AB.  The static_asserts above guarantee the
	 * offsets are correct; this guarantees the bytes themselves
	 * decode portably.
	 */
	return ((uint16_t)(uint8_t)stid->other[PROXY_STATEID_BOOT_SEQ_OFF + 0]
		<< 8) |
	       (uint16_t)(uint8_t)stid->other[PROXY_STATEID_BOOT_SEQ_OFF + 1];
}

bool proxy_stateid_is_stale(const stateid4 *stid, uint16_t current_boot_seq)
{
	if (!stid)
		return true;
	return proxy_stateid_extract_boot_seq(stid) != current_boot_seq;
}

bool proxy_stateid_other_eq(const stateid4 *a, const stateid4 *b)
{
	if (!a || !b)
		return false;
	return memcmp(a->other, b->other, NFS4_OTHER_SIZE) == 0;
}
