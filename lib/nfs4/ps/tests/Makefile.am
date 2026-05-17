# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -I$(top_srcdir)/lib/include		\
	-I$(top_srcdir)/lib/nfs4/client		\
	-I$(top_srcdir)/lib/nfs4/include	\
	-I$(top_srcdir)/lib/nfs4/ps		\
	-I$(top_builddir)/lib/xdr		\
	$(TIRPC_CFLAGS)				\
	@CHECK_CFLAGS@

# Default LDADD covers the full transitive chain because the static
# libreffs_nfs4_ps archive exposes ps_proxy_ops -> ps_state ->
# ps_renewal -> LOG/TRACE/EC, and clang 22 / Fedora 44 strict linking
# (ld.bfd, no lazy resolution for static archives in --as-needed) drags
# the whole chain in once any PS symbol is referenced.  Past attempts
# at a "lean" default broke under every new PS slice; this default is
# the contract.  ps_sb_alloc_test still has an extra CFLAGS-only override
# for fs_test_harness include paths.
LDADD = @CHECK_LIBS@							\
	$(top_builddir)/lib/tests/libreffs_test.la			\
	$(top_builddir)/lib/fs/tests/libreffs_fs_test.la		\
	$(top_builddir)/lib/nfs4/server/libreffs_nfs4_server.la		\
	$(top_builddir)/lib/nfs4/dstore/libreffs_dstore.la		\
	$(top_builddir)/lib/nfs4/ps/libreffs_nfs4_ps_sb.la		\
	$(top_builddir)/lib/nfs4/ps/libreffs_nfs4_ps.la			\
	$(top_builddir)/lib/ec/libreffs_ec.la				\
	$(top_builddir)/lib/nfs4/client/libreffs_nfs4_client.la		\
	$(top_builddir)/lib/tls/libreffs_tls.la				\
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

# ps_sb_alloc_test exercises super_block_alloc + mount inside a live
# listener namespace, so it needs the fs_test_harness include paths
# in addition to the default LDADD chain (which already covers the
# fs/backends/rcu/rpc/io libs).
ps_sb_alloc_test_CFLAGS = $(AM_CFLAGS)					\
	-I$(top_srcdir)/lib/fs/tests					\
	-I$(top_srcdir)/lib/nfs4/include				\
	-I$(top_srcdir)/lib/tests					\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS)				\
	$(UUID_CFLAGS) $(OPENSSL_CFLAGS)				\
	$(URING_CFLAGS) $(HDR_HISTOGRAM_CFLAGS)				\
	$(JEMALLOC_CFLAGS) $(ZLIB_CFLAGS) $(ZSTD_CFLAGS)

# ps_inode_test uses the same fs_test_harness include paths as
# ps_sb_alloc_test.  LDADD comes from the default (no override).
ps_inode_test_CFLAGS = $(ps_sb_alloc_test_CFLAGS)

# ps_shortcircuit_test reaches into the local DS sb via real
# super_block_find / inode_find / data_block_* calls, so it needs
# the same fs_test_harness include paths as ps_sb_alloc_test.
ps_shortcircuit_test_CFLAGS = $(ps_sb_alloc_test_CFLAGS)

check_PROGRAMS = $(TESTS)
LOG_COMPILER = $(top_srcdir)/scripts/timed-test.sh

TESTS = \
	ec_pipeline_dispatch_test \
	ec_pipeline_stripe_test \
	ps_discovery_test \
	ps_inode_test \
	ps_listener_lifecycle_test \
	ps_local_addr_test \
	ps_mount_client_test \
	ps_owner_test \
	ps_proxy_creds_forward_test \
	ps_proxy_kick_test \
	ps_proxy_ops_test \
	ps_proxy_pipeline_commit_test \
	ps_proxy_pipeline_write_test \
	ps_reconnect_test \
	ps_sb_alloc_test \
	ps_sb_test \
	ps_security_test \
	ps_shortcircuit_test \
	ps_state_test \
	ps_write_buffer_rmw_test \
	ps_write_buffer_test \
	ps_write_buffer_verf_test
