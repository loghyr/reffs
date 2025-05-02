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
        STAT1_OK = 0,                      /* no error */
        STAT1ERR_PERM = 1,                /* Not owner */
        STAT1ERR_NOENT = 2,               /* No such file or directory */
        STAT1ERR_IO = 5,                  /* I/O error */
        STAT1ERR_ENXIO = 6,               /* No such device or address */
        STAT1ERR_BADF = 9,                /* Bad file number */
        STAT1ERR_AGAIN = 11,              /* Try again */
        STAT1ERR_NOMEM = 12,              /* Out of memory */
        STAT1ERR_ACCES = 13,              /* Permission denied */
        STAT1ERR_BUSY = 16,               /* Device or resource busy */
        STAT1ERR_EXIST = 17,              /* File exists */
        STAT1ERR_NOTDIR = 20,             /* Not a directory */
        STAT1ERR_INVAL = 22,              /* Invalid argument */
        STAT1ERR_NOSPC = 28,              /* No space left for device */
        STAT1ERR_STALE = 70,              /* shr/ino combination is stale */
        STAT1ERR_ALREADY = 114,           /* Operation already in progress */
        STAT1ERR_NOTSUPP = 10004,         /* Operation not supported */
        STAT1ERR_SERVERFAULT = 10006,     /* A failure on the server */
        STAT1ERR_DELAY = 10008,           /* Busy, try again */
        STAT1ERR_BADXDR = 10036,          /* XDR decode failed */
        STAT1ERR_BADNAME = 10041          /* name not supported */
};

struct stat_op1 {
	unsigned int	so_op;
	unsigned hyper	so_calls;
	unsigned hyper	so_errors;
	unsigned hyper	so_max_duration;
	unsigned hyper	so_total_duration;
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

struct GATHER1args {
	unsigned int	sga_program;
	unsigned int	sga_version;
};

struct GATHER1resok {
	stat_program1	sgr_program;
};

union GATHER1res switch (stat_stat1 sgr_status) {
	case STAT1_OK:
		GATHER1resok	sgr_resok;
	default:
		void;
};

struct CONTEXT1resok {
	unsigned hyper	scr_created;
	unsigned hyper	scr_freed;
};

union CONTEXT1res switch (stat_stat1 scr_status) {
	case STAT1_OK:
		CONTEXT1resok	scr_resok;
	default:
		void;
};

const STAT_PORT = 20049;

/*
 * All ops MUST have the first four bytes reserved for status.
 * By default, return a stat_stat1, not a void.
 */
program STAT_PROGRAM {
	version STAT_V1 {
		void STATPROC1_NULL(void) = 0;
		GATHER1res STATPROC1_GATHER(GATHER1args) = 1;
		CONTEXT1res STATPROC1_CONTEXT(void) = 2;
	} = 1;
} = 211768;
