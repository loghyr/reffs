/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_CMP_H
#define _REFFS_CMP_H

#include <string.h>

enum reffs_text_case {
	reffs_text_case_insensitive = 0,
	reffs_text_case_sensitive = 1,
};

// Right place for now?
typedef int (*reffs_strng_compare)(const char *s1, const char *s2);

void reffs_case_set(enum reffs_text_case rtc);
enum reffs_text_case reffs_case_get(void);
reffs_strng_compare reffs_text_case_cmp(void);
reffs_strng_compare reffs_text_case_cmp_of(enum reffs_text_case);

#endif /* _REFFS_CMP_H */
