/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "reffs/rcu.h"

struct buffer_free {
	char *s;
	struct rcu_head rcu;
};

static void buffer_free_rcu(struct rcu_head *rcu)
{
	struct buffer_free *bf = caa_container_of(rcu, struct buffer_free, rcu);

	free(bf->s);
	free(bf);
}

void reffs_string_release(char *s)
{
	if (!s)
		return;

	struct buffer_free *bf = malloc(sizeof(*bf));
	if (!bf)
		free(s);

	bf->s = s;
	call_rcu(&bf->rcu, buffer_free_rcu);
}
