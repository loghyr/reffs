/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_ERROR_INJECT_H
#define _REFFS_NFS4_ERROR_INJECT_H

#include <stdbool.h>
#include <stdint.h>

#include "nfsv42_xdr.h"

/*
 * One-shot NFSv4 error injection, a probe-driven test trigger: the next
 * op whose current filehandle is the armed file fails with the armed
 * status (default NFS4ERR_BADHANDLE), then the injection disarms.  See
 * .claude/design/error-inject.md.
 */

struct compound;

#define ERROR_INJECT_MAX_OPS 16

/* Values are mirrored by probe_errinject_state1. */
enum error_inject_state {
	ERROR_INJECT_NONE = 0, /* never armed */
	ERROR_INJECT_ARMED = 1,
	ERROR_INJECT_FIRED = 2,
	ERROR_INJECT_DISARMED = 3,
};

struct error_inject_params {
	clientid4 eip_clientid; /* 0: match by eip_addr */
	uint32_t eip_addr; /* IPv4, network order; 0 with clientid 0: any */
	uint64_t eip_sb_id;
	uint64_t eip_ino;
	nfsstat4 eip_status; /* 0: NFS4ERR_BADHANDLE */
	uint32_t eip_nops; /* 0: any op but SEQUENCE and the FH setters */
	uint32_t eip_ops[ERROR_INJECT_MAX_OPS];
};

struct error_inject_info {
	uint32_t eii_id; /* 0: never armed */
	enum error_inject_state eii_state;
	struct error_inject_params eii_params; /* eip_status resolved */
	uint64_t eii_armed_ns; /* CLOCK_REALTIME */
	uint64_t eii_fired_ns; /* CLOCK_REALTIME; 0 until FIRED */
	uint32_t eii_fired_op;
	uint32_t eii_fired_opidx; /* index of the op in its COMPOUND */
	uint32_t eii_fired_xid;
	clientid4 eii_fired_clientid;
	uint32_t eii_skipped; /* ops on the file the op set excluded */
};

/*
 * Arm, replacing any earlier injection.  Returns 0 and the new id, or
 * -EINVAL (no sb_id/ino, too many ops, an op out of range, or
 * NFS4_OK as the status).
 */
int error_inject_arm(const struct error_inject_params *params,
		     uint32_t *id_out);

/* Returns -ENOENT if nothing is armed. */
int error_inject_disarm(void);

/* The current or last injection; -ENOENT if none was ever armed. */
int error_inject_status(struct error_inject_info *out);

/*
 * Called by dispatch_compound() before op runs.  Returns the status
 * to fail op with (and disarms), or NFS4_OK.
 */
nfsstat4 error_inject_check(struct compound *compound, uint32_t op);

#endif /* _REFFS_NFS4_ERROR_INJECT_H */
