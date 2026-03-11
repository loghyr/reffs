/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

struct work_item {
	struct rcu_head wi_rcu;
	struct urcu_ref wi_ref;
	struct cds_list_head wi_link;
	uint64_t wi_id;
	uint64_t wi_payload;
};

CDS_LIST_HEAD(work_item_list);
static pthread_mutex_t work_item_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_item_cond = PTHREAD_COND_INITIALIZER;
uint64_t next_id = 0;
bool clean_up = false;

struct work_item *work_item_get(struct work_item *wi)
{
	if (!wi)
		return NULL;

	if (!urcu_ref_get_unless_zero(&wi->wi_ref))
		return NULL;

	return wi;
}

static void work_item_free_rcu(struct rcu_head *rcu)
{
	struct work_item *wi = caa_container_of(rcu, struct work_item, wi_rcu);

	free(wi);
}

static void work_item_release(struct urcu_ref *ref)
{
	struct work_item *wi = caa_container_of(ref, struct work_item, wi_ref);
	printf("Releasing %5ld payload = %ld\n", wi->wi_id, wi->wi_payload);
	call_rcu(&wi->wi_rcu, work_item_free_rcu);
}

static void work_item_put(struct work_item *wi)
{
	if (!wi)
		return;

	urcu_ref_put(&wi->wi_ref, work_item_release);
}

static void *watcher(void __attribute__((unused)) * unused)
{
	struct work_item *wi;
	struct timespec ts;
	int ret;

	rcu_register_thread();

	if (clock_gettime(CLOCK_REALTIME, &ts)) {
		fprintf(stderr, "clock_gettime: %m\n");
		abort();
	}

	ts.tv_sec += 1;

	pthread_mutex_lock(&work_item_lock);
	ret = pthread_cond_timedwait(&work_item_cond, &work_item_lock, &ts);
	if (ret && ret != ETIMEDOUT) {
		fprintf(stderr, "pthread_cond_timedwait: %d\n", ret);
		abort();
	}

	/* Take out the odd ones */
	rcu_read_lock();
	cds_list_for_each_entry_rcu(wi, &work_item_list, wi_link)
		if (wi->wi_id % 2) {
			cds_list_del(&wi->wi_link);
			work_item_put(wi);
		}
	rcu_read_unlock();

	rcu_barrier();

	ret = pthread_cond_timedwait(&work_item_cond, &work_item_lock, &ts);
	if (ret && ret != ETIMEDOUT) {
		fprintf(stderr, "pthread_cond_timedwait: %d\n", ret);
		abort();
	}

	bool b = true;
	__atomic_store(&clean_up, &b, __ATOMIC_RELAXED);
	pthread_cond_signal(&work_item_cond);
	pthread_mutex_unlock(&work_item_lock);

	rcu_unregister_thread();

	return NULL;
}

static void *actor(void __attribute__((unused)) * unused)
{
	uint64_t i;
	struct work_item *wi;
	struct work_item *tmp;

	rcu_register_thread();

	while (true) {
		for (i = 0;; i++) {
			uint64_t id;
			__atomic_load(&next_id, &id, __ATOMIC_RELAXED);

			if (i < id + 1)
				break;

			rcu_read_lock();
			wi = NULL;
			cds_list_for_each_entry_rcu(tmp, &work_item_list,
						    wi_link)
				if (tmp->wi_id == i) {
					wi = work_item_get(tmp);
					break;
				}
			rcu_read_unlock();

			if (wi) {
				wi->wi_payload++;
				work_item_put(wi);
			}
		}

		if (cds_list_empty(&work_item_list))
			break;
	}

	rcu_unregister_thread();

	return NULL;
}

int main(int argc, char *argv[])
{
	uint64_t i;
	bool done = false;
	int ret;
	struct work_item *wi;

	pthread_t tid_actor;
	pthread_t tid_watcher;
	pthread_attr_t attr;

	rcu_register_thread();

	if (argc != 2) {
		fprintf(stderr, "%s <#items>\n", argv[0]);
		rcu_unregister_thread();
		return 1;
	}

	ret = pthread_attr_init(&attr);
	if (ret) {
		fprintf(stderr, "Could not init thread attributes: %d\n", ret);
		abort();
	}

	ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (ret) {
		fprintf(stderr, "Could not assign thread attributes: %d\n",
			ret);
		abort();
	}

	pthread_mutex_lock(&work_item_lock);
	for (i = 0; i < atol(argv[1]); i++) {
		wi = calloc(1, sizeof(*wi));
		if (!wi) {
			fprintf(stderr, "Could not alloc id = %d\n", i);
			abort();
		}

		wi->wi_id = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
		cds_list_add_rcu(&wi->wi_link, &work_item_list);
		urcu_ref_init(&wi->wi_ref);
	}
	pthread_mutex_unlock(&work_item_lock);

	ret = pthread_create(&tid_watcher, &attr, watcher, NULL);
	if (ret) {
		fprintf(stderr, "Could not create watcher thread: %d\n", ret);
		abort();
	}

	ret = pthread_create(&tid_actor, &attr, actor, NULL);
	if (ret) {
		fprintf(stderr, "Could not create actor thread: %d\n", ret);
		abort();
	}

	pthread_mutex_lock(&work_item_lock);
	while (!done) {
		pthread_cond_wait(&work_item_cond, &work_item_lock);
		__atomic_load(&clean_up, &done, __ATOMIC_RELAXED);
	}
	pthread_mutex_unlock(&work_item_lock);

	synchronize_rcu();
	rcu_barrier();

	rcu_read_lock();
	while (!cds_list_empty(&work_item_list)) {
		wi = cds_list_first_entry(&work_item_list, struct work_item,
					  wi_link);
		cds_list_del(&wi->wi_link);
		work_item_put(wi);
	}
	rcu_read_unlock();

	rcu_barrier();

	ret = pthread_cond_destroy(&work_item_cond);
	if (ret) {
		fprintf(stderr, "Could not destroy condition: %d\n", ret);
		abort();
	}

	ret = pthread_mutex_destroy(&work_item_lock);
	if (ret) {
		fprintf(stderr, "Could not destroy condition lock: %d\n", ret);
		abort();
	}

	rcu_unregister_thread();

	return 0;
}
