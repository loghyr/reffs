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
