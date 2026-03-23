/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * EC demo client — command-line tool for erasure-coded pNFS I/O.
 *
 * Usage:
 *   ec_demo write  --mds HOST --file NAME --input FILE  [--k K] [--m M]
 *   ec_demo read   --mds HOST --file NAME --output FILE [--k K] [--m M]
 *   ec_demo verify --mds HOST --file NAME --input FILE  [--k K] [--m M]
 *
 * Connects to the MDS via NFSv4.2, gets a Flex Files layout, resolves
 * data servers via GETDEVICEINFO, then does RS-encoded I/O directly
 * to the data servers via NFSv3.
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"

/* ------------------------------------------------------------------ */
/* File I/O helpers                                                    */
/* ------------------------------------------------------------------ */

static uint8_t *read_local_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");

	if (!f) {
		fprintf(stderr, "ec_demo: cannot open %s: %s\n", path,
			strerror(errno));
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	long len = ftell(f);

	if (len < 0) {
		fclose(f);
		return NULL;
	}
	fseek(f, 0, SEEK_SET);

	uint8_t *buf = malloc((size_t)len);

	if (!buf) {
		fclose(f);
		return NULL;
	}

	size_t nread = fread(buf, 1, (size_t)len, f);

	fclose(f);

	if (nread != (size_t)len) {
		free(buf);
		return NULL;
	}

	*out_len = (size_t)len;
	return buf;
}

static int write_local_file(const char *path, const uint8_t *data, size_t len)
{
	FILE *f = fopen(path, "wb");

	if (!f) {
		fprintf(stderr, "ec_demo: cannot create %s: %s\n", path,
			strerror(errno));
		return -1;
	}

	size_t written = fwrite(data, 1, len, f);

	fclose(f);
	return (written == len) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int cmd_write(const char *mds_host, const char *nfs_file,
		     const char *local_file, int k, int m)
{
	struct mds_session ms;
	size_t data_len;
	int ret;

	uint8_t *data = read_local_file(local_file, &data_len);

	if (!data)
		return 1;

	fprintf(stderr, "ec_demo: connecting to MDS %s\n", mds_host);
	ret = mds_session_create(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		free(data);
		return 1;
	}

	fprintf(stderr, "ec_demo: writing %zu bytes to %s (RS %d+%d)\n",
		data_len, nfs_file, k, m);
	ret = ec_write(&ms, nfs_file, data, data_len, k, m);
	if (ret)
		fprintf(stderr, "ec_demo: write failed: %d\n", ret);
	else
		fprintf(stderr, "ec_demo: write OK\n");

	mds_session_destroy(&ms);
	free(data);
	return ret ? 1 : 0;
}

static int cmd_read(const char *mds_host, const char *nfs_file,
		    const char *local_file, int k, int m, size_t expected_len)
{
	struct mds_session ms;
	int ret;

	fprintf(stderr, "ec_demo: connecting to MDS %s\n", mds_host);
	ret = mds_session_create(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		return 1;
	}

	size_t buf_len = expected_len ? expected_len : 16 * 1024 * 1024;
	uint8_t *buf = calloc(1, buf_len);

	if (!buf) {
		mds_session_destroy(&ms);
		return 1;
	}

	size_t out_len = 0;

	fprintf(stderr, "ec_demo: reading %s (RS %d+%d)\n", nfs_file, k, m);
	ret = ec_read(&ms, nfs_file, buf, buf_len, &out_len, k, m);
	if (ret) {
		fprintf(stderr, "ec_demo: read failed: %d\n", ret);
	} else {
		fprintf(stderr, "ec_demo: read %zu bytes\n", out_len);
		if (write_local_file(local_file, buf, out_len))
			ret = -1;
		else
			fprintf(stderr, "ec_demo: wrote %s\n", local_file);
	}

	free(buf);
	mds_session_destroy(&ms);
	return ret ? 1 : 0;
}

static int cmd_verify(const char *mds_host, const char *nfs_file,
		      const char *local_file, int k, int m)
{
	struct mds_session ms;
	size_t orig_len;
	int ret;

	uint8_t *orig = read_local_file(local_file, &orig_len);

	if (!orig)
		return 1;

	fprintf(stderr, "ec_demo: connecting to MDS %s\n", mds_host);
	ret = mds_session_create(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		free(orig);
		return 1;
	}

	uint8_t *buf = calloc(1, orig_len);

	if (!buf) {
		mds_session_destroy(&ms);
		free(orig);
		return 1;
	}

	size_t out_len = 0;

	fprintf(stderr, "ec_demo: verifying %s against %s (RS %d+%d)\n",
		nfs_file, local_file, k, m);
	ret = ec_read(&ms, nfs_file, buf, orig_len, &out_len, k, m);
	if (ret) {
		fprintf(stderr, "ec_demo: read failed: %d\n", ret);
	} else if (out_len < orig_len) {
		fprintf(stderr,
			"ec_demo: MISMATCH: read %zu bytes, expected %zu\n",
			out_len, orig_len);
		ret = -1;
	} else if (memcmp(orig, buf, orig_len) != 0) {
		/* Find first differing byte for diagnostic. */
		for (size_t i = 0; i < orig_len; i++) {
			if (orig[i] != buf[i]) {
				fprintf(stderr,
					"ec_demo: MISMATCH at offset %zu: "
					"expected 0x%02x, got 0x%02x\n",
					i, orig[i], buf[i]);
				break;
			}
		}
		ret = -1;
	} else {
		fprintf(stderr, "ec_demo: VERIFY OK (%zu bytes match)\n",
			orig_len);
	}

	free(buf);
	free(orig);
	mds_session_destroy(&ms);
	return ret ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Plain (non-EC) commands                                             */
/* ------------------------------------------------------------------ */

static int cmd_put(const char *mds_host, const char *nfs_file,
		   const char *local_file)
{
	struct mds_session ms;
	size_t data_len;
	int ret;

	uint8_t *data = read_local_file(local_file, &data_len);

	if (!data)
		return 1;

	fprintf(stderr, "ec_demo: connecting to MDS %s\n", mds_host);
	ret = mds_session_create(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		free(data);
		return 1;
	}

	fprintf(stderr, "ec_demo: put %zu bytes to %s\n", data_len, nfs_file);
	ret = plain_write(&ms, nfs_file, data, data_len);
	if (ret)
		fprintf(stderr, "ec_demo: put failed: %d\n", ret);
	else
		fprintf(stderr, "ec_demo: put OK\n");

	mds_session_destroy(&ms);
	free(data);
	return ret ? 1 : 0;
}

static int cmd_get(const char *mds_host, const char *nfs_file,
		   const char *local_file, size_t expected_len)
{
	struct mds_session ms;
	int ret;

	fprintf(stderr, "ec_demo: connecting to MDS %s\n", mds_host);
	ret = mds_session_create(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		return 1;
	}

	size_t buf_len = expected_len ? expected_len : 16 * 1024 * 1024;
	uint8_t *buf = calloc(1, buf_len);

	if (!buf) {
		mds_session_destroy(&ms);
		return 1;
	}

	size_t out_len = 0;

	fprintf(stderr, "ec_demo: get %s\n", nfs_file);
	ret = plain_read(&ms, nfs_file, buf, buf_len, &out_len);
	if (ret) {
		fprintf(stderr, "ec_demo: get failed: %d\n", ret);
	} else {
		fprintf(stderr, "ec_demo: got %zu bytes\n", out_len);
		if (write_local_file(local_file, buf, out_len))
			ret = -1;
		else
			fprintf(stderr, "ec_demo: wrote %s\n", local_file);
	}

	free(buf);
	mds_session_destroy(&ms);
	return ret ? 1 : 0;
}

static int cmd_check(const char *mds_host, const char *nfs_file,
		     const char *local_file)
{
	struct mds_session ms;
	size_t orig_len;
	int ret;

	uint8_t *orig = read_local_file(local_file, &orig_len);

	if (!orig)
		return 1;

	fprintf(stderr, "ec_demo: connecting to MDS %s\n", mds_host);
	ret = mds_session_create(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		free(orig);
		return 1;
	}

	uint8_t *buf = calloc(1, orig_len);

	if (!buf) {
		mds_session_destroy(&ms);
		free(orig);
		return 1;
	}

	size_t out_len = 0;

	fprintf(stderr, "ec_demo: check %s against %s\n", nfs_file, local_file);
	ret = plain_read(&ms, nfs_file, buf, orig_len, &out_len);
	if (ret) {
		fprintf(stderr, "ec_demo: read failed: %d\n", ret);
	} else if (out_len < orig_len) {
		fprintf(stderr,
			"ec_demo: MISMATCH: read %zu bytes, expected %zu\n",
			out_len, orig_len);
		ret = -1;
	} else if (memcmp(orig, buf, orig_len) != 0) {
		for (size_t i = 0; i < orig_len; i++) {
			if (orig[i] != buf[i]) {
				fprintf(stderr,
					"ec_demo: MISMATCH at offset %zu: "
					"expected 0x%02x, got 0x%02x\n",
					i, orig[i], buf[i]);
				break;
			}
		}
		ret = -1;
	} else {
		fprintf(stderr, "ec_demo: CHECK OK (%zu bytes match)\n",
			orig_len);
	}

	free(buf);
	free(orig);
	mds_session_destroy(&ms);
	return ret ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Usage and main                                                      */
/* ------------------------------------------------------------------ */

static void usage(void)
{
	fprintf(stderr,
		"Usage:\n"
		"  Plain (no erasure coding):\n"
		"  ec_demo put    --mds HOST --file NAME --input FILE\n"
		"  ec_demo get    --mds HOST --file NAME --output FILE"
		" [--size N]\n"
		"  ec_demo check  --mds HOST --file NAME --input FILE\n"
		"\n"
		"  Erasure-coded:\n"
		"  ec_demo write  --mds HOST --file NAME --input FILE"
		" [--k K] [--m M]\n"
		"  ec_demo read   --mds HOST --file NAME --output FILE"
		" [--k K] [--m M] [--size N]\n"
		"  ec_demo verify --mds HOST --file NAME --input FILE"
		" [--k K] [--m M]\n"
		"\n"
		"Options:\n"
		"  --mds HOST     MDS hostname or IP\n"
		"  --file NAME    NFS filename (in root of MDS export)\n"
		"  --input FILE   Local file to write/verify\n"
		"  --output FILE  Local file to write read data to\n"
		"  --k K          Data shards for EC (default: 4)\n"
		"  --m M          Parity shards for EC (default: 2)\n"
		"  --size N       Expected read size in bytes"
		" (default: 16M)\n");
}

static struct option long_options[] = {
	{ "mds", required_argument, NULL, 'h' },
	{ "file", required_argument, NULL, 'f' },
	{ "input", required_argument, NULL, 'i' },
	{ "output", required_argument, NULL, 'o' },
	{ "k", required_argument, NULL, 'k' },
	{ "m", required_argument, NULL, 'm' },
	{ "size", required_argument, NULL, 's' },
	{ "help", no_argument, NULL, '?' },
	{ NULL, 0, NULL, 0 },
};

int main(int argc, char *argv[])
{
	const char *mds_host = NULL;
	const char *nfs_file = NULL;
	const char *local_input = NULL;
	const char *local_output = NULL;
	int k = 4, m = 2;
	size_t read_size = 0;
	int opt;

	if (argc < 2) {
		usage();
		return 1;
	}

	const char *cmd = argv[1];

	/* Shift argv past the subcommand for getopt. */
	argc--;
	argv++;
	optind = 1;

	while ((opt = getopt_long(argc, argv, "h:f:i:o:k:m:s:?", long_options,
				  NULL)) != -1) {
		switch (opt) {
		case 'h':
			mds_host = optarg;
			break;
		case 'f':
			nfs_file = optarg;
			break;
		case 'i':
			local_input = optarg;
			break;
		case 'o':
			local_output = optarg;
			break;
		case 'k':
			k = atoi(optarg);
			break;
		case 'm':
			m = atoi(optarg);
			break;
		case 's':
			read_size = (size_t)atol(optarg);
			break;
		default:
			usage();
			return 1;
		}
	}

	if (!mds_host || !nfs_file) {
		fprintf(stderr, "ec_demo: --mds and --file are required\n");
		usage();
		return 1;
	}

	/* Plain (non-EC) commands. */
	if (strcmp(cmd, "put") == 0) {
		if (!local_input) {
			fprintf(stderr, "ec_demo: put requires --input\n");
			return 1;
		}
		return cmd_put(mds_host, nfs_file, local_input);
	}

	if (strcmp(cmd, "get") == 0) {
		if (!local_output) {
			fprintf(stderr, "ec_demo: get requires --output\n");
			return 1;
		}
		return cmd_get(mds_host, nfs_file, local_output, read_size);
	}

	if (strcmp(cmd, "check") == 0) {
		if (!local_input) {
			fprintf(stderr, "ec_demo: check requires --input\n");
			return 1;
		}
		return cmd_check(mds_host, nfs_file, local_input);
	}

	/* EC commands need valid k/m. */
	if (k < 1 || m < 1 || k + m > 255) {
		fprintf(stderr, "ec_demo: invalid k=%d m=%d\n", k, m);
		return 1;
	}

	if (strcmp(cmd, "write") == 0) {
		if (!local_input) {
			fprintf(stderr, "ec_demo: write requires --input\n");
			return 1;
		}
		return cmd_write(mds_host, nfs_file, local_input, k, m);
	}

	if (strcmp(cmd, "read") == 0) {
		if (!local_output) {
			fprintf(stderr, "ec_demo: read requires --output\n");
			return 1;
		}
		return cmd_read(mds_host, nfs_file, local_output, k, m,
				read_size);
	}

	if (strcmp(cmd, "verify") == 0) {
		if (!local_input) {
			fprintf(stderr, "ec_demo: verify requires --input\n");
			return 1;
		}
		return cmd_verify(mds_host, nfs_file, local_input, k, m);
	}

	fprintf(stderr, "ec_demo: unknown command '%s'\n", cmd);
	usage();
	return 1;
}
