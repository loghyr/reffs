/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * TSAN annotations for io_uring synchronization.
 *
 * io_uring submission acts as a kernel-mediated memory barrier between
 * the SQE producer and the CQE consumer.  TSAN cannot model this, so
 * we annotate it explicitly.
 *
 * Usage:
 *   TSAN_RELEASE(ic) after io_uring_submit succeeds
 *   TSAN_ACQUIRE(ic) in CQE handler after loading ic from user_data
 */

#ifndef _REFFS_TSAN_URING_H
#define _REFFS_TSAN_URING_H

#if defined(__SANITIZE_THREAD__) || defined(__has_feature)
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#define TSAN_RELEASE(addr) __tsan_release(addr)
#define TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#endif
#endif

#ifndef TSAN_RELEASE
#define TSAN_RELEASE(addr) ((void)(addr))
#define TSAN_ACQUIRE(addr) ((void)(addr))
#endif

#endif /* _REFFS_TSAN_URING_H */
