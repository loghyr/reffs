/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2025 Thomas D. Haynes <loghyr@gmail.com> All Rights Reserved.
 *
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
        PROBE1_TRACE_CAT_ALL = 6
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

struct FS_USAGE1args {
	bool	fua_human_readable;
	string	fua_mount_path<>;
};

struct FS_USAGE1resok {
	unsigned hyper	fur_total_bytes;
	unsigned hyper	fur_free_bytes;
	unsigned hyper	fur_used_bytes;
	unsigned hyper	fur_total_files;
	unsigned hyper	fur_free_files;
	unsigned hyper	fur_used_files;
};

union FS_USAGE1res switch (probe_stat1 fur_status) {
	case PROBE1_OK:
		FS_USAGE1resok	fur_resok;
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
		IO_CONTEXTS_LIST1args PROBEPROC1_IO_CONTEXTS_LIST(IO_CONTEXTS_LIST1args) = 8;
		FD_INFOS_LIST1res PROBEPROC1_FD_INFOS_LIST(FD_INFOS_LIST1args) = 9;
		FS_USAGE1res PROBEPROC1_FS_USAGE(FS_USAGE1args) = 10;
	} = 1;
} = 211768;
