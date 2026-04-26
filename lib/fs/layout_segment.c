/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/layout_segment.h"

struct layout_segments *layout_segments_alloc(void)
{
	return calloc(1, sizeof(struct layout_segments));
}

void layout_segments_free(struct layout_segments *lss)
{
	if (!lss)
		return;

	for (uint32_t i = 0; i < lss->lss_count; i++)
		free(lss->lss_segs[i].ls_files);
	free(lss->lss_segs);
	free(lss);
}

int layout_segments_add(struct layout_segments *lss,
			const struct layout_segment *seg)
{
	struct layout_segment *new_segs;
	uint32_t n = lss->lss_count + 1;

	new_segs = realloc(lss->lss_segs, n * sizeof(*new_segs));
	if (!new_segs)
		return -ENOMEM;

	lss->lss_segs = new_segs;
	lss->lss_segs[lss->lss_count] = *seg;
	lss->lss_count = n;
	atomic_fetch_add_explicit(&lss->lss_gen, 1, memory_order_relaxed);
	return 0;
}
