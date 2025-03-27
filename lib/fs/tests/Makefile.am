# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -Im4 -I.. @CHECK_CFLAGS@ \
	-I$(top_srcdir)/lib/include \
	-I$(top_builddir)/lib/xdr

LDADD = \
	@CHECK_LIBS@ \
	$(top_builddir)/lib/fs/libreffs_fs.la \
	$(top_builddir)/lib/utils/libreffs_utils.la

check_PROGRAMS = $(TESTS)

TESTS = \
	fuse \
	inode_hashing_no_check

#	inode_hashing \
#	fofofo
