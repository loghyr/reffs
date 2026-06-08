/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * krb5_client_core -- the single-client krb5 NFS test body, factored
 * out of nfs_krb5_test so nfs_krb5_multiclient can fork N workers
 * over the same code.  See krb5_client_core.h.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"

#include "krb5_client_core.h"

const char *krb5_sec_name(enum ec_sec_flavor sec)
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

int krb5_client_once(const struct krb5_client_args *a)
{
	uint32_t block_size = a->block_size ? a->block_size : 4096;
	uint32_t nblocks = a->nblocks ? a->nblocks : 2;
	uint32_t total_size = block_size * nblocks;
	int ret;

	int failed = 0;
	int passed = 0;
	int test_num = 0;

	/* Unique test filename -- cleaned up at exit. */
	char test_file[128];

	snprintf(test_file, sizeof(test_file), ".krb5_test_%d", (int)getpid());

	/* ---- Test 1: GSS session ---- */
	printf("TEST %d: GSS session (%s) to %s ... ", ++test_num,
	       krb5_sec_name(a->sec), a->server);
	fflush(stdout);

	struct mds_session ms;

	memset(&ms, 0, sizeof(ms));
	mds_session_set_owner(&ms, a->owner);

	ret = mds_session_create_sec(&ms, a->server, a->sec);
	if (ret) {
		printf("FAIL (ret=%d)\n", ret);
		printf("  Hint: kinit, keytab, KDC reachable?\n");
		return 1;
	}
	printf("PASS\n");
	passed++;

	/* ---- Test 2: WRITE ---- */
	printf("TEST %d: WRITE %s (%u KB) ... ", ++test_num, test_file,
	       total_size / 1024);
	fflush(stdout);

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

	ret = mds_file_open(&ms, test_file, &mf, NULL);
	if (ret) {
		printf("FAIL (open: %d)\n", ret);
		failed++;
		goto cleanup;
	}

	for (uint32_t b = 0; b < nblocks && !ret; b++)
		ret = mds_file_write(&ms, &mf, write_buf + b * block_size,
				     block_size, (uint64_t)b * block_size);
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

	ret = mds_file_open(&ms, test_file, &mf, NULL);
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

	/* ---- Owner of the file this run created ---- */
	if (a->getattr_self) {
		printf("TEST %d: GETATTR owner of %s ... ", ++test_num,
		       test_file);
		fflush(stdout);

		ret = mds_file_open(&ms, test_file, &mf, NULL);
		if (ret) {
			printf("FAIL (open: %d)\n", ret);
			failed++;
		} else {
			char owner[256] = { 0 };

			ret = mds_file_getattr(&ms, &mf, owner, sizeof(owner),
					       NULL, 0);
			mds_file_close(&ms, &mf);
			if (ret) {
				printf("FAIL (getattr: %d)\n", ret);
				failed++;
			} else if (owner[0] == '\0') {
				printf("FAIL (empty owner)\n");
				failed++;
			} else if (a->expect_owner &&
				   strcmp(owner, a->expect_owner) != 0) {
				printf("FAIL (owner=%s expected=%s)\n", owner,
				       a->expect_owner);
				failed++;
			} else {
				printf("PASS (owner=%s)\n", owner);
				passed++;
			}
		}
	}

	/* ---- Optional: GETATTR owner ---- */
	if (a->file) {
		printf("TEST %d: GETATTR owner on %s ... ", ++test_num,
		       a->file);
		fflush(stdout);

		ret = mds_file_open(&ms, a->file, &mf, NULL);
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
			if (a->setowner && !ret) {
				printf("TEST %d: SETATTR owner -> %s ... ",
				       ++test_num, a->setowner);
				fflush(stdout);

				ret = mds_file_setattr_owner(&ms, &mf,
							     a->setowner, NULL);
				if (!ret) {
					char o2[256] = { 0 };

					ret = mds_file_getattr(&ms, &mf, o2,
							       sizeof(o2), NULL,
							       0);
					if (!ret && !strcmp(o2, a->setowner)) {
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
