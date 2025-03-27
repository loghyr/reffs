# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

reffs_PYTHON = \
	__init__.py \
	reffs_echo_xdr_const.py \
	reffs_echo_xdr_type.py \
	reffs_echo_xdr_pack.py \
	rpc_xdr_const.py \
	rpc_xdr_type.py \
	rpc_xdr_pack.py

reffs_echo_xdr_const.py reffs_echo_xdr_type.py and reffs_echo_xdr_pack.py: $(top_srcdir)/lib/xdr/reffs_echo_xdr.x
	$(top_builddir)/lib/scripts/reffs/xdrgen.py $(top_srcdir)/lib/xdr/reffs_echo_xdr.x

rpc_xdr_const.py rpc_xdr_type.py rpc_xdr_pack.py: $(top_srcdir)/lib/xdr/rpc_xdr.x
	$(top_builddir)/lib/scripts/reffs/xdrgen.py $(top_srcdir)/lib/xdr/rpc_xdr.x

reffsdir = ${pythondir}/reffs

all: reffs_echo_xdr_const.py rpc_xdr_const.py
