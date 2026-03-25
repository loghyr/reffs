/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * MDS layout segments — per-inode data placement metadata.
 *
 * Each layout segment describes how a byte range of an MDS file is
 * distributed across data servers.  The segment records the dstore
 * IDs (not addresses/paths) and NFSv3 filehandles for the data files,
 * plus cached DS-side attributes for GETATTR aggregation.
 *
 * Designed for striping and continuations from the start:
 * - Each segment covers a byte range (offset + length)
 * - Each segment has its own stripe config (unit, k, m)
 * - Multiple segments per inode support continuations
 * - The demo uses a single segment covering the whole file
 */

#ifndef _REFFS_LAYOUT_SEGMENT_H
#define _REFFS_LAYOUT_SEGMENT_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define LAYOUT_SEG_MAX_FH 64 /* NFSv3 FHSIZE3 */
#define LAYOUT_SEG_MAX_FILES 32 /* max data files per segment */

/*
 * On-disk: cached attributes for a single data file on a DS.
 * Refreshed at LAYOUTRETURN time.
 */
struct layout_data_file_disk {
	uint32_t ldf_dstore_id;
	uint32_t ldf_fh_len;
	uint8_t ldf_fh[LAYOUT_SEG_MAX_FH];
	int64_t ldf_size;
	struct timespec ldf_atime;
	struct timespec ldf_mtime;
	struct timespec ldf_ctime;
	uint32_t ldf_uid;
	uint32_t ldf_gid;
	uint16_t ldf_mode;
	uint16_t ldf_pad;
};

/*
 * On-disk: a layout segment header followed by nfiles data file entries.
 * Written as: layout_segment_disk + nfiles * layout_data_file_disk
 */
struct layout_segment_disk {
	uint64_t ls_offset; /* byte range start */
	uint64_t ls_length; /* byte range length (0 = entire file) */
	uint32_t ls_stripe_unit; /* bytes per stripe (0 = whole file) */
	uint16_t ls_k; /* data devices */
	uint16_t ls_m; /* parity devices */
	uint32_t ls_nfiles; /* number of data file entries */
	uint32_t ls_layout_type; /* layouttype4: LAYOUT4_FLEX_FILES, etc. */
};

/*
 * In-memory: a layout data file reference.
 * Same content as disk form but with explicit struct for clarity.
 */
struct layout_data_file {
	uint32_t ldf_dstore_id;
	uint8_t ldf_fh[LAYOUT_SEG_MAX_FH];
	uint32_t ldf_fh_len;
	int64_t ldf_size;
	struct timespec ldf_atime;
	struct timespec ldf_mtime;
	struct timespec ldf_ctime;
	uint32_t ldf_uid;
	uint32_t ldf_gid;
	uint16_t ldf_mode;
	bool ldf_stale; /* true if last GETATTR to this DS failed */

	/*
	 * Timestamps for the last dstore op (monotonic ns, in-memory only).
	 * Written by the per-slot fanout thread — each ldf is owned by
	 * exactly one slot per fanout, so no synchronization needed.
	 */
	uint64_t ldf_last_op_sent_ns;
	uint64_t ldf_last_op_recv_ns;
};

/*
 * In-memory: a layout segment with its data file array.
 */
struct layout_segment {
	uint64_t ls_offset;
	uint64_t ls_length;
	uint32_t ls_stripe_unit;
	uint16_t ls_k;
	uint16_t ls_m;
	uint32_t ls_nfiles;
	uint32_t ls_layout_type; /* layouttype4: LAYOUT4_FLEX_FILES, etc. */
	struct layout_data_file *ls_files; /* array of ls_nfiles entries */
};

/*
 * In-memory: all layout segments for an inode.
 */
struct layout_segments {
	uint32_t lss_count;
	struct layout_segment *lss_segs; /* array of lss_count entries */
};

/* Allocate an empty layout_segments container. */
struct layout_segments *layout_segments_alloc(void);

/* Free a layout_segments container and all its segments/files. */
void layout_segments_free(struct layout_segments *lss);

/*
 * Add a segment to the container.  The segment's ls_files array
 * is owned by the container after this call.
 * Returns 0 on success, -ENOMEM on failure.
 */
int layout_segments_add(struct layout_segments *lss,
			const struct layout_segment *seg);

#endif /* _REFFS_LAYOUT_SEGMENT_H */
