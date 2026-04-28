/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Proxy assignment queue -- slice 6c-y.
 *
 * FIFO queue of proxy_assignment_item.  Mutex-guarded; concurrent
 * producers + a single consumer (PROXY_PROGRESS reply builder)
 * are safe.  See lib/nfs4/include/nfs4/proxy_assignment_queue.h
 * for the contract.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include "nfs4/proxy_assignment_queue.h"

struct paq_node {
	STAILQ_ENTRY(paq_node) pq_link;
	struct proxy_assignment_item pq_item;
};

STAILQ_HEAD(paq_head, paq_node);

static pthread_mutex_t paq_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct paq_head paq_head = STAILQ_HEAD_INITIALIZER(paq_head);
static size_t paq_count;
static bool paq_inited;

int proxy_assignment_queue_init(void)
{
	pthread_mutex_lock(&paq_mutex);
	if (!paq_inited) {
		STAILQ_INIT(&paq_head);
		paq_count = 0;
		paq_inited = true;
	}
	pthread_mutex_unlock(&paq_mutex);
	return 0;
}

void proxy_assignment_queue_fini(void)
{
	pthread_mutex_lock(&paq_mutex);
	if (!paq_inited) {
		pthread_mutex_unlock(&paq_mutex);
		return;
	}
	struct paq_node *n;

	while ((n = STAILQ_FIRST(&paq_head)) != NULL) {
		STAILQ_REMOVE_HEAD(&paq_head, pq_link);
		free(n);
	}
	paq_count = 0;
	paq_inited = false;
	pthread_mutex_unlock(&paq_mutex);
}

int proxy_assignment_queue_push(const struct proxy_assignment_item *item)
{
	if (!item)
		return -EINVAL;

	struct paq_node *n = malloc(sizeof(*n));

	if (!n)
		return -ENOMEM;
	n->pq_item = *item;

	pthread_mutex_lock(&paq_mutex);
	if (!paq_inited) {
		pthread_mutex_unlock(&paq_mutex);
		free(n);
		return -EINVAL;
	}
	STAILQ_INSERT_TAIL(&paq_head, n, pq_link);
	paq_count++;
	pthread_mutex_unlock(&paq_mutex);
	return 0;
}

size_t proxy_assignment_queue_pop(struct proxy_assignment_item *out, size_t max)
{
	if (!out || max == 0)
		return 0;

	size_t took = 0;

	pthread_mutex_lock(&paq_mutex);
	if (!paq_inited) {
		pthread_mutex_unlock(&paq_mutex);
		return 0;
	}
	while (took < max) {
		struct paq_node *n = STAILQ_FIRST(&paq_head);

		if (!n)
			break;
		STAILQ_REMOVE_HEAD(&paq_head, pq_link);
		paq_count--;
		out[took++] = n->pq_item;
		free(n);
	}
	pthread_mutex_unlock(&paq_mutex);
	return took;
}

size_t proxy_assignment_queue_len(void)
{
	pthread_mutex_lock(&paq_mutex);
	size_t n = paq_inited ? paq_count : 0;

	pthread_mutex_unlock(&paq_mutex);
	return n;
}
