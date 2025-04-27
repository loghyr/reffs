/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_FILEHANDLE_H
#define _REFFS_FILEHANDLE_H

#include <stdint.h>
#include <stdbool.h>

#define FILEHANDLE_VERSION_1 (1)
#define FILEHANDLE_VERSION_CURR (1)

struct network_file_handle {
	uint16_t nfh_vers;
	uint16_t nfh_reserved_1;
	uint16_t nfh_reserved_2;
	uint16_t nfh_reserved_3;
	uint64_t nfh_sb;
	uint64_t nfh_ino;
};

static inline bool network_file_handles_equal(struct network_file_handle *a,
					      struct network_file_handle *b)
{
	return a->nfh_sb == b->nfh_sb && a->nfh_ino == b->nfh_ino &&
	       a->nfh_reserved_1 == b->nfh_reserved_1 &&
	       a->nfh_reserved_2 == b->nfh_reserved_2 &&
	       a->nfh_reserved_3 == b->nfh_reserved_3;
}

struct network_file_handle *network_file_handle_construct(uint64_t sbi,
							  uint64_t ino);

void network_file_handle_init(void);

#endif
