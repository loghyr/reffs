/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TIME_H
#define _REFFS_TIME_H

#include <time.h>

static inline int64_t reffs_timespec_to_ns(struct timespec *ts)
{
	return ts->tv_sec * 1000000000UL + ts->tv_nsec;
}

#endif /* _REFFS_TIME_H */
