# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -I$(top_srcdir)/lib/include \
	-I$(top_srcdir)/lib/ec \
	@CHECK_CFLAGS@

LDADD = @CHECK_LIBS@ \
	$(top_builddir)/lib/ec/libreffs_ec.la

check_PROGRAMS = $(TESTS)

TESTS = \
	gf_test \
	matrix_test \
	mojette_test \
	mojette_codec_test \
	rs_test
