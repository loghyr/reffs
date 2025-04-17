/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_FILEHANDLE_H
#define _REFFS_FILEHANDLE_H

#include <stdint.h>

#define FILEHANDLE_VERSION_1 (1)
#define FILEHANDLE_VERSION_CURR (1)

struct network_file_handle {
	uint16_t nfh_version;
	uint16_t nfh_reserved_1;
	uint16_t nfh_reserved_2;
	uint16_t nfh_reserved_3;
	uint64_t nfh_fsid;
	uint64_t nfh_ino;
};

#endif
