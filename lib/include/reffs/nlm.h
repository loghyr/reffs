/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NLM_H
#define _REFFS_NLM_H

int nlm4_protocol_register(void);
int nlm4_protocol_deregister(void);
int nlm_protocol_register(void);
int nlm_protocol_deregister(void);

#endif
