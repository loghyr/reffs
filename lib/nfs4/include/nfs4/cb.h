/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_CB_H
#define _REFFS_NFS4_CB_H

#include <stdbool.h>

#include "nfsv42_xdr.h"

struct nfs4_session;

/*
 * nfs4_cb_recall -- send a CB_COMPOUND [CB_SEQUENCE, CB_RECALL] on the back
 * channel, fire-and-forget.
 *
 * Does not pause the compound.  Callers must return NFS4ERR_DELAY after this
 * call; the delegation return arrives separately as a DELEGRETURN on the
 * fore channel.
 *
 * Returns 0 on success, errno if the write could not be submitted.
 */
int nfs4_cb_recall(struct nfs4_session *session, const stateid4 *stateid,
		   const nfs_fh4 *fh, bool truncate);

#endif /* _REFFS_NFS4_CB_H */
