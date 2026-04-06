/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * reffs_id -- structured 64-bit identity type.
 *
 * Encodes identity type, domain index, and local ID in a single
 * uint64_t.  Stored on inodes, in ACLs, and in delegation/layout
 * tables.  Backward-compatible: REFFS_ID_MAKE(UNIX, 0, uid) has
 * the UNIX uid in the low 32 bits, so truncation to uint32_t
 * preserves the uid.
 *
 *  63    60 59       32 31            0
 *  +------+-----------+---------------+
 *  | type |  domain   |    local_id   |
 *  +------+-----------+---------------+
 *    4 bit   28 bit       32 bit
 */

#ifndef _REFFS_IDENTITY_TYPES_H
#define _REFFS_IDENTITY_TYPES_H

#include <stdint.h>

typedef uint64_t reffs_id;

enum reffs_id_type {
	REFFS_ID_UNIX = 0, /* local UNIX uid/gid (domain=0) */
	REFFS_ID_SID = 1, /* Windows SID RID (domain=idx) */
	REFFS_ID_KRB5 = 2, /* Kerberos principal (domain=realm_idx) */
	REFFS_ID_NAME = 3, /* name-mapped (user@domain string) */
	REFFS_ID_SYNTH = 4, /* synthetic/fencing credential */
	/* 5-14 reserved */
	REFFS_ID_NOBODY = 15, /* anonymous/unmapped */
};

#define REFFS_ID_TYPE(id) ((uint16_t)((id) >> 60))
#define REFFS_ID_DOMAIN(id) ((uint32_t)(((id) >> 32) & 0x0FFFFFFF))
#define REFFS_ID_LOCAL(id) ((uint32_t)((id) & 0xFFFFFFFF))

#define REFFS_ID_MAKE(type, domain, local) \
	(((uint64_t)(type) << 60) |        \
	 ((uint64_t)((domain) & 0x0FFFFFFF) << 32) | (uint64_t)(local))

/* Common identity values. */
#define REFFS_ID_NOBODY_VAL REFFS_ID_MAKE(REFFS_ID_NOBODY, 0, 65534)
#define REFFS_ID_ROOT_VAL REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 0)

/* Predicates. */
#define REFFS_ID_IS_UNIX(id) \
	(REFFS_ID_TYPE(id) == REFFS_ID_UNIX && REFFS_ID_DOMAIN(id) == 0)

#define REFFS_ID_IS_NOBODY(id) (REFFS_ID_TYPE(id) == REFFS_ID_NOBODY)

#endif /* _REFFS_IDENTITY_TYPES_H */
