# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = -Im4 -I. \
	-I$(top_srcdir)/lib/include	\
	-I$(top_builddir)/lib/xdr	\
	$(PTHREAD_CFLAGS)		\
	$(URCU_CFLAGS)			\
	$(URCU_CDS_CFLAGS)		\
	$(TIRPC_CFLAGS)			\
	$(OPENSSL_CFLAGS)		\
	$(ZSTD_CFLAGS)			\
	$(FUSE_CFLAGS)

lib_LTLIBRARIES = libreffs_fs.la
libreffs_fs_la_SOURCES = \
	client.c \
	client_match.c \
	data_block.c \
	dirent.c \
	evictor.c \
	filehandle.c \
	fs.c \
	fuse.c \
	inode.c \
	layout_segment.c \
	lock.c \
	ns.c \
	sb_registry.c \
	stateid.c \
	super_block.c \
	vfs.c

libreffs_fs_la_LIBADD = $(top_builddir)/lib/backends/libbackends.la
