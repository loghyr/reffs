/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_RCU_H
#define _REFFS_RCU_H

#include <string.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

void reffs_string_release(char *s);

#endif /* _REFFS_RCU_H */
