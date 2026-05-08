/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * PROXY data backend -- on-demand bytes via the EC pipeline.
 *
 * For super_blocks composed as md=RAM + data=PROXY (proxy SBs in
 * the proxy-server subsystem).  Metadata lives in RAM as a cache
 * of upstream MDS state; data has no on-disk persistence.  Reads
 * route through ec_pipeline (LAYOUTGET + CHUNK_READ + decode);
 * writes are not yet wired (Phase 4).
 *
 * The primary read path for PS Phase 3 is the
 * ps_proxy_pipeline_read() shim called from nfs4_op_read for
 * proxy SBs -- it carries op-handler-shaped concerns (count
 * clamping, NFS4ERR mapping, GSS rejection) that do not belong
 * inside a db_read.  This db_read is wired for completeness so
 * data_block_read() callers (truncate / setattr-size paths) get
 * a coherent answer; for typical NFS4 read traffic on proxy SBs
 * the shim is what fires.
 *
 * See .claude/design/proxy-server-phase3.md for the full slice
 * plan and design rationale.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/backend.h"
#include "reffs/data_block.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/super_block.h"

/*
 * proxy data is a stub -- the SIZE is tracked on db_size by the
 * generic data_block layer (set at db_alloc / db_resize) and the
 * actual bytes live upstream.  No private state needed here yet;
 * future caching slices may add it.
 */

int proxy_data_db_alloc(struct data_block *db __attribute__((unused)),
			struct inode *inode __attribute__((unused)),
			const char *buffer __attribute__((unused)),
			size_t size __attribute__((unused)),
			off_t offset __attribute__((unused)))
{
	/*
	 * No allocation work: db_size and the inode->i_sb proxy
	 * binding fully describe what proxy_db_read needs.  Caller
	 * may pass a non-NULL buffer (e.g. on first WRITE in Phase
	 * 4) -- ignored here; the WRITE path will own its own
	 * pipeline call.
	 */
	return 0;
}

void proxy_data_db_free(struct data_block *db __attribute__((unused)))
{
	/* No private state to free. */
}

ssize_t proxy_data_db_read(struct data_block *db __attribute__((unused)),
			   char *buffer __attribute__((unused)),
			   size_t size __attribute__((unused)),
			   off_t offset __attribute__((unused)))
{
	/*
	 * Phase 3 routes proxy READ through ps_proxy_pipeline_read()
	 * (op-handler shim), which calls ec_read_codec directly with
	 * the proxy SB's session and the file's upstream FH -- that
	 * path bypasses data_block_read entirely.  This stub returns
	 * -ENOSYS so any caller that DOES reach data_block_read on a
	 * proxy SB (e.g. a truncate/SETATTR-size resize that wants to
	 * read pre-existing data) fails loudly rather than silently
	 * returning zeros.
	 *
	 * Phase 3.5 will replace this stub with a proper db_read that
	 * carries forwarded creds.
	 */
	return -ENOSYS;
}

ssize_t proxy_data_db_write(struct data_block *db __attribute__((unused)),
			    const char *buffer __attribute__((unused)),
			    size_t size __attribute__((unused)),
			    off_t offset __attribute__((unused)))
{
	/* Phase 4 territory -- WRITE through pipeline not yet wired. */
	return -ENOSYS;
}

ssize_t proxy_data_db_resize(struct data_block *db, size_t size)
{
	/*
	 * Track the logical size on the generic data_block field;
	 * actual capacity lives upstream and is the MDS's concern.
	 * Used by SETATTR(size) handling.
	 */
	db->db_size = size;
	return 0;
}

size_t proxy_data_db_get_size(struct data_block *db)
{
	return db->db_size;
}

int proxy_data_db_get_fd(struct data_block *db __attribute__((unused)))
{
	/*
	 * No on-disk fd: data lives upstream, fetched via RPC.
	 * io_uring async paths must NOT pick a proxy SB as a target
	 * -- callers should check for db_get_fd < 0 and fall back to
	 * the synchronous RPC path.
	 */
	return -1;
}

void proxy_data_inode_cleanup(struct inode *inode __attribute__((unused)))
{
	/* No on-disk artefacts to remove. */
}
