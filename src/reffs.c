/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
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

int main(int __attribute__((unused)) argc,
	 char __attribute__((unused)) * argv[])
{
	int ret;

	pthread_attr_t attr;

	rcu_register_thread();

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

	rcu_unregister_thread();

	return 0;
}
