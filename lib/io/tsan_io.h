/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * TSAN annotations for I/O backend submission/completion synchronization.
 *
 * Backend submission (io_uring submit, aio_*, thread-pool enqueue) acts
 * as a memory barrier between the submitter and the completion handler.
 * TSAN cannot model the kernel- or thread-pool-mediated handoff, so we
 * annotate it explicitly.
 *
 * Usage:
 *   TSAN_RELEASE(ic) after submission succeeds
 *   TSAN_ACQUIRE(ic) in the completion handler after retrieving ic from
 *                    the completion record's user_data
 */

#ifndef _REFFS_TSAN_IO_H
#define _REFFS_TSAN_IO_H

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

#endif /* _REFFS_TSAN_IO_H */
