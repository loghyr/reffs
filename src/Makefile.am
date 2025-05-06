# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

ACLOCAL_AMFLAGS = -Im4 

AM_CFLAGS = \
	-I$(top_srcdir)/lib/include	\
	-I$(top_builddir)/lib/xdr		\
	$(TIRPC_CFLAGS)			\
	@CHECK_CFLAGS@			\
	$(URCU_CFLAGS)			\
	$(URCU_CDS_CFLAGS)		\
	$(FUSE_CFLAGS)			\
	$(ZLIB_CFLAGS)			\
	$(URING_CFLAGS)			\
	$(PTHREAD_CFLAGS)		\
	$(HDR_HISTOGRAM_CFLAGS)		\
	$(JEMALLOC_FLAGS)

LDADD = \
	$(top_builddir)/lib/xdr/libreffs_xdr.la		\
	$(top_builddir)/lib/trace/libreffs_trace.la	\
	$(top_builddir)/lib/io/libreffs_io.la		\
	$(top_builddir)/lib/rpc/libreffs_rpc.la		\
	$(top_builddir)/lib/mount3/libreffs_mount3.la	\
	$(top_builddir)/lib/nfs3/libreffs_nfs3.la	\
	$(top_builddir)/lib/probe1/libreffs_probe1.la	\
	$(top_builddir)/lib/fs/libreffs_fs.la		\
	$(top_builddir)/lib/utils/libreffs_utils.la	\
	$(TIRPC_LIBS)					\
	$(PTHREAD_LIBS)					\
	$(UUID_LIBS)					\
	$(URCU_LIBS)					\
	$(URCU_CDS_LIBS)				\
	$(XXHASH_LIBS)					\
	$(ZLIB_LIBS)					\
	$(URING_LIBS)					\
	$(FUSE_LIBS)					\
	$(HDR_HISTOGRAM_LIBS)				\
	$(JEMALLOC_LIBS)

recho_ev_SOURCES = recho_ev.c
recho_ev_LDADD = $(LDADD)

recho_cl_SOURCES = recho_cl.c
recho_cl_LDADD = $(LDADD)

reffs_SOURCES = reffs.c
reffs_LDADD = $(LDADD)

reffs_fuse_SOURCES = fuse.c
reffs_fuse_LDADD = $(LDADD)

reffs_nfs3_srv_SOURCES = nfs3_server.c
reffs_nfs3_srv_LDADD = $(LDADD)

reffs_probe1_clnt_SOURCES = probe1_client.c
reffs_probe1_clnt_LDADD = $(LDADD)

bin_PROGRAMS =			\
	recho_cl		\
	recho_ev		\
	reffs_fuse		\
	reffs			\
	reffs_nfs3_srv		\
	reffs_probe1_clnt

EXTRA_DIST = asan_nfs3_server.sh asan-suppressions.txt
dist_data_DATA = asan-suppressions.txt
dist_bin_SCRIPTS = asan_nfs3_server.sh

all-local:
	$(INSTALL_DATA) $(top_srcdir)/src/asan-suppressions.txt $(top_builddir)/src/asan-suppressions.txt
	cp $(top_srcdir)/src/asan_nfs3_server.sh $(top_builddir)/src/
	chmod +x $(top_builddir)/src/asan_nfs3_server.sh
