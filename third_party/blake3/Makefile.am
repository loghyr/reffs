# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

noinst_LTLIBRARIES = libblake3.la

libblake3_la_SOURCES = \
	blake3.c \
	blake3_dispatch.c \
	blake3_portable.c

libblake3_la_CFLAGS = \
	-DBLAKE3_NO_SSE2 \
	-DBLAKE3_NO_SSE41 \
	-DBLAKE3_NO_AVX2 \
	-DBLAKE3_NO_AVX512 \
	-DBLAKE3_USE_NEON=0

noinst_HEADERS = blake3.h blake3_impl.h

EXTRA_DIST = NOTICE.md
