# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = @CHECK_CFLAGS@			\
	-I$(top_srcdir)/lib/include		\
	$(ZSTD_CFLAGS)

AM_LDFLAGS = -rdynamic				\
	$(ASAN_LDFLAGS) $(LSAN_LDFLAGS)		\
	$(UBSAN_LDFLAGS) $(TSAN_LDFLAGS)

LDADD = \
	@CHECK_LIBS@					\
	$(top_builddir)/lib/trace/libreffs_trace.la	\
	$(ZSTD_LIBS)					\
	-lpthread

check_PROGRAMS = $(TESTS)
LOG_COMPILER = $(top_srcdir)/scripts/timed-test.sh

TESTS = trace_archive_test
