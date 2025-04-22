# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -Im4 -I. -I$(top_srcdir)/lib/include

AM_LDFLAGS = -rdynamic

lib_LTLIBRARIES = libreffs_utils.la
libreffs_utils_la_SOURCES =	\
	cmp.c			\
	rcu.c			\
	server.c		\
	trace.c
