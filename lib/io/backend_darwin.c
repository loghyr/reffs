/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * macOS (Darwin) I/O backend.  Thread-pool for file I/O +
 * kqueue EVFILT_READ/WRITE for sockets (shared with
 * lib/io/kqueue_socket.c once extracted by commit 4).
 *
 * Skeleton-only at this commit: file compiles but is not yet in
 * Makefile.am's source list.  Commits 5-6 implement the thread
 * pool + completion pipe; commit 7 wires it into the build.
 *
 * Why a thread pool and not POSIX aio?  Darwin's POSIX aio
 * implementation routes through a small fixed-size kernel
 * thread pool that serializes badly under concurrent load --
 * documented pathology since OS X 10.5.  A userspace thread
 * pool is the idiomatic Darwin pattern (as recommended by
 * Apple's own I/O guidance when not using libdispatch).
 *
 * Why not libdispatch / Grand Central Dispatch?  libdispatch
 * is Apple-only; using it would make this backend non-portable
 * to any future third BSD-like target.  A plain pthread pool
 * is trivially portable.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#if !defined(HAVE_DARWIN_THREADPOOL) && !defined(IO_BACKEND_DARWIN_SKELETON_OK)
#error "backend_darwin.c requires IO_BACKEND_DARWIN to be selected"
#endif

/*
 * Remaining implementation is deferred to commits 5 and 6 of the
 * macOS backend series.  This file is present in commit 3 so the
 * IO_BACKEND_DARWIN conditional wiring in configure.ac and
 * Makefile.am can be tested for non-activation without a missing-
 * file error; the #error fence above ensures it is never actually
 * compiled until commit 7 flips IO_BACKEND_DARWIN on for darwin*.
 */
