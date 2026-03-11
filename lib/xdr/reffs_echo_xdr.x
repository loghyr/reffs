/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * The Reffs Echo protocol is used for example purposes to
 * learn how to decode RPC.
 */

typedef opaque re_message_string1<>;

/*
 * Expect any NFSv4.2 error here.
 */
enum re_stat1 {
        RE1_OK = 0,                      /* no error */
        RE1_ERR_PERM = 1,                /* Not owner */
        RE1_ERR_NOENT = 2,               /* No such file or directory */
        RE1_ERR_IO = 5,                  /* I/O error */
        RE1_ERR_ENXIO = 6,               /* No such device or address */
        RE1_ERR_BADF = 9,                /* Bad file number */
        RE1_ERR_AGAIN = 11,              /* Try again */
        RE1_ERR_NOMEM = 12,              /* Out of memory */
        RE1_ERR_ACCES = 13,              /* Permission denied */
        RE1_ERR_BUSY = 16,               /* Device or resource busy */
        RE1_ERR_EXIST = 17,              /* File exists */
        RE1_ERR_NOTDIR = 20,             /* Not a directory */
        RE1_ERR_INVAL = 22,              /* Invalid argument */
        RE1_ERR_NOSPC = 28,              /* No space left for device */
        RE1_ERR_STALE = 70,              /* shr/ino combination is stale */
        RE1_ERR_ALREADY = 114,           /* Operation already in progress */
        RE1_ERR_NOTSUPP = 10004,         /* Operation not supported */
        RE1_ERR_SERVERFAULT = 10006,     /* A failure on the server */
        RE1_ERR_DELAY = 10008,           /* Busy, try again */
        RE1_ERR_BADXDR = 10036,          /* XDR decode failed */
        RE1_ERR_BADNAME = 10041          /* name not supported */
};

struct re_message1_args {
	re_message_string1 rma_message;
};

union re_message1_res switch (re_stat1 rmr_status) {
	case RE1_OK:
		re_message_string1 rmr_reply;
	default:
		void;
};

const RE_PORT = 3049;

program RE_ADMIN_PROGRAM {
	version RE_ADMIN_V1 {
		void RE_PROC1_NULL(void) = 0;
		re_message1_res RE_PROC1_MESSAGE(re_message1_args) = 1;
	} = 1;
} = 304097;
