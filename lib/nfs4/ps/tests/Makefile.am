# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -I$(top_srcdir)/lib/include		\
	-I$(top_srcdir)/lib/nfs4/client		\
	-I$(top_srcdir)/lib/nfs4/ps		\
	-I$(top_builddir)/lib/xdr		\
	$(TIRPC_CFLAGS)				\
	@CHECK_CFLAGS@

LDADD = @CHECK_LIBS@						\
	$(top_builddir)/lib/nfs4/ps/libreffs_nfs4_ps.la		\
	$(top_builddir)/lib/nfs4/client/libreffs_nfs4_client.la	\
	$(top_builddir)/lib/xdr/libreffs_xdr.la			\
	$(TIRPC_LIBS)

# ps_sb_alloc_test exercises super_block_alloc + mount inside a live
# listener namespace, so it needs the full fs/backends/rcu/rpc/io dep
# graph plus fs_test_harness.  This is the only PS test that pays
# that cost -- the default LDADD above stays lean for everyone else.
ps_sb_alloc_test_CFLAGS = $(AM_CFLAGS)					\
	-I$(top_srcdir)/lib/fs/tests					\
	-I$(top_srcdir)/lib/nfs4/include				\
	-I$(top_srcdir)/lib/tests					\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS)				\
	$(UUID_CFLAGS) $(OPENSSL_CFLAGS)				\
	$(URING_CFLAGS) $(HDR_HISTOGRAM_CFLAGS)				\
	$(JEMALLOC_CFLAGS) $(ZLIB_CFLAGS) $(ZSTD_CFLAGS)
ps_sb_alloc_test_LDADD = @CHECK_LIBS@					\
	$(top_builddir)/lib/tests/libreffs_test.la			\
	$(top_builddir)/lib/fs/tests/libreffs_fs_test.la		\
	$(top_builddir)/lib/nfs4/server/libreffs_nfs4_server.la		\
	$(top_builddir)/lib/nfs4/dstore/libreffs_dstore.la		\
	$(top_builddir)/lib/nfs4/ps/libreffs_nfs4_ps_sb.la		\
	$(top_builddir)/lib/nfs4/ps/libreffs_nfs4_ps.la			\
	$(top_builddir)/lib/nfs4/client/libreffs_nfs4_client.la		\
	$(top_builddir)/lib/nlm/libnlm.la				\
	$(top_builddir)/lib/nsm/libnsm.la				\
	$(top_builddir)/lib/fs/libreffs_fs.la				\
	$(top_builddir)/lib/rpc/libreffs_rpc.la				\
	$(top_builddir)/lib/io/libreffs_io.la				\
	$(top_builddir)/lib/utils/libreffs_utils.la			\
	$(top_builddir)/lib/trace/libreffs_trace.la			\
	$(top_builddir)/lib/backends/libbackends.la			\
	$(top_builddir)/lib/xdr/libreffs_xdr.la				\
	-lxxhash -lpthread						\
	$(URCU_LIBS) $(URCU_CDS_LIBS) $(FUSE_LIBS)			\
	$(TIRPC_LIBS) $(UUID_LIBS) $(OPENSSL_LIBS)			\
	$(URING_LIBS) $(ZLIB_LIBS) $(ZSTD_LIBS)				\
	$(HDR_HISTOGRAM_LIBS) $(JEMALLOC_LIBS)

# ps_inode_test uses the same fs_test_harness + real-SB setup as
# ps_sb_alloc_test; mirror the CFLAGS / LDADD overrides so its
# link resolves against the full fs / backends / rcu chain.
ps_inode_test_CFLAGS = $(ps_sb_alloc_test_CFLAGS)
ps_inode_test_LDADD = $(ps_sb_alloc_test_LDADD)

check_PROGRAMS = $(TESTS)
LOG_COMPILER = $(top_srcdir)/scripts/timed-test.sh

TESTS = \
	ec_pipeline_stripe_test \
	ps_discovery_test \
	ps_inode_test \
	ps_mount_client_test \
	ps_proxy_creds_forward_test \
	ps_proxy_ops_test \
	ps_sb_alloc_test \
	ps_sb_test \
	ps_state_test
