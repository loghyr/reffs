/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_OPS_H
#define _REFFS_NFS4_OPS_H

struct client;
struct compound;
struct server_state;

/*
 * NFSv4 op return flags.
 *
 * Ops return 0 for synchronous completion.  If the op went async
 * (called task_pause), it sets NFS4_OP_FLAG_ASYNC.  The caller must
 * NOT touch the compound, rpc_trans, or task after this flag is set.
 */
#define NFS4_OP_FLAG_ASYNC (1u << 0)
#define NFS4_OP_FLAG_REPLAY \
	(1u << 1) /* slot replay -- result already in c_res */

uint32_t nfs4_op_access(struct compound *compound);
uint32_t nfs4_op_close(struct compound *compound);
uint32_t nfs4_op_commit(struct compound *compound);
uint32_t nfs4_op_create(struct compound *compound);
uint32_t nfs4_op_delegpurge(struct compound *compound);
uint32_t nfs4_op_delegreturn(struct compound *compound);
uint32_t nfs4_op_getattr(struct compound *compound);
uint32_t nfs4_op_getfh(struct compound *compound);
uint32_t nfs4_op_link(struct compound *compound);
uint32_t nfs4_op_lock(struct compound *compound);
uint32_t nfs4_op_lockt(struct compound *compound);
uint32_t nfs4_op_locku(struct compound *compound);
uint32_t nfs4_op_lookup(struct compound *compound);
uint32_t nfs4_op_lookupp(struct compound *compound);
uint32_t nfs4_op_nverify(struct compound *compound);
uint32_t nfs4_op_open(struct compound *compound);
uint32_t nfs4_op_openattr(struct compound *compound);
uint32_t nfs4_op_open_confirm(struct compound *compound);
uint32_t nfs4_op_open_downgrade(struct compound *compound);
uint32_t nfs4_op_putfh(struct compound *compound);
uint32_t nfs4_op_putpubfh(struct compound *compound);
uint32_t nfs4_op_putrootfh(struct compound *compound);
uint32_t nfs4_op_read(struct compound *compound);
uint32_t nfs4_op_readdir(struct compound *compound);
uint32_t nfs4_op_readlink(struct compound *compound);
uint32_t nfs4_op_remove(struct compound *compound);
uint32_t nfs4_op_rename(struct compound *compound);
uint32_t nfs4_op_renew(struct compound *compound);
uint32_t nfs4_op_restorefh(struct compound *compound);
uint32_t nfs4_op_savefh(struct compound *compound);
uint32_t nfs4_op_secinfo(struct compound *compound);
uint32_t nfs4_op_setattr(struct compound *compound);
uint32_t nfs4_op_setclientid(struct compound *compound);
uint32_t nfs4_op_setclientid_confirm(struct compound *compound);
uint32_t nfs4_op_verify(struct compound *compound);
uint32_t nfs4_op_write(struct compound *compound);
uint32_t nfs4_op_release_lockowner(struct compound *compound);
uint32_t nfs4_op_backchannel_ctl(struct compound *compound);
uint32_t nfs4_op_bind_conn_to_session(struct compound *compound);
uint32_t nfs4_op_exchange_id(struct compound *compound);
uint32_t nfs4_op_create_session(struct compound *compound);
uint32_t nfs4_op_destroy_session(struct compound *compound);
uint32_t nfs4_op_free_stateid(struct compound *compound);
uint32_t nfs4_op_get_dir_delegation(struct compound *compound);
uint32_t nfs4_op_getdeviceinfo(struct compound *compound);
uint32_t nfs4_op_getdevicelist(struct compound *compound);
uint32_t nfs4_op_layoutcommit(struct compound *compound);
uint32_t nfs4_op_layoutget(struct compound *compound);
uint32_t nfs4_op_layoutreturn(struct compound *compound);
uint32_t nfs4_op_secinfo_no_name(struct compound *compound);
uint32_t nfs4_op_sequence(struct compound *compound);
uint32_t nfs4_op_set_ssv(struct compound *compound);
uint32_t nfs4_op_test_stateid(struct compound *compound);
uint32_t nfs4_op_want_delegation(struct compound *compound);
uint32_t nfs4_op_destroy_clientid(struct compound *compound);
uint32_t nfs4_op_reclaim_complete(struct compound *compound);
uint32_t nfs4_op_allocate(struct compound *compound);
uint32_t nfs4_op_copy(struct compound *compound);
uint32_t nfs4_op_copy_notify(struct compound *compound);
uint32_t nfs4_op_deallocate(struct compound *compound);
uint32_t nfs4_op_io_advise(struct compound *compound);
uint32_t nfs4_op_layouterror(struct compound *compound);
uint32_t nfs4_op_layoutstats(struct compound *compound);
uint32_t nfs4_op_offload_cancel(struct compound *compound);
uint32_t nfs4_op_offload_status(struct compound *compound);
uint32_t nfs4_op_read_plus(struct compound *compound);
uint32_t nfs4_op_seek(struct compound *compound);
uint32_t nfs4_op_write_same(struct compound *compound);
uint32_t nfs4_op_clone(struct compound *compound);
uint32_t nfs4_op_getxattr(struct compound *compound);
uint32_t nfs4_op_setxattr(struct compound *compound);
uint32_t nfs4_op_listxattrs(struct compound *compound);
uint32_t nfs4_op_removexattr(struct compound *compound);
uint32_t nfs4_op_access_mask(struct compound *compound);
uint32_t nfs4_op_chunk_commit(struct compound *compound);
uint32_t nfs4_op_chunk_error(struct compound *compound);
uint32_t nfs4_op_chunk_finalize(struct compound *compound);
uint32_t nfs4_op_chunk_header_read(struct compound *compound);
uint32_t nfs4_op_chunk_lock(struct compound *compound);
uint32_t nfs4_op_chunk_read(struct compound *compound);
uint32_t nfs4_op_chunk_repaired(struct compound *compound);
uint32_t nfs4_op_chunk_rollback(struct compound *compound);
uint32_t nfs4_op_chunk_unlock(struct compound *compound);
uint32_t nfs4_op_chunk_write(struct compound *compound);
uint32_t nfs4_op_chunk_write_repair(struct compound *compound);
uint32_t nfs4_op_illegal(struct compound *compound);

const char *nfs4_op_name(nfs_opnum4 op);

/* Stateid validation and write verifier (file.c). */
struct inode;
struct stateid;
struct server_state;
nfsstat4 nfs4_stateid_resolve(struct compound *compound, struct inode *inode,
			      const stateid4 *wire, bool want_write,
			      struct stateid **out_stid);
void nfs4_write_verf(struct server_state *ss, verifier4 out_verf);

/* Export security enforcement (security.c). */
nfsstat4 nfs4_check_wrongsec(struct compound *compound);
bool nfs4_putfh_should_check_wrongsec(struct compound *compound);
nfsstat4 nfs4_build_secinfo(struct compound *compound, SECINFO4resok *resok);
nfsstat4 nfs4_check_rofs(struct compound *compound);

struct authunix_parms;
nfsstat4 nfs4_apply_createattrs(fattr4 *fattr, struct inode *inode,
				bitmap4 *attrsset, struct authunix_parms *ap);

/* Returns true if any client holds a write layout on this inode. */
bool inode_has_write_layout(struct inode *inode);

/*
 * nfs4_layout_implicit_return_rw - implicitly return the write layout for
 * this client on compound->c_inode (called from CLOSE and DELEGRETURN).
 * Returns NFS4_OP_FLAG_ASYNC if a reflected GETATTR fan-out was launched.
 */
uint32_t
nfs4_layout_implicit_return_rw(struct compound *compound,
			       uint32_t (*resume_fn)(struct rpc_trans *));

/*
 * nfs4_op_layoutreturn_resume - fan-out resume for LAYOUTRETURN, CLOSE,
 * and DELEGRETURN implicit write-layout returns.
 */
uint32_t nfs4_op_layoutreturn_resume(struct rpc_trans *rt);

/*
 * nfs4_recall_dir_delegations - recall all directory delegations on dir
 * except those held by exclude.
 *
 * Fire-and-forget: sends CB_RECALL for each delegation stateid held by
 * other clients, does not wait for DELEGRETURN.  Called after a
 * directory-modifying VFS op succeeds (CREATE, REMOVE, RENAME, LINK).
 */
void nfs4_recall_dir_delegations(struct server_state *ss, struct inode *dir,
				 struct client *exclude);

#define NFS4_OP_NUM(compound) \
	((compound)->c_args->argarray.argarray_val[(compound)->c_curr_op].argop)

#define NFS4_OP_ARG_SETUP(compound, field)                               \
	(&(compound)                                                     \
		  ->c_args->argarray.argarray_val[(compound)->c_curr_op] \
		  .nfs_argop4_u.field)

#define NFS4_OP_RES_SETUP(compound, field)                              \
	(&(compound)                                                    \
		  ->c_res->resarray.resarray_val[(compound)->c_curr_op] \
		  .nfs_resop4_u.field)

#define NFS4_OP_RESOK_SETUP(res, union_field, resok_field) \
	(&(res)->union_field.resok_field)

#endif /* _REFFS_NFS4_OPS_H */
