/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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
#define NAMELEN (100)

#define MAX_LISTENERS (5)
#define MAX_CONNECTORS (10)

struct client {
	struct rcu_head c_rcu;
	struct urcu_ref c_ref;
	struct cds_list_head c_link;
	uint64_t c_id;
	int c_fd;
	struct sockaddr_in c_addr;
	char c_addr_str[NAMELEN];
	char c_name[NAMELEN];
};

CDS_LIST_HEAD(client_list);
static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t client_cond = PTHREAD_COND_INITIALIZER;
uint64_t next_id = 0;

bool stop_processing = false;

struct client *client_get(struct client *client)
{
	if (!client)
		return NULL;

	if (!urcu_ref_get_unless_zero(&client->c_ref))
		return NULL;

	return client;
}

static void client_free_rcu(struct rcu_head *rcu)
{
	struct client *client = caa_container_of(rcu, struct client, c_rcu);

	free(client);
}

static void client_release(struct urcu_ref *ref)
{
	struct client *client = caa_container_of(ref, struct client, c_ref);
	printf("Releasing %5ld fd = %d\n", client->c_id, client->c_fd);
	close(client->c_fd);

	call_rcu(&client->c_rcu, client_free_rcu);
}

static void client_put(struct client *client)
{
	if (!client)
		return;

	urcu_ref_put(&client->c_ref, client_release);
}

struct client *assign_connector(int listener)
{
	struct hostent *host;
	char *hostaddr;
	socklen_t len;
	int ret;

	struct client *client;

	client = calloc(1, sizeof(*client));
	if (!client)
		FAIL("Could not alloc a client");

	client->c_id = uatomic_add_return(&next_id, 1);
	cds_list_add_rcu(&client->c_link, &client_list);
	urcu_ref_init(&client->c_ref);

	len = sizeof(client->c_addr);

	client->c_fd =
		accept(listener, (struct sockaddr *)&client->c_addr, &len);
	if (client->c_fd < 0)
		FAIL("Could not open socket: %d", client->c_fd);

	host = gethostbyaddr((const char *)&client->c_addr.sin_addr.s_addr,
			     sizeof(client->c_addr.sin_addr.s_addr), AF_INET);
	if (!host) {
		ret = errno;
		FAIL("Could not gethostbyaddr(): %d", ret);
	}
	strncpy(client->c_name, host->h_name, NAMELEN - 1);

	hostaddr = inet_ntoa(client->c_addr.sin_addr);
	if (!hostaddr) {
		ret = errno;
		FAIL("Could not inet_ntoa(): %d", ret);
	}

	strncpy(client->c_addr_str, hostaddr, NAMELEN - 1);
}

static void *connector_thread(void *vclient)
{
	struct client *client = vclient;

	char buf[BUFSIZE];
	ssize_t n;

	rcu_register_thread();

	if (!client)
		FAIL("Did not get a client");

	while (1) {
		bzero(buf, BUFSIZE);
		n = read(client->c_fd, buf, BUFSIZE);
		if (n < 0)
			FAIL("Could not read from socket: %d", n);

		printf("%s (aka %s) said %ld bytes: %s", client->c_name,
		       client->c_addr_str, n, buf);

		n = write(client->c_fd, buf, strlen(buf));
		if (n < 0)
			FAIL("Could not write to socket: %d", n);

		if (!strncmp(buf, "kill", 4) || !strncmp(buf, "done", 4))
			break;
	}

	if (!strncmp(buf, "kill", 4)) {
		uatomic_set(&stop_processing, true);
	}

	/*
	 * Fine for now...
	 */
	cds_list_del(&client->c_link);
	client_put(client);

	rcu_unregister_thread();

	return NULL;
}

int main(int argc, char *argv[])
{
	int ret;
	unsigned short port = 3049;
	unsigned short num_listeners = 3;
	unsigned short i;
	unsigned short j;

	pid_t pid = getpid();

	int listener;
	int connector;
	int opt;
	int listner_opt = 1;

	struct sockaddr_in server;
	pthread_t tid_connector;
	pthread_attr_t attr;

	atomic_flag af;

	struct pollfd *pfds;

	struct client *client;

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

	pfds = calloc(num_listeners, sizeof(*pfds));
	if (!pfds)
		FAIL("Could not allocate memory");

	/*
	 * Two goals:
	 * 1) Multiple ports being monitored
	 * 2) Multiple clients connecting to a port.
	 */
	for (i = 0; i < num_listeners; i++) {
		pfds[i].fd = socket(AF_INET, SOCK_STREAM, 0);
		if (pfds[i].fd < 0)
			FAIL("Could not open socket: %d", pfds[i].fd);

		setsockopt(pfds[i].fd, SOL_SOCKET, SO_REUSEADDR,
			   (const void *)&listner_opt, sizeof(int));

		bzero((char *)&server, sizeof(server));
		server.sin_family = AF_INET;
		server.sin_addr.s_addr = htonl(INADDR_ANY);
		server.sin_port = htons(port + i);

		ret = bind(pfds[i].fd, (struct sockaddr *)&server,
			   sizeof(server));
		if (ret < 0)
			FAIL("Could not bind server socket: %d", ret);

		ret = listen(pfds[i].fd, 5);
		if (ret < 0)
			FAIL("Could not listen on server socket: %d", ret);

		pfds[i].events = POLLIN;
	}

	while (!uatomic_read(&stop_processing)) {
		ret = poll(pfds, num_listeners, -1);
		if (ret < 0)
			FAIL("Could not poll: %d", ret);

		for (i = 0; i < num_listeners; i++) {
			if (pfds[i].revents != 0) {
				client = assign_connector(pfds[i].fd);
				ret = pthread_create(&tid_connector, &attr,
						     connector_thread, client);
			}
		}
	}

	free(pfds);

	rcu_unregister_thread();

	return 0;
}
