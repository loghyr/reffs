/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_ERRORS_H
#define _REFFS_ERRORS_H

#include "nfsv42_xdr.h"
#include <stdbool.h>

bool nfs4_error_valid_for_op(enum nfs_opnum4 op, enum nfsstat4 stat);
bool nfs4_error_valid_for_cb_op(enum nfs_cb_opnum4 op, enum nfsstat4 stat);

#endif /* _REFFS_ERRORS_H */
