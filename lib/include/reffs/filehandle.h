/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_FILEHANDLE_H
#define _REFFS_FILEHANDLE_H

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#define FILEHANDLE_VERSION_1 (1)
#define FILEHANDLE_VERSION_CURR (1)

struct network_file_handle {
	uint16_t nfh_vers;
	uint16_t nfh_reserved_1;
	uint16_t nfh_reserved_2;
	uint16_t nfh_reserved_3;
	uint64_t nfh_sb;
	uint64_t nfh_ino;
} __attribute__((packed));

static_assert(sizeof(struct network_file_handle) == 24,
	      "network_file_handle has unexpected padding");

static inline bool network_file_handles_equal(struct network_file_handle *a,
					      struct network_file_handle *b)
{
	return a->nfh_sb == b->nfh_sb && a->nfh_ino == b->nfh_ino &&
	       a->nfh_reserved_1 == b->nfh_reserved_1 &&
	       a->nfh_reserved_2 == b->nfh_reserved_2 &&
	       a->nfh_reserved_3 == b->nfh_reserved_3;
}

static inline bool network_file_handle_empty(struct network_file_handle *nfh)
{
	return nfh->nfh_sb == 0 && nfh->nfh_ino == 0 &&
	       nfh->nfh_reserved_1 == 0 && nfh->nfh_reserved_2 == 0 &&
	       nfh->nfh_reserved_3 == 0;
}

struct network_file_handle *network_file_handle_construct(uint64_t sbi,
							  uint64_t ino);

#endif
