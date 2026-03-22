# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

ACLOCAL_AMFLAGS = -Im4 

AM_CFLAGS = \
	-I$(top_srcdir)/lib/include	\
	-I$(top_builddir)/lib/xdr	\
	-I$(top_srcdir)/third_party/tomlc99	\
	$(TIRPC_CFLAGS)			\
	@CHECK_CFLAGS@			\
	$(URCU_CFLAGS)			\
	$(URCU_CDS_CFLAGS)		\
	$(FUSE_CFLAGS)			\
	$(ZLIB_CFLAGS)			\
	$(URING_CFLAGS)			\
	$(PTHREAD_CFLAGS)		\
	$(OPENSSL_CFLAGS)		\
	$(HDR_HISTOGRAM_CFLAGS)		\
	$(ZSTD_CFLAGS)			\
	$(JEMALLOC_FLAGS)

LDADD = \
	$(top_builddir)/lib/nfs4/libreffs_nfs4.la	\
	$(top_builddir)/lib/nfs3/libreffs_nfs3.la	\
	$(top_builddir)/lib/mds/libreffs_mds.la		\
	$(top_builddir)/lib/mount3/libreffs_mount3.la	\
	$(top_builddir)/lib/nlm/libnlm.la		\
	$(top_builddir)/lib/nsm/libnsm.la		\
	$(top_builddir)/lib/probe1/libreffs_probe1.la	\
	$(top_builddir)/lib/fs/libreffs_fs.la		\
	$(top_builddir)/lib/rpc/libreffs_rpc.la		\
	$(top_builddir)/lib/io/libreffs_io.la		\
	$(top_builddir)/lib/config/libreffs_config.la	\
	$(top_builddir)/lib/utils/libreffs_utils.la	\
	$(top_builddir)/lib/trace/libreffs_trace.la	\
	$(top_builddir)/lib/backends/libbackends.la	\
	$(top_builddir)/lib/xdr/libreffs_xdr.la		\
	$(TIRPC_LIBS)					\
	$(PTHREAD_LIBS)					\
	$(UUID_LIBS)					\
	$(URCU_LIBS)					\
	$(URCU_CDS_LIBS)				\
	$(XXHASH_LIBS)					\
	$(ZLIB_LIBS)					\
	$(URING_LIBS)					\
	$(FUSE_LIBS)					\
	$(OPENSSL_LIBS)					\
	$(HDR_HISTOGRAM_LIBS)				\
	$(ZSTD_LIBS)					\
	$(JEMALLOC_LIBS)

reffsd_SOURCES = reffsd.c
reffsd_LDADD = $(LDADD)

reffs_probe1_clnt_SOURCES = probe1_client.c
reffs_probe1_clnt_LDADD = $(LDADD)

bin_PROGRAMS = \
	reffsd \
	reffs_probe1_clnt

EXTRA_DIST = asan_reffsd.sh asan-suppressions.txt
dist_data_DATA = asan-suppressions.txt
dist_bin_SCRIPTS = asan_reffsd.sh

all-local:
	@if [ "$(top_srcdir)" != "$(top_builddir)" ]; then \
		$(INSTALL_DATA) $(top_srcdir)/src/asan-suppressions.txt $(top_builddir)/src/asan-suppressions.txt; \
		cp $(top_srcdir)/src/asan_reffsd.sh $(top_builddir)/src/; \
		chmod +x $(top_builddir)/src/asan_reffsd.sh; \
	fi
