/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2025 Thomas D. Haynes <loghyr@gmail.com> All Rights Reserved.
 *
 * Gather stats from the reffs server
 */

struct stat_time1 {
	unsigned int seconds;
	unsigned int nseconds;
};

enum stat_stat1 {
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

struct stat_op1 {
	unsigned int	so_op;
	string		so_name<>;
	unsigned hyper	so_calls;
	unsigned hyper	so_errors;
	unsigned hyper	so_max_duration;
	unsigned hyper	so_total_duration;
	unsigned hyper	so_bucket_1ms;
	unsigned hyper	so_bucket_10ms;
	unsigned hyper	so_bucket_100ms;
	unsigned hyper	so_bucket_1s;
	unsigned hyper	so_bucket_10s;
	unsigned hyper	so_bucket_rest;
	unsigned hyper	so_median_ns;
	unsigned hyper	so_p90_ns;
	unsigned hyper	so_p99_ns;
	unsigned hyper	so_p999_ns;
	unsigned hyper	so_min_ns;
	unsigned hyper	so_max_ns;
	unsigned hyper	so_mean_ns;
};

struct stat_program1 {
	unsigned int	sp_program;
	unsigned int	sp_version;
	unsigned hyper	sp_count;
	unsigned hyper	sp_replied_errors;
	unsigned hyper	sp_rejected_errors;
	unsigned hyper	sp_accepted_errors;
	unsigned hyper	sp_authed_errors;
	stat_op1	sp_ops<>;
};

struct STATS_GATHER1args {
	unsigned int	psga_program;
	unsigned int	psga_version;
};

struct STATS_GATHER1resok {
	stat_program1	psgr_program;
};

union STATS_GATHER1res switch (stat_stat1 psgr_status) {
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

union CONTEXT1res switch (stat_stat1 pcr_status) {
	case PROBE1_OK:
		CONTEXT1resok	pcr_resok;
	default:
		void;
};

const PROBE_PORT = 20490;

/*
 * All ops MUST have the first four bytes reserved for status.
 * By default, return a stat_stat1, not a void.
 */
program PROBE_PROGRAM {
	version PROBE_V1 {
		void PROBEPROC1_NULL(void) = 0;
		STATS_GATHER1res PROBEPROC1_STATS_GATHER(STATS_GATHER1args) = 1;
		CONTEXT1res PROBEPROC1_CONTEXT(void) = 2;
	} = 1;
} = 211768;
