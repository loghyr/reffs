/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_MEMORY_H
#define _REFFS_MEMORY_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void *memdup(const void *src, size_t n)
{
	if (!src || n == 0)
		return NULL;

	void *dst = malloc(n);
	if (!dst)
		return NULL;

	memcpy(dst, src, n);
	return dst;
}

#endif /* _REFFS_MEMORY_H */
