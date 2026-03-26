# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 -I. \
	-I$(top_srcdir)/lib/include \
	-I$(top_srcdir)/lib/fs/tests \
	-I$(top_srcdir)/lib/tests \
	-I$(top_builddir)/lib/xdr \
	$(TIRPC_CFLAGS) \
	$(URCU_CFLAGS) \
	$(URCU_CDS_CFLAGS) \
	@CHECK_CFLAGS@

AM_LDFLAGS = -rdynamic \
	$(ASAN_LDFLAGS) $(LSAN_LDFLAGS) \
	$(UBSAN_LDFLAGS) $(TSAN_LDFLAGS)

check_PROGRAMS = $(TESTS)
LOG_COMPILER = $(top_srcdir)/scripts/timed-test.sh

TESTS = nlm_test

nlm_test_SOURCES = nlm_test.c
nlm_test_LDADD = \
	$(top_builddir)/lib/tests/libreffs_test.la \
	$(top_builddir)/lib/fs/tests/libreffs_fs_test.la \
	$(top_builddir)/lib/nlm/libnlm.la \
	$(top_builddir)/lib/fs/libreffs_fs.la \
	$(top_builddir)/lib/trace/libreffs_trace.la \
	$(top_builddir)/lib/utils/libreffs_utils.la \
	@CHECK_LIBS@ \
	$(TIRPC_LIBS) \
	$(URCU_LIBS) $(URCU_CDS_LIBS) \
	$(ZSTD_LIBS) $(UUID_LIBS) $(FUSE_LIBS) \
	-lpthread -lxxhash
