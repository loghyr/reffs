/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_OPS_H
#define _REFFS_NFS4_OPS_H

struct compound;

void nfs4_op_access(struct compound *c);
void nfs4_op_close(struct compound *c);
void nfs4_op_commit(struct compound *c);
void nfs4_op_create(struct compound *c);
void nfs4_op_delegpurge(struct compound *c);
void nfs4_op_delegreturn(struct compound *c);
void nfs4_op_getattr(struct compound *c);
void nfs4_op_getfh(struct compound *c);
void nfs4_op_link(struct compound *c);
void nfs4_op_lock(struct compound *c);
void nfs4_op_lockt(struct compound *c);
void nfs4_op_locku(struct compound *c);
void nfs4_op_lookup(struct compound *c);
void nfs4_op_lookupp(struct compound *c);
void nfs4_op_nverify(struct compound *c);
void nfs4_op_open(struct compound *c);
void nfs4_op_openattr(struct compound *c);
void nfs4_op_open_confirm(struct compound *c);
void nfs4_op_open_downgrade(struct compound *c);
void nfs4_op_putfh(struct compound *c);
void nfs4_op_putpubfh(struct compound *c);
void nfs4_op_putrootfh(struct compound *c);
void nfs4_op_read(struct compound *c);
void nfs4_op_readdir(struct compound *c);
void nfs4_op_readlink(struct compound *c);
void nfs4_op_remove(struct compound *c);
void nfs4_op_rename(struct compound *c);
void nfs4_op_renew(struct compound *c);
void nfs4_op_restorefh(struct compound *c);
void nfs4_op_savefh(struct compound *c);
void nfs4_op_secinfo(struct compound *c);
void nfs4_op_setattr(struct compound *c);
void nfs4_op_setclientid(struct compound *c);
void nfs4_op_setclientid_confirm(struct compound *c);
void nfs4_op_verify(struct compound *c);
void nfs4_op_write(struct compound *c);
void nfs4_op_release_lockowner(struct compound *c);
void nfs4_op_backchannel_ctl(struct compound *c);
void nfs4_op_bind_conn_to_session(struct compound *c);
void nfs4_op_exchange_id(struct compound *c);
void nfs4_op_create_session(struct compound *c);
void nfs4_op_destroy_session(struct compound *c);
void nfs4_op_free_stateid(struct compound *c);
void nfs4_op_get_dir_delegation(struct compound *c);
void nfs4_op_getdeviceinfo(struct compound *c);
void nfs4_op_getdevicelist(struct compound *c);
void nfs4_op_layoutcommit(struct compound *c);
void nfs4_op_layoutget(struct compound *c);
void nfs4_op_layoutreturn(struct compound *c);
void nfs4_op_secinfo_no_name(struct compound *c);
void nfs4_op_sequence(struct compound *c);
void nfs4_op_set_ssv(struct compound *c);
void nfs4_op_test_stateid(struct compound *c);
void nfs4_op_want_delegation(struct compound *c);
void nfs4_op_destroy_clientid(struct compound *c);
void nfs4_op_reclaim_complete(struct compound *c);
void nfs4_op_allocate(struct compound *c);
void nfs4_op_copy(struct compound *c);
void nfs4_op_copy_notify(struct compound *c);
void nfs4_op_deallocate(struct compound *c);
void nfs4_op_io_advise(struct compound *c);
void nfs4_op_layouterror(struct compound *c);
void nfs4_op_layoutstats(struct compound *c);
void nfs4_op_offload_cancel(struct compound *c);
void nfs4_op_offload_status(struct compound *c);
void nfs4_op_read_plus(struct compound *c);
void nfs4_op_seek(struct compound *c);
void nfs4_op_write_same(struct compound *c);
void nfs4_op_clone(struct compound *c);
void nfs4_op_getxattr(struct compound *c);
void nfs4_op_setxattr(struct compound *c);
void nfs4_op_listxattrs(struct compound *c);
void nfs4_op_removexattr(struct compound *c);
void nfs4_op_access_mask(struct compound *c);
void nfs4_op_chunk_commit(struct compound *c);
void nfs4_op_chunk_error(struct compound *c);
void nfs4_op_chunk_finalize(struct compound *c);
void nfs4_op_chunk_header_read(struct compound *c);
void nfs4_op_chunk_lock(struct compound *c);
void nfs4_op_chunk_read(struct compound *c);
void nfs4_op_chunk_repaired(struct compound *c);
void nfs4_op_chunk_rollback(struct compound *c);
void nfs4_op_chunk_unlock(struct compound *c);
void nfs4_op_chunk_write(struct compound *c);
void nfs4_op_chunk_write_repair(struct compound *c);
void nfs4_op_illegal(struct compound *c);

const char *nfs4_op_name(nfs_opnum4 op);

#define NFS4_OP_NUM(c)                                  \
	((COMPOUND4args *)(ph)->ph_args)                \
		->argarray.argarray_val[(c)->c_curr_op] \
		.argop

#define NFS4_OP_ARG_SETUP(c, ph, field)                   \
	(&((COMPOUND4args *)(ph)->ph_args)                \
		  ->argarray.argarray_val[(c)->c_curr_op] \
		  .nfs_argop4_u.field)

#define NFS4_OP_RES_SETUP(c, ph, field)                   \
	(&((COMPOUND4res *)(ph)->ph_res)                  \
		  ->resarray.resarray_val[(c)->c_curr_op] \
		  .nfs_resop4_u.field)

#define NFS4_OP_RESOK_SETUP(res, union_field, resok_field) \
	(&(res)->union_field.resok_field)

#endif /* _REFFS_NFS4_OPS_H */
