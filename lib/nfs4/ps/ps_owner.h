/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_OWNER_H
#define _REFFS_PS_OWNER_H

#include <stdint.h>

/*
 * Open-owner / lock-owner wrapping for the proxy server.
 *
 * Multiple end clients connecting through the same PS share that
 * PS's NFSv4.1 session to the upstream MDS.  The MDS keys
 * stateowners by (session_clientid, owner_string).  Two end clients
 * whose raw owner strings happen to collide (most easily seen
 * with NFSv4.0 where the open_owner4 is "openowner" or similar
 * defaulted strings) would land on the same MDS stateowner and
 * confuse SHARE_DENY accounting.
 *
 * Disambiguate by prefixing the raw owner with the END CLIENT's
 * clientid4 encoded as 8 big-endian bytes.  The MDS treats the
 * resulting blob as opaque, so no MDS-side support is needed.
 * Big-endian on the wire keeps two PSes that wrap with the same
 * clientid4 byte-identical regardless of host endianness.
 */

#define PS_OWNER_TAG_SIZE 8u /* clientid4 bytes prepended */

/*
 * ps_owner_wrapped_size -- size of the wrapped buffer for a raw
 * owner of `raw_len` bytes.  Use to size a stack / heap allocation
 * before calling ps_owner_wrap.
 */
static inline uint32_t ps_owner_wrapped_size(uint32_t raw_len)
{
	return PS_OWNER_TAG_SIZE + raw_len;
}

/*
 * ps_owner_wrap -- prefix `end_client_id` (as 8 BE bytes) to `raw`
 * and write the result to `out`.  `out` must have at least
 * ps_owner_wrapped_size(raw_len) bytes available.  Writes the
 * total wrapped length to `*out_len_out`.
 *
 * Passing raw_len == 0 is allowed -- the wrapped form is just the
 * 8-byte clientid tag.  Passing raw == NULL with raw_len == 0 is
 * also allowed (no copy needed).
 *
 * Returns:
 *   0        success
 *   -EINVAL  out / out_len_out NULL, or raw NULL with raw_len > 0
 *   -ENOSPC  out_size < ps_owner_wrapped_size(raw_len)
 */
int ps_owner_wrap(uint64_t end_client_id, const uint8_t *raw, uint32_t raw_len,
		  uint8_t *out, uint32_t out_size, uint32_t *out_len_out);

#endif /* _REFFS_PS_OWNER_H */
