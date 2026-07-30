# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -I$(top_srcdir)/lib/include \
	-I$(top_srcdir)/lib/ec \
	@CHECK_CFLAGS@

LDADD = @CHECK_LIBS@ \
	$(top_builddir)/lib/ec/libreffs_ec.la

check_PROGRAMS = $(TESTS)
LOG_COMPILER = $(top_srcdir)/scripts/timed-test.sh

# snapraid_test and isa_l_test depend on encoders that are only
# compiled with --enable-private-encodings; see lib/ec/Makefile.am.
if HAVE_PRIVATE_ENCODINGS
PRIVATE_TESTS = snapraid_test isa_l_test
else
PRIVATE_TESTS =
endif

TESTS = \
	gf_test \
	matrix_test \
	mirror_test \
	mojette_test \
	mojette_encoding_test \
	rs_test \
	xor_test \
	linux_md_test \
	$(PRIVATE_TESTS)
