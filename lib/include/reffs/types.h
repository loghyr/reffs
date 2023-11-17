/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TYPES_H
#define _REFFS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

enum reffs_life_action {
	reffs_life_action_birth = 0,
	reffs_life_action_load = 1,
	reffs_life_action_unload = 2,
	reffs_life_action_death = 3
};

enum reffs_text_case {
	reffs_text_case_insensitive = 0,
	reffs_text_case_sensitive = 1,
};

// Right place for now?
typedef int (*reffs_strng_compare)(const char *s1, const char *s2);

#endif /* _REFFS_TYPES_H */
