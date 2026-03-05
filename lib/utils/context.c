/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pthread.h>
#include <stdlib.h>
#include "reffs/context.h"

static __thread struct reffs_context thread_ctx = {
	.uid = 0,
	.gid = 0
};

void reffs_set_context(struct reffs_context *ctx)
{
	if (ctx) {
		thread_ctx.uid = ctx->uid;
		thread_ctx.gid = ctx->gid;
	} else {
		thread_ctx.uid = 0;
		thread_ctx.gid = 0;
	}
}

struct reffs_context *reffs_get_context(void)
{
	return &thread_ctx;
}
