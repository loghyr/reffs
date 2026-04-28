/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "host_port.h"

/*
 * Parse a NUL-terminated decimal port "1".."65535".  Returns the
 * port on success, -EINVAL on empty / non-decimal / out-of-range.
 */
static int parse_port(const char *s)
{
	if (!s || s[0] == '\0')
		return -EINVAL;

	long val = 0;

	for (const char *p = s; *p; p++) {
		if (!isdigit((unsigned char)*p))
			return -EINVAL;
		val = val * 10 + (*p - '0');
		if (val > 65535)
			return -EINVAL;
	}
	if (val < 1)
		return -EINVAL;
	return (int)val;
}

int mds_parse_host_port(const char *input, char *host_buf, size_t host_buf_sz,
			int *port_out)
{
	if (!input || !host_buf || host_buf_sz == 0 || !port_out)
		return -EINVAL;

	*port_out = 0;
	host_buf[0] = '\0';

	size_t in_len = strlen(input);

	if (in_len == 0)
		return -EINVAL;

	/*
	 * Bracketed form [host] or [host]:port.  Required for any IPv6
	 * literal that wants an explicit port -- otherwise the colons in
	 * the address are indistinguishable from the host:port separator.
	 */
	if (input[0] == '[') {
		const char *rb = strchr(input, ']');

		if (!rb || rb == input + 1)
			return -EINVAL;

		size_t host_len = (size_t)(rb - input - 1);

		if (host_len + 1 > host_buf_sz)
			return -EINVAL;
		memcpy(host_buf, input + 1, host_len);
		host_buf[host_len] = '\0';

		const char *tail = rb + 1;

		if (*tail == '\0')
			return 0;
		if (*tail != ':')
			return -EINVAL;

		int port = parse_port(tail + 1);

		if (port < 0)
			return -EINVAL;
		*port_out = port;
		return 0;
	}

	/*
	 * Unbracketed form.  Count colons to disambiguate IPv4/host
	 * from IPv6 literal:
	 *   0 colons -> "host", no port
	 *   1 colon  -> "host:port"
	 *   >1 colons -> IPv6 literal with no port (use brackets to
	 *                attach a port).
	 */
	int ncolons = 0;
	const char *first_colon = NULL;
	const char *last_colon = NULL;

	for (const char *p = input; *p; p++) {
		if (*p == ':') {
			if (!first_colon)
				first_colon = p;
			last_colon = p;
			ncolons++;
		}
	}

	if (ncolons == 0) {
		if (in_len + 1 > host_buf_sz)
			return -EINVAL;
		memcpy(host_buf, input, in_len + 1);
		return 0;
	}

	if (ncolons == 1) {
		size_t host_len = (size_t)(first_colon - input);

		if (host_len == 0 || host_len + 1 > host_buf_sz)
			return -EINVAL;
		memcpy(host_buf, input, host_len);
		host_buf[host_len] = '\0';

		int port = parse_port(first_colon + 1);

		if (port < 0)
			return -EINVAL;
		*port_out = port;
		return 0;
	}

	/* ncolons > 1: treat as bare IPv6 literal, no port. */
	(void)last_colon;
	if (in_len + 1 > host_buf_sz)
		return -EINVAL;
	memcpy(host_buf, input, in_len + 1);
	return 0;
}
