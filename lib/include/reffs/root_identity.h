/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_ROOT_IDENTITY_H
#define _REFFS_ROOT_IDENTITY_H

#include <uuid/uuid.h>

/* Only the native POSIX root calls this, before namespace publication. */
int reffs_root_identity_load_or_create(const char *backend_path,
				       const char *state_dir, uuid_t root_uuid);

#endif
