/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef _REFFS_FFV2_PROTOTYPE_INTERNAL_H
#define _REFFS_FFV2_PROTOTYPE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

struct ffv2_prototype_nl_io {
	void *context;
	int (*open)(void *context);
	int (*bind)(void *context, int fd, const struct sockaddr *address,
		    socklen_t address_len);
	ssize_t (*send)(void *context, int fd, const void *buffer, size_t len,
			const struct sockaddr *address, socklen_t address_len);
	ssize_t (*receive)(void *context, int fd, void *buffer, size_t len,
			   struct sockaddr *address, socklen_t *address_len);
	int (*close)(void *context, int fd);
};

/* Direct receive-boundary seam for the generic-netlink envelope tests. */
int ffv2_prototype_nl_open_test(const struct ffv2_prototype_nl_io *io,
				uint16_t *family);

#endif /* _REFFS_FFV2_PROTOTYPE_INTERNAL_H */
