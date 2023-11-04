/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "reffs/log.h"

#define BUFSIZE 1024

int main(int argc, char *argv[])
{
	int ret;
	unsigned short port;

	int listener;
	int connector;
	int opt = 1;

	struct sockaddr_in server;
	struct sockaddr_in client;

	struct hostent *host;
	char buf[BUFSIZE];

	char *hostaddr;

	socklen_t len;
	ssize_t n;

	atomic_flag af;

	atomic_flag_clear(&af); // Figure how to init it!

	ret = 1;
	WARN_ONCE(&af, "Running %s at %d", argv[0], ret++);
	WARN_ONCE(&af, "Running %s at %d", argv[0], ret++);

	if (argc != 2)
		FAIL("usage: %s <port>", argv[0]);

	port = atoi(argv[1]);

	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0)
		FAIL("Could not open socket: %d", listener);

	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const void *)&opt,
		   sizeof(int));

	bzero((char *)&server, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl(INADDR_ANY);
	server.sin_port = htons(port);

	ret = bind(listener, (struct sockaddr *)&server, sizeof(server));
	if (ret < 0)
		FAIL("Could not bind server socket: %d", ret);

	ret = listen(listener, 5);
	if (ret < 0)
		FAIL("Could not listen on server socket: %d", ret);

	len = sizeof(client);
	while (1) {
		connector = accept(listener, (struct sockaddr *)&client, &len);
		if (connector < 0)
			FAIL("Could not open socket: %d", connector);

		host = gethostbyaddr((const char *)&client.sin_addr.s_addr,
				     sizeof(client.sin_addr.s_addr), AF_INET);
		if (!host) {
			ret = errno;
			FAIL("Could not gethostbyaddr(): %d", ret);
		}

		hostaddr = inet_ntoa(client.sin_addr);
		if (!hostaddr) {
			ret = errno;
			FAIL("Could not inet_ntoa(): %d", ret);
		}

		while (1) {
		bzero(buf, BUFSIZE);
		n = read(connector, buf, BUFSIZE);
		if (n < 0)
			FAIL("Could not read from socket: %d", n);

		printf("%s (aka %s) said %ld bytes: %s", host->h_name,
		       hostaddr, n, buf);

		n = write(connector, buf, strlen(buf));
		if (n < 0)
			FAIL("Could not write to socket: %d", n);

		if (!strncmp(buf, "kill", 4) || !strncmp(buf, "done", 4))
			break;
		}

		close(connector);

		if (!strncmp(buf, "kill", 4))
			break;
	}

	return 0;
}
