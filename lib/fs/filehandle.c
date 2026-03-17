/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
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
