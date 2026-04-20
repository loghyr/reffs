# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

# dstore must come before server / client: both LIBADD
# libreffs_dstore.la so its objects get folded into the dylib at
# link time on macOS (see lib/nfs4/server/Makefile.am for rationale).
SUBDIRS = include common dstore server client tests
