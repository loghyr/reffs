/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * WCC (Weak Cache Consistency) checking for dstore operations.
 *
 * After a dstore SETATTR returns post-op attributes, compare them
 * against the cached values in the layout_data_file to detect:
 *   - WWWL (Write Without Write Layout): the DS file changed
 *     without an outstanding write layout
 *   - Backwards-moving timestamps: possible DS reboot / clock reset
 *
 * See mds.md "WCC Data and Write Layout Checking".
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/dstore_wcc.h"
#include "reffs/layout_segment.h"
#include "reffs/log.h"

/* Return true if a > b as struct timespec. */
static bool timespec_gt(const struct timespec *a, const struct timespec *b)
{
	if (a->tv_sec != b->tv_sec)
		return a->tv_sec > b->tv_sec;
	return a->tv_nsec > b->tv_nsec;
}

/* Return true if a < b as struct timespec. */
static bool timespec_lt(const struct timespec *a, const struct timespec *b)
{
	return timespec_gt(b, a);
}

/* Return true if a != b as struct timespec. */
static bool timespec_ne(const struct timespec *a, const struct timespec *b)
{
	return a->tv_sec != b->tv_sec || a->tv_nsec != b->tv_nsec;
}

void dstore_wcc_check(const struct dstore_wcc *wcc,
		      struct layout_data_file *ldf, bool has_write_layout,
		      uint32_t dstore_id, uint64_t ino)
{
	if (!wcc || !wcc->wcc_valid || !ldf)
		return;

	/*
	 * Check for backwards-moving timestamps.  This can indicate a
	 * DS reboot with clock reset.  Log for operator investigation.
	 */
	if (timespec_lt(&wcc->wcc_mtime, &ldf->ldf_mtime))
		LOG("WWWL: dstore[%u] ino=%lu mtime went backwards "
		    "(%ld.%09ld -> %ld.%09ld) — possible DS reboot",
		    dstore_id, ino, (long)ldf->ldf_mtime.tv_sec,
		    ldf->ldf_mtime.tv_nsec, (long)wcc->wcc_mtime.tv_sec,
		    wcc->wcc_mtime.tv_nsec);

	if (timespec_lt(&wcc->wcc_ctime, &ldf->ldf_ctime))
		LOG("WWWL: dstore[%u] ino=%lu ctime went backwards "
		    "(%ld.%09ld -> %ld.%09ld) — possible DS reboot",
		    dstore_id, ino, (long)ldf->ldf_ctime.tv_sec,
		    ldf->ldf_ctime.tv_nsec, (long)wcc->wcc_ctime.tv_sec,
		    wcc->wcc_ctime.tv_nsec);

	/*
	 * Check for WWWL: mtime or ctime changed without a write layout.
	 * Ignore atime-only changes — reads update atime legitimately.
	 */
	if (!has_write_layout) {
		bool mtime_changed =
			timespec_ne(&wcc->wcc_mtime, &ldf->ldf_mtime);
		bool ctime_changed =
			timespec_ne(&wcc->wcc_ctime, &ldf->ldf_ctime);

		if (mtime_changed || ctime_changed)
			LOG("WWWL: dstore[%u] ino=%lu DS file changed "
			    "without write layout (mtime %s, ctime %s)",
			    dstore_id, ino, mtime_changed ? "changed" : "same",
			    ctime_changed ? "changed" : "same");
	}

	/* Update cached attrs from WCC post-op data. */
	ldf->ldf_size = wcc->wcc_size;
	ldf->ldf_mtime = wcc->wcc_mtime;
	ldf->ldf_ctime = wcc->wcc_ctime;
}
