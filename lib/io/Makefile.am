# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = -Im4 @CHECK_CFLAGS@					\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS) $(FUSE_CFLAGS)	\
	$(TIRPC_CFLAGS) $(ZLIB_CFLAGS) $(HDR_HISTOGRAM_CFLAGS)	\
	$(OPENSSL_CFLAGS) $(ZSTD_CFLAGS)			\
	-I$(top_srcdir)/lib/include				\
	-I$(top_builddir)/lib/xdr

AM_LDFLAGS = -rdynamic

lib_LTLIBRARIES = libreffs_io.la

# Backend-agnostic sources, always compiled.
libreffs_io_la_SOURCES =	\
	conn_info.c		\
	context.c		\
	handlers.c		\
	lsnr.c			\
	net_state.c		\
	tls.c			\
	worker.c

# Exactly one I/O backend is compiled.  See configure.ac: on Linux
# with liburing installed, IO_BACKEND_LIBURING is set and the seven
# io_uring-specific files below are compiled.  On FreeBSD with aio +
# kqueue available, IO_BACKEND_KQUEUE is set and backend_kqueue.c
# is compiled instead.
if IO_BACKEND_LIBURING
libreffs_io_la_SOURCES +=	\
	accept.c		\
	backend.c		\
	connect.c		\
	handler.c		\
	heartbeat.c		\
	read.c			\
	write.c
endif

if IO_BACKEND_KQUEUE
libreffs_io_la_SOURCES += backend_kqueue.c
libreffs_io_la_SOURCES += kqueue_common.c
libreffs_io_la_SOURCES += kqueue_socket.c
endif

if IO_BACKEND_DARWIN
libreffs_io_la_SOURCES += kqueue_common.c
libreffs_io_la_SOURCES += kqueue_socket.c
# backend_darwin.c wired in by later commits (commits 5-7)
# of the macOS backend series.
endif
