/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/wfcqueue.h>
#include <urcu/ref.h>
#include "reffs/log.h"

static void usage(const char *me)
{
	fprintf(stdout, "Usage: %s [options]\n", me);
	fprintf(stdout, "Options:\n");
	fprintf(stdout, " -h  --help         Show help\n");
	fprintf(stdout, " -l  --listeners    How many port listeners\n");
	fprintf(stdout, " -p  --port         Starting port number\n");
}

static struct option options[] = {
	{ "help", no_argument, 0, 'h' },
	{ "listeners", required_argument, 0, 'l' },
	{ "port", required_argument, 0, 'p' },
	{ NULL, 0, NULL, 0 },
};

#define BUFSIZE (1024)

#define MAX_LISTENERS (5)
#define MAX_CONNECTORS (10)

#define LISTENER_IS_CLIENT (1U << 0)
#define LISTENER_IS_SERVER (1U << 1)
#define LISTENER_IS_DEAF (1U << 2)

struct listener {
	struct rcu_head l_rcu;
	struct urcu_ref l_ref;
	struct cds_list_head l_link;
	uint32_t l_flags;
	uint64_t l_id;
	int l_fd;
	struct sockaddr_in6 l_addr;
	char l_addr_str[INET6_ADDRSTRLEN];
};

struct queue {
	struct cds_wfcq_head head;
	struct cds_wfcq_tail tail;
};

/*
 * Add an action and data here and
 * we have a msg structure.
 */
struct listener_queue {
	struct cds_wfcq_node lq_node;
	struct listener *lq_lsnr;
};

CDS_LIST_HEAD(listener_list);

// static pthread_mutex_t listener_lock = PTHREAD_MUTEX_INITIALIZER;
// static pthread_cond_t listener_cond = PTHREAD_COND_INITIALIZER;

uint64_t next_id = 0;

bool stop_processing = false;

static struct listener *listener_get(struct listener *lsnr)
{
	if (!lsnr)
		return NULL;

	if (!urcu_ref_get_unless_zero(&lsnr->l_ref))
		return NULL;

	return lsnr;
}

static void listener_free_rcu(struct rcu_head *rcu)
{
	struct listener *lsnr = caa_container_of(rcu, struct listener, l_rcu);

	free(lsnr);
}

static void listener_release(struct urcu_ref *ref)
{
	struct listener *lsnr = caa_container_of(ref, struct listener, l_ref);
	printf("Releasing %5ld fd = %d\n", lsnr->l_id, lsnr->l_fd);
	close(lsnr->l_fd);

	call_rcu(&lsnr->l_rcu, listener_free_rcu);
}

static void listener_put(struct listener *lsnr)
{
	if (!lsnr)
		return;

	urcu_ref_put(&lsnr->l_ref, listener_release);
}

static struct listener *listener_find(int fd)
{
	struct listener *lsnr = NULL;
	struct listener *tmp;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(tmp, &listener_list, l_link) {
		uint32_t lf_flags;
		__atomic_load(&tmp->l_flags, &l_flags, __ATOMIC_RELAXED);

		if (!(l_flags & LISTENER_IS_DEAF) && fd == tmp->l_fd) {
			lsnr = listener_get(tmp);
			break;
		}
	}
	rcu_read_unlock();

	return lsnr;
}

static void listener_find_and_close(int fd)
{
	struct listener *lsnr;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(lsnr, &listener_list, l_link) {
		uint32_t lf_flags;
		__atomic_load(&lsnr->l_flags, &l_flags, __ATOMIC_RELAXED);

		if (!(l_flags & LISTENER_IS_DEAF) && fd == lsnr->l_fd) {
			__atomic_fetch_or(&lsnr->l_flags, LISTENER_IS_DEAF,
					  __ATOMIC_RELAXED);
			cds_list_del(&lsnr->l_link);
			listener_put(lsnr);
			break;
		}
	}
	rcu_read_unlock();
}

struct listener *listener_alloc(uint32_t flags)
{
	struct listener *lsnr;

	lsnr = calloc(1, sizeof(*lsnr));
	if (!lsnr)
		FAIL("Could not alloc a lsnr");

	lsnr->l_flags = flags;

	lsnr->l_id = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
	cds_list_add_rcu(&lsnr->l_link, &listener_list);
	urcu_ref_init(&lsnr->l_ref);

	return lsnr;
}

int attach_listener(struct listener *lsnr, int listener)
{
	socklen_t len;
	int ret;

	len = sizeof(lsnr->l_addr);
	lsnr->l_fd = accept(listener, (struct sockaddr *)&lsnr->l_addr, &len);
	if (lsnr->l_fd < 0)
		FAIL("Could not open socket: %d", lsnr->l_fd);

	ret = getpeername(lsnr->l_fd, (struct sockaddr *)&lsnr->l_addr, &len);
	if (ret < 0) {
		ret = errno;
		FAIL("Could not getpeername(): %d", ret);
		// Handle
	}

	if (!inet_ntop(AF_INET6, &lsnr->l_addr.sin6_addr, lsnr->l_addr_str,
		       sizeof(lsnr->l_addr_str))) {
		ret = errno;
		LOG("Could not inet_ntop(): %d", ret);
		strcpy(lsnr->l_addr_str, "unkown");
	}

	return 0;
}

int attach_server(struct listener *lsnr, unsigned short port)
{
	int ret;
	int listner_opt = 1;

	struct sockaddr_in6 addr;

	lsnr->l_fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (lsnr->l_fd < 0)
		FAIL("Could not open socket: %d", lsnr->l_fd);

	setsockopt(lsnr->l_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
		   (const void *)&listner_opt, sizeof(int));

	bzero((char *)&addr, sizeof(addr));
	addr.sin6_family = AF_INET6;
	memcpy(&addr.sin6_addr, &in6addr_any, sizeof(in6addr_any));
	addr.sin6_port = htons(port);

	ret = bind(lsnr->l_fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0)
		FAIL("Could not bind server socket: %d", ret);

	ret = listen(lsnr->l_fd, 5);
	if (ret < 0)
		FAIL("Could not listen on server socket: %d", ret);

	return 0;
}

static void *connector_thread(void *vqueue)
{
	struct queue *queue = vqueue;
	struct listener *lsnr;

	char buf[BUFSIZE];
	ssize_t n;

	struct cds_wfcq_node *node;
	struct listener_queue *lq;

	rcu_register_thread();

	if (!queue)
		FAIL("Did not get a queue");

	while (1) {
		node = cds_wfcq_dequeue_blocking(&queue->head, &queue->tail);
		if (!node)
			continue;

		lq = caa_container_of(node, struct listener_queue, lq_node);
		lsnr = lq->lq_lsnr;
		free(node);

		bzero(buf, BUFSIZE);
		n = read(lsnr->l_fd, buf, BUFSIZE);
		if (n < 0)
			FAIL("Could not read from socket: %d", n);

		printf("%s said %ld bytes: %s", lsnr->l_addr_str, n, buf);

		n = write(lsnr->l_fd, buf, strlen(buf));
		if (n < 0)
			FAIL("Could not write to socket: %d", n);

		if (!strncmp(buf, "done", 4)) {
			__atomic_fetch_or(&lsnr->l_flags, LISTENER_IS_DEAF,
					  __ATOMIC_RELAXED);
			cds_list_del(&lsnr->l_link);
			listener_put(lsnr);
		}

		listener_put(lsnr);
	}

	rcu_unregister_thread();

	return NULL;
}

#define MAX_EVENTS (10)

static void server(int epfd, struct queue *queue)
{
	int count;
	int i;
	int ret;
	struct epoll_event events[MAX_EVENTS];
	struct epoll_event event;

	struct listener *lsnr;
	struct listener *fnd;

	struct listener_queue *lq;

	while (1) {
		bool b;
		__atomic_load(&stop_processing, &b, __ATOMIC_RELAXED);
		if (b)
			break;

		count = epoll_wait(epfd, events, MAX_EVENTS, 30000);

		for (i = 0; i < count; i++) {
			if (events[i].events & (EPOLLERR | EPOLLHUP) ||
			    !(events[i].events & EPOLLIN)) {
				LOG("epoll error = %u", events[i].events);
				listener_find_and_close(events[i].data.fd);
				continue;
			}

			fnd = listener_find(events[i].data.fd);
			if (!fnd)
				FAIL("Could not find listener for %d",
				     events[i].data.fd);
			if (fnd->l_flags & LISTENER_IS_SERVER) {
				lsnr = listener_alloc(LISTENER_IS_CLIENT);
				attach_listener(lsnr, events[i].data.fd);

				event.data.fd = lsnr->l_fd;
				event.events = EPOLLIN | EPOLLET;
				if (epoll_ctl(epfd, EPOLL_CTL_ADD,
					      event.data.fd, &event)) {
					ret = errno;
					FAIL("Could not epoll_ctl(): %d", ret);
				}

			} else {
				lq = malloc(sizeof(*lq));
				if (!lq)
					FAIL("Could not alloc lq");
				lq->lq_lsnr = fnd;
				fnd = NULL;
				cds_wfcq_node_init(&lq->lq_node);
				cds_wfcq_enqueue(&queue->head, &queue->tail,
						 &lq->lq_node);
			}

			listener_put(fnd);
		}
	}
}

static void done_signal_handler(int signum)
{
	LOG("Got signal %d", signum);
}

static void shutdown_signal_handler(int signum)
{
	struct sigaction sa = {
		.sa_handler = done_signal_handler,
		.sa_flags = SA_RESTART,
	};

	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

	LOG("Got signal %d", signum);
	bool b = true;
	__atomic_store(&stop_processing, &b, __ATOMIC_RELAXED);
}

int main(int argc, char *argv[])
{
	int ret;

	int i;
	int num_listeners = 3;

	int epfd;
	struct epoll_event event;

	unsigned short port = 3049;

	pid_t pid = getpid();

	int opt;
	pthread_t tid_connector;
	pthread_attr_t attr;

	atomic_flag af;

	struct listener *lsnr;

	struct queue queue;

	sigset_t sigmask;
	struct sigaction sa = {
		.sa_handler = shutdown_signal_handler,
		.sa_flags = SA_RESTART,
	};

	sigemptyset(&sigmask);
	ret = sigaddset(&sigmask, SIGTERM);
	if (ret < 0) {
		ret = errno;
		FAIL("Could not sigaddset(): %d", ret);
	}

	ret = sigaddset(&sigmask, SIGINT);
	if (ret < 0) {
		ret = errno;
		FAIL("Could not sigaddset(): %d", ret);
	}

	ret = pthread_sigmask(SIG_BLOCK, &sigmask, NULL);
	if (ret < 0) {
		ret = errno;
		FAIL("Could not pthread_sigmask(): %d", ret);
	}

	rcu_register_thread();

	atomic_flag_clear(&af); // Figure how to init it!

	WARN_ONCE(&af, "Running %s (%d) at port %d and with %d listeners",
		  argv[0], pid, port, num_listeners);

	while ((opt = getopt_long(argc, argv, "hl:p:", options, NULL)) != -1) {
		switch (opt) {
		case 'l':
			num_listeners = atoi(optarg);
			if (num_listeners > 5)
				FAIL("Only support up to 5 listeners - have %d",
				     num_listeners);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'h':
			usage(argv[0]);
			rcu_unregister_thread(); // WARNING!!!
			exit(1);
		}
	}

	ret = pthread_attr_init(&attr);
	if (ret)
		FAIL("Could not init thread attributes: %d", ret);

	ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (ret)
		FAIL("Could not assign thread attributes: %d", ret);

	cds_wfcq_init(&queue.head, &queue.tail);

	epfd = epoll_create1(0);
	if (epfd < 0) {
		ret = errno;
		FAIL("Could not epoll_create1(): %d", ret);
	}

	sa.sa_mask = sigmask;
	ret = sigaction(SIGTERM, &sa, NULL);
	if (ret < 0) {
		ret = errno;
		FAIL("Could not sigaction(): %d", ret);
	}

	ret = sigaction(SIGINT, &sa, NULL);
	if (ret < 0) {
		ret = errno;
		FAIL("Could not sigaction(): %d", ret);
	}

	ret = pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);
	if (ret < 0) {
		ret = errno;
		FAIL("Could not pthread_sigmask(): %d", ret);
	}

	for (i = 0; i < num_listeners; i++) {
		lsnr = listener_alloc(LISTENER_IS_SERVER);
		attach_server(lsnr, port + i);

		event.data.fd = lsnr->l_fd;
		event.events = EPOLLIN | EPOLLET;
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, event.data.fd, &event)) {
			ret = errno;
			FAIL("Could not epoll_ctl(): %d", ret);
		}

		ret = pthread_create(&tid_connector, &attr, connector_thread,
				     &queue);
		if (ret)
			FAIL("Could not create thread %d", ret);
	}

	server(epfd, &queue);

	ret = pthread_sigmask(SIG_BLOCK, &sigmask, NULL);
	if (ret < 0) {
		ret = errno;
		FAIL("Could not pthread_sigmask(): %d", ret);
	}

	synchronize_rcu();
	rcu_barrier();

	rcu_read_lock();
	while (!cds_list_empty(&listener_list)) {
		lsnr = cds_list_first_entry(&listener_list, struct listener,
					    l_link);
		cds_list_del(&lsnr->l_link);
		listener_put(lsnr);
	}
	rcu_read_unlock();

	rcu_barrier();

	close(epfd);

	rcu_unregister_thread();

	return 0;
}
