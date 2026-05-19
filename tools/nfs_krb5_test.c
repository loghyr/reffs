/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * nfs_krb5_test -- standalone Kerberos 5 NFS security tester.
 *
 * Connects to an NFSv4 server using RPCSEC_GSS (krb5, krb5i, or
 * krb5p), establishes a session, writes a test file, reads it back
 * with CRC verification, and cleans up.
 *
 * No kernel NFS client involvement -- pure userspace.  Can be
 * pointed at any NFSv4 server for security verification.
 *
 * Prerequisites:
 *   - Valid TGT (run kinit first)
 *   - Server has a keytab for nfs/<hostname>
 *
 * Usage:
 *   nfs_krb5_test --server <host> [--sec krb5|krb5i|krb5p]
 *                 [--file <name>] [--setowner <user@domain>]
 *
 * This is a thin CLI wrapper; the test body lives in
 * krb5_client_core.c (krb5_client_once), shared with
 * nfs_krb5_multiclient.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ec_client.h"

#include "krb5_client_core.h"

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

int main(int argc, char *argv[])
{
	struct krb5_client_args a;

	memset(&a, 0, sizeof(a));
	a.sec = EC_SEC_KRB5;

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
			a.server = optarg;
			break;
		case 'x':
			if (!strcasecmp(optarg, "krb5"))
				a.sec = EC_SEC_KRB5;
			else if (!strcasecmp(optarg, "krb5i"))
				a.sec = EC_SEC_KRB5I;
			else if (!strcasecmp(optarg, "krb5p"))
				a.sec = EC_SEC_KRB5P;
			else {
				fprintf(stderr, "Unknown sec: %s\n", optarg);
				return 2;
			}
			break;
		case 'f':
			a.file = optarg;
			break;
		case 'o':
			a.setowner = optarg;
			break;
		case 'i':
			a.owner = optarg;
			break;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 2;
		}
	}

	if (!a.server) {
		fprintf(stderr, "Error: --server is required\n\n");
		usage();
		return 2;
	}

	return krb5_client_once(&a);
}
