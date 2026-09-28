/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef _REFFS_FIXED_LAYOUT_H
#define _REFFS_FIXED_LAYOUT_H

#include <stdint.h>

struct inode;

/*
 * Claim the configured fixed vector for inode.  Returns one when fixed mode
 * applies and the exact segment is assigned, zero when fixed mode does not
 * apply, or a negative errno on exhaustion or an inconsistent generation.
 */
int ffv2_fixed_layout_assign(struct inode *inode, uint32_t layout_type);

#endif /* _REFFS_FIXED_LAYOUT_H */
