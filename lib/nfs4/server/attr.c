/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "nfsv42_xdr.h"
#include "reffs/dstore.h"
#include "reffs/dstore_fanout.h"
#include "reffs/dstore_wcc.h"
#include "reffs/idmap.h"
#include "reffs/identity.h"
#include "reffs/inode.h"
#include "reffs/layout_segment.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "reffs/task.h"
#include "reffs/filehandle.h"
#include "reffs/utf8string.h"
#include "reffs/rcu.h"
#include "reffs/dirent.h"
#include "reffs/identity.h"
#include "nfs4/attr.h"
#include "nfsv42_names.h"
#include "reffs/time.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/vfs.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/stateid.h"
#include "nfs4/cb.h"
#include "nfs4/session.h"
#include "nfs4/client.h"
#include "nfs4/owner.h"
#include "reffs/cmp.h"

struct nfsv42_attr {
	fattr4_supported_attrs supported_attrs;
	fattr4_type type;
	fattr4_fh_expire_type fh_expire_type;
	fattr4_change change;
	fattr4_size size;
	fattr4_link_support link_support;
	fattr4_symlink_support symlink_support;
	fattr4_named_attr named_attr;
	fattr4_fsid fsid;
	fattr4_unique_handles unique_handles;
	fattr4_lease_time lease_time;
	fattr4_rdattr_error rdattr_error;
	fattr4_acl acl;
	fattr4_aclsupport aclsupport;
	fattr4_archive archive;
	fattr4_cansettime cansettime;
	fattr4_case_insensitive case_insensitive;
	fattr4_case_preserving case_preserving;
	fattr4_chown_restricted chown_restricted;
	fattr4_filehandle filehandle;
	fattr4_fileid fileid;
	fattr4_files_avail files_avail;
	fattr4_files_free files_free;
	fattr4_files_total files_total;
	fattr4_fs_locations fs_locations;
	fattr4_hidden hidden;
	fattr4_homogeneous homogeneous;
	fattr4_maxfilesize maxfilesize;
	fattr4_maxlink maxlink;
	fattr4_maxname maxname;
	fattr4_maxread maxread;
	fattr4_maxwrite maxwrite;
	fattr4_mimetype mimetype;
	fattr4_mode mode;
	fattr4_no_trunc no_trunc;
	fattr4_numlinks numlinks;
	fattr4_owner owner;
	fattr4_owner_group owner_group;
	fattr4_quota_avail_hard quota_avail_hard;
	fattr4_quota_avail_soft quota_avail_soft;
	fattr4_quota_used quota_used;
	fattr4_rawdev rawdev;
	fattr4_space_avail space_avail;
	fattr4_space_free space_free;
	fattr4_space_total space_total;
	fattr4_space_used space_used;
	fattr4_system system;
	fattr4_time_access time_access;
	fattr4_time_access_set time_access_set;
	fattr4_time_backup time_backup;
	fattr4_time_create time_create;
	fattr4_time_delta time_delta;
	fattr4_time_metadata time_metadata;
	fattr4_time_modify time_modify;
	fattr4_time_modify_set time_modify_set;
	fattr4_mounted_on_fileid mounted_on_fileid;
	fattr4_dir_notif_delay dir_notif_delay;
	fattr4_dirent_notif_delay dirent_notif_delay;
	fattr4_dacl dacl;
	fattr4_sacl sacl;
	fattr4_change_policy change_policy;
	fattr4_fs_status fs_status;
	fattr4_fs_layout_types fs_layout_types;
	fattr4_layout_hint layout_hint;
	fattr4_layout_types layout_types;
	fattr4_layout_blksize layout_blksize;
	fattr4_layout_alignment layout_alignment;
	fattr4_fs_locations_info fs_locations_info;
	fattr4_mdsthreshold mdsthreshold;
	fattr4_retention_get retention_get;
	fattr4_retention_set retention_set;
	fattr4_retentevt_get retentevt_get;
	fattr4_retentevt_set retentevt_set;
	fattr4_retention_hold retention_hold;
	fattr4_mode_set_masked mode_set_masked;
	fattr4_suppattr_exclcreat suppattr_exclcreat;
	fattr4_fs_charset_cap fs_charset_cap;
	fattr4_clone_blksize clone_blksize;
	fattr4_space_freed space_freed;
	fattr4_change_attr_type change_attr_type;
	fattr4_sec_label sec_label;
	fattr4_mode_umask mode_umask;
	fattr4_xattr_support xattr_support;
	fattr4_offline offline;
	fattr4_time_deleg_access time_deleg_access;
	fattr4_time_deleg_modify time_deleg_modify;
	fattr4_open_arguments open_arguments;
	fattr4_uncacheable_file_data uncacheable_file_data;
	fattr4_uncacheable_dirent_metadata uncacheable_dirent_metadata;
	fattr4_coding_block_size coding_block_size;
};

static struct nfsv42_attr system_attrs = {
	.fh_expire_type = FH4_PERSISTENT,
	.link_support = true,
	.symlink_support = true,
	.named_attr = false,
	.chown_restricted = false,
	.unique_handles = true,
	.lease_time = 45,
	.aclsupport = false,
	.cansettime = true,
	.case_insensitive = false,
	.case_preserving = true,
	.rdattr_error = NFS4_OK,
	.homogeneous = true,
	.maxfilesize = INT64_MAX,
	.maxlink = -1,
	.maxname = 255,
	.maxread = 1024 * 1024,
	.maxwrite = 1024 * 1024,
	.no_trunc = true,
	.numlinks = 255,
	.fs_charset_cap = FSCHARSET_CAP4_ALLOWS_ONLY_UTF8,
	.change_attr_type = NFS4_CHANGE_TYPE_IS_VERSION_COUNTER,
	.clone_blksize = 4096,
	.xattr_support = false,
	.time_delta = { .seconds = 0, .nseconds = 1 },
	.coding_block_size = 4096,
};
bitmap4 *supported_attributes = &system_attrs.supported_attrs;

struct nfsv42_attr_ops {
	uint32_t nao_attr;
	count4 (*nao_count)(struct nfsv42_attr *nattr);
	nfsstat4 (*nao_xdr)(XDR *xdrs, struct nfsv42_attr *nattr);
	bool (*nao_equal)(struct nfsv42_attr *a, struct nfsv42_attr *b);
};

static count4 supported_attrs_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_supported_attrs,
			  &nattr->supported_attrs);
}

static nfsstat4 supported_attrs_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_supported_attrs(xdrs, &nattr->supported_attrs))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool supported_attrs_equal(struct nfsv42_attr __attribute__((unused)) *
					  a,
				  struct nfsv42_attr *b)
{
	return bitmap4_equal(supported_attributes, &b->supported_attrs);
}

static count4 type_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_type, &nattr->type);
}

static nfsstat4 type_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_type(xdrs, &nattr->type))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool type_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->type == b->type;
}

static count4 fh_expire_type_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_fh_expire_type,
			  &nattr->fh_expire_type);
}

static nfsstat4 fh_expire_type_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fh_expire_type(xdrs, &nattr->fh_expire_type))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fh_expire_type_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->fh_expire_type == b->fh_expire_type;
}

static count4 change_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_change, &nattr->change);
}

static nfsstat4 change_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_change(xdrs, &nattr->change))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool change_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->change == b->change;
}

static count4 size_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_size, &nattr->size);
}

static nfsstat4 size_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_size(xdrs, &nattr->size))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool size_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->size == b->size;
}

static count4 link_support_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_link_support,
			  &nattr->link_support);
}

static nfsstat4 link_support_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_link_support(xdrs, &nattr->link_support))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool link_support_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->link_support == b->link_support;
}

static count4 symlink_support_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_symlink_support,
			  &nattr->symlink_support);
}

static nfsstat4 symlink_support_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_symlink_support(xdrs, &nattr->symlink_support))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool symlink_support_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->symlink_support == b->symlink_support;
}

static count4 named_attr_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_named_attr, &nattr->named_attr);
}

static nfsstat4 named_attr_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_named_attr(xdrs, &nattr->named_attr))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool named_attr_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->named_attr == b->named_attr;
}

static count4 fsid_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_fsid, &nattr->fsid);
}

static nfsstat4 fsid_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fsid(xdrs, &nattr->fsid))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fsid_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->fsid.major == b->fsid.major && a->fsid.minor == b->fsid.minor;
}

static count4 unique_handles_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_unique_handles,
			  &nattr->unique_handles);
}

static nfsstat4 unique_handles_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_unique_handles(xdrs, &nattr->unique_handles))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool unique_handles_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->unique_handles == b->unique_handles;
}

static count4 lease_time_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_lease_time, &nattr->lease_time);
}

static nfsstat4 lease_time_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_lease_time(xdrs, &nattr->lease_time))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool lease_time_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->lease_time == b->lease_time;
}

static count4 rdattr_error_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_rdattr_error,
			  &nattr->rdattr_error);
}

static nfsstat4 rdattr_error_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_rdattr_error(xdrs, &nattr->rdattr_error))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool rdattr_error_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->rdattr_error == b->rdattr_error;
}

static count4 acl_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_acl, &nattr->acl);
}

static nfsstat4 acl_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_acl(xdrs, &nattr->acl))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool acl_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->acl.fattr4_acl_len != b->acl.fattr4_acl_len)
		return false;

	for (u_int i = 0; i < a->acl.fattr4_acl_len; i++) {
		if (a->acl.fattr4_acl_val[i].type !=
		    b->acl.fattr4_acl_val[i].type)
			return false;

		if (a->acl.fattr4_acl_val[i].flag !=
		    b->acl.fattr4_acl_val[i].flag)
			return false;

		if (a->acl.fattr4_acl_val[i].access_mask !=
		    b->acl.fattr4_acl_val[i].access_mask)
			return false;

		if (utf8string_cmp(&a->acl.fattr4_acl_val[i].who,
				   &b->acl.fattr4_acl_val[i].who))
			return false;
	}

	return true;
}

static count4 aclsupport_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_aclsupport, &nattr->aclsupport);
}

static nfsstat4 aclsupport_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_aclsupport(xdrs, &nattr->aclsupport))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool aclsupport_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->aclsupport == b->aclsupport;
}

static count4 archive_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_archive, &nattr->archive);
}

static nfsstat4 archive_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_archive(xdrs, &nattr->archive))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool archive_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->archive == b->archive;
}

static count4 cansettime_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_cansettime, &nattr->cansettime);
}

static nfsstat4 cansettime_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_cansettime(xdrs, &nattr->cansettime))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool cansettime_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->cansettime == b->cansettime;
}

static count4 case_insensitive_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_case_insensitive,
			  &nattr->case_insensitive);
}

static nfsstat4 case_insensitive_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_case_insensitive(xdrs, &nattr->case_insensitive))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool case_insensitive_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->case_insensitive == b->case_insensitive;
}

static count4 case_preserving_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_case_preserving,
			  &nattr->case_preserving);
}

static nfsstat4 case_preserving_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_case_preserving(xdrs, &nattr->case_preserving))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool case_preserving_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->case_preserving == b->case_preserving;
}

static count4 chown_restricted_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_chown_restricted,
			  &nattr->chown_restricted);
}

static nfsstat4 chown_restricted_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_chown_restricted(xdrs, &nattr->chown_restricted))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool chown_restricted_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->chown_restricted == b->chown_restricted;
}

static count4 filehandle_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_filehandle, &nattr->filehandle);
}

static nfsstat4 filehandle_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_filehandle(xdrs, &nattr->filehandle))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool filehandle_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (b->filehandle.nfs_fh4_len != a->filehandle.nfs_fh4_len)
		return false;

	return network_file_handles_equal(
		(struct network_file_handle *)a->filehandle.nfs_fh4_val,
		(struct network_file_handle *)b->filehandle.nfs_fh4_val);
}

static count4 fileid_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_fileid, &nattr->fileid);
}

static nfsstat4 fileid_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fileid(xdrs, &nattr->fileid))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fileid_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->fileid == b->fileid;
}

static count4 files_avail_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_files_avail,
			  &nattr->files_avail);
}

static nfsstat4 files_avail_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_files_avail(xdrs, &nattr->files_avail))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool files_avail_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->files_avail == b->files_avail;
}

static count4 files_free_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_files_free, &nattr->files_free);
}

static nfsstat4 files_free_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_files_free(xdrs, &nattr->files_free))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool files_free_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->files_free == b->files_free;
}

static count4 files_total_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_files_total,
			  &nattr->files_total);
}

static nfsstat4 files_total_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_files_total(xdrs, &nattr->files_total))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool files_total_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->files_total == b->files_total;
}

static count4 fs_locations_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_fs_locations,
			  &nattr->fs_locations);
}

static nfsstat4 fs_locations_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fs_locations(xdrs, &nattr->fs_locations))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static inline bool fs_location_equal(fs_location4 *a, fs_location4 *b)
{
	if (a->rootpath.pathname4_len != b->rootpath.pathname4_len)
		return false;

	if (a->server.server_len != b->server.server_len)
		return false;

	for (u_int i = 0; i < a->rootpath.pathname4_len; i++) {
		if (utf8string_cmp(&a->rootpath.pathname4_val[i],
				   &b->rootpath.pathname4_val[i]))
			return false;
	}

	for (u_int i = 0; i < a->server.server_len; i++) {
		if (utf8string_cmp(&a->server.server_val[i],
				   &b->server.server_val[i]))
			return false;
	}

	return true;
}

static bool fs_locations_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->fs_locations.fs_root.pathname4_len !=
	    b->fs_locations.fs_root.pathname4_len)
		return false;

	for (u_int i = 0; i < a->fs_locations.fs_root.pathname4_len; i++) {
		if (utf8string_cmp(&a->fs_locations.fs_root.pathname4_val[i],
				   &b->fs_locations.fs_root.pathname4_val[i]))
			return false;
	}

	for (u_int i = 0; i < a->fs_locations.locations.locations_len; i++) {
		if (!fs_location_equal(
			    &a->fs_locations.locations.locations_val[i],
			    &b->fs_locations.locations.locations_val[i]))
			return false;
	}

	return true;
}

static count4 hidden_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_hidden, &nattr->hidden);
}

static nfsstat4 hidden_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_hidden(xdrs, &nattr->hidden))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool hidden_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->hidden == b->hidden;
}

static count4 homogeneous_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_homogeneous,
			  &nattr->homogeneous);
}

static nfsstat4 homogeneous_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_homogeneous(xdrs, &nattr->homogeneous))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool homogeneous_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->homogeneous == b->homogeneous;
}

static count4 maxfilesize_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_maxfilesize,
			  &nattr->maxfilesize);
}

static nfsstat4 maxfilesize_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_maxfilesize(xdrs, &nattr->maxfilesize))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool maxfilesize_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->maxfilesize == b->maxfilesize;
}

static count4 maxlink_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_maxlink, &nattr->maxlink);
}

static nfsstat4 maxlink_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_maxlink(xdrs, &nattr->maxlink))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool maxlink_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->maxlink == b->maxlink;
}

static count4 maxname_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_maxname, &nattr->maxname);
}

static nfsstat4 maxname_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_maxname(xdrs, &nattr->maxname))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool maxname_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->maxname == b->maxname;
}

static count4 maxread_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_maxread, &nattr->maxread);
}

static nfsstat4 maxread_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_maxread(xdrs, &nattr->maxread))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool maxread_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->maxread == b->maxread;
}

static count4 maxwrite_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_maxwrite, &nattr->maxwrite);
}

static nfsstat4 maxwrite_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_maxwrite(xdrs, &nattr->maxwrite))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool maxwrite_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->maxwrite == b->maxwrite;
}

static count4 mimetype_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_mimetype, &nattr->mimetype);
}

static nfsstat4 mimetype_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mimetype(xdrs, &nattr->mimetype))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mimetype_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (utf8string_cmp(&a->mimetype, &b->mimetype))
		return false;

	return true;
}

static count4 mode_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_mode, &nattr->mode);
}

static nfsstat4 mode_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mode(xdrs, &nattr->mode))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mode_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->mode == b->mode;
}

static count4 no_trunc_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_no_trunc, &nattr->no_trunc);
}

static nfsstat4 no_trunc_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_no_trunc(xdrs, &nattr->no_trunc))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool no_trunc_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->no_trunc == b->no_trunc;
}

static count4 numlinks_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_numlinks, &nattr->numlinks);
}

static nfsstat4 numlinks_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_numlinks(xdrs, &nattr->numlinks))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool numlinks_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->numlinks == b->numlinks;
}

static count4 owner_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_owner, &nattr->owner);
}

static nfsstat4 owner_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_owner(xdrs, &nattr->owner))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool owner_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return utf8string_cmp(&a->owner, &b->owner);
}

static count4 owner_group_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_owner_group,
			  &nattr->owner_group);
}

static nfsstat4 owner_group_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_owner_group(xdrs, &nattr->owner_group))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool owner_group_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return utf8string_cmp(&a->owner_group, &b->owner_group);
}

static count4 quota_avail_hard_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_quota_avail_hard,
			  &nattr->quota_avail_hard);
}

static nfsstat4 quota_avail_hard_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_quota_avail_hard(xdrs, &nattr->quota_avail_hard))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool quota_avail_hard_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->quota_avail_hard == b->quota_avail_hard;
}

static count4 quota_avail_soft_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_quota_avail_soft,
			  &nattr->quota_avail_soft);
}

static nfsstat4 quota_avail_soft_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_quota_avail_soft(xdrs, &nattr->quota_avail_soft))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool quota_avail_soft_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->quota_avail_soft == b->quota_avail_soft;
}

static count4 quota_used_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_quota_used, &nattr->quota_used);
}

static nfsstat4 quota_used_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_quota_used(xdrs, &nattr->quota_used))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool quota_used_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->quota_used == b->quota_used;
}

static count4 rawdev_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_rawdev, &nattr->rawdev);
}

static nfsstat4 rawdev_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_rawdev(xdrs, &nattr->rawdev))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool rawdev_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->rawdev.specdata1 == b->rawdev.specdata1 &&
	       a->rawdev.specdata2 == b->rawdev.specdata2;
}

static count4 space_avail_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_space_avail,
			  &nattr->space_avail);
}

static nfsstat4 space_avail_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_space_avail(xdrs, &nattr->space_avail))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool space_avail_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->space_avail == b->space_avail;
}

static count4 space_free_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_space_free, &nattr->space_free);
}

static nfsstat4 space_free_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_space_free(xdrs, &nattr->space_free))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool space_free_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->space_free == b->space_free;
}

static count4 space_total_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_space_total,
			  &nattr->space_total);
}

static nfsstat4 space_total_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_space_total(xdrs, &nattr->space_total))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool space_total_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->space_total == b->space_total;
}

static count4 space_used_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_space_used, &nattr->space_used);
}

static nfsstat4 space_used_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_space_used(xdrs, &nattr->space_used))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool space_used_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->space_used == b->space_used;
}

static count4 system_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_system, &nattr->system);
}

static nfsstat4 system_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_system(xdrs, &nattr->system))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool system_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->system == b->system;
}

static count4 time_access_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_time_access,
			  &nattr->time_access);
}

static nfsstat4 time_access_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_access(xdrs, &nattr->time_access))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_access_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_access, &b->time_access);
}

static count4 time_access_set_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_time_access_set,
			  &nattr->time_access_set);
}

static nfsstat4 time_access_set_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_access_set(xdrs, &nattr->time_access_set))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_access_set_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->time_access_set.set_it != b->time_access_set.set_it)
		return false;
	if (a->time_access_set.set_it == SET_TO_CLIENT_TIME4)
		return nfstime4_eq(&a->time_access_set.settime4_u.time,
				   &b->time_access_set.settime4_u.time);
	return true;
}

static count4 time_backup_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_time_backup,
			  &nattr->time_backup);
}

static nfsstat4 time_backup_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_backup(xdrs, &nattr->time_backup))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_backup_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_backup, &b->time_backup);
}

static count4 time_create_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_time_create,
			  &nattr->time_create);
}

static nfsstat4 time_create_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_create(xdrs, &nattr->time_create))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_create_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_create, &b->time_create);
}

static count4 time_delta_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_time_delta, &nattr->time_delta);
}

static nfsstat4 time_delta_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_delta(xdrs, &nattr->time_delta))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_delta_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_delta, &b->time_delta);
}

static count4 time_metadata_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_time_metadata,
			  &nattr->time_metadata);
}

static nfsstat4 time_metadata_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_metadata(xdrs, &nattr->time_metadata))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_metadata_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_metadata, &b->time_metadata);
}

static count4 time_modify_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_time_modify,
			  &nattr->time_modify);
}

static nfsstat4 time_modify_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_modify(xdrs, &nattr->time_modify))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_modify_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_modify, &b->time_modify);
}

static count4 time_modify_set_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_time_modify_set,
			  &nattr->time_modify_set);
}

static nfsstat4 time_modify_set_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_modify_set(xdrs, &nattr->time_modify_set))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_modify_set_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->time_modify_set.set_it != b->time_modify_set.set_it)
		return false;
	if (a->time_modify_set.set_it == SET_TO_CLIENT_TIME4)
		return nfstime4_eq(&a->time_modify_set.settime4_u.time,
				   &b->time_modify_set.settime4_u.time);
	return true;
}

static count4 mounted_on_fileid_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_mounted_on_fileid,
			  &nattr->mounted_on_fileid);
}

static nfsstat4 mounted_on_fileid_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mounted_on_fileid(xdrs, &nattr->mounted_on_fileid))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mounted_on_fileid_equal(struct nfsv42_attr *a,
				    struct nfsv42_attr *b)
{
	return a->mounted_on_fileid == b->mounted_on_fileid;
}

static count4 dir_notif_delay_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_dir_notif_delay,
			  &nattr->dir_notif_delay);
}

static nfsstat4 dir_notif_delay_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_dir_notif_delay(xdrs, &nattr->dir_notif_delay))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool dir_notif_delay_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->dir_notif_delay, &b->dir_notif_delay);
}

static count4 dirent_notif_delay_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_dirent_notif_delay,
			  &nattr->dirent_notif_delay);
}

static nfsstat4 dirent_notif_delay_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_dirent_notif_delay(xdrs, &nattr->dirent_notif_delay))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool dirent_notif_delay_equal(struct nfsv42_attr *a,
				     struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->dirent_notif_delay, &b->dirent_notif_delay);
}

static count4 dacl_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_dacl, &nattr->dacl);
}

static nfsstat4 dacl_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_dacl(xdrs, &nattr->dacl))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool nfsace4_equal(nfsace4 *a, nfsace4 *b)
{
	if (a->type != b->type)
		return false;

	if (a->flag != b->flag)
		return false;

	if (a->access_mask != b->access_mask)
		return false;

	if (utf8string_cmp(&a->who, &b->who))
		return false;

	return true;
}

static bool dacl_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->dacl.na41_flag != b->dacl.na41_flag)
		return false;

	if (a->dacl.na41_aces.na41_aces_len != b->dacl.na41_aces.na41_aces_len)
		return false;

	for (u_int i = 0; i < a->dacl.na41_aces.na41_aces_len; i++) {
		if (!nfsace4_equal(&a->dacl.na41_aces.na41_aces_val[i],
				   &b->dacl.na41_aces.na41_aces_val[i]))
			return false;
	}

	return true;
}

static count4 sacl_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_sacl, &nattr->sacl);
}

static nfsstat4 sacl_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_sacl(xdrs, &nattr->sacl))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool sacl_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->sacl.na41_flag != b->sacl.na41_flag)
		return false;

	if (a->sacl.na41_aces.na41_aces_len != b->sacl.na41_aces.na41_aces_len)
		return false;

	for (u_int i = 0; i < a->sacl.na41_aces.na41_aces_len; i++) {
		if (!nfsace4_equal(&a->sacl.na41_aces.na41_aces_val[i],
				   &b->sacl.na41_aces.na41_aces_val[i]))
			return false;
	}

	return true;
}

static count4 change_policy_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_change_policy,
			  &nattr->change_policy);
}

static nfsstat4 change_policy_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_change_policy(xdrs, &nattr->change_policy))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool change_policy_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->change_policy.cp_major == b->change_policy.cp_major &&
	       a->change_policy.cp_minor == b->change_policy.cp_minor;
}

static count4 fs_status_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_fs_status, &nattr->fs_status);
}

static nfsstat4 fs_status_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fs_status(xdrs, &nattr->fs_status))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fs_status_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->fs_status.fss_absent != b->fs_status.fss_absent)
		return false;

	if (a->fs_status.fss_type != b->fs_status.fss_type)
		return false;

	if (a->fs_status.fss_age != b->fs_status.fss_age)
		return false;

	if (utf8string_cmp(&a->fs_status.fss_source, &b->fs_status.fss_source))
		return false;

	if (utf8string_cmp(&a->fs_status.fss_current,
			   &b->fs_status.fss_current))
		return false;

	return nfstime4_eq(&a->fs_status.fss_version,
			   &b->fs_status.fss_version);
}

static count4 fs_layout_types_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_fs_layout_types,
			  &nattr->fs_layout_types);
}

static nfsstat4 fs_layout_types_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fs_layout_types(xdrs, &nattr->fs_layout_types))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fs_layout_types_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->fs_layout_types.fattr4_fs_layout_types_len !=
	    b->fs_layout_types.fattr4_fs_layout_types_len)
		return false;

	for (u_int i = 0; i < a->fs_layout_types.fattr4_fs_layout_types_len;
	     i++)
		if (a->fs_layout_types.fattr4_fs_layout_types_val[i] !=
		    b->fs_layout_types.fattr4_fs_layout_types_val[i])
			return false;

	return true;
}

static count4 layout_hint_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_layout_hint,
			  &nattr->layout_hint);
}

static nfsstat4 layout_hint_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_layout_hint(xdrs, &nattr->layout_hint))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool layout_hint_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->layout_hint.loh_type != b->layout_hint.loh_type)
		return false;
	if (a->layout_hint.loh_body.loh_body_len !=
	    b->layout_hint.loh_body.loh_body_len)
		return false;
	return memcmp(a->layout_hint.loh_body.loh_body_val,
		      b->layout_hint.loh_body.loh_body_val,
		      a->layout_hint.loh_body.loh_body_len) == 0;
}

static count4 layout_types_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_layout_types,
			  &nattr->layout_types);
}

static nfsstat4 layout_types_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_layout_types(xdrs, &nattr->layout_types))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool layout_types_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->layout_types.fattr4_layout_types_len !=
	    b->layout_types.fattr4_layout_types_len)
		return false;

	for (u_int i = 0; i < a->layout_types.fattr4_layout_types_len; i++)
		if (a->layout_types.fattr4_layout_types_val[i] !=
		    b->layout_types.fattr4_layout_types_val[i])
			return false;

	return true;
}

static count4 layout_blksize_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_layout_blksize,
			  &nattr->layout_blksize);
}

static nfsstat4 layout_blksize_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_layout_blksize(xdrs, &nattr->layout_blksize))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool layout_blksize_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->layout_blksize == b->layout_blksize;
}

static count4 layout_alignment_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_layout_alignment,
			  &nattr->layout_alignment);
}

static nfsstat4 layout_alignment_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_layout_alignment(xdrs, &nattr->layout_alignment))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool layout_alignment_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->layout_alignment == b->layout_alignment;
}

static count4 fs_locations_info_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_fs_locations_info,
			  &nattr->fs_locations_info);
}

static nfsstat4 fs_locations_info_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fs_locations_info(xdrs, &nattr->fs_locations_info))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fs_locations_info_equal(struct nfsv42_attr *a,
				    struct nfsv42_attr *b)
{
	fs_locations_info4 *ai = &a->fs_locations_info;
	fs_locations_info4 *bi = &b->fs_locations_info;

	if (ai->fli_flags != bi->fli_flags)
		return false;

	if (ai->fli_valid_for != bi->fli_valid_for)
		return false;

	if (ai->fli_fs_root.pathname4_len != bi->fli_fs_root.pathname4_len)
		return false;

	for (u_int i = 0; i < ai->fli_fs_root.pathname4_len; i++) {
		if (utf8string_cmp(&ai->fli_fs_root.pathname4_val[i],
				   &bi->fli_fs_root.pathname4_val[i]))
			return false;
	}

	if (ai->fli_items.fli_items_len != bi->fli_items.fli_items_len)
		return false;

	/* For simplicity, we just check lengths and flag equality here,
	 * as deep comparison of fs_locations_item4 is complex.
	 */

	return true;
}

static count4 mdsthreshold_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_mdsthreshold,
			  &nattr->mdsthreshold);
}

static nfsstat4 mdsthreshold_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mdsthreshold(xdrs, &nattr->mdsthreshold))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mdsthreshold_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->mdsthreshold.mth_hints.mth_hints_len !=
	    b->mdsthreshold.mth_hints.mth_hints_len)
		return false;

	for (u_int i = 0; i < a->mdsthreshold.mth_hints.mth_hints_len; i++) {
		threshold_item4 *ai =
			&a->mdsthreshold.mth_hints.mth_hints_val[i];
		threshold_item4 *bi =
			&b->mdsthreshold.mth_hints.mth_hints_val[i];

		if (ai->thi_layout_type != bi->thi_layout_type)
			return false;
		if (!bitmap4_equal(&ai->thi_hintset, &bi->thi_hintset))
			return false;
		/* ... further fields if needed ... */
	}

	return true;
}

static count4 retention_get_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_retention_get,
			  &nattr->retention_get);
}

static nfsstat4 retention_get_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_retention_get(xdrs, &nattr->retention_get))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool retention_get_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->retention_get.rg_duration != b->retention_get.rg_duration)
		return false;

	if (a->retention_get.rg_begin_time.rg_begin_time_len !=
	    b->retention_get.rg_begin_time.rg_begin_time_len)
		return false;

	if (a->retention_get.rg_begin_time.rg_begin_time_len) {
		return nfstime4_eq(
			a->retention_get.rg_begin_time.rg_begin_time_val,
			b->retention_get.rg_begin_time.rg_begin_time_val);
	}

	return true;
}

static count4 retention_set_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_retention_set,
			  &nattr->retention_set);
}

static nfsstat4 retention_set_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_retention_set(xdrs, &nattr->retention_set))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool retention_set_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->retention_set.rs_enable != b->retention_set.rs_enable)
		return false;
	if (a->retention_set.rs_duration.rs_duration_len !=
	    b->retention_set.rs_duration.rs_duration_len)
		return false;
	if (a->retention_set.rs_duration.rs_duration_len)
		return *a->retention_set.rs_duration.rs_duration_val ==
		       *b->retention_set.rs_duration.rs_duration_val;
	return true;
}

static count4 retentevt_get_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_retentevt_get,
			  &nattr->retentevt_get);
}

static nfsstat4 retentevt_get_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_retentevt_get(xdrs, &nattr->retentevt_get))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool retentevt_get_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->retentevt_get.rg_duration != b->retentevt_get.rg_duration)
		return false;

	if (a->retentevt_get.rg_begin_time.rg_begin_time_len !=
	    b->retentevt_get.rg_begin_time.rg_begin_time_len)
		return false;

	if (a->retentevt_get.rg_begin_time.rg_begin_time_len) {
		return nfstime4_eq(
			a->retentevt_get.rg_begin_time.rg_begin_time_val,
			b->retentevt_get.rg_begin_time.rg_begin_time_val);
	}

	return true;
}

static count4 retentevt_set_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_retentevt_set,
			  &nattr->retentevt_set);
}

static nfsstat4 retentevt_set_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_retentevt_set(xdrs, &nattr->retentevt_set))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool retentevt_set_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->retentevt_set.rs_enable != b->retentevt_set.rs_enable)
		return false;
	if (a->retentevt_set.rs_duration.rs_duration_len !=
	    b->retentevt_set.rs_duration.rs_duration_len)
		return false;
	if (a->retentevt_set.rs_duration.rs_duration_len)
		return *a->retentevt_set.rs_duration.rs_duration_val ==
		       *b->retentevt_set.rs_duration.rs_duration_val;
	return true;
}

static count4 retention_hold_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_retention_hold,
			  &nattr->retention_hold);
}

static nfsstat4 retention_hold_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_retention_hold(xdrs, &nattr->retention_hold))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool retention_hold_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->retention_hold == b->retention_hold;
}

static count4 mode_set_masked_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_mode_set_masked,
			  &nattr->mode_set_masked);
}

static nfsstat4 mode_set_masked_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mode_set_masked(xdrs, &nattr->mode_set_masked))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mode_set_masked_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->mode_set_masked.mm_value_to_set ==
		       b->mode_set_masked.mm_value_to_set &&
	       a->mode_set_masked.mm_mask_bits ==
		       b->mode_set_masked.mm_mask_bits;
}

static count4 suppattr_exclcreat_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_suppattr_exclcreat,
			  &nattr->suppattr_exclcreat);
}

static nfsstat4 suppattr_exclcreat_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_suppattr_exclcreat(xdrs, &nattr->suppattr_exclcreat))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool suppattr_exclcreat_equal(struct nfsv42_attr *a,
				     struct nfsv42_attr *b)
{
	return bitmap4_equal(&a->suppattr_exclcreat, &b->suppattr_exclcreat);
}

static count4 fs_charset_cap_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_fs_charset_cap,
			  &nattr->fs_charset_cap);
}

static nfsstat4 fs_charset_cap_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fs_charset_cap(xdrs, &nattr->fs_charset_cap))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fs_charset_cap_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->fs_charset_cap == b->fs_charset_cap;
}

static count4 clone_blksize_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_clone_blksize,
			  &nattr->clone_blksize);
}

static nfsstat4 clone_blksize_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_clone_blksize(xdrs, &nattr->clone_blksize))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool clone_blksize_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->clone_blksize == b->clone_blksize;
}

static count4 space_freed_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_space_freed,
			  &nattr->space_freed);
}

static nfsstat4 space_freed_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_space_freed(xdrs, &nattr->space_freed))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool space_freed_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->space_freed == b->space_freed;
}

static count4 change_attr_type_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_change_attr_type,
			  &nattr->change_attr_type);
}

static nfsstat4 change_attr_type_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_change_attr_type(xdrs, &nattr->change_attr_type))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool change_attr_type_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->change_attr_type == b->change_attr_type;
}

static count4 sec_label_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_sec_label, &nattr->sec_label);
}

static nfsstat4 sec_label_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_sec_label(xdrs, &nattr->sec_label))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool sec_label_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->sec_label.slai_lfs.lfs_lfs != b->sec_label.slai_lfs.lfs_lfs ||
	    a->sec_label.slai_lfs.lfs_pi != b->sec_label.slai_lfs.lfs_pi)
		return false;

	if (a->sec_label.slai_data.slai_data_len !=
	    b->sec_label.slai_data.slai_data_len)
		return false;

	if (a->sec_label.slai_data.slai_data_len == 0)
		return true;

	return memcmp(a->sec_label.slai_data.slai_data_val,
		      b->sec_label.slai_data.slai_data_val,
		      a->sec_label.slai_data.slai_data_len) == 0;
}

static count4 mode_umask_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_mode_umask, &nattr->mode_umask);
}

static nfsstat4 mode_umask_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mode_umask(xdrs, &nattr->mode_umask))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mode_umask_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->mode_umask.mu_mode == b->mode_umask.mu_mode &&
	       a->mode_umask.mu_umask == b->mode_umask.mu_umask;
}

static count4 xattr_support_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_xattr_support,
			  &nattr->xattr_support);
}

static nfsstat4 xattr_support_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_xattr_support(xdrs, &nattr->xattr_support))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool xattr_support_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->xattr_support == b->xattr_support;
}

static count4 offline_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_offline, &nattr->offline);
}

static nfsstat4 offline_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_offline(xdrs, &nattr->offline))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool offline_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->offline == b->offline;
}

static count4 time_deleg_access_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_time_deleg_access,
			  &nattr->time_deleg_access);
}

static nfsstat4 time_deleg_access_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_deleg_access(xdrs, &nattr->time_deleg_access))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_deleg_access_equal(struct nfsv42_attr *a,
				    struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_deleg_access, &b->time_deleg_access);
}

static count4 time_deleg_modify_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_time_deleg_modify,
			  &nattr->time_deleg_modify);
}

static nfsstat4 time_deleg_modify_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_deleg_modify(xdrs, &nattr->time_deleg_modify))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_deleg_modify_equal(struct nfsv42_attr *a,
				    struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_deleg_modify, &b->time_deleg_modify);
}

static count4 open_arguments_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_open_arguments,
			  &nattr->open_arguments);
}

static nfsstat4 open_arguments_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_open_arguments(xdrs, &nattr->open_arguments))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool open_arguments_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (!bitmap4_equal(&a->open_arguments.oa_share_access,
			   &b->open_arguments.oa_share_access))
		return false;

	if (!bitmap4_equal(&a->open_arguments.oa_share_deny,
			   &b->open_arguments.oa_share_deny))
		return false;

	if (!bitmap4_equal(&a->open_arguments.oa_share_access_want,
			   &b->open_arguments.oa_share_access_want))
		return false;

	if (!bitmap4_equal(&a->open_arguments.oa_open_claim,
			   &b->open_arguments.oa_open_claim))
		return false;

	return bitmap4_equal(&a->open_arguments.oa_createmode,
			     &b->open_arguments.oa_createmode);
}

static count4 uncacheable_file_data_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_uncacheable_file_data,
			  &nattr->uncacheable_file_data);
}

static nfsstat4 uncacheable_file_data_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_uncacheable_file_data(xdrs,
					      &nattr->uncacheable_file_data))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool uncacheable_file_data_equal(struct nfsv42_attr *a,
					struct nfsv42_attr *b)
{
	return a->uncacheable_file_data == b->uncacheable_file_data;
}

static count4 uncacheable_dirent_metadata_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_uncacheable_dirent_metadata,
			  &nattr->uncacheable_dirent_metadata);
}

static nfsstat4 uncacheable_dirent_metadata_xdr(XDR *xdrs,
						struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_uncacheable_dirent_metadata(
		    xdrs, &nattr->uncacheable_dirent_metadata))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool uncacheable_dirent_metadata_equal(struct nfsv42_attr *a,
					      struct nfsv42_attr *b)
{
	return a->uncacheable_dirent_metadata == b->uncacheable_dirent_metadata;
}

static count4 coding_block_size_count(struct nfsv42_attr *nattr)
{
	return xdr_sizeof((xdrproc_t)xdr_fattr4_coding_block_size,
			  &nattr->coding_block_size);
}

static nfsstat4 coding_block_size_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_coding_block_size(xdrs, &nattr->coding_block_size))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool coding_block_size_equal(struct nfsv42_attr *a,
				    struct nfsv42_attr *b)
{
	return a->coding_block_size == b->coding_block_size;
}

static struct nfsv42_attr_ops nao[] = {
	{ FATTR4_SUPPORTED_ATTRS, supported_attrs_count, supported_attrs_xdr,
	  supported_attrs_equal },
	{ FATTR4_TYPE, type_count, type_xdr, type_equal },
	{ FATTR4_FH_EXPIRE_TYPE, fh_expire_type_count, fh_expire_type_xdr,
	  fh_expire_type_equal },
	{ FATTR4_CHANGE, change_count, change_xdr, change_equal },
	{ FATTR4_SIZE, size_count, size_xdr, size_equal },
	{ FATTR4_LINK_SUPPORT, link_support_count, link_support_xdr,
	  link_support_equal },
	{ FATTR4_SYMLINK_SUPPORT, symlink_support_count, symlink_support_xdr,
	  symlink_support_equal },
	{ FATTR4_NAMED_ATTR, named_attr_count, named_attr_xdr,
	  named_attr_equal },
	{ FATTR4_FSID, fsid_count, fsid_xdr, fsid_equal },
	{ FATTR4_UNIQUE_HANDLES, unique_handles_count, unique_handles_xdr,
	  unique_handles_equal },
	{ FATTR4_LEASE_TIME, lease_time_count, lease_time_xdr,
	  lease_time_equal },
	{ FATTR4_RDATTR_ERROR, rdattr_error_count, rdattr_error_xdr,
	  rdattr_error_equal },
	{ FATTR4_ACL, acl_count, acl_xdr, acl_equal },
	{ FATTR4_ACLSUPPORT, aclsupport_count, aclsupport_xdr,
	  aclsupport_equal },
	{ FATTR4_ARCHIVE, archive_count, archive_xdr, archive_equal },
	{ FATTR4_CANSETTIME, cansettime_count, cansettime_xdr,
	  cansettime_equal },
	{ FATTR4_CASE_INSENSITIVE, case_insensitive_count, case_insensitive_xdr,
	  case_insensitive_equal },
	{ FATTR4_CASE_PRESERVING, case_preserving_count, case_preserving_xdr,
	  case_preserving_equal },
	{ FATTR4_CHOWN_RESTRICTED, chown_restricted_count, chown_restricted_xdr,
	  chown_restricted_equal },
	{ FATTR4_FILEHANDLE, filehandle_count, filehandle_xdr,
	  filehandle_equal },
	{ FATTR4_FILEID, fileid_count, fileid_xdr, fileid_equal },
	{ FATTR4_FILES_AVAIL, files_avail_count, files_avail_xdr,
	  files_avail_equal },
	{ FATTR4_FILES_FREE, files_free_count, files_free_xdr,
	  files_free_equal },
	{ FATTR4_FILES_TOTAL, files_total_count, files_total_xdr,
	  files_total_equal },
	{ FATTR4_FS_LOCATIONS, fs_locations_count, fs_locations_xdr,
	  fs_locations_equal },
	{ FATTR4_HIDDEN, hidden_count, hidden_xdr, hidden_equal },
	{ FATTR4_HOMOGENEOUS, homogeneous_count, homogeneous_xdr,
	  homogeneous_equal },
	{ FATTR4_MAXFILESIZE, maxfilesize_count, maxfilesize_xdr,
	  maxfilesize_equal },
	{ FATTR4_MAXLINK, maxlink_count, maxlink_xdr, maxlink_equal },
	{ FATTR4_MAXNAME, maxname_count, maxname_xdr, maxname_equal },
	{ FATTR4_MAXREAD, maxread_count, maxread_xdr, maxread_equal },
	{ FATTR4_MAXWRITE, maxwrite_count, maxwrite_xdr, maxwrite_equal },
	{ FATTR4_MIMETYPE, mimetype_count, mimetype_xdr, mimetype_equal },
	{ FATTR4_MODE, mode_count, mode_xdr, mode_equal },
	{ FATTR4_NO_TRUNC, no_trunc_count, no_trunc_xdr, no_trunc_equal },
	{ FATTR4_NUMLINKS, numlinks_count, numlinks_xdr, numlinks_equal },
	{ FATTR4_OWNER, owner_count, owner_xdr, owner_equal },
	{ FATTR4_OWNER_GROUP, owner_group_count, owner_group_xdr,
	  owner_group_equal },
	{ FATTR4_QUOTA_AVAIL_HARD, quota_avail_hard_count, quota_avail_hard_xdr,
	  quota_avail_hard_equal },
	{ FATTR4_QUOTA_AVAIL_SOFT, quota_avail_soft_count, quota_avail_soft_xdr,
	  quota_avail_soft_equal },
	{ FATTR4_QUOTA_USED, quota_used_count, quota_used_xdr,
	  quota_used_equal },
	{ FATTR4_RAWDEV, rawdev_count, rawdev_xdr, rawdev_equal },
	{ FATTR4_SPACE_AVAIL, space_avail_count, space_avail_xdr,
	  space_avail_equal },
	{ FATTR4_SPACE_FREE, space_free_count, space_free_xdr,
	  space_free_equal },
	{ FATTR4_SPACE_TOTAL, space_total_count, space_total_xdr,
	  space_total_equal },
	{ FATTR4_SPACE_USED, space_used_count, space_used_xdr,
	  space_used_equal },
	{ FATTR4_SYSTEM, system_count, system_xdr, system_equal },
	{ FATTR4_TIME_ACCESS, time_access_count, time_access_xdr,
	  time_access_equal },
	{ FATTR4_TIME_ACCESS_SET, time_access_set_count, time_access_set_xdr,
	  time_access_set_equal },
	{ FATTR4_TIME_BACKUP, time_backup_count, time_backup_xdr,
	  time_backup_equal },
	{ FATTR4_TIME_CREATE, time_create_count, time_create_xdr,
	  time_create_equal },
	{ FATTR4_TIME_DELTA, time_delta_count, time_delta_xdr,
	  time_delta_equal },
	{ FATTR4_TIME_METADATA, time_metadata_count, time_metadata_xdr,
	  time_metadata_equal },
	{ FATTR4_TIME_MODIFY, time_modify_count, time_modify_xdr,
	  time_modify_equal },
	{ FATTR4_TIME_MODIFY_SET, time_modify_set_count, time_modify_set_xdr,
	  time_modify_set_equal },
	{ FATTR4_MOUNTED_ON_FILEID, mounted_on_fileid_count,
	  mounted_on_fileid_xdr, mounted_on_fileid_equal },
	{ FATTR4_DIR_NOTIF_DELAY, dir_notif_delay_count, dir_notif_delay_xdr,
	  dir_notif_delay_equal },
	{ FATTR4_DIRENT_NOTIF_DELAY, dirent_notif_delay_count,
	  dirent_notif_delay_xdr, dirent_notif_delay_equal },
	{ FATTR4_DACL, dacl_count, dacl_xdr, dacl_equal },
	{ FATTR4_SACL, sacl_count, sacl_xdr, sacl_equal },
	{ FATTR4_CHANGE_POLICY, change_policy_count, change_policy_xdr,
	  change_policy_equal },
	{ FATTR4_FS_STATUS, fs_status_count, fs_status_xdr, fs_status_equal },
	{ FATTR4_FS_LAYOUT_TYPES, fs_layout_types_count, fs_layout_types_xdr,
	  fs_layout_types_equal },
	{ FATTR4_LAYOUT_HINT, layout_hint_count, layout_hint_xdr,
	  layout_hint_equal },
	{ FATTR4_LAYOUT_TYPES, layout_types_count, layout_types_xdr,
	  layout_types_equal },
	{ FATTR4_LAYOUT_BLKSIZE, layout_blksize_count, layout_blksize_xdr,
	  layout_blksize_equal },
	{ FATTR4_LAYOUT_ALIGNMENT, layout_alignment_count, layout_alignment_xdr,
	  layout_alignment_equal },
	{ FATTR4_FS_LOCATIONS_INFO, fs_locations_info_count,
	  fs_locations_info_xdr, fs_locations_info_equal },
	{ FATTR4_MDSTHRESHOLD, mdsthreshold_count, mdsthreshold_xdr,
	  mdsthreshold_equal },
	{ FATTR4_RETENTION_GET, retention_get_count, retention_get_xdr,
	  retention_get_equal },
	{ FATTR4_RETENTION_SET, retention_set_count, retention_set_xdr,
	  retention_set_equal },
	{ FATTR4_RETENTEVT_GET, retentevt_get_count, retentevt_get_xdr,
	  retentevt_get_equal },
	{ FATTR4_RETENTEVT_SET, retentevt_set_count, retentevt_set_xdr,
	  retentevt_set_equal },
	{ FATTR4_RETENTION_HOLD, retention_hold_count, retention_hold_xdr,
	  retention_hold_equal },
	{ FATTR4_MODE_SET_MASKED, mode_set_masked_count, mode_set_masked_xdr,
	  mode_set_masked_equal },
	{ FATTR4_SUPPATTR_EXCLCREAT, suppattr_exclcreat_count,
	  suppattr_exclcreat_xdr, suppattr_exclcreat_equal },
	{ FATTR4_FS_CHARSET_CAP, fs_charset_cap_count, fs_charset_cap_xdr,
	  fs_charset_cap_equal },
	{ FATTR4_CLONE_BLKSIZE, clone_blksize_count, clone_blksize_xdr,
	  clone_blksize_equal },
	{ FATTR4_SPACE_FREED, space_freed_count, space_freed_xdr,
	  space_freed_equal },
	{ FATTR4_CHANGE_ATTR_TYPE, change_attr_type_count, change_attr_type_xdr,
	  change_attr_type_equal },
	{ FATTR4_SEC_LABEL, sec_label_count, sec_label_xdr, sec_label_equal },
	{ FATTR4_MODE_UMASK, mode_umask_count, mode_umask_xdr,
	  mode_umask_equal },
	{ FATTR4_XATTR_SUPPORT, xattr_support_count, xattr_support_xdr,
	  xattr_support_equal },
	{ FATTR4_OFFLINE, offline_count, offline_xdr, offline_equal },
	{ FATTR4_TIME_DELEG_ACCESS, time_deleg_access_count,
	  time_deleg_access_xdr, time_deleg_access_equal },
	{ FATTR4_TIME_DELEG_MODIFY, time_deleg_modify_count,
	  time_deleg_modify_xdr, time_deleg_modify_equal },
	{ FATTR4_OPEN_ARGUMENTS, open_arguments_count, open_arguments_xdr,
	  open_arguments_equal },
	{ FATTR4_UNCACHEABLE_FILE_DATA, uncacheable_file_data_count,
	  uncacheable_file_data_xdr, uncacheable_file_data_equal },
	{ FATTR4_UNCACHEABLE_DIRENT_METADATA, uncacheable_dirent_metadata_count,
	  uncacheable_dirent_metadata_xdr, uncacheable_dirent_metadata_equal },
	{ FATTR4_CODING_BLOCK_SIZE, coding_block_size_count,
	  coding_block_size_xdr, coding_block_size_equal }
};

int nfs4_attribute_init(void)
{
	bitmap4 *bm = supported_attributes;
	bitmap4_init(bm, FATTR4_ATTRIBUTE_MAX);

	bitmap4_attribute_set(bm, FATTR4_SUPPORTED_ATTRS);
	bitmap4_attribute_set(bm, FATTR4_TYPE);
	bitmap4_attribute_set(bm, FATTR4_FH_EXPIRE_TYPE);
	bitmap4_attribute_set(bm, FATTR4_CHANGE);
	bitmap4_attribute_set(bm, FATTR4_SIZE);
	bitmap4_attribute_set(bm, FATTR4_LINK_SUPPORT);
	bitmap4_attribute_set(bm, FATTR4_SYMLINK_SUPPORT);
	bitmap4_attribute_set(bm, FATTR4_NAMED_ATTR);
	bitmap4_attribute_set(bm, FATTR4_FSID);
	bitmap4_attribute_set(bm, FATTR4_UNIQUE_HANDLES);
	bitmap4_attribute_set(bm, FATTR4_LEASE_TIME);
	bitmap4_attribute_set(bm, FATTR4_RDATTR_ERROR);
	bitmap4_attribute_set(bm, FATTR4_FILEHANDLE);
	bitmap4_attribute_set(bm, FATTR4_SUPPATTR_EXCLCREAT);
	bitmap4_attribute_clear(bm, FATTR4_ACL);
	bitmap4_attribute_clear(bm, FATTR4_ACLSUPPORT);
	bitmap4_attribute_set(bm, FATTR4_ARCHIVE);
	bitmap4_attribute_set(bm, FATTR4_CANSETTIME);
	bitmap4_attribute_set(bm, FATTR4_CASE_INSENSITIVE);
	bitmap4_attribute_set(bm, FATTR4_CASE_PRESERVING);
	bitmap4_attribute_set(bm, FATTR4_CHOWN_RESTRICTED);
	bitmap4_attribute_set(bm, FATTR4_FILEID);
	bitmap4_attribute_set(bm, FATTR4_FILES_AVAIL);
	bitmap4_attribute_set(bm, FATTR4_FILES_FREE);
	bitmap4_attribute_set(bm, FATTR4_FILES_TOTAL);
	bitmap4_attribute_clear(bm, FATTR4_FS_LOCATIONS);
	bitmap4_attribute_set(bm, FATTR4_HIDDEN);
	bitmap4_attribute_set(bm, FATTR4_HOMOGENEOUS);
	bitmap4_attribute_set(bm, FATTR4_MAXFILESIZE);
	bitmap4_attribute_set(bm, FATTR4_MAXLINK);
	bitmap4_attribute_set(bm, FATTR4_MAXNAME);
	bitmap4_attribute_set(bm, FATTR4_MAXREAD);
	bitmap4_attribute_set(bm, FATTR4_MAXWRITE);
	bitmap4_attribute_clear(bm, FATTR4_MIMETYPE);
	bitmap4_attribute_set(bm, FATTR4_MODE);
	bitmap4_attribute_set(bm, FATTR4_NO_TRUNC);
	bitmap4_attribute_set(bm, FATTR4_NUMLINKS);
	bitmap4_attribute_set(bm, FATTR4_OWNER);
	bitmap4_attribute_set(bm, FATTR4_OWNER_GROUP);
	bitmap4_attribute_clear(bm, FATTR4_QUOTA_AVAIL_HARD);
	bitmap4_attribute_clear(bm, FATTR4_QUOTA_AVAIL_SOFT);
	bitmap4_attribute_clear(bm, FATTR4_QUOTA_USED);
	bitmap4_attribute_set(bm, FATTR4_RAWDEV);
	bitmap4_attribute_set(bm, FATTR4_SPACE_AVAIL);
	bitmap4_attribute_set(bm, FATTR4_SPACE_FREE);
	bitmap4_attribute_set(bm, FATTR4_SPACE_TOTAL);
	bitmap4_attribute_set(bm, FATTR4_SPACE_USED);
	bitmap4_attribute_set(bm, FATTR4_SYSTEM);
	bitmap4_attribute_set(bm, FATTR4_TIME_ACCESS);
	bitmap4_attribute_set(bm, FATTR4_TIME_ACCESS_SET);
	bitmap4_attribute_set(bm, FATTR4_TIME_BACKUP);
	bitmap4_attribute_set(bm, FATTR4_TIME_CREATE);
	bitmap4_attribute_set(bm, FATTR4_TIME_DELTA);
	bitmap4_attribute_set(bm, FATTR4_TIME_METADATA);
	bitmap4_attribute_set(bm, FATTR4_TIME_MODIFY);
	bitmap4_attribute_set(bm, FATTR4_TIME_MODIFY_SET);
	bitmap4_attribute_set(bm, FATTR4_MOUNTED_ON_FILEID);
	bitmap4_attribute_set(bm, FATTR4_DIR_NOTIF_DELAY);
	bitmap4_attribute_set(bm, FATTR4_DIRENT_NOTIF_DELAY);
	bitmap4_attribute_clear(bm, FATTR4_DACL);
	bitmap4_attribute_clear(bm, FATTR4_SACL);
	bitmap4_attribute_set(bm, FATTR4_CHANGE_POLICY);
	bitmap4_attribute_set(bm, FATTR4_FS_STATUS);
	/*
	 * Layout attributes: advertise when role is MDS or combined.
	 * The server_state may not be available during early init
	 * (system_attrs_init runs before server_state_init), so
	 * default to cleared.  inode_to_nattr populates the actual
	 * values at GETATTR time.
	 */
	bitmap4_attribute_clear(bm, FATTR4_FS_LAYOUT_TYPES);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_TYPES);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_HINT);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_BLKSIZE);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_ALIGNMENT);
	bitmap4_attribute_clear(bm, FATTR4_FS_LOCATIONS_INFO);
	bitmap4_attribute_clear(bm, FATTR4_MDSTHRESHOLD);
	bitmap4_attribute_set(bm, FATTR4_RETENTION_GET);
	bitmap4_attribute_set(bm, FATTR4_RETENTION_SET);
	bitmap4_attribute_set(bm, FATTR4_RETENTEVT_GET);
	bitmap4_attribute_set(bm, FATTR4_RETENTEVT_SET);
	bitmap4_attribute_set(bm, FATTR4_RETENTION_HOLD);
	bitmap4_attribute_set(bm, FATTR4_MODE_SET_MASKED);
	bitmap4_attribute_set(bm, FATTR4_FS_CHARSET_CAP);
	bitmap4_attribute_set(bm, FATTR4_CLONE_BLKSIZE);
	bitmap4_attribute_set(bm, FATTR4_SPACE_FREED);
	bitmap4_attribute_set(bm, FATTR4_CHANGE_ATTR_TYPE);
	bitmap4_attribute_set(bm, FATTR4_SEC_LABEL);
	bitmap4_attribute_clear(bm, FATTR4_MODE_UMASK);
	bitmap4_attribute_clear(bm, FATTR4_XATTR_SUPPORT);
	bitmap4_attribute_set(bm, FATTR4_OFFLINE);
	bitmap4_attribute_set(bm, FATTR4_TIME_DELEG_ACCESS);
	bitmap4_attribute_set(bm, FATTR4_TIME_DELEG_MODIFY);
	bitmap4_attribute_set(bm, FATTR4_OPEN_ARGUMENTS);
	bitmap4_attribute_set(bm, FATTR4_UNCACHEABLE_FILE_DATA);
	bitmap4_attribute_set(bm, FATTR4_UNCACHEABLE_DIRENT_METADATA);
	bitmap4_attribute_set(bm, FATTR4_CODING_BLOCK_SIZE);

	/*
	 * suppattr_exclcreat: attributes this server will honour when set
	 * during an EXCLUSIVE4_1 create.  Must match nattr_is_settable()
	 * exactly — read-only required attributes must not appear here.
	 */
	if (bitmap4_init(&system_attrs.suppattr_exclcreat,
			 FATTR4_ATTRIBUTE_MAX))
		return -ENOMEM;

	{
		bitmap4 *se = &system_attrs.suppattr_exclcreat;

		bitmap4_attribute_set(se, FATTR4_SIZE);
		bitmap4_attribute_set(se, FATTR4_ARCHIVE);
		bitmap4_attribute_set(se, FATTR4_HIDDEN);
		bitmap4_attribute_set(se, FATTR4_MODE);
		bitmap4_attribute_set(se, FATTR4_MODE_SET_MASKED);
		bitmap4_attribute_set(se, FATTR4_OWNER);
		bitmap4_attribute_set(se, FATTR4_OWNER_GROUP);
		bitmap4_attribute_set(se, FATTR4_SYSTEM);
		bitmap4_attribute_set(se, FATTR4_TIME_ACCESS_SET);
		bitmap4_attribute_set(se, FATTR4_TIME_CREATE);
		bitmap4_attribute_set(se, FATTR4_TIME_MODIFY_SET);
		bitmap4_attribute_set(se, FATTR4_UNCACHEABLE_FILE_DATA);
		bitmap4_attribute_set(se, FATTR4_UNCACHEABLE_DIRENT_METADATA);
		bitmap4_attribute_set(se, FATTR4_SEC_LABEL);
	}

	return 0;
}

void nfs4_attr_enable_layouts(void)
{
	bitmap4 *bm = supported_attributes;

	bitmap4_attribute_set(bm, FATTR4_FS_LAYOUT_TYPES);
	bitmap4_attribute_set(bm, FATTR4_LAYOUT_TYPES);
}

int nfs4_attribute_fini(void)
{
	bitmap4_destroy(supported_attributes);
	bitmap4_destroy(&system_attrs.suppattr_exclcreat);

	return 0;
}

#ifdef NOT_NOW_BROWN_COW
static void __attribute__((constructor)) nfs4_attribute_load(void)
{
	if (nfs4_attribute_init() != 0)
		abort();
}

static void __attribute__((destructor)) nfs4_attribute_unload(void)
{
	nfs4_attribute_fini();
}
#endif

static void nattr_release(struct nfsv42_attr *nattr)
{
	free(nattr->supported_attrs.bitmap4_val);
	free(nattr->suppattr_exclcreat.bitmap4_val);
	free(nattr->filehandle.nfs_fh4_val);
	free(nattr->fs_layout_types.fattr4_fs_layout_types_val);
	free(nattr->layout_types.fattr4_layout_types_val);
	utf8string_free(&nattr->owner);
	utf8string_free(&nattr->owner_group);
	free(nattr->sec_label.slai_data.slai_data_val);
}

/* ------------------------------------------------------------------ */
/* SETATTR helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Return true if @attr may be set via SETATTR on this server.
 *
 * Unsupported write attrs (acl, dacl, sacl, layout_hint, mimetype,
 * retention/time-deleg) are not listed here so any request containing
 * them yields NFS4ERR_ATTRNOTSUPP.
 */
static bool nattr_is_settable(uint32_t attr)
{
	switch (attr) {
	case FATTR4_SIZE:
	case FATTR4_ARCHIVE:
	case FATTR4_HIDDEN:
	case FATTR4_MODE:
	case FATTR4_MODE_SET_MASKED:
	case FATTR4_OWNER:
	case FATTR4_OWNER_GROUP:
	case FATTR4_SYSTEM:
	case FATTR4_TIME_ACCESS_SET:
	case FATTR4_TIME_CREATE:
	case FATTR4_TIME_DELEG_ACCESS:
	case FATTR4_TIME_DELEG_MODIFY:
	case FATTR4_TIME_MODIFY_SET:
	case FATTR4_UNCACHEABLE_FILE_DATA:
	case FATTR4_UNCACHEABLE_DIRENT_METADATA:
	case FATTR4_SEC_LABEL:
		return true;
	default:
		return false;
	}
}

/*
 * Decode the fattr4 from a SETATTR4args into @nattr.
 *
 * First validates that every attr in the attrmask is settable; returns
 * NFS4ERR_ATTRNOTSUPP if not.  Then XDR-decodes each attr in bitmap
 * order.  Returns NFS4ERR_BADXDR on decode failure, NFS4_OK otherwise.
 *
 * On success the caller must call nattr_release() to free owner /
 * owner_group strings decoded by XDR.
 *
 * mode and mode_set_masked are mutually exclusive (RFC 5661 §6.2.4);
 * returns NFS4ERR_INVAL if both are present.
 */
static nfsstat4 nattr_from_fattr4(fattr4 *fattr, struct nfsv42_attr *nattr)
{
	u_int scan_bits = fattr->attrmask.bitmap4_len * 32U;
	XDR sptr;
	nfsstat4 status = NFS4_OK;
	bool have_mode = false;
	bool have_mode_masked = false;

	/* Validate: all requested attrs must be settable. */
	for (u_int i = 0; i < scan_bits; i++) {
		if (!bitmap4_attribute_is_set(&fattr->attrmask, i))
			continue;
		if (!nattr_is_settable(i))
			return NFS4ERR_ATTRNOTSUPP;
		if (i == FATTR4_MODE)
			have_mode = true;
		if (i == FATTR4_MODE_SET_MASKED)
			have_mode_masked = true;
	}
	if (have_mode && have_mode_masked)
		return NFS4ERR_INVAL;

	xdrmem_create(&sptr, fattr->attr_vals.attrlist4_val,
		      fattr->attr_vals.attrlist4_len, XDR_DECODE);

	for (u_int i = 0; i < scan_bits && status == NFS4_OK; i++) {
		if (!bitmap4_attribute_is_set(&fattr->attrmask, i))
			continue;
		bool ok;

		switch (i) {
		case FATTR4_SIZE:
			ok = xdr_fattr4_size(&sptr, &nattr->size);
			break;
		case FATTR4_ARCHIVE:
			ok = xdr_fattr4_archive(&sptr, &nattr->archive);
			break;
		case FATTR4_HIDDEN:
			ok = xdr_fattr4_hidden(&sptr, &nattr->hidden);
			break;
		case FATTR4_MODE:
			ok = xdr_fattr4_mode(&sptr, &nattr->mode);
			break;
		case FATTR4_MODE_SET_MASKED:
			ok = xdr_fattr4_mode_set_masked(
				&sptr, &nattr->mode_set_masked);
			break;
		case FATTR4_OWNER:
			ok = xdr_fattr4_owner(&sptr, &nattr->owner);
			break;
		case FATTR4_OWNER_GROUP:
			ok = xdr_fattr4_owner_group(&sptr, &nattr->owner_group);
			break;
		case FATTR4_SYSTEM:
			ok = xdr_fattr4_system(&sptr, &nattr->system);
			break;
		case FATTR4_TIME_ACCESS_SET:
			ok = xdr_fattr4_time_access_set(
				&sptr, &nattr->time_access_set);
			break;
		case FATTR4_TIME_CREATE:
			ok = xdr_fattr4_time_create(&sptr, &nattr->time_create);
			break;
		case FATTR4_TIME_DELEG_ACCESS:
			ok = xdr_fattr4_time_deleg_access(
				&sptr, &nattr->time_deleg_access);
			break;
		case FATTR4_TIME_DELEG_MODIFY:
			ok = xdr_fattr4_time_deleg_modify(
				&sptr, &nattr->time_deleg_modify);
			break;
		case FATTR4_TIME_MODIFY_SET:
			ok = xdr_fattr4_time_modify_set(
				&sptr, &nattr->time_modify_set);
			break;
		case FATTR4_UNCACHEABLE_FILE_DATA:
			ok = xdr_fattr4_uncacheable_file_data(
				&sptr, &nattr->uncacheable_file_data);
			break;
		case FATTR4_UNCACHEABLE_DIRENT_METADATA:
			ok = xdr_fattr4_uncacheable_dirent_metadata(
				&sptr, &nattr->uncacheable_dirent_metadata);
			break;
		case FATTR4_SEC_LABEL:
			ok = xdr_fattr4_sec_label(&sptr, &nattr->sec_label);
			break;
		default:
			ok = false;
			break;
		}
		if (!ok)
			status = NFS4ERR_BADXDR;
	}

	xdr_destroy(&sptr);
	return status;
}

/*
 * Apply @nattr (decoded from a SETATTR request) to @inode.
 *
 * POSIX-compatible attrs (size, mode, uid/gid, atime, mtime) are
 * handled by vfs_setattr, which enforces ownership/permission rules
 * and performs the size truncation.
 *
 * NFSv4-specific attrs (archive, hidden, system, uncacheable flags,
 * time_create, mode_set_masked) are applied directly under
 * i_attr_mutex after vfs_setattr returns.
 *
 * @attrmask: the set of attrs that were decoded (from SETATTR4args).
 * @attrsset: output bitmap, populated with attrs that were actually set.
 *
 * Returns NFS4_OK on success; the caller is responsible for calling
 * nattr_release() on @nattr.
 */
static nfsstat4 nattr_to_inode(struct nfsv42_attr *nattr, bitmap4 *attrmask,
			       bitmap4 *attrsset, struct inode *inode,
			       struct authunix_parms *ap, bool size_access_ok)
{
	u_int scan_bits = attrmask->bitmap4_len * 32U;
	struct reffs_sattr rs;
	nfsstat4 status = NFS4_OK;
	int ret;
	bool have_posix = false;
	bool have_nfs4 = false;

	memset(&rs, 0, sizeof(rs));

	/* ---- POSIX attrs: hand off to vfs_setattr ---- */
	for (u_int i = 0; i < scan_bits; i++) {
		if (!bitmap4_attribute_is_set(attrmask, i))
			continue;
		switch (i) {
		case FATTR4_SIZE:
			rs.size = nattr->size;
			rs.size_set = true;
			rs.size_access_ok = size_access_ok;
			have_posix = true;
			break;
		case FATTR4_MODE:
			rs.mode = nattr->mode & 07777;
			rs.mode_set = true;
			have_posix = true;
			break;
		case FATTR4_MODE_SET_MASKED:
			/*
			 * Compute the final mode now, using the current
			 * inode mode as the base.  vfs_setattr will apply
			 * it via rs.mode.
			 */
			pthread_mutex_lock(&inode->i_attr_mutex);
			rs.mode = (inode->i_mode &
				   ~nattr->mode_set_masked.mm_mask_bits) |
				  (nattr->mode_set_masked.mm_value_to_set &
				   nattr->mode_set_masked.mm_mask_bits);
			rs.mode &= 07777;
			pthread_mutex_unlock(&inode->i_attr_mutex);
			rs.mode_set = true;
			have_posix = true;
			break;
		case FATTR4_OWNER:
			ret = reffs_owner_to_uid(&nattr->owner, &rs.uid);
			if (ret) {
				status = NFS4ERR_BADOWNER;
				goto out;
			}
			rs.uid_set = true;
			have_posix = true;
			break;
		case FATTR4_OWNER_GROUP:
			ret = reffs_owner_group_to_gid(&nattr->owner_group,
						       &rs.gid);
			if (ret) {
				status = NFS4ERR_BADOWNER;
				goto out;
			}
			rs.gid_set = true;
			have_posix = true;
			break;
		case FATTR4_TIME_ACCESS_SET:
			if (nattr->time_access_set.set_it ==
			    SET_TO_CLIENT_TIME4) {
				nfstime4_to_timespec(
					&nattr->time_access_set.settime4_u.time,
					&rs.atime);
			}
			rs.atime_set = true;
			rs.atime_now = (nattr->time_access_set.set_it ==
					SET_TO_SERVER_TIME4);
			have_posix = true;
			break;
		case FATTR4_TIME_MODIFY_SET:
			if (nattr->time_modify_set.set_it ==
			    SET_TO_CLIENT_TIME4) {
				nfstime4_to_timespec(
					&nattr->time_modify_set.settime4_u.time,
					&rs.mtime);
			}
			rs.mtime_set = true;
			rs.mtime_now = (nattr->time_modify_set.set_it ==
					SET_TO_SERVER_TIME4);
			have_posix = true;
			break;
		case FATTR4_TIME_DELEG_ACCESS:
			/* RFC 9754: absolute atime from delegating client. */
			nfstime4_to_timespec(&nattr->time_deleg_access,
					     &rs.atime);
			rs.atime_set = true;
			rs.atime_now = false;
			have_posix = true;
			break;
		case FATTR4_TIME_DELEG_MODIFY:
			/* RFC 9754: absolute mtime from delegating client. */
			nfstime4_to_timespec(&nattr->time_deleg_modify,
					     &rs.mtime);
			rs.mtime_set = true;
			rs.mtime_now = false;
			have_posix = true;
			break;
		default:
			have_nfs4 = true;
			break;
		}
	}

	if (have_posix) {
		ret = vfs_setattr(inode, &rs, ap);
		if (ret) {
			status = errno_to_nfs4(ret, OP_SETATTR);
			goto out;
		}
	}

	/* ---- NFSv4-specific attrs: apply under i_attr_mutex ---- */
	if (!have_nfs4)
		goto record_set;

	pthread_mutex_lock(&inode->i_attr_mutex);

	for (u_int i = 0; i < scan_bits; i++) {
		if (!bitmap4_attribute_is_set(attrmask, i))
			continue;
		switch (i) {
		case FATTR4_ARCHIVE:
			if (nattr->archive)
				inode->i_attr_flags |= INODE_IS_ARCHIVED;
			else
				inode->i_attr_flags &= ~INODE_IS_ARCHIVED;
			break;
		case FATTR4_HIDDEN:
			if (nattr->hidden)
				inode->i_attr_flags |= INODE_IS_HIDDEN;
			else
				inode->i_attr_flags &= ~INODE_IS_HIDDEN;
			break;
		case FATTR4_SYSTEM:
			if (nattr->system)
				inode->i_attr_flags |= INODE_IS_SYSTEM;
			else
				inode->i_attr_flags &= ~INODE_IS_SYSTEM;
			break;
		case FATTR4_UNCACHEABLE_FILE_DATA:
			if (nattr->uncacheable_file_data)
				inode->i_attr_flags |=
					INODE_IS_UNCACHEABLE_FILE_DATA;
			else
				inode->i_attr_flags &=
					~INODE_IS_UNCACHEABLE_FILE_DATA;
			break;
		case FATTR4_UNCACHEABLE_DIRENT_METADATA:
			if (nattr->uncacheable_dirent_metadata)
				inode->i_attr_flags |=
					INODE_IS_UNCACHEABLE_DIRENT_METADATA;
			else
				inode->i_attr_flags &=
					~INODE_IS_UNCACHEABLE_DIRENT_METADATA;
			break;
		case FATTR4_TIME_CREATE:
			nfstime4_to_timespec(&nattr->time_create,
					     &inode->i_btime);
			break;
		case FATTR4_SEC_LABEL:
			if (nattr->sec_label.slai_data.slai_data_len >
			    REFFS_SEC_LABEL_MAX) {
				pthread_mutex_unlock(&inode->i_attr_mutex);
				status = NFS4ERR_NAMETOOLONG;
				goto out;
			}
			inode->i_sec_label_lfs =
				nattr->sec_label.slai_lfs.lfs_lfs;
			inode->i_sec_label_pi =
				nattr->sec_label.slai_lfs.lfs_pi;
			inode->i_sec_label_len =
				nattr->sec_label.slai_data.slai_data_len;
			if (inode->i_sec_label_len > 0)
				memcpy(inode->i_sec_label,
				       nattr->sec_label.slai_data.slai_data_val,
				       inode->i_sec_label_len);
			break;
		default:
			/* POSIX attrs handled above */
			break;
		}
	}

	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME);
	inode_sync_to_disk(inode);
	pthread_mutex_unlock(&inode->i_attr_mutex);

record_set:
	/* Record what was set: copy attrmask → attrsset on success. */
	if (bitmap4_copy(attrmask, attrsset) != 0)
		status = NFS4ERR_DELAY;

out:
	return status;
}

static nfsstat4 inode_to_nattr(struct server_state *ss, struct inode *inode,
			       struct nfsv42_attr *nattr)
{
	uint16_t type = inode->i_mode & S_IFMT;
	struct super_block *sb = inode->i_sb;
	int ret = 0;

	switch (type) {
	case S_IFLNK:
		nattr->type = NF4LNK;
		break;
	case S_IFREG:
		nattr->type = NF4REG;
		break;
	case S_IFDIR:
		nattr->type = NF4DIR;
		break;
	case S_IFCHR:
		nattr->type = NF4CHR;
		break;
	case S_IFBLK:
		nattr->type = NF4BLK;
		break;
	case S_IFIFO:
		nattr->type = NF4FIFO;
		break;
	case S_IFSOCK:
		nattr->type = NF4SOCK;
		break;
	}

	ret = bitmap4_copy(supported_attributes, &nattr->supported_attrs);
	if (ret)
		goto out;

	nattr->filehandle.nfs_fh4_val =
		(char *)network_file_handle_construct(sb->sb_id, inode->i_ino);
	if (!nattr->filehandle.nfs_fh4_val)
		goto out;

	nattr->filehandle.nfs_fh4_len = sizeof(struct network_file_handle);

	ret = reffs_owner_from_uid(&nattr->owner,
				   reffs_id_to_uid(inode->i_uid));
	if (ret)
		goto out;

	ret = reffs_owner_group_from_gid(&nattr->owner_group,
					 reffs_id_to_uid(inode->i_gid));
	if (ret)
		goto out;

	nattr->fh_expire_type = system_attrs.fh_expire_type;
	nattr->change =
		atomic_load_explicit(&inode->i_changeid, memory_order_relaxed);
	nattr->size = inode->i_size;
	nattr->link_support = system_attrs.link_support;
	nattr->symlink_support = system_attrs.symlink_support;
	nattr->named_attr = system_attrs.named_attr;
	nattr->fsid.major = inode->i_sb->sb_id;
	nattr->fsid.minor = 0;
	nattr->unique_handles = system_attrs.unique_handles;
	nattr->lease_time = system_attrs.lease_time;
	nattr->rdattr_error = system_attrs.rdattr_error;
	nattr->aclsupport = system_attrs.aclsupport;
	nattr->archive = inode->i_attr_flags & INODE_IS_ARCHIVED;
	nattr->cansettime = system_attrs.cansettime;
	nattr->case_insensitive =
		(reffs_case_get() == reffs_text_case_insensitive);
	nattr->case_preserving = system_attrs.case_preserving;
	nattr->chown_restricted = system_attrs.chown_restricted;

	nattr->fileid = inode->i_ino;
	size_t iu =
		atomic_load_explicit(&sb->sb_inodes_used, memory_order_relaxed);

	nattr->files_avail = sb->sb_inodes_max - iu;
	nattr->files_free = sb->sb_inodes_max - iu;
	nattr->files_total = sb->sb_inodes_max;
	nattr->hidden = inode->i_attr_flags & INODE_IS_HIDDEN;
	nattr->homogeneous = system_attrs.homogeneous;
	nattr->maxfilesize = system_attrs.maxfilesize;
	nattr->maxlink = system_attrs.maxlink;
	nattr->maxname = system_attrs.maxname;
	nattr->maxread = system_attrs.maxread;
	nattr->maxwrite = system_attrs.maxwrite;
	nattr->mode = inode->i_mode & 07777;
	nattr->no_trunc = system_attrs.no_trunc;
	nattr->numlinks =
		atomic_load_explicit(&inode->i_nlink, memory_order_relaxed);
	nattr->quota_avail_hard = system_attrs.quota_avail_hard;
	nattr->quota_avail_soft = system_attrs.quota_avail_soft;
	nattr->rawdev.specdata1 = inode->i_dev_major;
	nattr->rawdev.specdata2 = inode->i_dev_minor;
	size_t bu;

	bu = atomic_load_explicit(&sb->sb_bytes_used, memory_order_relaxed);
	nattr->space_avail = sb->sb_bytes_max - bu;
	nattr->space_free = sb->sb_bytes_max - bu;
	nattr->space_total = sb->sb_bytes_max;
	nattr->space_used = inode->i_used * sb->sb_block_size;
	nattr->system = inode->i_attr_flags & INODE_IS_SYSTEM;

	nattr->time_delta = system_attrs.time_delta;
	timespec_to_nfstime4(&inode->i_ctime, &nattr->time_metadata);
	timespec_to_nfstime4(&inode->i_mtime, &nattr->time_modify);
	timespec_to_nfstime4(&inode->i_atime, &nattr->time_access);
	timespec_to_nfstime4(&inode->i_btime, &nattr->time_create);

	/*
	 * RFC 8881 §5.8.2.19: for the root of a mounted fs,
	 * mounted_on_fileid is the fileid of the covered directory
	 * in the parent fs (the "mounted-on" object).
	 */
	if (inode->i_ino == INODE_ROOT_ID && sb->sb_id != SUPER_BLOCK_ROOT_ID &&
	    sb->sb_mount_dirent)
		nattr->mounted_on_fileid = sb->sb_mount_dirent->rd_ino;
	else
		nattr->mounted_on_fileid = inode->i_ino;
	ret = bitmap4_copy(&system_attrs.suppattr_exclcreat,
			   &nattr->suppattr_exclcreat);
	if (ret)
		goto out;
	nattr->fs_charset_cap = system_attrs.fs_charset_cap;
	nattr->clone_blksize = system_attrs.clone_blksize;
	// nattr->space_freed;
	nattr->change_attr_type = system_attrs.change_attr_type;

	/* RFC 7861: security label from inode */
	nattr->sec_label.slai_lfs.lfs_lfs = inode->i_sec_label_lfs;
	nattr->sec_label.slai_lfs.lfs_pi = inode->i_sec_label_pi;
	if (inode->i_sec_label_len > 0) {
		nattr->sec_label.slai_data.slai_data_val =
			malloc(inode->i_sec_label_len);
		if (nattr->sec_label.slai_data.slai_data_val) {
			nattr->sec_label.slai_data.slai_data_len =
				inode->i_sec_label_len;
			memcpy(nattr->sec_label.slai_data.slai_data_val,
			       inode->i_sec_label, inode->i_sec_label_len);
		}
	}

	// nattr->mode_umask;
	nattr->xattr_support = system_attrs.xattr_support;
	nattr->offline = inode->i_attr_flags & INODE_IS_OFFLINE;
	nattr->uncacheable_file_data = inode->i_attr_flags &
				       INODE_IS_UNCACHEABLE_FILE_DATA;
	nattr->uncacheable_dirent_metadata =
		inode->i_attr_flags & INODE_IS_UNCACHEABLE_DIRENT_METADATA;
	nattr->coding_block_size = system_attrs.coding_block_size;

	/*
	 * Advertise Flex Files layout support when the server role
	 * is MDS or combined.  The client uses this to decide whether
	 * to request layouts via LAYOUTGET.
	 */
	if (ss->ss_exchgid_flags & EXCHGID4_FLAG_USE_PNFS_MDS) {
		nattr->fs_layout_types.fattr4_fs_layout_types_val =
			calloc(2, sizeof(layouttype4));
		if (nattr->fs_layout_types.fattr4_fs_layout_types_val) {
			nattr->fs_layout_types.fattr4_fs_layout_types_len = 2;
			nattr->fs_layout_types.fattr4_fs_layout_types_val[0] =
				LAYOUT4_FLEX_FILES;
			nattr->fs_layout_types.fattr4_fs_layout_types_val[1] =
				LAYOUT4_FLEX_FILES_V2;
		}

		nattr->layout_types.fattr4_layout_types_val =
			calloc(2, sizeof(layouttype4));
		if (nattr->layout_types.fattr4_layout_types_val) {
			nattr->layout_types.fattr4_layout_types_len = 2;
			nattr->layout_types.fattr4_layout_types_val[0] =
				LAYOUT4_FLEX_FILES;
			nattr->layout_types.fattr4_layout_types_val[1] =
				LAYOUT4_FLEX_FILES_V2;
		}
	}

out:
	return NFS4_OK;
}

/*
 * Resume callback for GETATTR after async dstore fan-out.
 *
 * Updates the inode's cached attrs from the DS responses, then
 * falls through to the normal GETATTR encoding path.
 */
static uint32_t nfs4_op_getattr_resume(struct rpc_trans *rt)
{
	struct compound *compound = rt->rt_compound;
	GETATTR4res *res = NFS4_OP_RES_SETUP(compound, opgetattr);
	nfsstat4 *status = &res->status;
	struct dstore_fanout *df = rt->rt_async_data;
	struct inode *inode = compound->c_inode;

	rt->rt_async_data = NULL;

	int fanout_ret = dstore_fanout_result(df);

	if (fanout_ret < 0) {
		/*
		 * Not all DSes responded.  Per design: return DELAY
		 * so the client retries.
		 */
		dstore_fanout_free(df);
		*status = NFS4ERR_DELAY;
		return 0;
	}

	/*
	 * Update inode cached attrs from the DS responses.
	 * Use the latest mtime/atime/size across all mirrors.
	 * Set inode times to NOW (clocks may not be in sync).
	 */
	struct layout_segments *lss = inode->i_layout_segments;

	if (lss && lss->lss_count > 0) {
		struct layout_segment *seg = &lss->lss_segs[0];
		int64_t max_size = 0;
		bool any_changed = false;

		for (uint32_t i = 0; i < seg->ls_nfiles; i++) {
			struct layout_data_file *ldf = &seg->ls_files[i];

			if (ldf->ldf_stale)
				continue;
			if (ldf->ldf_size > max_size)
				max_size = ldf->ldf_size;
		}

		pthread_mutex_lock(&inode->i_attr_mutex);
		if (max_size > inode->i_size) {
			inode->i_size = max_size;
			any_changed = true;

			/* Update space accounting for MDS inodes with
			 * pNFS layouts — data lives on the DS, so the
			 * MDS has no local data block.  Compute i_used
			 * from i_size.
			 */
			struct super_block *sb = inode->i_sb;
			int64_t old_used = inode->i_used;

			inode->i_used =
				inode->i_size / sb->sb_block_size +
				(inode->i_size % sb->sb_block_size ? 1 : 0);

			int64_t used_delta =
				(inode->i_used - old_used) * sb->sb_block_size;
			if (used_delta > 0)
				atomic_fetch_add_explicit(&sb->sb_bytes_used,
							  (size_t)used_delta,
							  memory_order_relaxed);
			else if (used_delta < 0)
				atomic_fetch_sub_explicit(&sb->sb_bytes_used,
							  (size_t)(-used_delta),
							  memory_order_relaxed);
		}
		if (any_changed) {
			struct timespec now;

			clock_gettime(CLOCK_REALTIME, &now);
			inode->i_mtime = now;
			inode->i_ctime = now;
		}
		pthread_mutex_unlock(&inode->i_attr_mutex);
	}

	dstore_fanout_free(df);

	/*
	 * Now encode the GETATTR response with fresh inode attrs.
	 * We must do this here because dispatch_compound will advance
	 * past this op after the resume callback returns.
	 */
	GETATTR4args *args = NFS4_OP_ARG_SETUP(compound, opgetattr);
	GETATTR4resok *resok = NFS4_OP_RESOK_SETUP(res, GETATTR4res_u, resok4);
	bitmap4 *attr_request = &args->attr_request;
	fattr4 *fattr = &resok->obj_attributes;
	struct nfsv42_attr nattr = { 0 };
	int ret;

	if (attr_request->bitmap4_len == 0)
		return 0;

	ret = bitmap4_init(&fattr->attrmask, FATTR4_ATTRIBUTE_MAX);
	if (ret) {
		*status = errno_to_nfs4(ret, NFS4_OP_NUM(compound));
		return 0;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);
	ret = inode_to_nattr(compound->c_server_state, inode, &nattr);
	pthread_mutex_unlock(&inode->i_attr_mutex);
	if (ret) {
		*status = errno_to_nfs4(ret, NFS4_OP_NUM(compound));
		return 0;
	}

	fattr->attr_vals.attrlist4_len = 0;
	u_int scan_bits = attr_request->bitmap4_len * 32U;

	for (u_int i = 0; i < scan_bits; i++) {
		if (bitmap4_attribute_is_set(attr_request, i) &&
		    bitmap4_attribute_is_set(supported_attributes, i))
			fattr->attr_vals.attrlist4_len +=
				nao[i].nao_count(&nattr);
	}

	fattr->attr_vals.attrlist4_val =
		calloc(fattr->attr_vals.attrlist4_len, sizeof(char));
	if (!fattr->attr_vals.attrlist4_val) {
		*status = NFS4ERR_DELAY;
		fattr->attr_vals.attrlist4_len = 0;
		nattr_release(&nattr);
		return 0;
	}

	XDR xdrs;

	xdrmem_create(&xdrs, fattr->attr_vals.attrlist4_val,
		      fattr->attr_vals.attrlist4_len, XDR_ENCODE);

	for (u_int i = 0; i < scan_bits; i++) {
		if (bitmap4_attribute_is_set(attr_request, i) &&
		    bitmap4_attribute_is_set(supported_attributes, i)) {
			*status = nao[i].nao_xdr(&xdrs, &nattr);
			if (*status) {
				fattr->attr_vals.attrlist4_len = 0;
				free(fattr->attr_vals.attrlist4_val);
				fattr->attr_vals.attrlist4_val = NULL;
				break;
			}
			bitmap4_attribute_set(&fattr->attrmask, i);
		}
	}

	xdr_destroy(&xdrs);
	nattr_release(&nattr);
	return 0;
}

/*
 * CB_GETATTR resume callback.
 *
 * Called when the client responds to CB_GETATTR (or timeout fires).
 * If the client returned valid attrs, update the inode's cached
 * times before encoding the GETATTR response.
 */
static uint32_t nfs4_op_getattr_cb_resume(struct rpc_trans *rt)
{
	struct compound *compound = rt->rt_compound;
	GETATTR4res *res = NFS4_OP_RES_SETUP(compound, opgetattr);
	nfsstat4 *status = &res->status;
	struct cb_pending *cp = rt->rt_async_data;
	struct inode *inode = compound->c_inode;
	int cb_status;

	rt->rt_async_data = NULL;
	cb_status = atomic_load_explicit(&cp->cp_status, memory_order_acquire);

	if (cb_status == 0 && cp->cp_res.status == NFS4_OK &&
	    cp->cp_res.resarray.resarray_len >= 2) {
		/*
		 * Extract CB_GETATTR result (op index 1, after CB_SEQUENCE).
		 * The client returns size, atime, mtime in fattr4 format.
		 * For now, we just update the inode's mtime/atime from
		 * whatever the client returned.  A full implementation
		 * would decode the fattr4 bitmap and update selectively.
		 *
		 * TODO: decode fattr4 from CB_GETATTR4resok and merge
		 * individual attrs (size, atime, mtime) into the inode.
		 * For the initial implementation, having the CB round-trip
		 * work is the important part; attr merging is next.
		 */
		TRACE("CB_GETATTR reply OK for ino=%lu", inode->i_ino);
	} else if (cb_status == -ETIMEDOUT) {
		TRACE("CB_GETATTR timeout for ino=%lu, using cached attrs",
		      inode->i_ino);
	} else {
		TRACE("CB_GETATTR failed (%d) for ino=%lu, using cached attrs",
		      cb_status, inode->i_ino);
	}

	cb_pending_free(cp);

	/*
	 * Continue with normal GETATTR encoding using (possibly updated)
	 * inode attrs.  This duplicates the tail of nfs4_op_getattr but
	 * is necessary because we're in the resume path.
	 */
	GETATTR4args *args = NFS4_OP_ARG_SETUP(compound, opgetattr);
	GETATTR4resok *resok = NFS4_OP_RESOK_SETUP(res, GETATTR4res_u, resok4);
	bitmap4 *attr_request = &args->attr_request;
	fattr4 *fattr = &resok->obj_attributes;
	struct nfsv42_attr nattr = { 0 };
	int ret;

	if (attr_request->bitmap4_len == 0)
		return 0;

	ret = bitmap4_init(&fattr->attrmask, FATTR4_ATTRIBUTE_MAX);
	if (ret) {
		*status = errno_to_nfs4(ret, NFS4_OP_NUM(compound));
		return 0;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);
	ret = inode_to_nattr(compound->c_server_state, inode, &nattr);
	pthread_mutex_unlock(&inode->i_attr_mutex);
	if (ret) {
		*status = errno_to_nfs4(ret, NFS4_OP_NUM(compound));
		return 0;
	}

	fattr->attr_vals.attrlist4_len = 0;
	u_int scan_bits = attr_request->bitmap4_len * 32U;

	for (u_int i = 0; i < scan_bits; i++) {
		if (bitmap4_attribute_is_set(attr_request, i) &&
		    bitmap4_attribute_is_set(supported_attributes, i))
			fattr->attr_vals.attrlist4_len +=
				nao[i].nao_count(&nattr);
	}

	fattr->attr_vals.attrlist4_val =
		calloc(fattr->attr_vals.attrlist4_len, sizeof(char));
	if (!fattr->attr_vals.attrlist4_val) {
		*status = NFS4ERR_DELAY;
		fattr->attr_vals.attrlist4_len = 0;
		nattr_release(&nattr);
		return 0;
	}

	XDR xdrs;

	xdrmem_create(&xdrs, fattr->attr_vals.attrlist4_val,
		      fattr->attr_vals.attrlist4_len, XDR_ENCODE);

	for (u_int i = 0; i < scan_bits; i++) {
		if (bitmap4_attribute_is_set(attr_request, i) &&
		    bitmap4_attribute_is_set(supported_attributes, i)) {
			*status = nao[i].nao_xdr(&xdrs, &nattr);
			if (*status) {
				fattr->attr_vals.attrlist4_len = 0;
				free(fattr->attr_vals.attrlist4_val);
				fattr->attr_vals.attrlist4_val = NULL;
				break;
			}
			bitmap4_attribute_set(&fattr->attrmask, i);
		}
	}

	xdr_destroy(&xdrs);
	nattr_release(&nattr);
	return 0;
}

uint32_t nfs4_op_getattr(struct compound *compound)
{
	GETATTR4args *args = NFS4_OP_ARG_SETUP(compound, opgetattr);
	GETATTR4res *res = NFS4_OP_RES_SETUP(compound, opgetattr);
	nfsstat4 *status = &res->status;
	GETATTR4resok *resok = NFS4_OP_RESOK_SETUP(res, GETATTR4res_u, resok4);

	bitmap4 *attr_request = &args->attr_request;
	fattr4 *fattr = &resok->obj_attributes;

	u_int i;
	u_int scan_bits = attr_request->bitmap4_len * 32U;

	XDR sptr;
	XDR *xdrs = &sptr;

	int ret = 0;

	struct inode *inode = compound->c_inode;
	struct nfsv42_attr nattr = { 0 };

	/* Nothing requested */
	if (attr_request->bitmap4_len == 0)
		goto out;

	/*
	 * MDS mode: if there is an active write layout on this inode
	 * and the DS attrs haven't already been refreshed in this
	 * compound (e.g., by a preceding LAYOUTRETURN), fan out
	 * GETATTR to all DSes to refresh cached attrs.
	 */
	if (inode && inode->i_layout_segments &&
	    inode->i_layout_segments->lss_count > 0 &&
	    !(compound->c_flags & COMPOUND_DS_ATTRS_REFRESHED) &&
	    inode_has_write_layout(inode)) {
		struct layout_segment *seg =
			&inode->i_layout_segments->lss_segs[0];

		struct dstore_fanout *df = dstore_fanout_alloc(seg->ls_nfiles);
		if (!df) {
			*status = NFS4ERR_DELAY;
			goto out;
		}

		df->df_op = FANOUT_GETATTR;

		for (uint32_t fi = 0; fi < seg->ls_nfiles; fi++) {
			struct layout_data_file *ldf = &seg->ls_files[fi];
			struct fanout_slot *slot = &df->df_slots[fi];

			slot->fs_ds = dstore_find(ldf->ldf_dstore_id);
			if (!slot->fs_ds) {
				LOG("GETATTR: dstore[%u] not found for "
				    "ino=%lu — check data_server config",
				    ldf->ldf_dstore_id, inode->i_ino);
				dstore_fanout_free(df);
				*status = NFS4ERR_DELAY;
				goto out;
			}
			memcpy(slot->fs_fh, ldf->ldf_fh, ldf->ldf_fh_len);
			slot->fs_fh_len = ldf->ldf_fh_len;
			slot->fs_ldf = ldf;
		}

		struct rpc_trans *rt = compound->c_rt;
		struct task *t = rt->rt_task;

		compound->c_flags |= COMPOUND_DS_ATTRS_REFRESHED;
		rt->rt_next_action = nfs4_op_getattr_resume;
		rt->rt_async_data = df;
		task_pause(t);
		dstore_fanout_launch(df, t);
		return NFS4_OP_FLAG_ASYNC;
	}

	/*
	 * CB_GETATTR: if another client holds a delegation with
	 * timestamp management (RFC 9754 _ATTRS_DELEG), send
	 * CB_GETATTR to get authoritative timestamps before encoding
	 * the GETATTR response.
	 *
	 * We exclude the requesting client (compound->c_nfs4_client)
	 * from the search — if this client holds the delegation, it
	 * already has authoritative values locally.
	 */
	if (inode) {
		struct client *req_client =
			compound->c_nfs4_client ?
				nfs4_client_to_client(compound->c_nfs4_client) :
				NULL;
		struct stateid *deleg_stid =
			stateid_inode_find_delegation(inode, req_client);

		if (deleg_stid) {
			struct delegation_stateid *ds =
				stid_to_delegation(deleg_stid);

			if (ds->ds_timestamps && deleg_stid->s_client) {
				struct nfs4_client *nc =
					client_to_nfs4(deleg_stid->s_client);
				struct nfs4_session *sess =
					nc ? nfs4_session_find_for_client(
						     compound->c_server_state,
						     nc) :
					     NULL;

				if (sess) {
					struct cb_pending *cp =
						cb_pending_alloc(
							compound->c_rt->rt_task,
							compound,
							OP_CB_GETATTR);
					if (cp) {
						struct network_file_handle cb_nfh =
							compound->c_curr_nfh;
						cb_nfh.nfh_ino = inode->i_ino;
						nfs_fh4 cb_fh4 = {
							.nfs_fh4_len =
								sizeof(cb_nfh),
							.nfs_fh4_val =
								(char *)&cb_nfh,
						};

						struct rpc_trans *rt =
							compound->c_rt;

						rt->rt_next_action =
							nfs4_op_getattr_cb_resume;
						rt->rt_async_data = cp;
						task_pause(rt->rt_task);

						nfs4_cb_getattr_send(
							sess, &cb_fh4,
							attr_request, cp);

						nfs4_session_put(sess);
						stateid_put(deleg_stid);
						return NFS4_OP_FLAG_ASYNC;
					}
					nfs4_session_put(sess);
				}
			}
			stateid_put(deleg_stid);
		}
	}

	ret = bitmap4_init(&fattr->attrmask, FATTR4_ATTRIBUTE_MAX);
	if (ret) {
		*status = errno_to_nfs4(ret, NFS4_OP_NUM(compound));
		goto out;
	}

	/* Note: Once we copy it, all bets are off as to the contents! */
	pthread_mutex_lock(&inode->i_attr_mutex);
	ret = inode_to_nattr(compound->c_server_state, inode, &nattr);
	pthread_mutex_unlock(&inode->i_attr_mutex);
	if (ret) {
		*status = errno_to_nfs4(ret, NFS4_OP_NUM(compound));
		goto out;
	}

	fattr->attr_vals.attrlist4_len = 0;
	for (i = 0; i < scan_bits; i++) {
		if (bitmap4_attribute_is_set(attr_request, i)) {
			bool b = bitmap4_attribute_is_set(supported_attributes,
							  i);
			if (b) {
				fattr->attr_vals.attrlist4_len +=
					nao[i].nao_count(&nattr);
			}
		}
	}

	fattr->attr_vals.attrlist4_val =
		calloc(fattr->attr_vals.attrlist4_len, sizeof(char));
	if (!fattr->attr_vals.attrlist4_val) {
		*status = NFS4ERR_DELAY;
		fattr->attr_vals.attrlist4_len = 0;
		goto out;
	}

	xdrmem_create(xdrs, fattr->attr_vals.attrlist4_val,
		      fattr->attr_vals.attrlist4_len, XDR_ENCODE);

	for (i = 0; i < scan_bits; i++) {
		if (bitmap4_attribute_is_set(attr_request, i)) {
			bool b = bitmap4_attribute_is_set(supported_attributes,
							  i);
			if (b) {
				*status = nao[i].nao_xdr(xdrs, &nattr);
				if (*status) {
					fattr->attr_vals.attrlist4_len = 0;
					free(fattr->attr_vals.attrlist4_val);
					fattr->attr_vals.attrlist4_val = NULL;
					break;
				}
				bitmap4_attribute_set(&fattr->attrmask, i);
			}
		}
	}

	xdr_destroy(xdrs);

out:
	nattr_release(&nattr);

	return 0;
}

/* ------------------------------------------------------------------ */
/* READDIR helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Compute an 8-byte cookie verifier from the directory's mtime.
 * Stable across calls as long as the directory is not modified.
 * Called under inode->i_attr_mutex.
 */
static void dir_make_cookieverf(struct inode *inode, verifier4 cv)
{
	uint64_t v = (uint64_t)inode->i_mtime.tv_sec * 1000000000ULL +
		     (uint64_t)inode->i_mtime.tv_nsec;
	memcpy(cv, &v, sizeof(verifier4));
}

/*
 * Encode the requested attributes for one READDIR entry into @out.
 * Mirrors nfs4_op_getattr: nao_count sizing pass, then nao_xdr encode.
 *
 * On success returns NFS4_OK and @out->attrmask / @out->attr_vals are
 * populated (caller owns both allocations).
 * On failure both fields are left zeroed.
 */
static nfsstat4 entry4_encode_attrs(bitmap4 *attr_request,
				    struct nfsv42_attr *nattr, fattr4 *out)
{
	u_int i;
	u_int scan_bits = attr_request->bitmap4_len * 32U;
	XDR sptr;
	nfsstat4 status = NFS4_OK;
	int ret;

	ret = bitmap4_init(&out->attrmask, FATTR4_ATTRIBUTE_MAX);
	if (ret)
		return NFS4ERR_DELAY;

	out->attr_vals.attrlist4_len = 0;
	for (i = 0; i < scan_bits; i++) {
		if (bitmap4_attribute_is_set(attr_request, i) &&
		    bitmap4_attribute_is_set(supported_attributes, i)) {
			bitmap4_attribute_set(&out->attrmask, i);
			out->attr_vals.attrlist4_len += nao[i].nao_count(nattr);
		}
	}

	out->attr_vals.attrlist4_val = calloc(out->attr_vals.attrlist4_len, 1);
	if (!out->attr_vals.attrlist4_val) {
		out->attr_vals.attrlist4_len = 0;
		bitmap4_destroy(&out->attrmask);
		return NFS4ERR_DELAY;
	}

	xdrmem_create(&sptr, out->attr_vals.attrlist4_val,
		      out->attr_vals.attrlist4_len, XDR_ENCODE);
	for (i = 0; i < scan_bits; i++) {
		if (bitmap4_attribute_is_set(attr_request, i) &&
		    bitmap4_attribute_is_set(supported_attributes, i)) {
			status = nao[i].nao_xdr(&sptr, nattr);
			if (status) {
				free(out->attr_vals.attrlist4_val);
				out->attr_vals.attrlist4_val = NULL;
				out->attr_vals.attrlist4_len = 0;
				bitmap4_destroy(&out->attrmask);
				break;
			}
		}
	}
	xdr_destroy(&sptr);
	return status;
}

/*
 * Synthesize an fattr4 containing only rdattr_error for an entry whose
 * inode could not be loaded.  Called when FATTR4_RDATTR_ERROR is in the
 * client's attr_request.
 */
static nfsstat4 entry4_encode_rdattr_error(nfsstat4 error, fattr4 *out)
{
	struct nfsv42_attr nattr = { .rdattr_error = error };
	count4 sz;
	XDR sptr;
	nfsstat4 status;
	int ret;

	ret = bitmap4_init(&out->attrmask, FATTR4_ATTRIBUTE_MAX);
	if (ret)
		return NFS4ERR_DELAY;

	bitmap4_attribute_set(&out->attrmask, FATTR4_RDATTR_ERROR);
	sz = nao[FATTR4_RDATTR_ERROR].nao_count(&nattr);
	out->attr_vals.attrlist4_val = calloc(sz, 1);
	if (!out->attr_vals.attrlist4_val) {
		bitmap4_destroy(&out->attrmask);
		return NFS4ERR_DELAY;
	}
	out->attr_vals.attrlist4_len = sz;

	xdrmem_create(&sptr, out->attr_vals.attrlist4_val, sz, XDR_ENCODE);
	status = nao[FATTR4_RDATTR_ERROR].nao_xdr(&sptr, &nattr);
	xdr_destroy(&sptr);

	if (status) {
		free(out->attr_vals.attrlist4_val);
		out->attr_vals.attrlist4_val = NULL;
		out->attr_vals.attrlist4_len = 0;
		bitmap4_destroy(&out->attrmask);
	}
	return status;
}

uint32_t nfs4_op_readdir(struct compound *compound)
{
	READDIR4args *args = NFS4_OP_ARG_SETUP(compound, opreaddir);
	READDIR4res *res = NFS4_OP_RES_SETUP(compound, opreaddir);
	nfsstat4 *status = &res->status;
	READDIR4resok *resok = NFS4_OP_RESOK_SETUP(res, READDIR4res_u, resok4);

	struct inode *inode = compound->c_inode;
	struct super_block *sb = compound->c_curr_sb;
	struct reffs_dirent *dir_de = NULL;
	bool dir_de_rdlocked = false;

	/*
	 * Phase-1 snapshot: identity fields captured under rcu_read_lock.
	 * We must not call dirent_ensure_inode (may block on I/O) while
	 * holding the read-side lock.
	 */
	struct {
		struct reffs_dirent *rd;
		uint64_t rd_cookie;
		const char *rd_name;
	} *snap = NULL;
	size_t snap_count = 0, snap_cap = 0;

	entry4 *e_prev = NULL;
	int ret = 0;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_BADHANDLE;
		return 0;
	}

	if (!S_ISDIR(inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		return 0;
	}

	if ((count4)sizeof(READDIR4res) > args->maxcount) {
		*status = NFS4ERR_TOOSMALL;
		return 0;
	}

	ret = inode_access_check(inode, &compound->c_ap, R_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, NFS4_OP_NUM(compound));
		return 0;
	}

	/*
	 * After PUTFH the inode may be loaded without its dirent chain.
	 * Reconstruct before walking rd_children.  Root (i_dirent == NULL
	 * by design) falls back to sb->sb_dirent below.
	 */
	if (!inode->i_dirent && compound->c_curr_nfh.nfh_ino != INODE_ROOT_ID) {
		ret = inode_reconstruct_path_to_root(inode);
		if (ret) {
			*status = NFS4ERR_STALE;
			return 0;
		}
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	dir_de = inode->i_dirent ? inode->i_dirent : sb->sb_dirent;
	pthread_rwlock_rdlock(&dir_de->rd_rwlock);
	dir_de_rdlocked = true;

	/* Compute / verify cookieverf. */
	verifier4 cv;
	dir_make_cookieverf(inode, cv);
	if (args->cookie != 0 &&
	    memcmp(args->cookieverf, cv, sizeof(verifier4)) != 0) {
		*status = NFS4ERR_NOT_SAME;
		goto out_unlock;
	}
	memcpy(resok->cookieverf, cv, sizeof(verifier4));

	/*
	 * Phase 1: snapshot dirent identity under rcu_read_lock.
	 * Pre-allocate outside RCU; restart if overflow.
	 */
	if (!snap) {
		snap_cap = 64;
		snap = calloc(snap_cap, sizeof(*snap));
		if (!snap) {
			*status = NFS4ERR_DELAY;
			goto out_unlock;
		}
	}

restart_snap:
	snap_count = 0;
	rcu_read_lock();
	{
		struct reffs_dirent *rd;

		cds_list_for_each_entry_rcu(rd, &dir_de->rd_children,
					    rd_siblings) {
			if (rd->rd_cookie <= args->cookie)
				continue;
			if (snap_count == snap_cap) {
				rcu_read_unlock();
				size_t new_cap = snap_cap * 2;
				void *tmp =
					realloc(snap, new_cap * sizeof(*snap));
				if (!tmp) {
					free(snap);
					snap = NULL;
					*status = NFS4ERR_DELAY;
					goto out_unlock;
				}
				snap = tmp;
				snap_cap = new_cap;
				goto restart_snap;
			}
			snap[snap_count].rd = rd;
			snap[snap_count].rd_cookie = rd->rd_cookie;
			snap[snap_count].rd_name = rd->rd_name;
			snap_count++;
		}
	}
	rcu_read_unlock();

	/*
	 * Phase 1 complete: release rd_rwlock before Phase 2.
	 * dirent_ensure_inode may block on I/O; holding rd_rwlock across
	 * that would deadlock against vfs_lock_dirs (writer) in concurrent
	 * create threads.
	 */
	pthread_rwlock_unlock(&dir_de->rd_rwlock);
	dir_de_rdlocked = false;

	/*
	 * Phase 2a: pre-warm the idmap cache for owner strings.
	 *
	 * If the client requested FATTR4_OWNER or FATTR4_OWNER_GROUP,
	 * collect unique uid/gid values from the snapshot entries and
	 * resolve them in parallel with a bounded timeout.  This avoids
	 * per-entry blocking on external resolvers during encoding.
	 */
	bool wants_owner =
		bitmap4_attribute_is_set(&args->attr_request, FATTR4_OWNER);
	bool wants_group = bitmap4_attribute_is_set(&args->attr_request,
						    FATTR4_OWNER_GROUP);

	if ((wants_owner || wants_group) && snap_count > 0) {
		uid_t pw_uids[64];
		gid_t pw_gids[64];
		int npu = 0, npg = 0;

		for (size_t si = 0; si < snap_count; si++) {
			struct inode *child = dirent_ensure_inode(snap[si].rd);

			if (!child)
				continue;
			if (wants_owner && npu < 64) {
				uid_t u = reffs_id_to_uid(child->i_uid);
				bool dup = false;

				for (int j = 0; j < npu; j++)
					if (pw_uids[j] == u) {
						dup = true;
						break;
					}
				if (!dup)
					pw_uids[npu++] = u;
			}
			if (wants_group && npg < 64) {
				gid_t g = reffs_id_to_uid(child->i_gid);
				bool dup = false;

				for (int j = 0; j < npg; j++)
					if (pw_gids[j] == g) {
						dup = true;
						break;
					}
				if (!dup)
					pw_gids[npg++] = g;
			}
			inode_active_put(child);
		}
		if (npu > 0 || npg > 0)
			idmap_prewarm(pw_uids, npu, pw_gids, npg, 0);
	}

	/*
	 * Phase 2b: fault in inodes, encode attrs, build the reply.
	 *
	 * Two limits (RFC 5661 §18.23):
	 *   dircount  – non-attribute directory data (cookie + name)
	 *   maxcount  – total wire bytes of the complete reply
	 *
	 * Wire cost per entry:
	 *   dir_bytes   = 4 (bool) + 8 (cookie) + 4 (name len) + roundup4(namelen)
	 *   entry_bytes = dir_bytes
	 *               + 4 (bitmap4 len) + 4*attrmask_words (bitmap words)
	 *               + 4 (attrlist len) + roundup4(attr_bytes)
	 *
	 * attr_request->bitmap4_len is used as an upper bound for the response
	 * attrmask word count (the actual attrmask can only be a subset).
	 */
	count4 total_dir_bytes = (count4)sizeof(READDIR4res);
	count4 total_max_bytes = (count4)sizeof(READDIR4res);
	u_int scan_bits = args->attr_request.bitmap4_len * 32U;
	bool rdattr_error_requested = bitmap4_attribute_is_set(
		&args->attr_request, FATTR4_RDATTR_ERROR);

	for (size_t si = 0; si < snap_count; si++) {
		struct inode *child = dirent_ensure_inode(snap[si].rd);
		nfsstat4 attr_status = NFS4_OK;
		struct nfsv42_attr nattr = { 0 };
		entry4 *e;

		if (!child) {
			if (!rdattr_error_requested)
				goto past_eof;
			attr_status = NFS4ERR_SERVERFAULT;
		} else {
			pthread_mutex_lock(&child->i_attr_mutex);
			ret = inode_to_nattr(compound->c_server_state, child,
					     &nattr);
			pthread_mutex_unlock(&child->i_attr_mutex);
			inode_active_put(child);
			if (ret)
				attr_status = errno_to_nfs4(
					ret, NFS4_OP_NUM(compound));
		}

		/*
		 * Mount-point fsid override: if this entry has a sb
		 * mounted on it, show the child sb's fsid so the
		 * client detects the filesystem boundary.
		 */
		if (attr_status == NFS4_OK &&
		    (__atomic_load_n(&snap[si].rd->rd_state, __ATOMIC_ACQUIRE) &
		     RD_MOUNTED_ON)) {
			struct super_block *msb =
				super_block_find_mounted_on(snap[si].rd);
			if (msb) {
				nattr.fsid.major = msb->sb_id;
				super_block_put(msb);
			}
		}

		/* Compute wire sizes before committing to the entry. */
		const char *name = snap[si].rd_name;
		size_t namelen = strlen(name);
		count4 dir_bytes = 4 + 8 + 4 + (count4)((namelen + 3) & ~3u);
		count4 attr_bytes = 0;

		if (attr_status == NFS4_OK) {
			for (u_int i = 0; i < scan_bits; i++) {
				if (bitmap4_attribute_is_set(
					    &args->attr_request, i) &&
				    bitmap4_attribute_is_set(
					    supported_attributes, i))
					attr_bytes += nao[i].nao_count(&nattr);
			}
		}

		count4 attrmask_words = args->attr_request.bitmap4_len;
		count4 entry_bytes =
			dir_bytes + 4 + 4 * attrmask_words /* attrmask XDR */
			+ 4 + ((attr_bytes + 3) & ~3u); /* attrlist XDR */

		if (args->dircount &&
		    total_dir_bytes + dir_bytes > args->dircount) {
			nattr_release(&nattr);
			goto past_eof;
		}
		if (total_max_bytes + entry_bytes > args->maxcount) {
			nattr_release(&nattr);
			goto past_eof;
		}

		e = calloc(1, sizeof(*e));
		if (!e) {
			nattr_release(&nattr);
			goto past_eof;
		}

		e->name.utf8string_val = strdup(name);
		if (!e->name.utf8string_val) {
			free(e);
			nattr_release(&nattr);
			goto past_eof;
		}
		e->name.utf8string_len = (u_int)namelen;
		e->cookie = snap[si].rd_cookie;

		if (attr_status == NFS4_OK)
			attr_status = entry4_encode_attrs(&args->attr_request,
							  &nattr, &e->attrs);
		nattr_release(&nattr);

		if (attr_status) {
			if (rdattr_error_requested)
				attr_status = entry4_encode_rdattr_error(
					attr_status, &e->attrs);
			if (attr_status) {
				free(e->name.utf8string_val);
				free(e);
				goto past_eof;
			}
		}

		total_dir_bytes += dir_bytes;
		total_max_bytes += entry_bytes;

		if (!resok->reply.entries)
			resok->reply.entries = e;
		else
			e_prev->nextentry = e;
		e_prev = e;
	}

	free(snap);
	snap = NULL;
	resok->reply.eof = true;

past_eof:
	free(snap);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);

out_unlock:
	if (dir_de_rdlocked)
		pthread_rwlock_unlock(&dir_de->rd_rwlock);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	return 0;
}

/*
 * Resume callback for SETATTR after async dstore truncate fan-out.
 */
static uint32_t nfs4_op_setattr_resume(struct rpc_trans *rt)
{
	struct compound *compound = rt->rt_compound;
	SETATTR4args *args = NFS4_OP_ARG_SETUP(compound, opsetattr);
	SETATTR4res *res = NFS4_OP_RES_SETUP(compound, opsetattr);
	nfsstat4 *status = &res->status;
	struct dstore_fanout *df = rt->rt_async_data;

	rt->rt_async_data = NULL;

	int fanout_ret = dstore_fanout_result(df);

	/* Check WCC from each slot before freeing the fanout. */
	bool has_wl = inode_has_write_layout(compound->c_inode);

	for (uint32_t i = 0; i < df->df_total; i++) {
		struct fanout_slot *slot = &df->df_slots[i];

		dstore_wcc_check(&slot->fs_wcc, slot->fs_ldf, has_wl,
				 slot->fs_ldf ? slot->fs_ldf->ldf_dstore_id : 0,
				 compound->c_inode->i_ino);
	}

	dstore_fanout_free(df);

	if (fanout_ret < 0) {
		*status = NFS4ERR_IO;
		return 0;
	}

	/*
	 * DS truncates succeeded.  Now apply the full SETATTR
	 * locally (size + any other attrs the client requested).
	 */
	struct nfsv42_attr nattr = { 0 };
	fattr4 *fattr = &args->obj_attributes;

	*status = nattr_from_fattr4(fattr, &nattr);
	if (*status == NFS4_OK)
		*status = nattr_to_inode(&nattr, &fattr->attrmask,
					 &res->attrsset, compound->c_inode,
					 &compound->c_ap, false);

	nattr_release(&nattr);
	return 0;
}

uint32_t nfs4_op_setattr(struct compound *compound)
{
	SETATTR4args *args = NFS4_OP_ARG_SETUP(compound, opsetattr);
	SETATTR4res *res = NFS4_OP_RES_SETUP(compound, opsetattr);
	nfsstat4 *status = &res->status;

	struct nfsv42_attr nattr = { 0 };
	fattr4 *fattr = &args->obj_attributes;
	bool nattr_valid = false;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_BADHANDLE;
		goto out;
	}

	if (nfs4_check_grace()) {
		*status = NFS4ERR_GRACE;
		goto out;
	}

	/*
	 * RFC 9754: TIME_DELEG_ACCESS / TIME_DELEG_MODIFY are only
	 * valid when the client holds a timestamp delegation on this
	 * file.  Verify the stateid is a delegation with ds_timestamps.
	 */
	if (bitmap4_attribute_is_set(&fattr->attrmask,
				     FATTR4_TIME_DELEG_ACCESS) ||
	    bitmap4_attribute_is_set(&fattr->attrmask,
				     FATTR4_TIME_DELEG_MODIFY)) {
		uint32_t seqid, id, type, cookie;
		unpack_stateid4(&args->stateid, &seqid, &id, &type, &cookie);
		struct stateid *stid = NULL;
		if (!stateid4_is_special(&args->stateid))
			stid = stateid_find(compound->c_inode, id);
		if (!stid || stid->s_tag != Delegation_Stateid ||
		    stid->s_cookie != cookie ||
		    !stid_to_delegation(stid)->ds_timestamps) {
			stateid_put(stid);
			*status = NFS4ERR_BAD_STATEID;
			goto out;
		}
		stateid_put(stid);
	}

	/*
	 * MDS mode: if SETATTR includes a size change and the file
	 * has layout segments, fan out the truncate to all DSes
	 * asynchronously, then resume to apply attrs locally.
	 */
	if (bitmap4_attribute_is_set(&fattr->attrmask, FATTR4_SIZE) &&
	    compound->c_inode->i_layout_segments &&
	    compound->c_inode->i_layout_segments->lss_count > 0) {
		*status = nattr_from_fattr4(fattr, &nattr);
		if (*status)
			goto out;

		struct layout_segments *lss =
			compound->c_inode->i_layout_segments;
		struct layout_segment *seg = &lss->lss_segs[0];

		struct dstore_fanout *df = dstore_fanout_alloc(seg->ls_nfiles);
		if (!df) {
			nattr_release(&nattr);
			*status = NFS4ERR_DELAY;
			goto out;
		}

		df->df_op = FANOUT_TRUNCATE;
		df->df_size = nattr.size;
		nattr_release(&nattr);

		for (uint32_t i = 0; i < seg->ls_nfiles; i++) {
			struct layout_data_file *ldf = &seg->ls_files[i];
			struct fanout_slot *slot = &df->df_slots[i];

			slot->fs_ds = dstore_find(ldf->ldf_dstore_id);
			if (!slot->fs_ds) {
				LOG("SETATTR: dstore[%u] not found for "
				    "ino=%lu — check data_server config",
				    ldf->ldf_dstore_id,
				    compound->c_inode->i_ino);
				dstore_fanout_free(df);
				*status = NFS4ERR_DELAY;
				goto out;
			}
			memcpy(slot->fs_fh, ldf->ldf_fh, ldf->ldf_fh_len);
			slot->fs_fh_len = ldf->ldf_fh_len;
			slot->fs_ldf = ldf;
		}

		struct rpc_trans *rt = compound->c_rt;
		struct task *t = rt->rt_task;

		rt->rt_next_action = nfs4_op_setattr_resume;
		rt->rt_async_data = df;
		task_pause(t);
		dstore_fanout_launch(df, t);
		return NFS4_OP_FLAG_ASYNC;
	}

	/*
	 * If the SETATTR includes a size change, check whether the
	 * stateid grants write access.  If so, skip the file-mode W_OK
	 * check in vfs_setattr — this matches POSIX ftruncate semantics
	 * where the fd's access mode governs, not the file permissions.
	 */
	bool has_write_stateid = false;

	if (bitmap4_attribute_is_set(&fattr->attrmask, FATTR4_SIZE) &&
	    !stateid4_is_special(&args->stateid)) {
		struct stateid *stid = NULL;

		*status = nfs4_stateid_resolve(compound, compound->c_inode,
					       &args->stateid, true, &stid);
		if (*status) {
			/*
			 * Bad stateid for a size change — reject.
			 * Non-size SETATTRs with bad stateids are
			 * tolerated (the stateid is advisory).
			 */
			goto out;
		}
		has_write_stateid = true;
		stateid_put(stid);
	}

	*status = nattr_from_fattr4(fattr, &nattr);
	if (*status)
		goto out;
	nattr_valid = true;

	*status = nattr_to_inode(&nattr, &fattr->attrmask, &res->attrsset,
				 compound->c_inode, &compound->c_ap,
				 has_write_stateid);

out:
	if (nattr_valid)
		nattr_release(&nattr);

	return 0;
}

/*
 * nfs4_apply_createattrs - apply an fattr4 to a newly created inode.
 *
 * Used by OPEN CREATE to honour createattrs supplied by the client.
 * @attrsset may point to an unallocated bitmap4 (len=0, val=NULL); in
 * that case attribute-set reporting is silently skipped.
 */
nfsstat4 nfs4_apply_createattrs(fattr4 *fattr, struct inode *inode,
				bitmap4 *attrsset, struct authunix_parms *ap)
{
	struct nfsv42_attr nattr = { 0 };
	nfsstat4 status;

	status = nattr_from_fattr4(fattr, &nattr);
	if (status)
		return status;

	status = nattr_to_inode(&nattr, &fattr->attrmask, attrsset, inode, ap,
				true);
	nattr_release(&nattr);
	return status;
}

/*
 * Shared implementation for VERIFY and NVERIFY.
 *
 * Encodes the current inode's attrs for the same attrmask the client
 * sent, then compares the bytes.  If the buffers match the attrs are
 * equal; if not they differ.
 *
 * VERIFY  succeeds (NFS4_OK) when attrs are equal; fails with
 *         NFS4ERR_NOT_SAME when they differ.
 * NVERIFY succeeds (NFS4_OK) when attrs differ; fails with
 *         NFS4ERR_SAME when they are equal.
 */
static nfsstat4 verify_common(struct compound *compound, fattr4 *obj_attrs,
			      bool invert, nfs_opnum4 opnum)
{
	struct inode *inode = compound->c_inode;
	u_int scan_bits = obj_attrs->attrmask.bitmap4_len * 32U;
	struct nfsv42_attr cur = { 0 };
	char *cur_buf = NULL;
	count4 cur_bytes = 0;
	XDR sptr;
	nfsstat4 status = NFS4_OK;
	int ret;
	bool attrs_equal;

	pthread_mutex_lock(&inode->i_attr_mutex);
	ret = inode_to_nattr(compound->c_server_state, inode, &cur);
	pthread_mutex_unlock(&inode->i_attr_mutex);
	if (ret) {
		status = errno_to_nfs4(ret, opnum);
		goto out;
	}

	/* Size the current-attr encode buffer. */
	for (u_int i = 0; i < scan_bits; i++) {
		if (bitmap4_attribute_is_set(&obj_attrs->attrmask, i) &&
		    bitmap4_attribute_is_set(supported_attributes, i))
			cur_bytes += nao[i].nao_count(&cur);
	}

	cur_buf = calloc(cur_bytes, 1);
	if (!cur_buf) {
		status = NFS4ERR_DELAY;
		goto out;
	}

	xdrmem_create(&sptr, cur_buf, cur_bytes, XDR_ENCODE);
	for (u_int i = 0; i < scan_bits && status == NFS4_OK; i++) {
		if (bitmap4_attribute_is_set(&obj_attrs->attrmask, i) &&
		    bitmap4_attribute_is_set(supported_attributes, i)) {
			if (nao[i].nao_xdr(&sptr, &cur) != NFS4_OK)
				status = NFS4ERR_SERVERFAULT;
		}
	}
	xdr_destroy(&sptr);
	if (status)
		goto out;

	attrs_equal = (cur_bytes == obj_attrs->attr_vals.attrlist4_len) &&
		      (memcmp(cur_buf, obj_attrs->attr_vals.attrlist4_val,
			      cur_bytes) == 0);

	if (invert)
		status = attrs_equal ? NFS4ERR_SAME : NFS4_OK;
	else
		status = attrs_equal ? NFS4_OK : NFS4ERR_NOT_SAME;

out:
	free(cur_buf);
	nattr_release(&cur);
	return status;
}

uint32_t nfs4_op_verify(struct compound *compound)
{
	VERIFY4args *args = NFS4_OP_ARG_SETUP(compound, opverify);
	VERIFY4res *res = NFS4_OP_RES_SETUP(compound, opverify);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_BADHANDLE;
		return 0;
	}

	*status = verify_common(compound, &args->obj_attributes, false,
				OP_VERIFY);

	return 0;
}

uint32_t nfs4_op_nverify(struct compound *compound)
{
	NVERIFY4args *args = NFS4_OP_ARG_SETUP(compound, opnverify);
	NVERIFY4res *res = NFS4_OP_RES_SETUP(compound, opnverify);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_BADHANDLE;
		return 0;
	}

	*status = verify_common(compound, &args->obj_attributes, true,
				OP_NVERIFY);

	return 0;
}

uint32_t nfs4_op_access(struct compound *compound)
{
	ACCESS4args *args = NFS4_OP_ARG_SETUP(compound, opaccess);
	ACCESS4res *res = NFS4_OP_RES_SETUP(compound, opaccess);
	nfsstat4 *status = &res->status;
	ACCESS4resok *resok = NFS4_OP_RESOK_SETUP(res, ACCESS4res_u, resok4);

	if (!compound->c_inode) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	/*
	 * Map each requested ACCESS4 bit to a POSIX mode check.
	 * Every bit we can evaluate goes into resok->supported;
	 * bits the caller actually holds go into resok->access.
	 *
	 * ACCESS4_READ    → R_OK
	 * ACCESS4_LOOKUP  → X_OK  (directory search)
	 * ACCESS4_MODIFY  → W_OK
	 * ACCESS4_EXTEND  → W_OK  (append / grow)
	 * ACCESS4_DELETE  → W_OK  (write on the object)
	 * ACCESS4_EXECUTE → X_OK  (file execute)
	 */
	static const struct {
		uint32_t bit;
		int mode;
	} checks[] = {
		{ ACCESS4_READ, R_OK },	  { ACCESS4_LOOKUP, X_OK },
		{ ACCESS4_MODIFY, W_OK }, { ACCESS4_EXTEND, W_OK },
		{ ACCESS4_DELETE, W_OK }, { ACCESS4_EXECUTE, X_OK },
	};

	resok->supported = 0;
	resok->access = 0;

	for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		if (!(args->access & checks[i].bit))
			continue;
		resok->supported |= checks[i].bit;
		if (inode_access_check(compound->c_inode, &compound->c_ap,
				       checks[i].mode) == 0)
			resok->access |= checks[i].bit;
	}

out:
	TRACE("%s status=%s(%d) access=0x%x supported=0x%x access_granted=0x%x",
	      __func__, nfs4_err_name(*status), *status, args->access,
	      resok->supported, resok->access);

	return 0;
}

uint32_t nfs4_op_access_mask(struct compound *compound)
{
	ACCESS_MASK4res *res = NFS4_OP_RES_SETUP(compound, opaccess_mask);
	nfsstat4 *status = &res->amr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
