/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "reffs/cmp.h"

static enum reffs_text_case reffs_rtc = reffs_text_case_sensitive;

void reffs_case_set(enum reffs_text_case rtc)
{
	reffs_rtc = rtc;
}

enum reffs_text_case reffs_case_get(void)
{
	return reffs_rtc;
}

reffs_strng_compare reffs_text_case_cmp(void)
{
	if (reffs_rtc == reffs_text_case_insensitive)
		return strcasecmp;
	return strcmp;
}

reffs_strng_compare reffs_text_case_cmp_of(enum reffs_text_case rtc)
{
	if (rtc == reffs_text_case_insensitive)
		return strcasecmp;
	return strcmp;
}
