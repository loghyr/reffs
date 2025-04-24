# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

SUBDIRS = tests

AM_CFLAGS = -Im4 -I. \
	-I$(top_srcdir)/lib/include	\
	-I$(top_builddir)/lib/xdr	\
	$(PTHREAD_CFLAGS)		\
	$(URCU_CFLAGS)			\
	$(URCU_CDS_CFLAGS)		\
	$(TIRPC_CFLAGS)			\
	$(FUSE_CFLAGS)

lib_LTLIBRARIES = libreffs_fs.la
libreffs_fs_la_SOURCES = \
	data_block.c \
	dirent.c \
	fs.c \
	fuse.c \
	inode.c \
	ns.c \
	super_block.c
