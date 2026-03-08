/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include "nfsv42_xdr.h"
#include "reffs/rcu.h"
#include "reffs/rpc.h"
#include "reffs/cmp.h"
#include "reffs/log.h"
#include "reffs/filehandle.h"
#include "reffs/test.h"
#include "reffs/time.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/data_block.h"
#include "reffs/server.h"
#include "reffs/vfs.h"
#include "reffs/identity.h"
#include "reffs/errno.h"
#include "reffs/trace/nfs4_server.h"
#include "errors.h"
#include "ops.h"
#include "nfs4_internal.h"

struct op_dispatch {
	nfs_opnum4 op;
	void (*cb)(struct compound *c);
};

#define OP_MAX (OP_ACCESS_MASK + 1)

struct op_dispatch op_table[OP_MAX] = {
	{ OP_ACCESS, nfs4_op_access },
	{ OP_CLOSE, nfs4_op_close },
	{ OP_COMMIT, nfs4_op_commit },
	{ OP_CREATE, nfs4_op_create },
	{ OP_DELEGPURGE, nfs4_op_delegpurge },
	{ OP_DELEGRETURN, nfs4_op_delegreturn },
	{ OP_GETATTR, nfs4_op_getattr },
	{ OP_GETFH, nfs4_op_getfh },
	{ OP_LINK, nfs4_op_link },
	{ OP_LOCK, nfs4_op_lock },
	{ OP_LOCKT, nfs4_op_lockt },
	{ OP_LOCKU, nfs4_op_locku },
	{ OP_LOOKUP, nfs4_op_lookup },
	{ OP_LOOKUPP, nfs4_op_lookupp },
	{ OP_NVERIFY, nfs4_op_nverify },
	{ OP_OPEN, nfs4_op_open },
	{ OP_OPENATTR, nfs4_op_openattr },
	{ OP_OPEN_CONFIRM, nfs4_op_open_confirm },
	{ OP_OPEN_DOWNGRADE, nfs4_op_open_downgrade },
	{ OP_PUTFH, nfs4_op_putfh },
	{ OP_PUTPUBFH, nfs4_op_putpubfh },
	{ OP_PUTROOTFH, nfs4_op_putrootfh },
	{ OP_READ, nfs4_op_read },
	{ OP_READDIR, nfs4_op_readdir },
	{ OP_READLINK, nfs4_op_readlink },
	{ OP_REMOVE, nfs4_op_remove },
	{ OP_RENAME, nfs4_op_rename },
	{ OP_RENEW, nfs4_op_renew },
	{ OP_RESTOREFH, nfs4_op_restorefh },
	{ OP_SAVEFH, nfs4_op_savefh },
	{ OP_SECINFO, nfs4_op_secinfo },
	{ OP_SETATTR, nfs4_op_setattr },
	{ OP_SETCLIENTID, nfs4_op_setclientid },
	{ OP_SETCLIENTID_CONFIRM, nfs4_op_setclientid_confirm },
	{ OP_VERIFY, nfs4_op_verify },
	{ OP_WRITE, nfs4_op_write },
	{ OP_RELEASE_LOCKOWNER, nfs4_op_release_lockowner },
	{ OP_BACKCHANNEL_CTL, nfs4_op_backchannel_ctl },
	{ OP_BIND_CONN_TO_SESSION, nfs4_op_bind_conn_to_session },
	{ OP_EXCHANGE_ID, nfs4_op_exchange_id },
	{ OP_CREATE_SESSION, nfs4_op_create_session },
	{ OP_DESTROY_SESSION, nfs4_op_destroy_session },
	{ OP_FREE_STATEID, nfs4_op_free_stateid },
	{ OP_GET_DIR_DELEGATION, nfs4_op_get_dir_delegation },
	{ OP_GETDEVICEINFO, nfs4_op_getdeviceinfo },
	{ OP_GETDEVICELIST, nfs4_op_getdevicelist },
	{ OP_LAYOUTCOMMIT, nfs4_op_layoutcommit },
	{ OP_LAYOUTGET, nfs4_op_layoutget },
	{ OP_LAYOUTRETURN, nfs4_op_layoutreturn },
	{ OP_SECINFO_NO_NAME, nfs4_op_secinfo_no_name },
	{ OP_SEQUENCE, nfs4_op_sequence },
	{ OP_SET_SSV, nfs4_op_set_ssv },
	{ OP_TEST_STATEID, nfs4_op_test_stateid },
	{ OP_WANT_DELEGATION, nfs4_op_want_delegation },
	{ OP_DESTROY_CLIENTID, nfs4_op_destroy_clientid },
	{ OP_RECLAIM_COMPLETE, nfs4_op_reclaim_complete },
	{ OP_ALLOCATE, nfs4_op_allocate },
	{ OP_COPY, nfs4_op_copy },
	{ OP_COPY_NOTIFY, nfs4_op_copy_notify },
	{ OP_DEALLOCATE, nfs4_op_deallocate },
	{ OP_IO_ADVISE, nfs4_op_io_advise },
	{ OP_LAYOUTERROR, nfs4_op_layouterror },
	{ OP_LAYOUTSTATS, nfs4_op_layoutstats },
	{ OP_OFFLOAD_CANCEL, nfs4_op_offload_cancel },
	{ OP_OFFLOAD_STATUS, nfs4_op_offload_status },
	{ OP_READ_PLUS, nfs4_op_read_plus },
	{ OP_SEEK, nfs4_op_seek },
	{ OP_WRITE_SAME, nfs4_op_write_same },
	{ OP_CLONE, nfs4_op_clone },
	{ OP_GETXATTR, nfs4_op_getxattr },
	{ OP_SETXATTR, nfs4_op_setxattr },
	{ OP_LISTXATTRS, nfs4_op_listxattrs },
	{ OP_REMOVEXATTR, nfs4_op_removexattr },
	{ OP_ACCESS_MASK, nfs4_op_access_mask },
};

void dispatch_compound(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	COMPOUND4args *args = ((COMPOUND4args *)ph->ph_args);
	COMPOUND4res *res = ((COMPOUND4res *)ph->ph_res);

	for (c->c_curr_op = 0; c->c_curr_op < args->argarray.argarray_len;
	     c->c_curr_op++) {
		nfs_argop4 *argop = &args->argarray.argarray_val[c->c_curr_op];
		nfs_resop4 *resop = &res->resarray.resarray_val[c->c_curr_op];
		bool found = false;

		for (int i = 0; i < OP_MAX; i++) {
			if (op_table[i].op == argop->argop) {
				op_table[i].cb(c);
				found = true;
				break;
			}
		}

		if (!found) {
			nfs4_op_illegal(c);
		}

		if (resop->nfs_resop4_u.opillegal.status) {
			res->status = resop->nfs_resop4_u.opillegal.status;
			res->resarray.resarray_len = c->c_curr_op + 1;
			return;
		}
	}

	res->status = NFS4_OK;
}
