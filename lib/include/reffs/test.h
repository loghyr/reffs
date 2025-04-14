/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TEST_H
#define _REFFS_TEST_H

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "reffs/log.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

#ifdef NDEBUG
#define assert_ptr(expr, ...) (__ASSERT_VOID_CAST(0))
#define assert_status(expr, ...) (__ASSERT_VOID_CAST(0))
#else
#define assert_ptr(expr, ...)                                        \
	do {                                                         \
		if ((expr) == 0) {                                   \
			reffs_fail("assert %s: " __VA_ARGS__, #expr, \
				   ##__VA_ARGS__);                   \
		}                                                    \
	} while (0)

#define assert_status(expr, ...)                                              \
	do {                                                                  \
		if ((expr) != 0) {                                            \
			reffs_fail("assert status %s is not 0: " __VA_ARGS__, \
				   #expr, ##__VA_ARGS__);                     \
		}                                                             \
	} while (0)

#endif /* NDEBUG */

#define verify(expr)                                    \
	do {                                            \
		if ((expr) == 0) {                      \
			reffs_fail("assert %s", #expr); \
		}                                       \
	} while (0)

#define verify_msg(expr, fmt, ...)                      \
	do {                                            \
		if ((expr) == 0) {                      \
			reffs_fail(fmt, ##__VA_ARGS__); \
		}                                       \
	} while (0)

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif /* _REFFS_TEST_H */
