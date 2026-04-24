/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ps_sb.h"

struct ps_sb_binding *ps_sb_binding_alloc(uint32_t listener_id,
					  const uint8_t *fh, uint32_t fh_len)
{
	struct ps_sb_binding *b;

	if (listener_id == 0 || !fh || fh_len == 0 || fh_len > PS_MAX_FH_SIZE)
		return NULL;

	b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;

	b->psb_listener_id = listener_id;
	b->psb_mds_fh_len = fh_len;
	memcpy(b->psb_mds_fh, fh, fh_len);
	return b;
}

void ps_sb_binding_free(struct ps_sb_binding *b)
{
	free(b);
}
