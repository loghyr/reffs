/*
 * SPDX-License-Identifier: GPL-2.0+
 * (C) 2023 Tom Haynes <loghyr@gmail.com>
 */

#ifndef _REFFS_WORKQUEUE_INTERNAL_H
#define _REFFS_WORKQUEUE_INTERNAL_H

#include <pthread.h>

struct msg {
	void *msg_data;
	void *(*msg_handler)(void *data);
};

struct workqueue {
	pthread_mutex_t wq_mutex;
};

#endif /* _REFFS_WORKQUEUE_INTERNAL_H */
