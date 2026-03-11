# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0

GEN_SOURCES =			\
	gss_const.py		\
	gss_pack.py		\
	gss_type.py		\
	rpc_const.py		\
	rpc_pack.py		\
	rpc_type.py		\
	rpclib.py		\
	rpc.py			\
	security.py

CLEANFILES = $(GEN_SOURCES)

reffs_PYTHON =			\
	__init__.py		\
	$(GEN_SOURCES)

gss_const.py gss_pack.py gss_type.py: $(top_srcdir)/scripts/reffs/pynfs/rpc/gss.x
	$(top_builddir)/scripts/reffs/pynfs/xdr/xdrgen.py $(top_srcdir)/scripts/reffs/pynfs/rpc/gss.x

rpc_const.py rpc_pack.py rpc_type.py: $(top_srcdir)/scripts/reffs/pynfs/rpc/rpc.x
	$(top_builddir)/scripts/reffs/pynfs/xdr/xdrgen.py $(top_srcdir)/scripts/reffs/pynfs/rpc/rpc.x

reffsdir = ${pythondir}/reffs/pynfs/rpc

all: gss_const.py rpc_const.py
