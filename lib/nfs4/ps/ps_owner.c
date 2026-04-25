/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <string.h>

#include "ps_owner.h"

int ps_owner_wrap(uint64_t end_client_id, const uint8_t *raw, uint32_t raw_len,
		  uint8_t *out, uint32_t out_size, uint32_t *out_len_out)
{
	uint32_t need;

	if (!out || !out_len_out)
		return -EINVAL;
	if (!raw && raw_len > 0)
		return -EINVAL;

	need = ps_owner_wrapped_size(raw_len);
	if (out_size < need)
		return -ENOSPC;

	out[0] = (uint8_t)(end_client_id >> 56);
	out[1] = (uint8_t)(end_client_id >> 48);
	out[2] = (uint8_t)(end_client_id >> 40);
	out[3] = (uint8_t)(end_client_id >> 32);
	out[4] = (uint8_t)(end_client_id >> 24);
	out[5] = (uint8_t)(end_client_id >> 16);
	out[6] = (uint8_t)(end_client_id >> 8);
	out[7] = (uint8_t)(end_client_id);

	if (raw_len > 0)
		memcpy(out + PS_OWNER_TAG_SIZE, raw, raw_len);

	*out_len_out = need;
	return 0;
}
