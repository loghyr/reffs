/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * er_demo -- demonstrate EXCHANGE_RANGE atomic file update.
 *
 * Atomic update workflow (er_demo update):
 *   1. Open the target file.
 *   2. Create and open a temporary clone file.
 *   3. Copy the entire target into the clone (CLONE op).
 *   4. Write the new content into the clone.
 *   5. Atomically swap the full byte range of target and clone
 *      (EXCHANGE_RANGE).  The target now holds the new content;
 *      the clone holds the old content.
 *   6. Close both files.
 *   7. Delete the clone.
 *
 * EXCHANGE_RANGE is atomic on the server -- readers of the target
 * see either the old content or the new content, never a partial mix.
 * The op is also self-inverse: running update twice with the same data
 * returns the file to its original state.
 *
 * Subcommands:
 *   put    --mds HOST --file NAME --data TEXT   create/overwrite file
 *   get    --mds HOST --file NAME               read and print file
 *   update --mds HOST --file NAME --data TEXT   atomic update
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ec_client.h"

/* clone suffix appended to the target filename during update */
#define ER_CLONE_SUFFIX ".__er_clone__"
#define ER_CLONE_SUFFIX_LEN 13

/* maximum bytes read by the get subcommand */
#define ER_READ_MAX (1024u * 1024u)

static const char *g_client_id = "er_demo";

/* ------------------------------------------------------------------ */
/* Helper: open a session                                               */
/* ------------------------------------------------------------------ */

static int session_open(struct mds_session *ms, const char *host)
{
	memset(ms, 0, sizeof(*ms));
	mds_session_set_owner(ms, g_client_id);

	int ret = mds_session_create(ms, host);

	if (ret) {
		fprintf(stderr, "er_demo: cannot connect to MDS %s: %s\n", host,
			strerror(-ret));
		return ret;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* put -- create or overwrite a file with literal text                  */
/* ------------------------------------------------------------------ */

static int cmd_put(const char *host, const char *name, const char *data)
{
	struct mds_session ms;
	struct mds_file mf;
	int ret;

	ret = session_open(&ms, host);
	if (ret)
		return ret;

	ret = mds_file_open(&ms, name, &mf);
	if (ret) {
		fprintf(stderr, "er_demo put: open %s failed: %s\n", name,
			strerror(-ret));
		mds_session_destroy(&ms);
		return ret;
	}

	ret = mds_file_write(&ms, &mf, (const uint8_t *)data,
			     (uint32_t)strlen(data), 0);
	if (ret)
		fprintf(stderr, "er_demo put: write %s failed: %s\n", name,
			strerror(-ret));

	mds_file_close(&ms, &mf);
	mds_session_destroy(&ms);
	return ret;
}

/* ------------------------------------------------------------------ */
/* get -- read and print a file                                          */
/* ------------------------------------------------------------------ */

static int cmd_get(const char *host, const char *name)
{
	struct mds_session ms;
	struct mds_file mf;
	uint8_t *buf;
	uint32_t nread = 0;
	int ret;

	ret = session_open(&ms, host);
	if (ret)
		return ret;

	ret = mds_file_open(&ms, name, &mf);
	if (ret) {
		fprintf(stderr, "er_demo get: open %s failed: %s\n", name,
			strerror(-ret));
		mds_session_destroy(&ms);
		return ret;
	}

	buf = malloc(ER_READ_MAX + 1);
	if (!buf) {
		mds_file_close(&ms, &mf);
		mds_session_destroy(&ms);
		return -ENOMEM;
	}

	ret = mds_file_read(&ms, &mf, buf, ER_READ_MAX, 0, &nread);
	if (ret) {
		fprintf(stderr, "er_demo get: read %s failed: %s\n", name,
			strerror(-ret));
	} else {
		buf[nread] = '\0';
		printf("%s", (char *)buf);
		/* ensure trailing newline when content lacks one */
		if (nread > 0 && buf[nread - 1] != '\n')
			printf("\n");
	}

	free(buf);
	mds_file_close(&ms, &mf);
	mds_session_destroy(&ms);
	return ret;
}

/* ------------------------------------------------------------------ */
/* update -- atomic file update via CLONE + EXCHANGE_RANGE              */
/* ------------------------------------------------------------------ */

static int cmd_update(const char *host, const char *name, const char *data)
{
	struct mds_session ms;
	struct mds_file mf_target;
	struct mds_file mf_clone;
	int ret;

	/* Build clone filename: "<name>.__er_clone__" */
	size_t namelen = strlen(name);
	char *clone_name = malloc(namelen + ER_CLONE_SUFFIX_LEN + 1);

	if (!clone_name)
		return -ENOMEM;
	memcpy(clone_name, name, namelen);
	memcpy(clone_name + namelen, ER_CLONE_SUFFIX, ER_CLONE_SUFFIX_LEN + 1);

	ret = session_open(&ms, host);
	if (ret) {
		free(clone_name);
		return ret;
	}

	/* Step 1: Open the target file. */
	ret = mds_file_open(&ms, name, &mf_target);
	if (ret) {
		fprintf(stderr, "er_demo update: open %s failed: %s\n", name,
			strerror(-ret));
		goto out_session;
	}

	/* Step 2: Create and open the clone file. */
	ret = mds_file_open(&ms, clone_name, &mf_clone);
	if (ret) {
		fprintf(stderr, "er_demo update: open clone %s failed: %s\n",
			clone_name, strerror(-ret));
		goto out_close_target;
	}

	/*
	 * Step 3: Clone the entire target into the clone file.
	 *
	 * count=0 means "to end of file" per RFC 7862 S15.13.  After CLONE,
	 * the clone file is an identical copy of the target.
	 */
	ret = mds_file_clone(&ms, &mf_target, &mf_clone, 0, 0, 0);
	if (ret) {
		fprintf(stderr, "er_demo update: CLONE %s -> %s failed: %s\n",
			name, clone_name, strerror(-ret));
		goto out_close_both;
	}

	/* Step 4: Write the new content into the clone. */
	ret = mds_file_write(&ms, &mf_clone, (const uint8_t *)data,
			     (uint32_t)strlen(data), 0);
	if (ret) {
		fprintf(stderr,
			"er_demo update: write to clone %s failed: %s\n",
			clone_name, strerror(-ret));
		goto out_close_both;
	}

	/*
	 * Step 5: Atomically swap the full content of target and clone.
	 *
	 * EXCHANGE_RANGE with count=0 extends to the end of the larger file
	 * (draft-haynes-nfsv4-swap S3).  After a successful swap:
	 *   target   holds the new content (written to clone in step 4)
	 *   clone    holds the old content (was in target before the swap)
	 *
	 * The swap is self-inverse: running update twice with the same data
	 * returns to the original state.
	 *
	 * The compound is: SEQUENCE + PUTFH(clone) + SAVEFH +
	 *   PUTFH(target) + EXCHANGE_RANGE, so clone is SAVED_FH (src)
	 *   and target is CURRENT_FH (dst).
	 */
	ret = mds_file_exchange_range(&ms, &mf_clone, &mf_target, 0, 0, 0);
	if (ret) {
		fprintf(stderr,
			"er_demo update: EXCHANGE_RANGE %s <-> %s failed: %s\n",
			clone_name, name, strerror(-ret));
	}

	/* Step 6: Close both files. */
out_close_both:
	mds_file_close(&ms, &mf_clone);
out_close_target:
	mds_file_close(&ms, &mf_target);

	/*
	 * Step 7: Delete the clone.  On success it holds the old content;
	 * on failure it may hold the new content written in step 4.  Remove
	 * in either case to avoid leaving the temp file behind.
	 */
	{
		int rm_ret = mds_file_remove(&ms, clone_name);

		if (rm_ret && ret == 0) {
			fprintf(stderr,
				"er_demo update: remove clone %s failed: %s\n",
				clone_name, strerror(-rm_ret));
			ret = rm_ret;
		}
	}

out_session:
	mds_session_destroy(&ms);
	free(clone_name);
	return ret;
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s put    --mds HOST --file NAME --data TEXT\n"
		"  %s get    --mds HOST --file NAME\n"
		"  %s update --mds HOST --file NAME --data TEXT\n"
		"\n"
		"Options:\n"
		"  --mds HOST    MDS hostname or IP address\n"
		"  --file NAME   filename on the server (root directory)\n"
		"  --data TEXT   content to write (put/update)\n"
		"  --id OWNER    client owner string (default: er_demo)\n",
		argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
	const char *host = NULL;
	const char *name = NULL;
	const char *data = NULL;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	const char *subcmd = argv[1];

	static const struct option long_opts[] = {
		{ "mds", required_argument, NULL, 'm' },
		{ "file", required_argument, NULL, 'f' },
		{ "data", required_argument, NULL, 'd' },
		{ "id", required_argument, NULL, 'i' },
		{ NULL, 0, NULL, 0 },
	};

	/* skip argv[0] and argv[1] (subcommand) for getopt */
	optind = 2;

	int opt;

	while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'm':
			host = optarg;
			break;
		case 'f':
			name = optarg;
			break;
		case 'd':
			data = optarg;
			break;
		case 'i':
			g_client_id = optarg;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!host) {
		fprintf(stderr, "er_demo: --mds is required\n");
		usage(argv[0]);
		return 1;
	}
	if (!name) {
		fprintf(stderr, "er_demo: --file is required\n");
		usage(argv[0]);
		return 1;
	}

	if (strcmp(subcmd, "put") == 0) {
		if (!data) {
			fprintf(stderr, "er_demo put: --data is required\n");
			return 1;
		}
		return cmd_put(host, name, data) ? 1 : 0;

	} else if (strcmp(subcmd, "get") == 0) {
		return cmd_get(host, name) ? 1 : 0;

	} else if (strcmp(subcmd, "update") == 0) {
		if (!data) {
			fprintf(stderr, "er_demo update: --data is required\n");
			return 1;
		}
		return cmd_update(host, name, data) ? 1 : 0;

	} else {
		fprintf(stderr, "er_demo: unknown subcommand '%s'\n", subcmd);
		usage(argv[0]);
		return 1;
	}
}
