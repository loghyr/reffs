# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

# The D1 storage model is a standalone userspace model.  It links
# against nothing in reffs and nothing in reffs links against it; it is
# built here so that the repository's own check gate runs its tests.

SUBDIRS = tests

AM_CFLAGS = -I$(srcdir)

noinst_LTLIBRARIES = libd1store.la
libd1store_la_SOURCES = \
	d1_codec.c \
	d1_digest.c

noinst_HEADERS = \
	d1_codec.h \
	d1_digest.h \
	d1_types.h
