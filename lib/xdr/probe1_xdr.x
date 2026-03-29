/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Gather stats from the reffs server
 */

/* INET6_ADDRSTRLEN + 8: %s.%hhu.%hhu */
const PROBE1_ADDR_LEN = 54;

typedef opaque probe1_address_string[PROBE1_ADDR_LEN];

struct probe_time1 {
	unsigned int seconds;
	unsigned int nseconds;
};

enum probe_stat1 {
        PROBE1_OK = 0,                      /* no error */
        PROBE1ERR_PERM = 1,                /* Not owner */
        PROBE1ERR_NOENT = 2,               /* No such file or directory */
        PROBE1ERR_IO = 5,                  /* I/O error */
        PROBE1ERR_ENXIO = 6,               /* No such device or address */
        PROBE1ERR_BADF = 9,                /* Bad file number */
        PROBE1ERR_AGAIN = 11,              /* Try again */
        PROBE1ERR_NOMEM = 12,              /* Out of memory */
        PROBE1ERR_ACCES = 13,              /* Permission denied */
        PROBE1ERR_BUSY = 16,               /* Device or resource busy */
        PROBE1ERR_EXIST = 17,              /* File exists */
        PROBE1ERR_NOTDIR = 20,             /* Not a directory */
        PROBE1ERR_INVAL = 22,              /* Invalid argument */
        PROBE1ERR_NOSPC = 28,              /* No space left for device */
        PROBE1ERR_STALE = 70,              /* shr/ino combination is stale */
        PROBE1ERR_ALREADY = 114,           /* Operation already in progress */
        PROBE1ERR_NOTSUPP = 10004,         /* Operation not supported */
        PROBE1ERR_SERVERFAULT = 10006,     /* A failure on the server */
        PROBE1ERR_DELAY = 10008,           /* Busy, try again */
        PROBE1ERR_BADXDR = 10036,          /* XDR decode failed */
        PROBE1ERR_BADNAME = 10041          /* name not supported */
};

struct probe_op1 {
	unsigned int	po_op;
	string		po_name<>;
	unsigned hyper	po_calls;
	unsigned hyper	po_errors;
	unsigned hyper	po_max_duration;
	unsigned hyper	po_total_duration;
	unsigned hyper	po_bucket_1ms;
	unsigned hyper	po_bucket_10ms;
	unsigned hyper	po_bucket_100ms;
	unsigned hyper	po_bucket_1s;
	unsigned hyper	po_bucket_10s;
	unsigned hyper	po_bucket_rest;
	unsigned hyper	po_median_ns;
	unsigned hyper	po_p90_ns;
	unsigned hyper	po_p99_ns;
	unsigned hyper	po_p999_ns;
	unsigned hyper	po_min_ns;
	unsigned hyper	po_max_ns;
	unsigned hyper	po_mean_ns;
};

struct probe_program1 {
	unsigned int	pp_program;
	unsigned int	pp_version;
	unsigned hyper	pp_count;
	unsigned hyper	pp_replied_errors;
	unsigned hyper	pp_rejected_errors;
	unsigned hyper	pp_accepted_errors;
	unsigned hyper	pp_authed_errors;
	probe_op1	pp_ops<>;
};

struct STATS_GATHER1args {
	unsigned int	psga_program;
	unsigned int	psga_version;
};

struct STATS_GATHER1resok {
	probe_program1	psgr_program;
};

union STATS_GATHER1res switch (probe_stat1 psgr_status) {
	case PROBE1_OK:
		STATS_GATHER1resok	psgr_resok;
	default:
		void;
};

struct CONTEXT1resok {
	unsigned hyper	pcr_created;
	unsigned hyper	pcr_freed;
	unsigned hyper	pcr_active_cancelled;
	unsigned hyper	pcr_active_destroyed;
	unsigned hyper	pcr_cancelled_freed;
	unsigned hyper	pcr_destroyed_freed;
};

union CONTEXT1res switch (probe_stat1 pcr_status) {
	case PROBE1_OK:
		CONTEXT1resok	pcr_resok;
	default:
		void;
};

typedef unsigned int probe_dump1;

enum probe_trace_category1 {
        PROBE1_TRACE_CAT_GENERAL = 0,
        PROBE1_TRACE_CAT_IO = 1,
        PROBE1_TRACE_CAT_RPC = 2,
        PROBE1_TRACE_CAT_NFS = 3,
        PROBE1_TRACE_CAT_NLM = 4,
        PROBE1_TRACE_CAT_FS = 5,
        PROBE1_TRACE_CAT_LOG = 6,
        PROBE1_TRACE_CAT_SECURITY = 7,
        PROBE1_TRACE_CAT_ALL = 8
};

struct TRACE_SET1args {
	probe_trace_category1	tsa_cat;
	unsigned int		tsa_set;
};

struct TRACES_LIST1args {
	probe_trace_category1	tla_cat;
};

struct probe_trace_list1 {
	probe_trace_category1	ptl_cat;
	unsigned int 		ptl_set;
};

struct TRACES_LIST1resok {
	probe_trace_list1	tlr_list<>;
};

union TRACES_LIST1res switch (probe_stat1 tlr_status) {
	case PROBE1_OK:
		TRACES_LIST1resok	tlr_resok;
	default:
		void;
};

struct HEARTBEAT1args {
	unsigned int		hba_period;
	unsigned int		hba_period_set;
};

struct HEARTBEAT1resok {
	unsigned int		hbr_period;
};

union HEARTBEAT1res switch (probe_stat1 hbr_status) {
	case PROBE1_OK:
		HEARTBEAT1resok		hbr_resok;
	default:
		void;
};

enum probe_op_type1 {
	PROBE1_OP_TYPE_ACCEPT = 1,
	PROBE1_OP_TYPE_READ = 2,
	PROBE1_OP_TYPE_WRITE = 3,
	PROBE1_OP_TYPE_CONNECT = 4,
	PROBE1_OP_TYPE_RPC_REQ = 5,
	PROBE1_OP_TYPE_HEARTBEAT = 6,
	PROBE1_OP_TYPE_ALL = 7
};

const PROBE1_IO_CONTEXT_ENTRY_STATE_ACTIVE           = 0x00000001;
const PROBE1_IO_CONTEXT_ENTRY_STATE_MARKED_DESTROYED = 0x00000002;
const PROBE1_IO_CONTEXT_ENTRY_STATE_PENDING_FREE     = 0x00000004;
const PROBE1_IO_CONTEXT_DIRECT_TLS_DATA              = 0x00000008;
const PROBE1_IO_CONTEXT_TLS_BIO_PROCESSED            = 0x00000010;

struct probe_io_context1 {
	probe_op_type1		pic_op_type;
	int			pic_fd;
	unsigned int		pic_id;
	unsigned int		pic_xid;

	unsigned hyper		pic_buffer_len;
	unsigned hyper		pic_position;
	unsigned hyper		pic_expected_len;

	unsigned hyper		pic_state;
	unsigned hyper		pic_count;

	probe_time1		pic_action_time;
};

struct probe_fd1 {
	int			pf_fd;
	unsigned int		pf_server_port;
	probe1_address_string	pf_client;
};

/* 0 means all fds */
struct IO_CONTEXTS_LIST1args {
	int			icla_fd;
	probe_op_type1		icla_op;
	unsigned hyper		icla_state;
};

struct IO_CONTEXTS_LIST1resok {
	probe_io_context1	iclr_pic<>;
	probe_time1		iclr_now;
};

union IO_CONTEXTS_LIST1res switch (probe_stat1 iclr_status) {
	case PROBE1_OK:
		IO_CONTEXTS_LIST1resok	iclr_resok;
	default:
		void;
};

/* 0 means all fds */
struct FD_INFOS_LIST1args {
	int			fila_fd;
};

struct FD_INFOS_LIST1resok {
	probe_fd1		filr_pf<>;
};

union FD_INFOS_LIST1res switch (probe_stat1 filr_status) {
	case PROBE1_OK:
		FD_INFOS_LIST1resok	filr_resok;
	default:
		void;
};

/* ------------------------------------------------------------------ */
/* Superblock management types                                         */
/* ------------------------------------------------------------------ */

enum probe_storage_type1 {
	PROBE1_STORAGE_RAM     = 0,
	PROBE1_STORAGE_POSIX   = 1,
	PROBE1_STORAGE_ROCKSDB = 2,
	PROBE1_STORAGE_FUSE    = 3
};

enum probe_sb_lifecycle1 {
	PROBE1_SB_CREATED   = 0,
	PROBE1_SB_MOUNTED   = 1,
	PROBE1_SB_UNMOUNTED = 2,
	PROBE1_SB_DESTROYED = 3
};

enum probe_auth_flavor1 {
	PROBE1_AUTH_SYS   = 1,
	PROBE1_AUTH_KRB5  = 390003,
	PROBE1_AUTH_KRB5I = 390004,
	PROBE1_AUTH_KRB5P = 390005,
	PROBE1_AUTH_TLS   = 0x40000001
};

const PROBE1_MAX_FLAVORS = 8;

struct probe_sb_info1 {
	unsigned hyper		psi_id;
	opaque			psi_uuid[16];
	string			psi_path<>;
	probe_sb_lifecycle1	psi_state;
	probe_storage_type1	psi_storage_type;
	probe_auth_flavor1	psi_flavors<PROBE1_MAX_FLAVORS>;
	unsigned hyper		psi_bytes_max;
	unsigned hyper		psi_bytes_used;
	unsigned hyper		psi_inodes_max;
	unsigned hyper		psi_inodes_used;
};

/* SB_LIST (op 13) */
struct SB_LIST1resok {
	probe_sb_info1	slr_sbs<>;
};
union SB_LIST1res switch (probe_stat1 slr_status) {
	case PROBE1_OK:
		SB_LIST1resok	slr_resok;
	default:
		void;
};

/* SB_CREATE (op 14) — server assigns the sb_id from monotonic counter */
struct SB_CREATE1args {
	string			sca_path<>;
	probe_storage_type1	sca_storage_type;
};
union SB_CREATE1res switch (probe_stat1 scr_status) {
	case PROBE1_OK:
		probe_sb_info1	scr_resok;
	default:
		void;
};

/* SB_MOUNT (op 15) — returns probe_stat1 directly (no resok) */
struct SB_MOUNT1args {
	unsigned hyper	sma_id;
	string		sma_path<>;
};

/* SB_UNMOUNT (op 16) — returns probe_stat1 directly */
struct SB_UNMOUNT1args {
	unsigned hyper	sua_id;
};

/* SB_DESTROY (op 17) — returns probe_stat1 directly */
struct SB_DESTROY1args {
	unsigned hyper	sda_id;
};

/* SB_GET (op 18) */
struct SB_GET1args {
	unsigned hyper	sga_id;
};
union SB_GET1res switch (probe_stat1 sgr_status) {
	case PROBE1_OK:
		probe_sb_info1	sgr_resok;
	default:
		void;
};

/* SB_SET_FLAVORS (op 19) — returns probe_stat1 directly */
struct SB_SET_FLAVORS1args {
	unsigned hyper		sfa_id;
	probe_auth_flavor1	sfa_flavors<PROBE1_MAX_FLAVORS>;
};

/* SB_LINT_FLAVORS (op 20) */
struct SB_LINT_FLAVORS1resok {
	unsigned int	lfr_warnings;
	string		lfr_messages<>;
};
union SB_LINT_FLAVORS1res switch (probe_stat1 lfr_status) {
	case PROBE1_OK:
		SB_LINT_FLAVORS1resok	lfr_resok;
	default:
		void;
};

/* ------------------------------------------------------------------ */
/* Per-sb stats types (extend existing resok structs)                  */
/* ------------------------------------------------------------------ */

struct probe_sb_fs_usage1 {
	unsigned hyper	sfu_sb_id;
	string		sfu_sb_path<>;
	unsigned hyper	sfu_total_bytes;
	unsigned hyper	sfu_free_bytes;
	unsigned hyper	sfu_used_bytes;
	unsigned hyper	sfu_total_files;
	unsigned hyper	sfu_free_files;
	unsigned hyper	sfu_used_files;
};

struct FS_USAGE1resok {
	unsigned hyper		fur_total_bytes;
	unsigned hyper		fur_free_bytes;
	unsigned hyper		fur_used_bytes;
	unsigned hyper		fur_total_files;
	unsigned hyper		fur_free_files;
	unsigned hyper		fur_used_files;
	probe_sb_fs_usage1	fur_per_sb<>;
};

union FS_USAGE1res switch (probe_stat1 fur_status) {
	case PROBE1_OK:
		FS_USAGE1resok	fur_resok;
	default:
		void;
};

struct probe_nfs4_op1 {
	unsigned int	pno_op;
	string		pno_name<>;
	unsigned hyper	pno_calls;
	unsigned hyper	pno_errors;
	unsigned hyper	pno_bytes_in;
	unsigned hyper	pno_bytes_out;
	unsigned hyper	pno_duration_total;
	unsigned hyper	pno_duration_max;
};

struct probe_sb_nfs4_op_stats1 {
	unsigned hyper	sns_sb_id;
	string		sns_sb_path<>;
	probe_nfs4_op1	sns_ops<>;
};

struct NFS4_OP_STATS1resok {
	probe_nfs4_op1			nosr_ops<>;
	probe_sb_nfs4_op_stats1		nosr_per_sb<>;
};

union NFS4_OP_STATS1res switch (probe_stat1 nosr_status) {
	case PROBE1_OK:
		NFS4_OP_STATS1resok	nosr_resok;
	default:
		void;
};

struct probe_layout_error1 {
	unsigned int	ple_id;
	string		ple_name<>;
	unsigned hyper	ple_total;
	unsigned hyper	ple_access;
	unsigned hyper	ple_io;
	unsigned hyper	ple_other;
};

struct LAYOUT_ERRORS1resok {
	probe_layout_error1	ler_global;
	probe_layout_error1	ler_dstores<>;
	probe_layout_error1	ler_clients<>;
	probe_layout_error1	ler_sbs<>;
};

union LAYOUT_ERRORS1res switch (probe_stat1 ler_status) {
	case PROBE1_OK:
		LAYOUT_ERRORS1resok	ler_resok;
	default:
		void;
};

const PROBE_PORT = 20490;

/*
 * All ops MUST have the first four bytes reserved for status.
 * By default, return a probe_stat1, not a void.
 */
program PROBE_PROGRAM {
	version PROBE_V1 {
		void PROBEPROC1_NULL(void) = 0;
		STATS_GATHER1res PROBEPROC1_STATS_GATHER(STATS_GATHER1args) = 1;
		CONTEXT1res PROBEPROC1_CONTEXT(void) = 2;
		probe_stat1 PROBEPROC1_RPC_DUMP_SET(probe_dump1) = 3;
		probe_stat1 PROBEPROC1_TRACE_SET(TRACE_SET1args) = 4;
		TRACES_LIST1res PROBEPROC1_TRACES_LIST(TRACES_LIST1args) = 5;
		probe_stat1 PROBEPROC1_GRACEFUL_CLEANUP(void) = 6;
		HEARTBEAT1res PROBEPROC1_HEARTBEAT(HEARTBEAT1args) = 7;
		IO_CONTEXTS_LIST1res PROBEPROC1_IO_CONTEXTS_LIST(IO_CONTEXTS_LIST1args) = 8;
		FD_INFOS_LIST1res PROBEPROC1_FD_INFOS_LIST(FD_INFOS_LIST1args) = 9;
		FS_USAGE1res PROBEPROC1_FS_USAGE(void) = 10;
		NFS4_OP_STATS1res PROBEPROC1_NFS4_OP_STATS(void) = 11;
		LAYOUT_ERRORS1res PROBEPROC1_LAYOUT_ERRORS(void) = 12;

		/* Superblock management ops (added for multi-export) */
		SB_LIST1res PROBEPROC1_SB_LIST(void) = 13;
		SB_CREATE1res PROBEPROC1_SB_CREATE(SB_CREATE1args) = 14;
		probe_stat1 PROBEPROC1_SB_MOUNT(SB_MOUNT1args) = 15;
		probe_stat1 PROBEPROC1_SB_UNMOUNT(SB_UNMOUNT1args) = 16;
		probe_stat1 PROBEPROC1_SB_DESTROY(SB_DESTROY1args) = 17;
		SB_GET1res PROBEPROC1_SB_GET(SB_GET1args) = 18;
		probe_stat1 PROBEPROC1_SB_SET_FLAVORS(SB_SET_FLAVORS1args) = 19;
		SB_LINT_FLAVORS1res PROBEPROC1_SB_LINT_FLAVORS(void) = 20;
	} = 1;
} = 211768;
