/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * proxy_deviceid.h -- reffs encoding of dstore_id into deviceid4.
 *
 * The proxy-server draft (draft-haynes-nfsv4-flexfiles-v2-proxy-server)
 * specifies the wire format of proxy_assignment4's source / target
 * fields as deviceid4 (RFC 8881 S3.3.7) -- a 16-byte opaque the MDS
 * issues and the PS dereferences via GETDEVICEINFO.
 *
 * Reffs's internal dstore identifier is a uint64_t assigned at
 * config-parse time (the [[data_server]] id field).  A
 * general-purpose proxy server would treat deviceid4 as opaque and
 * round-trip through GETDEVICEINFO; reffs is both the MDS issuing
 * the deviceid and the proxy/DS resolving it, so it can pack its
 * own dstore id into the deviceid4 directly.
 *
 * Encoding: big-endian uint64_t in the first 8 bytes; remaining 8
 * bytes are zero.  Round-trip stable for any reffs MDS <-> reffs
 * PS pair.
 */

#ifndef _REFFS_NFS4_PROXY_DEVICEID_H
#define _REFFS_NFS4_PROXY_DEVICEID_H

#include <stdint.h>
#include <string.h>

#if defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#define htobe64(x) OSSwapHostToBigInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#else
#include <endian.h>
#endif

#include "nfsv42_xdr.h"

static inline void proxy_deviceid_encode(char out[NFS4_DEVICEID4_SIZE],
					 uint64_t dstore_id)
{
	uint64_t be = htobe64(dstore_id);

	memcpy(out, &be, sizeof(be));
	memset(out + sizeof(be), 0, NFS4_DEVICEID4_SIZE - sizeof(be));
}

static inline uint64_t proxy_deviceid_decode(const char in[NFS4_DEVICEID4_SIZE])
{
	uint64_t be;

	memcpy(&be, in, sizeof(be));
	return be64toh(be);
}

#endif /* _REFFS_NFS4_PROXY_DEVICEID_H */
