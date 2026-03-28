/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * nfs_krb5_test — standalone Kerberos 5 NFS security tester.
 *
 * Connects to an NFSv4 server using RPCSEC_GSS (krb5, krb5i, or
 * krb5p), establishes a session, writes a test file, reads it back
 * with CRC verification, and cleans up.
 *
 * No kernel NFS client involvement — pure userspace.  Can be
 * pointed at any NFSv4 server for security verification.
 *
 * Prerequisites:
 *   - Valid TGT (run kinit first)
 *   - Server has a keytab for nfs/<hostname>
 *
 * Usage:
 *   nfs_krb5_test --server <host> [--sec krb5|krb5i|krb5p]
 *                 [--file <name>] [--setowner <user@domain>]
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"

static void usage(void)
{
	fprintf(stderr,
		"Usage: nfs_krb5_test --server <host> [options]\n"
		"\n"
		"Options:\n"
		"  --server, -s <host>       NFS server hostname (required)\n"
		"  --sec <krb5|krb5i|krb5p>  Security level (default: krb5)\n"
		"  --file, -f <name>         File for GETATTR owner test\n"
		"  --setowner <user@domain>  Test SETATTR owner round-trip\n"
		"  --id <string>             Client owner ID\n"
		"  --help, -h                This help\n"
		"\n"
		"Tests:\n"
		"  1. GSS session establishment\n"
		"  2. WRITE test file (8 KB, two 4 KB blocks)\n"
		"  3. READ + CRC verify\n"
		"  4. GETATTR owner (if --file)\n"
		"  5. SETATTR owner round-trip (if --setowner)\n"
		"  Cleanup: REMOVE test file\n");
}

static const char *sec_name(enum ec_sec_flavor sec)
{
	switch (sec) {
	case EC_SEC_KRB5:
		return "krb5";
	case EC_SEC_KRB5I:
		return "krb5i";
	case EC_SEC_KRB5P:
		return "krb5p";
	default:
		return "sys";
	}
}

/* Simple FNV-1a 32-bit hash as a lightweight CRC. */
static uint32_t fnv1a(const uint8_t *data, uint32_t len)
{
	uint32_t hash = 0x811c9dc5;

	for (uint32_t i = 0; i < len; i++) {
		hash ^= data[i];
		hash *= 0x01000193;
	}
	return hash;
}

int main(int argc, char *argv[])
{
	const char *server = NULL;
	const char *file = NULL;
	const char *setowner = NULL;
	const char *client_id = NULL;
	enum ec_sec_flavor sec = EC_SEC_KRB5;
	int ret;

	static struct option opts[] = {
		{ "server", required_argument, NULL, 's' },
		{ "sec", required_argument, NULL, 'x' },
		{ "file", required_argument, NULL, 'f' },
		{ "setowner", required_argument, NULL, 'o' },
		{ "id", required_argument, NULL, 'i' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	int opt;

	while ((opt = getopt_long(argc, argv, "s:f:o:i:h", opts, NULL)) != -1) {
		switch (opt) {
		case 's':
			server = optarg;
			break;
		case 'x':
			if (!strcasecmp(optarg, "krb5"))
				sec = EC_SEC_KRB5;
			else if (!strcasecmp(optarg, "krb5i"))
				sec = EC_SEC_KRB5I;
			else if (!strcasecmp(optarg, "krb5p"))
				sec = EC_SEC_KRB5P;
			else {
				fprintf(stderr, "Unknown sec: %s\n", optarg);
				return 2;
			}
			break;
		case 'f':
			file = optarg;
			break;
		case 'o':
			setowner = optarg;
			break;
		case 'i':
			client_id = optarg;
			break;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 2;
		}
	}

	if (!server) {
		fprintf(stderr, "Error: --server is required\n\n");
		usage();
		return 2;
	}

	int failed = 0;
	int passed = 0;
	int test_num = 0;

	/* Unique test filename — cleaned up at exit. */
	char test_file[128];

	snprintf(test_file, sizeof(test_file), ".krb5_test_%d", (int)getpid());

	/* ---- Test 1: GSS session ---- */
	printf("TEST %d: GSS session (%s) to %s ... ", ++test_num,
	       sec_name(sec), server);
	fflush(stdout);

	struct mds_session ms;

	memset(&ms, 0, sizeof(ms));
	mds_session_set_owner(&ms, client_id);

	ret = mds_session_create_sec(&ms, server, sec);
	if (ret) {
		printf("FAIL (ret=%d)\n", ret);
		printf("  Hint: kinit, keytab, KDC reachable?\n");
		return 1;
	}
	printf("PASS\n");
	passed++;

	/* ---- Test 2: WRITE ---- */
	printf("TEST %d: WRITE %s (8 KB) ... ", ++test_num, test_file);
	fflush(stdout);

	uint32_t block_size = 4096;
	uint32_t total_size = block_size * 2;
	uint8_t *write_buf = malloc(total_size);

	if (!write_buf) {
		printf("FAIL (malloc)\n");
		mds_session_destroy(&ms);
		return 1;
	}

	/* Deterministic pattern. */
	for (uint32_t i = 0; i < total_size; i++)
		write_buf[i] = (uint8_t)(i * 31 + 17);

	uint32_t write_crc = fnv1a(write_buf, total_size);

	struct mds_file mf;

	ret = mds_file_open(&ms, test_file, &mf);
	if (ret) {
		printf("FAIL (open: %d)\n", ret);
		failed++;
		goto cleanup;
	}

	ret = mds_file_write(&ms, &mf, write_buf, block_size, 0);
	if (!ret)
		ret = mds_file_write(&ms, &mf, write_buf + block_size,
				     block_size, block_size);
	mds_file_close(&ms, &mf);

	if (ret) {
		printf("FAIL (write: %d)\n", ret);
		failed++;
		goto remove;
	}
	printf("PASS (crc=0x%08x)\n", write_crc);
	passed++;

	/* ---- Test 3: READ + CRC verify ---- */
	printf("TEST %d: READ + CRC verify ... ", ++test_num);
	fflush(stdout);

	ret = mds_file_open(&ms, test_file, &mf);
	if (ret) {
		printf("FAIL (reopen: %d)\n", ret);
		failed++;
		goto remove;
	}

	uint8_t *read_buf = calloc(1, total_size);
	uint32_t nread = 0;

	ret = mds_file_read(&ms, &mf, read_buf, total_size, 0, &nread);
	mds_file_close(&ms, &mf);

	if (ret) {
		printf("FAIL (read: %d)\n", ret);
		failed++;
	} else if (nread != total_size) {
		printf("FAIL (short: %u/%u)\n", nread, total_size);
		failed++;
	} else {
		uint32_t read_crc = fnv1a(read_buf, nread);

		if (read_crc != write_crc) {
			printf("FAIL (crc: wrote=0x%08x read=0x%08x)\n",
			       write_crc, read_crc);
			for (uint32_t i = 0; i < total_size; i++) {
				if (write_buf[i] != read_buf[i]) {
					printf("  first diff at %u: "
					       "0x%02x vs 0x%02x\n",
					       i, write_buf[i], read_buf[i]);
					break;
				}
			}
			failed++;
		} else {
			printf("PASS (crc=0x%08x)\n", read_crc);
			passed++;
		}
	}
	free(read_buf);

	/* ---- Optional: GETATTR owner ---- */
	if (file) {
		printf("TEST %d: GETATTR owner on %s ... ", ++test_num, file);
		fflush(stdout);

		ret = mds_file_open(&ms, file, &mf);
		if (ret) {
			printf("FAIL (open: %d)\n", ret);
			failed++;
		} else {
			char owner[256] = { 0 };
			char group[256] = { 0 };

			ret = mds_file_getattr(&ms, &mf, owner, sizeof(owner),
					       group, sizeof(group));
			if (ret) {
				printf("FAIL (getattr: %d)\n", ret);
				failed++;
			} else {
				printf("PASS (owner=%s group=%s)\n", owner,
				       group);
				passed++;
			}

			/* ---- Optional: SETATTR owner ---- */
			if (setowner && !ret) {
				printf("TEST %d: SETATTR owner → %s ... ",
				       ++test_num, setowner);
				fflush(stdout);

				ret = mds_file_setattr_owner(&ms, &mf, setowner,
							     NULL);
				if (!ret) {
					char o2[256] = { 0 };

					ret = mds_file_getattr(&ms, &mf, o2,
							       sizeof(o2), NULL,
							       0);
					if (!ret && !strcmp(o2, setowner)) {
						printf("PASS\n");
						passed++;
					} else {
						printf("FAIL (got %s)\n", o2);
						failed++;
					}
				} else {
					printf("FAIL (%d)\n", ret);
					failed++;
				}
			}
			mds_file_close(&ms, &mf);
		}
	}

remove:
	/* ---- Cleanup: remove test file ---- */
	ret = mds_file_remove(&ms, test_file);
	if (ret)
		printf("WARN: cleanup remove failed (%d)\n", ret);

cleanup:
	free(write_buf);
	mds_session_destroy(&ms);

	printf("\n%d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
