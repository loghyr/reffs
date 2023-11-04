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
#include <stdarg.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUFSIZE 1024

static inline void fail(const char *function, int line, const char *msg, ...)
{
	va_list ap;
	va_start(ap, msg);
	fprintf(stderr, "%s:%d ", function, line);
	vfprintf(stderr, msg, ap);
	va_end(ap);
	abort();
}

#define FAIL(...) fail(__func__, __LINE__, __VA_ARGS__)

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

	int len;
	ssize_t n;

	if (argc != 2)
		FAIL("usage: %s <port>\n", argv[0]);

	port = atoi(argv[1]);

	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0)
		FAIL("Could not open socket: %d\n", listener);

	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const void *)&opt,
		   sizeof(int));

	bzero((char *)&server, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl(INADDR_ANY);
	server.sin_port = htons(port);

	ret = bind(listener, (struct sockaddr *)&server, sizeof(server));
	if (ret < 0)
		FAIL("Could not bind server socket: %d\n", ret);

	ret = listen(listener, 5);
	if (ret < 0)
		FAIL("Could not listen on server socket: %d\n", ret);

	len = sizeof(client);
	while (1) {
		connector = accept(listener, (struct sockaddr *)&client, &len);
		if (connector < 0)
			FAIL("Could not open socket: %d\n", connector);

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

		bzero(buf, BUFSIZE);
		n = read(connector, buf, BUFSIZE);
		if (n < 0)
			FAIL("Could not read from socket: %d\n", n);

		printf("%s (aka %s) said %d bytes: %s\n", host->h_name,
		       hostaddr, n, buf);

		n = write(connector, buf, strlen(buf));
		if (n < 0)
			FAIL("Could not write to socket: %d\n", n);

		close(connector);

		if (!strncmp(buf, "kill", 4))
			break;
	}

	return 0;
}
