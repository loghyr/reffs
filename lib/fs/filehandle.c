/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdint.h>
#include <stdlib.h>
#include <uuid/uuid.h>

#include "reffs/filehandle.h"
#include "reffs/server.h"

static uint16_t nfh_reserved_1 = 0;
static uint16_t nfh_reserved_2 = 0;
static uint16_t nfh_reserved_3 = 0;

struct network_file_handle *network_file_handle_construct(uint64_t sbi,
							  uint64_t ino)
{
	struct network_file_handle *nfh;

	nfh = calloc(1, sizeof(*nfh));
	if (!nfh)
		return nfh;

	nfh->nfh_vers = FILEHANDLE_VERSION_CURR;
	nfh->nfh_sb = sbi;
	nfh->nfh_ino = ino;
	nfh->nfh_reserved_1 = nfh_reserved_1;
	nfh->nfh_reserved_2 = nfh_reserved_2;
	nfh->nfh_reserved_3 = nfh_reserved_3;

	return nfh;
}

void network_file_handle_init(void)
{
	uuid_t *boot_uuid = server_boot_uuid_get();
	uint64_t bits = 0;

	// Use the last 6 bytes of the UUID (48 bits)
	// UUID is 16 bytes total
	bits |= ((uint64_t)(*boot_uuid)[10]) << 40;
	bits |= ((uint64_t)(*boot_uuid)[11]) << 32;
	bits |= ((uint64_t)(*boot_uuid)[12]) << 24;
	bits |= ((uint64_t)(*boot_uuid)[13]) << 16;
	bits |= ((uint64_t)(*boot_uuid)[14]) << 8;
	bits |= ((uint64_t)(*boot_uuid)[15]);

	nfh_reserved_1 = (bits >> 32) & 0xFFFF;
	nfh_reserved_2 = (bits >> 16) & 0xFFFF;
	nfh_reserved_3 = (bits >> 0) & 0xFFFF;
}
