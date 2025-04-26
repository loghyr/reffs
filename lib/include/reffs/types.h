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
	reffs_life_action_death = 3,
	reffs_life_action_delayed_death = 4,
	reffs_life_action_update = 5
};

#endif /* _REFFS_TYPES_H */
