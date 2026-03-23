/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * WCC (Weak Cache Consistency) checking for dstore operations.
 *
 * After a dstore SETATTR (truncate, fence, chmod), the NFSv3 post-op
 * attributes may reveal:
 *   - Writes that happened without an outstanding write layout (WWWL)
 *   - Backwards-moving timestamps (possible DS reboot)
 *
 * See mds.md "WCC Data and Write Layout Checking" for the full design.
 */

#ifndef _REFFS_DSTORE_WCC_H
#define _REFFS_DSTORE_WCC_H

#include <stdbool.h>
#include <stdint.h>

struct dstore_wcc;
struct layout_data_file;

/*
 * dstore_wcc_check -- compare post-op WCC against cached ldf attrs.
 *
 * @wcc:              post-op attributes from the dstore SETATTR
 * @ldf:              layout data file with cached attrs
 * @has_write_layout: true if a write layout was outstanding during the op
 * @dstore_id:        dstore index (for log messages)
 * @ino:              inode number (for log messages)
 *
 * Checks for:
 *   1. WWWL: mtime or ctime changed on the DS without a write layout.
 *      Atime-only changes are ignored (reads update atime legitimately).
 *   2. Backwards-moving timestamps: ctime or mtime went backwards,
 *      possibly indicating a DS reboot or clock reset.
 *
 * Updates ldf cached attrs (size, mtime, ctime) from WCC data regardless.
 */
void dstore_wcc_check(const struct dstore_wcc *wcc,
		      struct layout_data_file *ldf, bool has_write_layout,
		      uint32_t dstore_id, uint64_t ino);

#endif /* _REFFS_DSTORE_WCC_H */
