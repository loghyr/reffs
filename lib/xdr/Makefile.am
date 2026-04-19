# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

ACLOCAL_AMFLAGS = -Im4 -I.

AM_CFLAGS = $(TIRPC_CFLAGS)

# FreeBSD's <rpc/xdr.h> references bool_t but does not pull in
# <rpc/types.h> itself (libtirpc on Linux does).  The generated
# XDR headers include <rpc/xdr.h> directly; force-include
# <rpc/types.h> for every .c in this subdir so bool_t is in scope.
if HOST_FREEBSD
AM_CFLAGS += -include rpc/types.h
endif

GEN_SOURCES = \
	ref_cp_xdr.h		\
	ref_cp_xdr.c		\
	reffs_echo_xdr.h	\
	reffs_echo_xdr.c	\
	nfsv42_xdr.h		\
	nfsv42_xdr.c		\
	mntv3_xdr.h		\
	mntv3_xdr.c		\
	nfsv3_xdr.h		\
	nfsv3_xdr.c		\
	nlm_prot.h		\
	nlm_prot.c		\
	nlm4_prot.h		\
	nlm4_prot.c		\
	sm_inter.h		\
	sm_inter.c		\
	probe1_xdr.h		\
	probe1_xdr.c		\
	nfsv42_names.h		\
	nfsv42_names.c

# BUILT_SOURCES ensures all generated headers are created before any
# compilation begins.  Without this, a parallel make -jN from scratch
# can start compiling a .c file before the xdr-parser run that produces
# the included .h has finished.
BUILT_SOURCES = $(GEN_SOURCES)

CLEANFILES = $(GEN_SOURCES)

lib_LTLIBRARIES = libreffs_xdr.la
libreffs_xdr_la_SOURCES =	\
	$(GEN_SOURCES)		\
	ref_cp_xdr.x		\
	reffs_echo_xdr.x	\
	nfsv42_xdr.x		\
	mntv3_xdr.x		\
	nfsv3_xdr.x		\
	nlm_prot.x		\
	nlm4_prot.x		\
	sm_inter.x		\
	rpc_xdr.x

# xdr-parser generates both .h and .c in a single invocation.
# The sentinel .stamp file records the last successful run so make
# rebuilds both outputs when the .x source changes.
%.stamp: %.x
	$(XDR_PARSER) --lang c --output-dir $(builddir) $<
	touch $@

# Individual .h / .c targets depend on the .stamp so parallel make
# (-jN) does not run the generator twice for the same source.
%.h: %.stamp
	@test -f $@ || ($(MAKE) $(AM_MAKEFLAGS) $*.stamp && test -f $@)

%.c: %.stamp
	@test -f $@ || ($(MAKE) $(AM_MAKEFLAGS) $*.stamp && test -f $@)

CLEANFILES += $(GEN_SOURCES:.h=.stamp) $(GEN_SOURCES:.c=.stamp)

# Names table: generated from nfsv42_xdr.x, output prefix is "nfsv42"
# so the files are nfsv42_names.h and nfsv42_names.c.  Use a stamp
# (same as the other XDR files) to prevent parallel make -jN from
# invoking the generator twice when both .h and .c are needed at once.
nfsv42_names.stamp: nfsv42_xdr.x
	$(XDR_PARSER) --names --lang c --prefix nfsv42 \
		--output-dir $(builddir) $(srcdir)/nfsv42_xdr.x
	touch $@

nfsv42_names.h: nfsv42_names.stamp
	@test -f $@ || ($(MAKE) $(AM_MAKEFLAGS) nfsv42_names.stamp && test -f $@)

nfsv42_names.c: nfsv42_names.stamp
	@test -f $@ || ($(MAKE) $(AM_MAKEFLAGS) nfsv42_names.stamp && test -f $@)

CLEANFILES += nfsv42_names.stamp
