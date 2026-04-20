/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Darwin-only compatibility shims for base-libc SunRPC features
 * that Linux libtirpc and FreeBSD base libc both provide but
 * macOS omits.  Force-included on Darwin builds via configure.ac's
 * -include CPPFLAGS entry so every TU sees the shim automatically.
 *
 * Currently shims:
 *   - xdr_sizeof(xdrproc_t, void *): counting-stream XDR that
 *     accumulates the serialized byte count instead of writing.
 *
 * Future shims that need Darwin workarounds should land here.
 */

#ifndef _REFFS_DARWIN_RPC_COMPAT_H
#define _REFFS_DARWIN_RPC_COMPAT_H

#ifdef __APPLE__

#include <rpc/rpc.h>
#include <rpc/xdr.h>

/*
 * SunRPC identifier aliases.  Linux libtirpc (and FreeBSD base libc)
 * provide modern names; Darwin's base-libc SunRPC ships only the
 * historical pre-RFC-5531 names or omits constants entirely.  These
 * aliases let reffs source -- including generated XDR files -- use
 * the modern spellings unchanged.  Previously lived as CPPFLAGS -D
 * macros in configure.ac; moved here for source-visibility (see #24).
 */
#define xdr_uint32_t xdr_u_int32_t
#define xdr_uint64_t xdr_u_int64_t
#ifndef RPCSEC_GSS
#define RPCSEC_GSS 6 /* RFC 2203 */
#endif
#ifndef AUTH_SYS
#define AUTH_SYS AUTH_UNIX /* RFC 5531 modern name */
#endif
#ifndef AUTH_DH
#define AUTH_DH 3 /* RFC 5531 */
#endif
#define authsys_create authunix_create

static inline bool_t reffs_xdr_count_putlong(XDR *xdrs, const int *lp)
{
	(void)lp;
	xdrs->x_handy += 4;
	return TRUE;
}

static inline bool_t reffs_xdr_count_putbytes(XDR *xdrs, const char *addr,
					      unsigned int len)
{
	(void)addr;
	/*
	 * Just accumulate raw len.  xdr_opaque / xdr_bytes already
	 * call x_putbytes TWICE -- once for the payload, once for
	 * the XDR zero-padding up to the 4-byte boundary.  Rounding
	 * `len` up here too would double-count the padding and
	 * over-estimate the reply size (manifests as the rpc.c
	 * assertion `rt_offset == rt_reply_len`).  Matches the
	 * FreeBSD libc xdr_sizeof.c implementation.
	 */
	xdrs->x_handy += len;
	return TRUE;
}

static inline unsigned int reffs_xdr_count_getpostn(XDR *xdrs)
{
	return xdrs->x_handy;
}

static inline bool_t reffs_xdr_count_setpostn(XDR *xdrs, unsigned int pos)
{
	xdrs->x_handy = pos;
	return TRUE;
}

static inline int32_t *reffs_xdr_count_inline(XDR *xdrs, unsigned int len)
{
	(void)xdrs;
	(void)len;
	/* Force the generic x_putbytes slow path so we count correctly. */
	return NULL;
}

static inline void reffs_xdr_count_destroy(XDR *xdrs)
{
	(void)xdrs;
}

static inline bool_t reffs_xdr_count_control(XDR *xdrs, int req, void *info)
{
	(void)xdrs;
	(void)req;
	(void)info;
	return FALSE;
}

/*
 * Darwin declares `struct xdr_ops` nested inside the XDR typedef,
 * which can trip up file-scope static initialization in some
 * toolchains.  FreeBSD's libc xdr_sizeof implementation works
 * around this by building the xdr_ops struct on the stack and
 * filling fields individually -- use the same pattern here.
 */
static inline unsigned long xdr_sizeof(xdrproc_t fn, void *obj)
{
	XDR x;
	struct xdr_ops ops;

	ops.x_getlong = NULL;
	ops.x_putlong = reffs_xdr_count_putlong;
	ops.x_getbytes = NULL;
	ops.x_putbytes = reffs_xdr_count_putbytes;
	ops.x_getpostn = reffs_xdr_count_getpostn;
	ops.x_setpostn = reffs_xdr_count_setpostn;
	ops.x_inline = reffs_xdr_count_inline;
	ops.x_destroy = reffs_xdr_count_destroy;
	ops.x_control = reffs_xdr_count_control;

	x.x_op = XDR_ENCODE;
	x.x_ops = &ops;
	x.x_public = NULL;
	x.x_private = NULL;
	x.x_base = NULL;
	x.x_handy = 0;
	if (!fn(&x, obj, 0))
		return 0;
	return x.x_handy;
}

#endif /* __APPLE__ */

#endif /* _REFFS_DARWIN_RPC_COMPAT_H */
