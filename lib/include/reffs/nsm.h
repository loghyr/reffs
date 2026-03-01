/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_NSM_H
#define _REFFS_NSM_H

int sm_protocol_register(void);
int sm_protocol_deregister(void);
int sm_get_state(void);
void sm_notify_host(const char *hostname);

#endif
