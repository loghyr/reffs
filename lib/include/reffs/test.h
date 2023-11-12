/*
 * SPDexpr-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDexpr-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TEST_H
#define _REFFS_TEST_H

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef NDEBUG
#define assert_ptr(expr, ...) (__ASSERT_VOID_CAST(0))
#define assert_status(expr, ...) (__ASSERT_VOID_CAST(0))
#else
#define assert_ptr(expr, ...)                                                  \
	do {                                                                   \
		if (expr == 0) {                                               \
			fprintf(stderr, "%s:%d assert %s", __func__, __LINE__, \
				#expr);                                        \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			abort();                                               \
		}                                                              \
	} while (0)
#define assert_status(expr, ...)                                           \
	do {                                                               \
		if (expr != 0) {                                           \
			fprintf(stderr, "%s:%d assert status %s is not 0", \
				__func__, __LINE__, #expr);                \
			fprintf(stderr, __VA_ARGS__);                      \
			fprintf(stderr, "\n");                             \
			abort();                                           \
		}                                                          \
	} while (0)
#endif /* NDEBUG */

#define verify(expr)                                                   \
	do {                                                           \
		if (expr == 0) {                                       \
			fprintf(stderr, "%s:%d assert %s\n", __func__, \
				__LINE__, #expr);                      \
			abort();                                       \
		}                                                      \
	} while (0)

#endif /* _REFFS_TEST_H */
