/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_LAYOUT_RECALL_H
#define _REFFS_NFS4_LAYOUT_RECALL_H

#include <stdbool.h>
#include <stdint.h>

#include "nfsv42_xdr.h"

/*
 * On-demand CB_LAYOUTRECALL (FILE, FSID or ALL) aimed at one client,
 * resent on NFS4ERR_DELAY at a fixed interval until a time or count
 * limit.  A test trigger driven from the probe protocol; see
 * .claude/design/bulk-layoutrecall.md.
 */

struct cb_pending;
struct inode;
struct nfs4_session;

#define LAYOUT_RECALL_MAX 32
#define LAYOUT_RECALL_EVENTS 256

/* Values are mirrored by probe_layoutrecall_state1. */
enum layout_recall_state {
	LAYOUT_RECALL_IDLE = 0,
	LAYOUT_RECALL_SENDING = 1, /* send due, or reply awaited */
	LAYOUT_RECALL_WAIT_RESEND = 2, /* got DELAY, resend scheduled */
	LAYOUT_RECALL_ACKED = 3, /* got NFS4_OK, awaiting LAYOUTRETURN */
	LAYOUT_RECALL_DONE = 4,
	LAYOUT_RECALL_EXPIRED = 5, /* limit reached; nothing revoked */
	LAYOUT_RECALL_FAILED = 6,
	LAYOUT_RECALL_CANCELLED = 7,
};

/* Values are mirrored by probe_layoutrecall_event1. */
enum layout_recall_event_kind {
	LAYOUT_RECALL_EV_SEND = 1,
	LAYOUT_RECALL_EV_REPLY = 2,
	LAYOUT_RECALL_EV_TIMEOUT = 3, /* no reply, or reply undecodable */
	LAYOUT_RECALL_EV_RETURN = 4, /* covering LAYOUTRETURN seen */
	LAYOUT_RECALL_EV_DONE = 5,
	LAYOUT_RECALL_EV_EXPIRED = 6,
	LAYOUT_RECALL_EV_FAILED = 7,
	LAYOUT_RECALL_EV_CANCELLED = 8,
};

struct layout_recall_event {
	uint64_t lre_time_ns; /* CLOCK_REALTIME */
	uint32_t lre_kind;
	uint32_t lre_send; /* 1-based send this event belongs to */
	int32_t lre_cb_status; /* CB_COMPOUND status, or -errno */
	int32_t lre_op_status; /* CB_LAYOUTRECALL status */
	uint32_t lre_dropped; /* layout stateids freed (DONE) */
};

struct layout_recall_params {
	clientid4 lrp_clientid; /* 0: select by lrp_addr */
	uint32_t lrp_addr; /* IPv4, network order; 0 with clientid 0: sole client */
	layoutrecall_type4 lrp_type;
	layouttype4 lrp_layout_type;
	layoutiomode4 lrp_iomode;
	bool lrp_changed;
	uint64_t lrp_sb_id; /* FILE, FSID */
	uint64_t lrp_ino; /* FILE */
	uint64_t lrp_offset; /* FILE */
	uint64_t lrp_length; /* FILE */
	uint32_t lrp_interval_ms;
	uint32_t lrp_limit_ms; /* 0: no time limit */
	uint32_t lrp_limit_count; /* 0: no send-count limit */
};

struct layout_recall_info {
	uint32_t lri_id;
	enum layout_recall_state lri_state;
	struct layout_recall_params lri_params; /* lrp_clientid resolved */
	uint32_t lri_sends;
	uint32_t lri_nevents;
	uint32_t lri_events_lost; /* oldest events overwritten */
	struct layout_recall_event lri_events[LAYOUT_RECALL_EVENTS];
};

int layout_recall_init(void);
void layout_recall_fini(void);

/*
 * Start a recall.  Returns 0 and the new id, or -EINVAL (bad params),
 * -ENOENT (no such client / file / layout stateid for FILE), -ENOTUNIQ
 * (sole-client selection with several clients), -EBUSY (a recall to
 * that client is still outstanding; RFC 8881 sec 12.5.5.2.1.5), or
 * -ENOSPC (table full of outstanding recalls).
 */
int layout_recall_start(const struct layout_recall_params *params,
			uint32_t *id_out);

/* id 0 selects the most recently started recall. */
int layout_recall_status(uint32_t id, struct layout_recall_info *out);
int layout_recall_cancel(uint32_t id);

/*
 * True if a recall to clid is outstanding and covers inode, so a
 * LAYOUTGET on it must get NFS4ERR_RECALLCONFLICT (sec 12.5.5.2.1.5).
 */
bool layout_recall_layoutget_conflict(clientid4 clid,
				      const struct inode *inode);

/*
 * Check a LAYOUTRETURN against outstanding bulk recalls to clid.
 * Returns NFS4ERR_RECALLCONFLICT for a FILE return (one carrying a
 * stateid) of a file a bulk recall covers, or for a return whose type
 * differs from the bulk recall's (sec 12.5.5.2.1.5); else NFS4_OK.
 */
nfsstat4 layout_recall_layoutreturn_check(clientid4 clid,
					  layoutreturn_type4 type,
					  const struct inode *inode);

/*
 * A LAYOUTRETURN succeeded: complete the recalls whose scope and
 * iomode it covers (an ANY return covers every iomode).  Byte ranges
 * are not compared; FILE recalls are whole-file.
 */
void layout_recall_layoutreturn_done(clientid4 clid, layoutreturn_type4 type,
				     layoutiomode4 iomode,
				     const struct inode *inode);

/* ------------------------------------------------------------------ */
/* Test seams                                                          */

/*
 * Manual mode: no driver thread; the caller drives layout_recall_run()
 * and supplies the clock and the sender.  Replies are delivered by
 * completing the cb_pending handed to send() and calling its
 * cp_complete hook, as cb_reply_handler would.
 */
typedef int (*layout_recall_send_fn)(struct nfs4_session *session,
				     layouttype4 layout_type,
				     layoutiomode4 iomode, int changed,
				     const layoutrecall4 *recall,
				     struct cb_pending *cp);
void layout_recall_test_manual(uint64_t (*clock_ns)(void),
			       layout_recall_send_fn send);
void layout_recall_run(void);

#endif /* _REFFS_NFS4_LAYOUT_RECALL_H */
