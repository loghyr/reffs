# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

SUBDIRS = tests

AM_CFLAGS = -Im4 -I. \
	-I$(top_srcdir)/lib/include

lib_LTLIBRARIES = libreffs_fs.la
libreffs_fs_la_SOURCES = \
	data_block.c \
	dirent.c \
	fs.c \
	fuse.c \
	inode.c \
	super_block.c
