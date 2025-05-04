# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -Im4 @CHECK_CFLAGS@					\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS) $(FUSE_CFLAGS)	\
	$(TIRPC_CFLAGS) $(ZLIB_CFLAGS)				\
	-I$(top_srcdir)/lib/include				\
	-I$(top_builddir)/lib/xdr

AM_LDFLAGS = -rdynamic

lib_LTLIBRARIES = libreffs_io.la
libreffs_io_la_SOURCES =	\
	accept.c		\
	connect.c		\
	context.c		\
	handler.c		\
	heartbeat.c		\
	lsnr.c			\
	read.c			\
	worker.c		\
	write.c
