/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2023 Thomas D. Haynes <loghyr@gmail.com> All Rights Reserved.
 *
 * The Reference Control Protocol is used to communicate between
 * the metadata servers (mds) and the data servers (ds).
 *
 * See RFC8434 for a discussion on control protocols, mds, and ds.
 */

const RCP_UUID_LEN1 = 16;		/* Size of an UUID */
const RCP_PATH_LEN1 = 1024;		/* Maximum bytes in a path name */
const RCP_NAME_LEN1 = 255;		/* Maximum bytes in a name */


typedef unsigned hyper rcp_node_id1;
typedef unsigned hyper rcp_volume_id1;
typedef unsigned hyper rcp_share_id1;
typedef unsigned hyper rcp_inode_id1;
typedef unsigned int rpc_uid1;
typedef unsigned int rcp_gid1;
typedef unsigned hyper rcp_length1;
typedef unsigned hyper rcp_offset1;
typedef unsigned hyper rcp_size1;
typedef unsigned hyper rcp_cookie1;

/*
 * Each request will supply a rcp_trace_id1 which the
 * response will return back.
 */
typedef unsigned hyper rcp_trace_id1;

typedef opaque rcp_uuid1<RCP_UUID_LEN1>;
typedef string rcp_dir_path1<RCP_PATH_LEN1>;
typedef string rcp_name1<RCP_NAME_LEN1>;

const RCP_ADDR_LEN1 = 54; /* IPv4 or IPv6 address string and port encoded: %s.%hhu.%hhu */
typedef opaque rcp_address_string1[RCP_ADDR_LEN1];

struct rcp_time1 {
	unsigned int seconds;
	unsigned int nseconds;
};

/*
 * Expect any NFSv4.2 error here.
 */
enum rcp_stat1 {
        RCP1_OK = 0,                      /* no error */
        RCP1_ERR_PERM = 1,                /* Not owner */
        RCP1_ERR_NOENT = 2,               /* No such file or directory */
        RCP1_ERR_IO = 5,                  /* I/O error */
        RCP1_ERR_ENXIO = 6,               /* No such device or address */
        RCP1_ERR_BADF = 9,                /* Bad file number */
        RCP1_ERR_AGAIN = 11,              /* Try again */
        RCP1_ERR_NOMEM = 12,              /* Out of memory */
        RCP1_ERR_ACCES = 13,              /* Permission denied */
        RCP1_ERR_BUSY = 16,               /* Device or resource busy */
        RCP1_ERR_EXIST = 17,              /* File exists */
        RCP1_ERR_NOTDIR = 20,             /* Not a directory */
        RCP1_ERR_INVAL = 22,              /* Invalid argument */
        RCP1_ERR_NOSPC = 28,              /* No space left for device */
        RCP1_ERR_STALE = 70,              /* shr/ino combination is stale */
        RCP1_ERR_ALREADY = 114,           /* Operation already in progress */
        RCP1_ERR_NOTSUPP = 10004,         /* Operation not supported */
        RCP1_ERR_SERVERFAULT = 10006,     /* A failure on the server */
        RCP1_ERR_DELAY = 10008,           /* Busy, try again */
        RCP1_ERR_BADXDR = 10036,          /* XDR decode failed */
        RCP1_ERR_BADNAME = 10041          /* name not supported */
};

/*****************************************************************************
 * Operations for managing the data store
 *****************************************************************************/

/* Session start */

/*****************************************************************************
 * Operations for managing the data store's volumes
 *****************************************************************************/

/*
 * List all volumes on a node.
 */
struct rcp_volume_list1_args {
	rcp_trace_id1 rvla_trace_id;
	rcp_node_id1 rvla_node_id;        /* Which connecting mds? */
	rcp_cookie1 rvla_cookie;
};

struct rcp_volume_info1 {
	rcp_volume_id1 rvlr_volume_id;
	rcp_uuid1 rvlr_uuid;
	rcp_name1 rvlr_name;
	rcp_dir_path1 rvlr_path;
};

struct rcp_volume_list1 {
	rcp_trace_id1 rvl_trace_id;
	rcp_cookie1 rvl_cookie;
	bool rvl_eof;
	rcp_volume_info1 rvl_volumes<>;
};

union rcp_volume_list1_res switch (rcp_stat1 rvlr_status) {
	case RCP1_OK:
		rcp_volume_list1 rvlr_volume_list;
	default:
		void;
};

/*
 * List a specific volume on a node.
 */
struct rcp_volume_list1_args {
	rcp_trace_id1 rvsa_trace_id;
	rcp_node_id1 rvsa_node_id;        /* Which connecting mds? */
	rcp_volume_id1 rvsa_volume_id;
};

union rcp_volume_show1_res switch (rcp_stat1 rvsr_status) {
	case RCP1_OK:
		rcp_volume_info1 rvsr_volume;
	default:
		void;
};

/*****************************************************************************
 * Operations for managing the data store's files
 *****************************************************************************/

struct rcp_volume_fh1 {
	rcp_volume_id1 rvf_volume_id;
	rcp_inode_id1 rvf_inode_id;
};

/*
 * Grant a client access to a file
 */
const RCP_READ1  = 0x00000001;
const RCP_WRITE1 = 0x00000002;

struct rcp_byte_range1 {
	rcp_offset1 rbr_offset;
	rcp_length1 rbr_length;
};

/*
 * If a RPC from @rfaga_client arrives with
 * a credential of @rfaga_uid and @rfaga_uid,
 * then it is granted access according to
 * the @rfaga_flags state:
 *
 * @rfaga_flags & RCP_READ1 = READ
 * @rfaga_flags & RCP_WRITE1 = WRITE, COMMIT
 *
 * Note that the flags are built on top of
 * previous RCP_FILE_ACCESS_GRANT1 and
 * RCP_FILE_ACCESS_REVOKE1 calls.
 */
struct rcp_file_access_grant1_args {
	rcp_trace_id1 rfaga_trace_id;
	rcp_node_id1 rfaga_node_id;	/* Which connecting mds? */
	rcp_volume_fh1 rfaga_fh;
	rcp_address_string1 rfaga_client;
	rpc_uid1 rfaga_uid;
	rcp_gid1 rfaga_gid;
	rcp_byte_range1 rfaga_byte_range;
	unsigned int rfaga_flags;
};

/*
 * Revoke a client access to a file
 */
struct rcp_file_access_revoke1_args {
	rcp_trace_id1 rfara_trace_id;
	rcp_node_id1 rfara_node_id;	/* Which connecting mds? */
	rcp_volume_fh1 rfara_fh;
	rcp_address_string1 rfara_client;
	rpc_uid1 rfara_uid;
	rcp_gid1 rfara_gid;
	rcp_byte_range1 rfara_byte_range;
	unsigned int rfara_flags;
};

/*
 * Get the client access to a file
 */
struct rcp_file_access_show1_args {
	rcp_trace_id1 rfasa_trace_id;
	rcp_node_id1 rfasa_node_id;	/* Which connecting mds? */
	rcp_volume_fh1 rfasa_fh;
	rcp_address_string1 rfasa_client;
	rpc_uid1 rfasa_uid;
	rcp_gid1 rfasa_gid;
	rcp_byte_range1 rfasa_byte_range;
};

struct rcp_file_access_range1 {
	rcp_byte_range1 rfar_byte_range;
	unsigned int rfar_flags;
};

/*
 * Note that the @rfasa_byte_range might span
 * multiple access regions.
 */
struct rcp_file_access_show1 {
	rcp_trace_id1 rfasr_trace_id;
	rcp_file_access_range1 rfasr_file_access_range<>;
};

union rcp_file_access_show1_res switch (rcp_stat1 rfasr_status) {
	case RCP1_OK:
		rcp_file_access_show1 rfasr_access;
	default:
		void;
};

/*
 * Get all client access to a file
 */
struct rcp_file_access_list1_args {
	rcp_trace_id1 rfala_trace_id;
	rcp_node_id1 rfala_node_id;	/* Which connecting mds? */
	rcp_volume_fh1 rfala_fh;
	rcp_cookie1 rfala_cookie;
};

struct rcp_file_access1 {
	rcp_address_string1 rfa_client;
	rpc_uid1 rfa_uid;
	rcp_gid1 rfa_gid;
	rcp_byte_range1 rfa_byte_range;
};

struct rcp_file_access_list1 {
	rcp_trace_id1 rfalr_trace_id;
	rcp_cookie1 rfalr_cookie;
	bool rfalr_eof;
	rcp_file_access1 rfasr_file_access<>;
};

union rcp_file_access_show1_res switch (rcp_stat1 rfalr_status) {
	case RCP1_OK:
		rcp_file_access_list1 rfalr_access;
	default:
		void;
};

/*
 * Get the Ref File Info for a file.
 */
struct rcp_fattr1 {
	rcp_volume_fh1 rf_fh;
	rcp_size1 rf_size;
	rcp_size1 rf_used;
	rcp_time1 rf_access_time;
	rcp_time1 rf_modify_time;
	rcp_time1 rf_change_time;
};

struct rcp_getattr1_args {
	rcp_trace_id1 rga_trace_id;
	rcp_node_id1 rga_node_id;	/* Which connecting mds? */
	rcp_volume_fh1 rga_fh;
};

struct rcp_getattr1 {
	rcp_trace_id1 rg_trace_id;
	rcp_fattr1 fg_fattr;
};

union rcp_getattr1_res switch (rcp_stat1 rgr_status) {
	case RCP1_OK:
		rcp_getattr1 rgr_attr;
	default:
		void;
};

const RCP_PORT 4098;

program RCP_ADMIN_PROGRAM {
	version RCP_ADMIN_V1 {
		void RCP_PROC1_NULL(void) = 0;
		rcp_volume_list1_res RCP_VOLUME_LIST1(rcp_volume_list1_args) = 1;
		rcp_volume_show1_res RCP_VOLUME_SHOW1(rcp_volume_show1_args) = 2;
		rcp_stat1 RCP_FILE_ACCESS_GRANT1(rcp_file_access_grant1_args) = 3;
		rcp_stat1 RCP_FILE_ACCESS_REVOKE1(rcp_file_access_revoke1_args) = 4;
		rcp_file_access_show1_res RCP_FILE_ACCESS_SHOW1(rcp_file_access_show1_args) = 5;
		rcp_file_access_list1_res RCP_FILE_ACCESS_LIST1(rcp_file_access_list1_args) = 6;
		rcp_getattr1_res RCP_GETATTR1(rcp_getattr1_args) = 7;
	} = 1;
} = 304098;
