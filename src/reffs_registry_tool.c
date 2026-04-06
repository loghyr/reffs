/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * reffs_registry_tool -- standalone tool for inspecting and repairing
 * the superblock registry.  Works without a running server.
 *
 * Subcommands:
 *   dump  <state_dir>   Print registry contents as text
 *   check <state_dir>   Validate registry integrity
 *   repair-counter <state_dir>  Reconcile next_id with sb dirs
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "reffs/sb_registry.h"

static int read_registry(const char *state_dir, struct sb_registry_header *hdr,
			 struct sb_registry_entry **entries)
{
	char path[PATH_MAX];

	if (snprintf(path, sizeof(path), "%s/%s", state_dir,
		     SB_REGISTRY_FILE) >= (int)sizeof(path))
		return -ENAMETOOLONG;

	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
		return -errno;
	}

	ssize_t n = read(fd, hdr, sizeof(*hdr));

	if (n != (ssize_t)sizeof(*hdr)) {
		close(fd);
		fprintf(stderr, "Short header read (%zd bytes)\n", n);
		return -EINVAL;
	}

	if (hdr->srh_magic != SB_REGISTRY_MAGIC) {
		close(fd);
		fprintf(stderr, "Bad magic: 0x%08x (expected 0x%08x)\n",
			hdr->srh_magic, SB_REGISTRY_MAGIC);
		return -EINVAL;
	}

	*entries = NULL;
	if (hdr->srh_count == 0) {
		close(fd);
		return 0;
	}

	*entries = calloc(hdr->srh_count, sizeof(**entries));
	if (!*entries) {
		close(fd);
		return -ENOMEM;
	}

	size_t esz = hdr->srh_count * sizeof(**entries);

	n = read(fd, *entries, esz);
	close(fd);

	if (n != (ssize_t)esz) {
		free(*entries);
		*entries = NULL;
		fprintf(stderr, "Short entry read (%zd/%zu bytes)\n", n, esz);
		return -EINVAL;
	}
	return 0;
}

static const char *state_name(uint32_t state)
{
	switch (state) {
	case 0:
		return "CREATED";
	case 1:
		return "MOUNTED";
	case 2:
		return "UNMOUNTED";
	case 3:
		return "DESTROYED";
	}
	return "UNKNOWN";
}

static int cmd_dump(const char *state_dir)
{
	struct sb_registry_header hdr;
	struct sb_registry_entry *entries = NULL;
	int ret = read_registry(state_dir, &hdr, &entries);

	if (ret)
		return ret;

	printf("Registry: version=%u count=%u next_id=%u\n", hdr.srh_version,
	       hdr.srh_count, hdr.srh_next_id);

	for (uint32_t i = 0; i < hdr.srh_count; i++) {
		struct sb_registry_entry *e = &entries[i];
		char uuid_str[37];

		uuid_unparse(e->sre_uuid, uuid_str);
		printf("  [%u] id=%lu uuid=%s state=%s storage=%u path=%s\n", i,
		       (unsigned long)e->sre_id, uuid_str,
		       state_name(e->sre_state), e->sre_storage_type,
		       e->sre_path);
	}

	free(entries);
	return 0;
}

static int cmd_check(const char *state_dir)
{
	struct sb_registry_header hdr;
	struct sb_registry_entry *entries = NULL;
	int ret = read_registry(state_dir, &hdr, &entries);
	int errors = 0;

	if (ret)
		return ret;

	/* Version check. */
	if (hdr.srh_version != SB_REGISTRY_VERSION) {
		fprintf(stderr, "ERROR: version %u (expected %u)\n",
			hdr.srh_version, SB_REGISTRY_VERSION);
		errors++;
	}

	/* Counter vs max id. */
	uint64_t max_id = 0;

	for (uint32_t i = 0; i < hdr.srh_count; i++) {
		if (entries[i].sre_id > max_id)
			max_id = entries[i].sre_id;
	}
	if (hdr.srh_count > 0 && hdr.srh_next_id <= max_id) {
		fprintf(stderr,
			"ERROR: next_id=%u but max entry id=%lu "
			"(counter behind)\n",
			hdr.srh_next_id, (unsigned long)max_id);
		errors++;
	}

	/* Duplicate id check. */
	for (uint32_t i = 0; i < hdr.srh_count; i++) {
		for (uint32_t j = i + 1; j < hdr.srh_count; j++) {
			if (entries[i].sre_id == entries[j].sre_id) {
				fprintf(stderr,
					"ERROR: duplicate id %lu at "
					"entries %u and %u\n",
					(unsigned long)entries[i].sre_id, i, j);
				errors++;
			}
		}
	}

	/* UUID non-zero check. */
	uuid_t zero_uuid;

	uuid_clear(zero_uuid);
	for (uint32_t i = 0; i < hdr.srh_count; i++) {
		if (uuid_compare(entries[i].sre_uuid, zero_uuid) == 0) {
			fprintf(stderr,
				"ERROR: entry %u (id=%lu) has zero UUID\n", i,
				(unsigned long)entries[i].sre_id);
			errors++;
		}
	}

	free(entries);

	if (errors == 0)
		printf("Registry check: OK (%u entries)\n", hdr.srh_count);
	else
		fprintf(stderr, "Registry check: %d error(s)\n", errors);

	return errors > 0 ? 1 : 0;
}

static int cmd_repair_counter(const char *state_dir)
{
	struct sb_registry_header hdr;
	struct sb_registry_entry *entries = NULL;
	int ret = read_registry(state_dir, &hdr, &entries);

	if (ret)
		return ret;

	/* Scan sb_<id>/ directories to find max id. */
	DIR *dir = opendir(state_dir);

	if (!dir) {
		fprintf(stderr, "Cannot open %s: %s\n", state_dir,
			strerror(errno));
		free(entries);
		return -errno;
	}

	uint64_t max_id = 0;
	struct dirent *de;

	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "sb_", 3) != 0)
			continue;
		char *endp;
		unsigned long id = strtoul(de->d_name + 3, &endp, 10);

		if (*endp != '\0')
			continue;
		if (id > max_id)
			max_id = id;
	}
	closedir(dir);

	/* Also check entries in the registry. */
	for (uint32_t i = 0; i < hdr.srh_count; i++) {
		if (entries[i].sre_id > max_id)
			max_id = entries[i].sre_id;
	}

	uint32_t new_next = (max_id < SB_REGISTRY_FIRST_ID) ?
				    SB_REGISTRY_FIRST_ID :
				    (uint32_t)(max_id + 1);

	if (new_next == hdr.srh_next_id) {
		printf("Counter already correct: next_id=%u\n", new_next);
		free(entries);
		return 0;
	}

	printf("Repairing counter: %u --> %u (max id seen: %lu)\n",
	       hdr.srh_next_id, new_next, (unsigned long)max_id);

	/* Rewrite the registry with the fixed counter. */
	char path[PATH_MAX];
	char tmp[PATH_MAX];

	if (snprintf(path, sizeof(path), "%s/%s", state_dir,
		     SB_REGISTRY_FILE) >= (int)sizeof(path)) {
		free(entries);
		return -ENAMETOOLONG;
	}
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
		free(entries);
		return -ENAMETOOLONG;
	}

	hdr.srh_next_id = new_next;

	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0) {
		fprintf(stderr, "Cannot create %s: %s\n", tmp, strerror(errno));
		free(entries);
		return -errno;
	}

	ssize_t n = write(fd, &hdr, sizeof(hdr));

	if (n != (ssize_t)sizeof(hdr)) {
		close(fd);
		unlink(tmp);
		free(entries);
		return -EIO;
	}

	if (hdr.srh_count > 0 && entries) {
		size_t esz = hdr.srh_count * sizeof(*entries);

		n = write(fd, entries, esz);
		if (n != (ssize_t)esz) {
			close(fd);
			unlink(tmp);
			free(entries);
			return -EIO;
		}
	}

	fdatasync(fd);
	close(fd);

	if (rename(tmp, path)) {
		fprintf(stderr, "rename failed: %s\n", strerror(errno));
		unlink(tmp);
		free(entries);
		return -errno;
	}

	free(entries);
	printf("Counter repaired successfully.\n");
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <command> <state_dir>\n", prog);
	fprintf(stderr, "Commands:\n");
	fprintf(stderr, "  dump             Print registry contents\n");
	fprintf(stderr, "  check            Validate registry integrity\n");
	fprintf(stderr, "  repair-counter   Fix the sb_id counter\n");
}

int main(int argc, char *argv[])
{
	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	const char *cmd = argv[1];
	const char *state_dir = argv[2];

	if (strcmp(cmd, "dump") == 0)
		return cmd_dump(state_dir);
	if (strcmp(cmd, "check") == 0)
		return cmd_check(state_dir);
	if (strcmp(cmd, "repair-counter") == 0)
		return cmd_repair_counter(state_dir);

	fprintf(stderr, "Unknown command: %s\n", cmd);
	usage(argv[0]);
	return 1;
}
