# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

# Order is important!
# xdr at the front and tests at the end
# nfs4 must come before nfs3 / probe1: both LIBADD the
# libreffs_dstore.la convenience archive (which lives under
# nfs4/dstore/) so the static archive's objects get folded into the
# dylib on macOS (see lib/nfs4/server/Makefile.am for rationale).
SUBDIRS =		\
	xdr		\
	include		\
	trace		\
	backends	\
	utils		\
	config		\
	ec		\
	io		\
	rpc		\
	tls		\
	tests		\
	fs		\
	nlm		\
	nsm		\
	mount3		\
	nfs4		\
	nfs3		\
	probe1
