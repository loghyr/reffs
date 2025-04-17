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

static inline void timespec_to_nfstime3(const struct timespec *ts,
					struct nfstime3 *nt)
{
	if (ts->tv_sec > UINT32_MAX)
		nt->seconds = UINT32_MAX;
	else
		nt->seconds = (uint32_t)ts->tv_sec;

	nt->nseconds = (uint32_t)ts->tv_nsec;
}

static inline void nfstime3_to_timespec(const struct nfstime3 *nt,
					struct timespec *ts)
{
	ts->tv_sec = (time_t)nt->seconds;
	ts->tv_nsec = (long)nt->nseconds;
}

#endif /* _REFFS_TIME_H */
