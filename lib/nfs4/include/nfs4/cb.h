/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_CB_H
#define _REFFS_NFS4_CB_H

#include <stdbool.h>

#include "nfsv42_xdr.h"

struct compound;
struct nfs4_session;

/*
 * nfs4_cb_recall -- send a CB_RECALL on the back channel, then pause the
 * compound's task.  On resume, compound->c_cb_status holds the CB reply
 * status.
 *
 * On success (return 0) the task has been paused.  The op handler MUST NOT
 * touch the compound, rpc_trans, or task after this returns 0.
 *
 * Returns non-zero (errno) on error; no pause occurs.
 */
int nfs4_cb_recall(struct compound *compound, struct nfs4_session *session,
		   const stateid4 *stateid, const nfs_fh4 *fh, bool truncate);

#endif /* _REFFS_NFS4_CB_H */
