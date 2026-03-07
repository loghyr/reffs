# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -Im4 -I. \
	-I$(top_srcdir)/lib/include \
	-I$(top_builddir)/lib/xdr \
	$(TIRPC_CFLAGS) \
	$(URCU_CFLAGS) \
	$(URCU_CDS_CFLAGS) \
	@CHECK_CFLAGS@

AM_LDFLAGS = -rdynamic \
	$(ASAN_LDFLAGS) $(LSAN_LDFLAGS) \
	$(UBSAN_LDFLAGS) $(TSAN_LDFLAGS)

noinst_LTLIBRARIES = libnlm.la
libnlm_la_SOURCES = nlm_server.c nlm_lock.c

TESTS = nlm_test
check_PROGRAMS = $(TESTS)

nlm_test_SOURCES = tests/nlm_test.c
nlm_test_LDADD = \
	libnlm.la \
	$(top_builddir)/lib/fs/libreffs_fs.la \
	$(top_builddir)/lib/trace/libreffs_trace.la \
	$(top_builddir)/lib/utils/libreffs_utils.la \
	@CHECK_LIBS@ \
	$(TIRPC_LIBS) \
	$(URCU_LIBS) $(URCU_CDS_LIBS) \
	$(ZSTD_LIBS) $(UUID_LIBS) $(FUSE_LIBS) \
	-lpthread -lxxhash
