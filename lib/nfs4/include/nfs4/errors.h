/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_ERRORS_H
#define _REFFS_ERRORS_H

#include "nfsv42_xdr.h"
#include <errno.h>
#include <stdbool.h>

#include "reffs/errno.h"
#include "reffs/utf8string.h"

bool nfs4_error_valid_for_op(enum nfs_opnum4 op, enum nfsstat4 stat);
bool nfs4_error_valid_for_cb_op(enum nfs_cb_opnum4 op, enum nfsstat4 stat);

nfsstat4 errno_to_nfs4(int error, nfs_opnum4 op);

/*
 * Map an upstream nfsstat4 to a local negative errno suitable for
 * returning from a forwarder primitive.  The intent is round-trip
 * preservation: feeding the result back through errno_to_nfs4()
 * yields the original status for codes that have a clean errno
 * counterpart (NFS4ERR_ACCESS <-> -EACCES, NFS4ERR_NOENT <-> -ENOENT,
 * etc.).  Codes without a clean errno collapse to -EREMOTEIO so
 * callers see "remote-side problem we don't have a specific code
 * for" rather than zero.
 *
 * Returns 0 for NFS4_OK.  Always returns negative for any non-OK
 * status.
 *
 * Inlined in the header so PS tests don't need to pull in
 * libreffs_nfs4_server just to satisfy the link; the function is
 * a pure switch over the wire enum, no state, no side effects.
 */
static inline int nfs4_to_errno(nfsstat4 status)
{
	switch (status) {
	case NFS4_OK:
		return 0;
	case NFS4ERR_PERM:
		return -EPERM;
	case NFS4ERR_NOENT:
		return -ENOENT;
	case NFS4ERR_IO:
		return -EIO;
	case NFS4ERR_NXIO:
		return -ENXIO;
	case NFS4ERR_ACCESS:
		return -EACCES;
	case NFS4ERR_EXIST:
		return -EEXIST;
	case NFS4ERR_XDEV:
		return -EXDEV;
	case NFS4ERR_NOTDIR:
		return -ENOTDIR;
	case NFS4ERR_ISDIR:
		return -EISDIR;
	case NFS4ERR_INVAL:
		return -EINVAL;
	case NFS4ERR_FBIG:
		return -EFBIG;
	case NFS4ERR_NOSPC:
		return -ENOSPC;
	case NFS4ERR_ROFS:
		return -EROFS;
	case NFS4ERR_MLINK:
		return -EMLINK;
	case NFS4ERR_NAMETOOLONG:
		return -ENAMETOOLONG;
	case NFS4ERR_NOTEMPTY:
		return -ENOTEMPTY;
	case NFS4ERR_DQUOT:
		return -EDQUOT;
	case NFS4ERR_STALE:
		return -ESTALE;
	case NFS4ERR_BADHANDLE:
		return -EBADHANDLE;
	case NFS4ERR_NOTSUPP:
		return -ENOTSUP;
	case NFS4ERR_BAD_STATEID:
		return -EBADSTATEID;
	case NFS4ERR_STALE_STATEID:
		return -ESTALESTATEID;
	case NFS4ERR_OLD_STATEID:
		return -EOLDSTATEID;
	case NFS4ERR_EXPIRED:
		return -EEXPIREDSTATEID;
	default:
		return -EREMOTEIO;
	}
}

/*
 * Validate a wire path component and map to the appropriate NFS4 error.
 * Returns NFS4_OK (0) on success.
 */
static inline nfsstat4 nfs4_validate_component(const utf8string *name)
{
	int ret = utf8string_validate_component_default(name);

	switch (ret) {
	case 0:
		return NFS4_OK;
	case -ENAMETOOLONG:
		return NFS4ERR_NAMETOOLONG;
	case -EBADNAME:
		return NFS4ERR_BADNAME;
	default:
		return NFS4ERR_INVAL;
	}
}

#endif /* _REFFS_ERRORS_H */
