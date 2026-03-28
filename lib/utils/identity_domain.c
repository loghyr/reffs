/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Identity domain table.
 *
 * Maps a small index (0..N) to an external domain descriptor.
 * Domain 0 is always the local UNIX namespace.  New domains are
 * auto-created when authentication arrives from an unknown realm.
 *
 * The table is a simple array (domains are few — dozens at most).
 * Persisted to <state_dir>/identity_domains via write-temp/fsync/rename.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "reffs/identity_map.h"
#include "reffs/log.h"

static struct identity_domain domains[IDENTITY_DOMAIN_MAX];
static uint32_t domain_count;
static pthread_mutex_t domain_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Init / fini                                                         */
/* ------------------------------------------------------------------ */

int identity_domain_init(void)
{
	pthread_mutex_lock(&domain_lock);

	memset(domains, 0, sizeof(domains));
	domain_count = 1;

	/* Domain 0: local UNIX namespace (always present). */
	domains[0].id_index = 0;
	domains[0].id_type = REFFS_ID_UNIX;
	domains[0].id_flags = ID_DOMAIN_FLAG_ACTIVE;
	strncpy(domains[0].id_name, "local", sizeof(domains[0].id_name) - 1);

	pthread_mutex_unlock(&domain_lock);
	return 0;
}

void identity_domain_fini(void)
{
	pthread_mutex_lock(&domain_lock);
	domain_count = 0;
	memset(domains, 0, sizeof(domains));
	pthread_mutex_unlock(&domain_lock);
}

/* ------------------------------------------------------------------ */
/* Lookup / create                                                     */
/* ------------------------------------------------------------------ */

int identity_domain_find_or_create(const char *name, enum reffs_id_type type)
{
	int ret;

	if (!name || !name[0])
		return -EINVAL;

	pthread_mutex_lock(&domain_lock);

	/* Search for existing domain by name. */
	for (uint32_t i = 0; i < domain_count; i++) {
		if ((domains[i].id_flags & ID_DOMAIN_FLAG_ACTIVE) &&
		    strncmp(domains[i].id_name, name,
			    IDENTITY_DOMAIN_NAME_MAX) == 0) {
			ret = (int)i;
			goto out;
		}
	}

	/* Create a new domain. */
	if (domain_count >= IDENTITY_DOMAIN_MAX) {
		ret = -ENOSPC;
		goto out;
	}

	uint32_t idx = domain_count++;

	domains[idx].id_index = idx;
	domains[idx].id_type = type;
	domains[idx].id_flags = ID_DOMAIN_FLAG_ACTIVE;
	strncpy(domains[idx].id_name, name, sizeof(domains[idx].id_name) - 1);
	domains[idx].id_name[sizeof(domains[idx].id_name) - 1] = '\0';

	TRACE("identity_domain: created domain %u: %s (type %d)", idx, name,
	      type);
	ret = (int)idx;

out:
	pthread_mutex_unlock(&domain_lock);
	return ret;
}

const struct identity_domain *identity_domain_get(uint32_t index)
{
	if (index >= IDENTITY_DOMAIN_MAX)
		return NULL;

	pthread_mutex_lock(&domain_lock);

	const struct identity_domain *d = NULL;

	if (index < domain_count &&
	    (domains[index].id_flags & ID_DOMAIN_FLAG_ACTIVE))
		d = &domains[index];

	pthread_mutex_unlock(&domain_lock);
	return d;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

#define DOMAIN_FILE "identity_domains"
#define DOMAIN_MAGIC 0x52464944 /* "RFID" */
#define DOMAIN_VERSION 1

struct domain_disk_header {
	uint32_t dh_magic;
	uint32_t dh_version;
	uint32_t dh_count;
	uint32_t dh_reserved;
};

struct domain_disk_entry {
	uint32_t de_index;
	uint32_t de_type;
	uint32_t de_flags;
	uint32_t de_reserved;
	char de_name[IDENTITY_DOMAIN_NAME_MAX];
};

int identity_domain_persist(const char *state_dir)
{
	char path[512], tmp[520];

	if (snprintf(path, sizeof(path), "%s/%s", state_dir, DOMAIN_FILE) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;

	pthread_mutex_lock(&domain_lock);

	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0) {
		pthread_mutex_unlock(&domain_lock);
		return -errno;
	}

	struct domain_disk_header hdr = {
		.dh_magic = DOMAIN_MAGIC,
		.dh_version = DOMAIN_VERSION,
		.dh_count = domain_count,
	};

	ssize_t n = write(fd, &hdr, sizeof(hdr));

	if (n != (ssize_t)sizeof(hdr))
		goto err;

	for (uint32_t i = 0; i < domain_count; i++) {
		struct domain_disk_entry de = {
			.de_index = domains[i].id_index,
			.de_type = (uint32_t)domains[i].id_type,
			.de_flags = domains[i].id_flags,
		};

		strncpy(de.de_name, domains[i].id_name, sizeof(de.de_name));
		n = write(fd, &de, sizeof(de));
		if (n != (ssize_t)sizeof(de))
			goto err;
	}

	if (fdatasync(fd))
		goto err;

	close(fd);
	pthread_mutex_unlock(&domain_lock);

	if (rename(tmp, path)) {
		LOG("identity_domain_persist: rename: %m");
		unlink(tmp);
		return -errno;
	}

	return 0;

err:
	close(fd);
	unlink(tmp);
	pthread_mutex_unlock(&domain_lock);
	return -EIO;
}

int identity_domain_load(const char *state_dir)
{
	char path[512];

	if (snprintf(path, sizeof(path), "%s/%s", state_dir, DOMAIN_FILE) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;

	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return (errno == ENOENT) ? 0 : -errno;

	struct domain_disk_header hdr;
	ssize_t n = read(fd, &hdr, sizeof(hdr));

	if (n != (ssize_t)sizeof(hdr) || hdr.dh_magic != DOMAIN_MAGIC ||
	    hdr.dh_version != DOMAIN_VERSION) {
		close(fd);
		return -EINVAL;
	}

	if (hdr.dh_count > IDENTITY_DOMAIN_MAX) {
		close(fd);
		return -EOVERFLOW;
	}

	pthread_mutex_lock(&domain_lock);

	for (uint32_t i = 0; i < hdr.dh_count; i++) {
		struct domain_disk_entry de;

		n = read(fd, &de, sizeof(de));
		if (n != (ssize_t)sizeof(de)) {
			pthread_mutex_unlock(&domain_lock);
			close(fd);
			return -EIO;
		}

		if (de.de_index >= IDENTITY_DOMAIN_MAX)
			continue;

		domains[de.de_index].id_index = de.de_index;
		domains[de.de_index].id_type = (enum reffs_id_type)de.de_type;
		domains[de.de_index].id_flags = de.de_flags;
		strncpy(domains[de.de_index].id_name, de.de_name,
			sizeof(domains[de.de_index].id_name));
		domains[de.de_index]
			.id_name[sizeof(domains[de.de_index].id_name) - 1] =
			'\0';
	}

	domain_count = hdr.dh_count;
	pthread_mutex_unlock(&domain_lock);
	close(fd);

	TRACE("identity_domain: loaded %u domains", domain_count);
	return 0;
}
