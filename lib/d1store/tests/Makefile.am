# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -I$(top_srcdir)/lib/d1store

LDADD = $(top_builddir)/lib/d1store/libd1store.la

# The model's tests report through their exit status and depend on
# nothing outside the model.
TESTS = \
	d1_codec_test \
	d1_envelope_test

check_PROGRAMS = $(TESTS)

d1_codec_test_SOURCES = d1_codec_test.c
d1_envelope_test_SOURCES = d1_envelope_test.c
