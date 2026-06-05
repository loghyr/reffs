/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * EC demo client -- command-line tool for erasure-coded pNFS I/O.
 *
 * Usage:
 *   ec_demo write        --mds HOST --file NAME --input FILE  [--k K] [--m M]
 *   ec_demo read         --mds HOST --file NAME --output FILE [--k K] [--m M]
 *   ec_demo verify       --mds HOST --file NAME --input FILE  [--k K] [--m M]
 *   ec_demo write_verify --mds HOST --file NAME --input FILE  [--k K] [--m M]
 *
 * Connects to the MDS via NFSv4.2, gets a Flex Files layout, resolves
 * data servers via GETDEVICEINFO, then does RS-encoded I/O directly
 * to the data servers via NFSv3.
 *
 * Logging posture: this is a CLI demo, not a daemon, so user-facing
 * progress and error lines go through fprintf(stderr, ...) -- no
 * daemon-style timestamps or trace sink.  reffs's LOG/TRACE macros
 * are server-oriented (LOG to stderr with timestamp + line number,
 * TRACE to a separate trace file).  We deliberately do not use them
 * here; ec_demo's stderr is the user's tty.
 *
 * NOT_NOW_BROWN_COW: if benchmark introspection ever wants per-op
 * timing inside ec_demo, add a -v / -q flag and route the new lines
 * through TRACE() with a dedicated category, leaving the existing
 * fprintf progress messages intact.  Plan A follow-up #5 was filed
 * against this question and explicitly closed (Tier 5 cleanup pass)
 * with "no change" -- the current posture is correct for a demo CLI.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"
#include "mojette.h"

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
/* Session helper                                                      */
/* ------------------------------------------------------------------ */

static const char *g_client_id; /* set from --id, NULL = use PID */

static enum ec_sec_flavor g_sec = EC_SEC_SYS;

/*
 * Target Kerberos service principal name override.  When NULL,
 * the krb5 library defaults to nfs/<server-fqdn>@<REALM>.  When
 * non-NULL, passed verbatim to authgss_create_default.  Set by
 * --spn for the krb5 stress reproducer (see
 * .claude/design/krb5-stress-multi-xprt.md).
 */
static const char *g_spn;

/*
 * Number of parallel MDS sessions for the `burst` subcommand.
 * Each worker opens an independent mds_session (its own
 * EXCHANGE_ID + CREATE_SESSION + GSS context), driving the
 * server's per-handshake fan-out without forking N processes.
 * Closest in-process analogue to the many-concurrent-mounts shape
 * the krb5 stress reproducer needs to drive.  Set by --nsessions.
 *
 * Naming note: this knob was originally exposed as --nconnect,
 * which overloads the kernel / pd-protod mount option of the same
 * name.  The kernel nconnect means "N TCP transports under one
 * NFSv4 session" -- a multiplexing axis.  This burst opens N
 * independent sessions, which is a different stressor (server
 * GSS_ACCEPT_SEC_CONTEXT + principal-resolution fan-out, the
 * customer reproducer's target).  Renamed to --nsessions to
 * match the wire artifact; --nconnect is kept as a deprecated
 * alias that emits a stderr warning, for one cycle.
 */
static int g_nsessions = 1;

/*
 * Kernel-style nconnect: TCP transports per mds_session.  Set by
 * --nconnect M.  When > 1, burst workers call
 * mds_session_create_sec_spn_nc with M transports per session
 * (one EXCHANGE_ID + CREATE_SESSION + M-1 BIND_CONN_TO_SESSION).
 * Total wire transports per `burst` run = nsessions x nconnect.
 *
 * Was a deprecated alias for --nsessions for one cycle (the rename
 * after the term-overload review); now reclaimed for the kernel
 * meaning that matches the Linux mount option and pd-protod's
 * sxo_nconnect.  Drives the per-transport GSS context fan-out the
 * customer load shape produces -- N sessions x M transports each
 * with its own gss_init_sec_context -> gss_accept_sec_context
 * exchange against the server's identmap path.
 */
static int g_nconnect = 1;

/*
 * Optional list of target SPNs to rotate across burst workers.
 * Parsed from --spn-list a,b,c,... into a NULL-terminated
 * array of strings.  When set, worker i uses g_spn_list[i % N]
 * as its target SPN, overriding any single --spn value.  When
 * unset, every worker uses g_spn (or the krb5 library default
 * if g_spn is also unset).
 *
 * Per-worker SPN rotation drives the server's SPN-resolution
 * fan-out: with N different SPNs, each handshake forces the
 * server to look up a fresh principal, which is the protod
 * code path the customer reproducer is designed to stress.
 */
static char **g_spn_list;
static int g_spn_list_n;
/*
 * --source-ip ADDR (long-only, opt-val 258).  Optional local IPv4
 * source address to bind MDS- and DS-side TCP sockets to before
 * connect.  When non-NULL/non-empty, the session-setup code
 * threads this through struct mds_session::ms_source_ip; both
 * libtirpc-driven clnt_create paths (bare host with no source_ip
 * set) and manual-socket paths honour the binding.  See
 * lib/include/reffs/source_bind.h.
 */
static const char *g_source_ip;

/*
 * Optional directory of pre-baked Kerberos credential caches for
 * per-worker initiator-identity rotation under `burst`.  Parsed
 * from --ccache-dir DIR at flag-time.
 *
 * Driver: the customer load shape is many concurrent mounts, each
 * with its own machine-account ccache provisioned out of band (the
 * K8s "kerberos operator" model where each pod gets a pre-baked
 * krb5.ccache from a Kubernetes secret).  Each distinct initiator
 * principal lands in the server-side identmap path independently,
 * which is the wire pattern this stress generator drives at scale.
 *
 * Worker i picks g_ccache_list[i % g_ccache_n] as its KRB5CCNAME.
 *
 * --ccache-dir implies forked-worker mode rather than the threaded
 * default: setenv(KRB5CCNAME) is process-wide on glibc and libkrb5
 * resolves the ccache name at gss_acquire_cred time, so threaded
 * workers cannot each carry a distinct ccache without serialising
 * around the env-var assignment (which would defeat the burst).
 * Forked workers each have their own envp and their own libkrb5
 * context, which also more faithfully mirrors the per-pod
 * one-process-per-identity shape on the wire.
 */
static const char *g_ccache_dir;
static char **g_ccache_list;
static int g_ccache_n;

/*
 * Scan g_ccache_dir for regular files and populate g_ccache_list.
 * Skips dotfiles and non-regular entries.  Each ccache path stored
 * is the full absolute path the child will pass to setenv.
 *
 * Returns 0 with g_ccache_n > 0 on success, -errno on failure.
 * Does not validate that each file is a parseable krb5_cc -- a bad
 * ccache surfaces as a per-worker handshake failure with the
 * symbolic auth_stat name from the central log, which is the right
 * diagnostic shape.
 */
/*
 * Natural-order comparator on the basename of two paths.  Runs of
 * digits compare numerically, runs of non-digits compare via
 * strcmp.  Yields the order a human expects: cc_2 < cc_10 < cc_11
 * rather than the strcmp order cc_10 < cc_11 < cc_2.  This matters
 * because worker i picks g_ccache_list[i % N] and pairs with
 * g_spn_list[i % M]; if the two indexes don't align by name, a
 * caller curating parallel UPN-and-SPN lists gets the pairing
 * wrong without warning.
 */
static int natcmp_basename(const void *a, const void *b)
{
	const char *pa = *(const char *const *)a;
	const char *pb = *(const char *const *)b;
	const char *sa = strrchr(pa, '/');
	const char *sb = strrchr(pb, '/');

	sa = sa ? sa + 1 : pa;
	sb = sb ? sb + 1 : pb;

	while (*sa && *sb) {
		if (isdigit((unsigned char)*sa) &&
		    isdigit((unsigned char)*sb)) {
			unsigned long va = 0, vb = 0;

			while (isdigit((unsigned char)*sa))
				va = va * 10 + (unsigned)(*sa++ - '0');
			while (isdigit((unsigned char)*sb))
				vb = vb * 10 + (unsigned)(*sb++ - '0');
			if (va != vb)
				return va < vb ? -1 : 1;
			continue;
		}
		if (*sa != *sb)
			return (unsigned char)*sa - (unsigned char)*sb;
		sa++;
		sb++;
	}
	return (unsigned char)*sa - (unsigned char)*sb;
}

static int load_ccache_dir(const char *dir)
{
	DIR *d = opendir(dir);

	if (!d) {
		fprintf(stderr, "ec_demo: cannot opendir(%s): %s\n", dir,
			strerror(errno));
		return -errno;
	}

	struct dirent *de;

	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;

		char path[PATH_MAX];
		int n = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);

		if (n < 0 || (size_t)n >= sizeof(path))
			continue;

		struct stat st;

		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;

		char **grown = realloc(g_ccache_list,
				       (g_ccache_n + 1) * sizeof(char *));
		if (!grown)
			break;
		g_ccache_list = grown;
		g_ccache_list[g_ccache_n] = strdup(path);
		if (!g_ccache_list[g_ccache_n])
			break;
		g_ccache_n++;
	}

	closedir(d);

	if (g_ccache_n == 0) {
		fprintf(stderr,
			"ec_demo: --ccache-dir %s contains no usable ccache files\n",
			dir);
		return -ENOENT;
	}

	qsort(g_ccache_list, g_ccache_n, sizeof(char *), natcmp_basename);

	return 0;
}

static int session_open(struct mds_session *ms, const char *mds_host)
{
	memset(ms, 0, sizeof(*ms));
	mds_session_set_owner(ms, g_client_id);
	/*
	 * --source-ip: bind MDS-side TCP socket to this local IPv4
	 * address.  mds_session_create*'s memset-zero pattern preserves
	 * ms_source_ip via the same save/restore as ms_owner.
	 * ec_pipeline's ds_connect call sites also read this value via
	 * ctx->ctx_ms->ms_source_ip, so the DS-side NFSv3 sockets bind
	 * the same source.
	 */
	ms->ms_source_ip = g_source_ip;

	const char *sec_name[] = { "sys", "krb5", "krb5i", "krb5p" };

	if (g_spn && g_sec != EC_SEC_SYS)
		fprintf(stderr,
			"ec_demo: connecting to MDS %s (owner %s, sec=%s, spn=%s, nconnect=%d)\n",
			mds_host, ms->ms_owner, sec_name[g_sec], g_spn,
			g_nconnect);
	else
		fprintf(stderr,
			"ec_demo: connecting to MDS %s (owner %s, sec=%s, nconnect=%d)\n",
			mds_host, ms->ms_owner, sec_name[g_sec], g_nconnect);

	if (g_sec == EC_SEC_SYS)
		return mds_session_create(ms, mds_host);
	if (g_nconnect > 1)
		return mds_session_create_sec_spn_nc(ms, mds_host, g_sec, g_spn,
						     g_nconnect);
	return mds_session_create_sec_spn(ms, mds_host, g_sec, g_spn);
}

/* ------------------------------------------------------------------ */
/* burst -- N parallel-handshake stress for the krb5 multi-mount       */
/* reproducer (see .claude/design/krb5-stress-multi-xprt.md).          */
/* ------------------------------------------------------------------ */

struct burst_worker_args {
	const char *mds_host;
	const char *spn;
	int worker_idx;
	int result; /* 0 on success, -errno on failure */
	int handshake_ms;
};

static void *burst_worker(void *vargs)
{
	struct burst_worker_args *a = vargs;
	struct mds_session ms;
	struct timespec t0, t1;

	memset(&ms, 0, sizeof(ms));
	/*
	 * Same KRB5CCNAME / same g_client_id on every worker -- this
	 * reproducer drives "one user, N parallel handshakes" per
	 * Tom's 2026-05-30 confirmation.  All N EXCHANGE_IDs carry
	 * the same clientowner; the server resolves the first one
	 * (case 1 of nfs4_client_alloc_or_find -- new clientid) and
	 * the remaining N-1 land on case 2 (same principal, same
	 * verifier -- return existing clientid).  CREATE_SESSION
	 * then mints a fresh sessionid per worker.  N concurrent
	 * GSS_ACCEPT_SEC_CONTEXT on the server, which is the
	 * principal-resolution fan-out we want to drive.
	 */
	mds_session_set_owner(&ms, g_client_id);
	ms.ms_source_ip = g_source_ip;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	int ret;

	if (g_sec == EC_SEC_SYS)
		ret = mds_session_create(&ms, a->mds_host);
	else if (g_nconnect > 1)
		ret = mds_session_create_sec_spn_nc(&ms, a->mds_host, g_sec,
						    a->spn, g_nconnect);
	else
		ret = mds_session_create_sec_spn(&ms, a->mds_host, g_sec,
						 a->spn);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	a->handshake_ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 +
				(t1.tv_nsec - t0.tv_nsec) / 1000000);

	if (ret) {
		a->result = ret;
		return NULL;
	}

	mds_session_destroy(&ms);
	a->result = 0;
	return NULL;
}

/*
 * Forked-worker burst, taken when --ccache-dir is set.
 *
 * Each child sets KRB5CCNAME to g_ccache_list[i % g_ccache_n] and
 * adjusts its g_client_id to encode the worker index, so the
 * server sees a distinct (NFSv4 clientowner, krb5 initiator
 * principal) tuple per child.  Children run the same handshake
 * sequence as burst_worker() and exit(0) on success / exit(1) on
 * failure; the parent waitpid's and tallies pass/fail by exit
 * status.  Per-worker timing is not piped back -- the aggregate
 * wall-clock is what matters for the load shape, and individual
 * failures self-identify via the central mds_compound log
 * (auth_stat=... for the seal-broken path).
 */
static int cmd_burst_forked(const char *mds_host, int nsessions)
{
	pid_t *pids = calloc(nsessions, sizeof(pid_t));

	if (!pids) {
		fprintf(stderr, "ec_demo burst: out of memory\n");
		return 1;
	}

	const char *sec_name[] = { "sys", "krb5", "krb5i", "krb5p" };

	fprintf(stderr,
		"ec_demo burst: forking %d worker%s to %s (sec=%s, ccache-dir=%s [%d ccaches]%s%s)\n",
		nsessions, nsessions == 1 ? "" : "s", mds_host, sec_name[g_sec],
		g_ccache_dir, g_ccache_n,
		g_spn_list_n > 0 ? ", spn-list[" : (g_spn ? ", spn=" : ""),
		g_spn_list_n > 0 ? "rotating]" : (g_spn ? g_spn : ""));

	struct timespec t_start, t_end;

	clock_gettime(CLOCK_MONOTONIC, &t_start);

	for (int i = 0; i < nsessions; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			fprintf(stderr, "ec_demo burst: fork %d failed: %s\n",
				i, strerror(errno));
			pids[i] = -1;
			continue;
		}

		if (pid == 0) {
			/*
			 * Child.  Carry the parent's --spn / --spn-list
			 * decision through static globals (post-fork copy);
			 * override KRB5CCNAME and g_client_id so this worker
			 * presents a distinct initiator identity on the wire.
			 */
			const char *cc = g_ccache_list[i % g_ccache_n];

			if (setenv("KRB5CCNAME", cc, 1) != 0) {
				fprintf(stderr,
					"ec_demo burst: child %d setenv KRB5CCNAME=%s failed: %s\n",
					i, cc, strerror(errno));
				_exit(1);
			}

			/*
			 * Decorate g_client_id with the worker index so the
			 * server resolves N distinct clientowners (case 1
			 * of EXCHANGE_ID for each), matching the K8s
			 * one-pod-per-identity shape.  The decoration is
			 * stable per worker and idempotent across retries.
			 */
			char *owner = NULL;

			if (asprintf(&owner, "%s.%d", g_client_id, i) > 0 &&
			    owner)
				g_client_id = owner;

			struct burst_worker_args wa = {
				.mds_host = mds_host,
				.spn = g_spn_list_n > 0 ?
					       g_spn_list[i % g_spn_list_n] :
					       g_spn,
				.worker_idx = i,
			};

			burst_worker(&wa);
			_exit(wa.result == 0 ? 0 : 1);
		}

		pids[i] = pid;
	}

	int n_pass = 0, n_fail = 0;

	for (int i = 0; i < nsessions; i++) {
		if (pids[i] <= 0) {
			n_fail++;
			continue;
		}
		int status = 0;

		if (waitpid(pids[i], &status, 0) < 0) {
			n_fail++;
			continue;
		}
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			n_pass++;
		else
			n_fail++;
	}

	clock_gettime(CLOCK_MONOTONIC, &t_end);
	int total_ms = (int)((t_end.tv_sec - t_start.tv_sec) * 1000 +
			     (t_end.tv_nsec - t_start.tv_nsec) / 1000000);

	fprintf(stderr,
		"ec_demo burst: %d passed, %d failed in %d ms total"
		" (forked-worker mode; per-worker timing in child stderr)\n",
		n_pass, n_fail, total_ms);

	free(pids);
	return n_fail ? 1 : 0;
}

static int cmd_burst(const char *mds_host, int nsessions)
{
	if (nsessions < 1)
		nsessions = 1;

	/*
	 * --ccache-dir takes the forked-worker path; threaded path
	 * stays for the single-ccache "one user, N parallel handshakes"
	 * axis.
	 */
	if (g_ccache_dir)
		return cmd_burst_forked(mds_host, nsessions);

	pthread_t *tids = calloc(nsessions, sizeof(pthread_t));
	struct burst_worker_args *args = calloc(nsessions, sizeof(*args));

	if (!tids || !args) {
		free(tids);
		free(args);
		fprintf(stderr, "ec_demo burst: out of memory\n");
		return 1;
	}

	const char *sec_name[] = { "sys", "krb5", "krb5i", "krb5p" };

	if (g_spn_list_n > 0)
		fprintf(stderr,
			"ec_demo burst: opening %d parallel mds_session%s to %s "
			"(sec=%s, spn-list[%d])\n",
			nsessions, nsessions == 1 ? "" : "s", mds_host,
			sec_name[g_sec], g_spn_list_n);
	else
		fprintf(stderr,
			"ec_demo burst: opening %d parallel mds_session%s to %s (sec=%s%s%s)\n",
			nsessions, nsessions == 1 ? "" : "s", mds_host,
			sec_name[g_sec], g_spn ? ", spn=" : "",
			g_spn ? g_spn : "");

	struct timespec t_start, t_end;

	clock_gettime(CLOCK_MONOTONIC, &t_start);

	for (int i = 0; i < nsessions; i++) {
		args[i].mds_host = mds_host;
		/*
		 * Per-worker SPN selection: when --spn-list is set, rotate
		 * modularly so each worker drives a distinct target SPN
		 * (stresses the server's SPN-resolution path).  When unset,
		 * fall back to the single --spn / library-default behaviour.
		 */
		if (g_spn_list_n > 0)
			args[i].spn = g_spn_list[i % g_spn_list_n];
		else
			args[i].spn = g_spn;
		args[i].worker_idx = i;
		int err =
			pthread_create(&tids[i], NULL, burst_worker, &args[i]);

		if (err) {
			fprintf(stderr,
				"ec_demo burst: pthread_create %d failed: %s\n",
				i, strerror(err));
			args[i].result = -err;
			tids[i] = 0;
		}
	}

	int n_pass = 0, n_fail = 0;
	int min_ms = INT_MAX, max_ms = 0, sum_ms = 0;

	for (int i = 0; i < nsessions; i++) {
		if (tids[i])
			pthread_join(tids[i], NULL);
		if (args[i].result == 0) {
			n_pass++;
			if (args[i].handshake_ms < min_ms)
				min_ms = args[i].handshake_ms;
			if (args[i].handshake_ms > max_ms)
				max_ms = args[i].handshake_ms;
			sum_ms += args[i].handshake_ms;
		} else {
			n_fail++;
			fprintf(stderr,
				"ec_demo burst: worker %d FAIL ret=%d (%s)\n",
				i, args[i].result,
				strerror(args[i].result < 0 ? -args[i].result :
							      args[i].result));
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &t_end);
	int total_ms = (int)((t_end.tv_sec - t_start.tv_sec) * 1000 +
			     (t_end.tv_nsec - t_start.tv_nsec) / 1000000);

	fprintf(stderr,
		"ec_demo burst: %d passed, %d failed in %d ms total"
		" (handshake min=%d max=%d avg=%d ms)\n",
		n_pass, n_fail, total_ms, n_pass ? min_ms : 0, max_ms,
		n_pass ? sum_ms / n_pass : 0);

	free(tids);
	free(args);
	return n_fail ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int cmd_write(const char *mds_host, const char *nfs_file,
		     const char *local_file, int k, int m,
		     enum ec_codec_type codec_type, layouttype4 layout_type,
		     size_t shard_size, uint64_t range_offset,
		     size_t range_length)
{
	struct mds_session ms;
	size_t data_len;
	int ret;
	bool range_mode = (range_offset != 0 || range_length != 0);

	uint8_t *data = read_local_file(local_file, &data_len);

	if (!data)
		return 1;

	if (range_mode) {
		/*
		 * Partial-range mode (Track 1b, see
		 * .claude/design/chunk-collision-t1b.md): write
		 * data[0..range_length) into the MDS file at
		 * [range_offset, range_offset+range_length).
		 * `--length 0` means "use the whole input file".
		 */
		if (range_length == 0)
			range_length = data_len;
		if (range_length > data_len) {
			fprintf(stderr,
				"ec_demo: --length %zu exceeds input size "
				"%zu\n",
				range_length, data_len);
			free(data);
			return 1;
		}
	}

	ret = session_open(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		free(data);
		return 1;
	}

	if (range_mode) {
		fprintf(stderr,
			"ec_demo: writing %zu bytes to %s at offset %llu "
			"(%d+%d, shard=%zu, range mode)\n",
			range_length, nfs_file,
			(unsigned long long)range_offset, k, m, shard_size);
		ret = ec_write_codec_range(&ms, nfs_file, data, range_length,
					   range_offset, k, m, codec_type,
					   layout_type, shard_size);
	} else {
		fprintf(stderr,
			"ec_demo: writing %zu bytes to %s (%d+%d, shard=%zu)\n",
			data_len, nfs_file, k, m, shard_size);
		ret = ec_write_codec(&ms, nfs_file, data, data_len, k, m,
				     codec_type, layout_type, shard_size);
	}
	if (ret)
		fprintf(stderr, "ec_demo: write failed: %d\n", ret);
	else
		fprintf(stderr, "ec_demo: write OK\n");

	mds_session_destroy(&ms);
	free(data);
	return ret ? 1 : 0;
}

static int cmd_read(const char *mds_host, const char *nfs_file,
		    const char *local_file, int k, int m, size_t expected_len,
		    enum ec_codec_type codec_type, layouttype4 layout_type,
		    uint64_t skip_ds_mask, size_t shard_size)
{
	struct mds_session ms;
	int ret;

	ret = session_open(&ms, mds_host);
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

	fprintf(stderr, "ec_demo: reading %s (%d+%d, shard=%zu)\n", nfs_file, k,
		m, shard_size);
	ret = ec_read_codec(&ms, nfs_file, buf, buf_len, &out_len, k, m,
			    codec_type, layout_type, skip_ds_mask, shard_size);
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
		      const char *local_file, int k, int m,
		      enum ec_codec_type codec_type, layouttype4 layout_type,
		      uint64_t skip_ds_mask, size_t shard_size,
		      uint64_t range_offset, size_t range_length)
{
	struct mds_session ms;
	size_t orig_len;
	int ret;
	bool range_mode = (range_offset != 0 || range_length != 0);
	size_t cmp_len;

	uint8_t *orig = read_local_file(local_file, &orig_len);

	if (!orig)
		return 1;

	if (range_mode) {
		/*
		 * Partial-range verify (Track 1b): read the MDS file
		 * bytes [range_offset, range_offset+range_length) and
		 * compare against orig[0..range_length).  Symmetric
		 * with cmd_write: writers and verifiers pass the same
		 * --offset and --length so their per-rank input
		 * payloads stay aligned.  --length 0 means "use the
		 * whole input file".
		 */
		if (range_length == 0)
			range_length = orig_len;
		if (range_length > orig_len) {
			fprintf(stderr,
				"ec_demo: --length %zu exceeds input size "
				"%zu\n",
				range_length, orig_len);
			free(orig);
			return 1;
		}
		cmp_len = range_length;
	} else {
		cmp_len = orig_len;
	}

	ret = session_open(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		free(orig);
		return 1;
	}

	uint8_t *buf = calloc(1, cmp_len);

	if (!buf) {
		mds_session_destroy(&ms);
		free(orig);
		return 1;
	}

	size_t out_len = 0;

	if (range_mode) {
		fprintf(stderr,
			"ec_demo: verifying %s at offset %llu len %zu "
			"against %s (%d+%d, shard=%zu, range mode)\n",
			nfs_file, (unsigned long long)range_offset, cmp_len,
			local_file, k, m, shard_size);
		ret = ec_read_codec_range(&ms, nfs_file, buf, cmp_len,
					  range_offset, k, m, codec_type,
					  layout_type, shard_size);
		out_len = ret ? 0 : cmp_len;
	} else {
		fprintf(stderr,
			"ec_demo: verifying %s against %s (%d+%d, shard=%zu)\n",
			nfs_file, local_file, k, m, shard_size);
		ret = ec_read_codec(&ms, nfs_file, buf, cmp_len, &out_len, k, m,
				    codec_type, layout_type, skip_ds_mask,
				    shard_size);
	}
	if (ret) {
		fprintf(stderr, "ec_demo: read failed: %d\n", ret);
	} else if (out_len < cmp_len) {
		fprintf(stderr,
			"ec_demo: MISMATCH: read %zu bytes, expected %zu\n",
			out_len, cmp_len);
		ret = -1;
	} else if (memcmp(orig, buf, cmp_len) != 0) {
		/* Find first differing byte for diagnostic. */
		for (size_t i = 0; i < cmp_len; i++) {
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
			cmp_len);
	}

	free(buf);
	free(orig);
	mds_session_destroy(&ms);
	return ret ? 1 : 0;
}

/*
 * cmd_write_verify -- write then verify against the SAME session.
 *
 * Equivalent in semantics to `ec_demo write ... && ec_demo verify ...`
 * but reuses one mds_session across both phases.  Cuts reserved-port
 * consumption in half for the stress-driver shape (33 workers x 8
 * transports x 2 phases = 528 ports vs 264) by holding the 8
 * transports across the full lifetime instead of tearing them down
 * between write and verify.
 *
 * The arithmetic matters: bindresvport's usable reserved-port pool
 * is roughly 350-400 ports under 1024 after well-known service
 * exclusions, so a separate-process write+verify pair at this
 * worker x nconnect shape exceeds the pool on a single run.  Holding
 * one session keeps the per-run footprint at clients x nconnect.
 */
static int cmd_write_verify(const char *mds_host, const char *nfs_file,
			    const char *local_file, int k, int m,
			    enum ec_codec_type codec_type,
			    layouttype4 layout_type, uint64_t skip_ds_mask,
			    size_t shard_size, uint64_t range_offset,
			    size_t range_length)
{
	struct mds_session ms;
	size_t orig_len;
	uint8_t *buf = NULL;
	int ret;
	bool range_mode = (range_offset != 0 || range_length != 0);
	size_t cmp_len;

	uint8_t *orig = read_local_file(local_file, &orig_len);

	if (!orig)
		return 1;

	if (range_mode) {
		if (range_length == 0)
			range_length = orig_len;
		if (range_length > orig_len) {
			fprintf(stderr,
				"ec_demo: --length %zu exceeds input size %zu\n",
				range_length, orig_len);
			free(orig);
			return 1;
		}
		cmp_len = range_length;
	} else {
		cmp_len = orig_len;
	}

	ret = session_open(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		free(orig);
		return 1;
	}

	/* Write phase. */
	if (range_mode) {
		fprintf(stderr,
			"ec_demo: writing %zu bytes to %s at offset %llu "
			"(%d+%d, shard=%zu, range mode)\n",
			cmp_len, nfs_file, (unsigned long long)range_offset, k,
			m, shard_size);
		ret = ec_write_codec_range(&ms, nfs_file, orig, cmp_len,
					   range_offset, k, m, codec_type,
					   layout_type, shard_size);
	} else {
		fprintf(stderr,
			"ec_demo: writing %zu bytes to %s (%d+%d, shard=%zu)\n",
			orig_len, nfs_file, k, m, shard_size);
		ret = ec_write_codec(&ms, nfs_file, orig, orig_len, k, m,
				     codec_type, layout_type, shard_size);
	}
	if (ret) {
		fprintf(stderr, "ec_demo: write failed: %d\n", ret);
		goto out;
	}
	fprintf(stderr, "ec_demo: write OK\n");

	/* Verify phase, same session. */
	buf = calloc(1, cmp_len);
	if (!buf) {
		ret = -1;
		goto out;
	}

	size_t out_len = 0;

	if (range_mode) {
		fprintf(stderr,
			"ec_demo: verifying %s at offset %llu len %zu "
			"against %s (%d+%d, shard=%zu, range mode)\n",
			nfs_file, (unsigned long long)range_offset, cmp_len,
			local_file, k, m, shard_size);
		ret = ec_read_codec_range(&ms, nfs_file, buf, cmp_len,
					  range_offset, k, m, codec_type,
					  layout_type, shard_size);
		out_len = ret ? 0 : cmp_len;
	} else {
		fprintf(stderr,
			"ec_demo: verifying %s against %s (%d+%d, shard=%zu)\n",
			nfs_file, local_file, k, m, shard_size);
		ret = ec_read_codec(&ms, nfs_file, buf, cmp_len, &out_len, k, m,
				    codec_type, layout_type, skip_ds_mask,
				    shard_size);
	}
	if (ret) {
		fprintf(stderr, "ec_demo: read failed: %d\n", ret);
	} else if (out_len < cmp_len) {
		fprintf(stderr,
			"ec_demo: MISMATCH: read %zu bytes, expected %zu\n",
			out_len, cmp_len);
		ret = -1;
	} else if (memcmp(orig, buf, cmp_len) != 0) {
		for (size_t i = 0; i < cmp_len; i++) {
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
			cmp_len);
	}

out:
	free(buf);
	free(orig);
	mds_session_destroy(&ms);
	return ret ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Plain (non-EC) commands                                             */
/* ------------------------------------------------------------------ */

static int cmd_put(const char *mds_host, const char *nfs_file,
		   const char *local_file, layouttype4 layout_type)
{
	struct mds_session ms;
	size_t data_len;
	int ret;

	uint8_t *data = read_local_file(local_file, &data_len);

	if (!data)
		return 1;

	ret = session_open(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		free(data);
		return 1;
	}

	fprintf(stderr, "ec_demo: put %zu bytes to %s\n", data_len, nfs_file);
	ret = plain_write(&ms, nfs_file, data, data_len, layout_type);
	if (ret)
		fprintf(stderr, "ec_demo: put failed: %d\n", ret);
	else
		fprintf(stderr, "ec_demo: put OK\n");

	mds_session_destroy(&ms);
	free(data);
	return ret ? 1 : 0;
}

static int cmd_get(const char *mds_host, const char *nfs_file,
		   const char *local_file, size_t expected_len,
		   layouttype4 layout_type)
{
	struct mds_session ms;
	int ret;

	ret = session_open(&ms, mds_host);
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
	ret = plain_read(&ms, nfs_file, buf, buf_len, &out_len, layout_type);
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
		     const char *local_file, layouttype4 layout_type)
{
	struct mds_session ms;
	size_t orig_len;
	int ret;

	uint8_t *orig = read_local_file(local_file, &orig_len);

	if (!orig)
		return 1;

	ret = session_open(&ms, mds_host);
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
	ret = plain_read(&ms, nfs_file, buf, orig_len, &out_len, layout_type);
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
/* Identity commands                                                   */
/* ------------------------------------------------------------------ */

static int cmd_getowner(const char *mds_host, const char *nfs_file)
{
	struct mds_session ms;
	struct mds_file mf;
	char owner[256], owner_group[256];
	int ret;

	ret = session_open(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		return 1;
	}

	ret = mds_file_open(&ms, nfs_file, &mf);
	if (ret) {
		fprintf(stderr, "ec_demo: open failed: %d\n", ret);
		mds_session_destroy(&ms);
		return 1;
	}

	ret = mds_file_getattr(&ms, &mf, owner, sizeof(owner), owner_group,
			       sizeof(owner_group));
	if (ret) {
		fprintf(stderr, "ec_demo: getattr failed: %d\n", ret);
	} else {
		printf("owner=%s\n", owner);
		printf("owner_group=%s\n", owner_group);
	}

	mds_file_close(&ms, &mf);
	mds_session_destroy(&ms);
	return ret ? 1 : 0;
}

static int cmd_setowner(const char *mds_host, const char *nfs_file,
			const char *owner_str)
{
	struct mds_session ms;
	struct mds_file mf;
	int ret;

	ret = session_open(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		return 1;
	}

	ret = mds_file_open(&ms, nfs_file, &mf);
	if (ret) {
		fprintf(stderr, "ec_demo: open failed: %d\n", ret);
		mds_session_destroy(&ms);
		return 1;
	}

	fprintf(stderr, "ec_demo: setowner %s → %s\n", nfs_file, owner_str);
	ret = mds_file_setattr_owner(&ms, &mf, owner_str, NULL);
	if (ret) {
		fprintf(stderr, "ec_demo: setattr failed: %d\n", ret);
	} else {
		/* Read back to verify. */
		char owner[256], group[256];

		ret = mds_file_getattr(&ms, &mf, owner, sizeof(owner), group,
				       sizeof(group));
		if (ret == 0)
			printf("owner=%s\nowner_group=%s\n", owner, group);
	}

	mds_file_close(&ms, &mf);
	mds_session_destroy(&ms);
	return ret ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* bigfile: emulate CTHON04 bigfile test via inband MDS I/O           */
/* ------------------------------------------------------------------ */

/*
 * CTHON04 bigfile pattern: 8 KB blocks, each block filled with a
 * letter cycling a-z.  The letter for block N is 'a' + (N % 26).
 * N = absolute_offset / 8192.
 *
 * Fill buf[0..len-1] starting at file offset off.
 */
static void bigfile_fill(uint8_t *buf, size_t len, uint64_t off)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = (uint8_t)('a' + (((off + i) / 8192) % 26));
}

static int cmd_bigfile(const char *mds_host, const char *nfs_file,
		       size_t file_size, size_t chunk_size, bool delete_first)
{
	struct mds_session ms;
	struct mds_file mf;
	uint8_t *buf;
	int ret;

	ret = session_open(&ms, mds_host);
	if (ret) {
		fprintf(stderr, "ec_demo: session create failed: %d\n", ret);
		return 1;
	}

	/*
	 * Cap chunk_size to the server's negotiated fore-channel
	 * ca_maxrequestsize.  Each WRITE compound is SEQUENCE + PUTFH +
	 * WRITE; allow ~256 bytes of fixed XDR + RPC overhead so the
	 * data payload fits within the negotiated limit.
	 */
#define WRITE_HDR_OVERHEAD 256u
	if (ms.ms_maxrequestsize > WRITE_HDR_OVERHEAD) {
		uint32_t max_data = ms.ms_maxrequestsize - WRITE_HDR_OVERHEAD;

		if (chunk_size > max_data) {
			fprintf(stderr,
				"ec_demo: capping chunk size %zu -> %u"
				" (server maxrequestsize=%u)\n",
				chunk_size, max_data, ms.ms_maxrequestsize);
			chunk_size = max_data;
		}
	}

	buf = malloc(chunk_size);
	if (!buf) {
		fprintf(stderr, "ec_demo: malloc failed\n");
		mds_session_destroy(&ms);
		return 1;
	}

	/*
	 * If --delete-first: remove the existing file so it is re-created
	 * fresh.  This exercises the O_TRUNC path that clears stale data
	 * from a prior inode occupying the same ino_N.dat file.
	 * Ignore NOENT (file may not exist yet).
	 */
	if (delete_first) {
		ret = mds_file_remove(&ms, nfs_file);
		if (ret && ret != -ENOENT) {
			fprintf(stderr, "ec_demo: remove %s failed: %d\n",
				nfs_file, ret);
			mds_session_destroy(&ms);
			free(buf);
			return 1;
		}
		fprintf(stderr, "ec_demo: removed %s (--delete-first)\n",
			nfs_file);
	}

	/* Open (create if needed) the file for writing. */
	ret = mds_file_open(&ms, nfs_file, &mf);
	if (ret) {
		fprintf(stderr, "ec_demo: open %s failed: %d\n", nfs_file, ret);
		mds_session_destroy(&ms);
		free(buf);
		return 1;
	}

	/* Write file_size bytes in chunk_size pieces. */
	fprintf(stderr, "ec_demo: writing %zu bytes to %s (%zu-byte chunks)\n",
		file_size, nfs_file, chunk_size);

	size_t written = 0;

	while (written < file_size) {
		size_t n = chunk_size;

		if (n > file_size - written)
			n = file_size - written;

		bigfile_fill(buf, n, (uint64_t)written);

		ret = mds_file_write(&ms, &mf, buf, (uint32_t)n,
				     (uint64_t)written);
		if (ret) {
			fprintf(stderr,
				"ec_demo: write at offset %zu failed: %d\n",
				written, ret);
			mds_file_close(&ms, &mf);
			mds_session_destroy(&ms);
			free(buf);
			return 1;
		}
		written += n;
	}

	mds_file_close(&ms, &mf);
	fprintf(stderr, "ec_demo: write done, %zu bytes\n", written);

	/* Re-open for reading and verify the cycling pattern byte by byte. */
	ret = mds_file_open(&ms, nfs_file, &mf);
	if (ret) {
		fprintf(stderr, "ec_demo: reopen %s failed: %d\n", nfs_file,
			ret);
		mds_session_destroy(&ms);
		free(buf);
		return 1;
	}

	fprintf(stderr, "ec_demo: verifying %zu bytes from %s\n", file_size,
		nfs_file);

	size_t verified = 0;
	int mismatches = 0;

	while (verified < file_size) {
		size_t n = chunk_size;

		if (n > file_size - verified)
			n = file_size - verified;

		uint32_t nread = 0;

		ret = mds_file_read(&ms, &mf, buf, (uint32_t)n,
				    (uint64_t)verified, &nread);
		if (ret) {
			fprintf(stderr,
				"ec_demo: read at offset %zu failed: %d\n",
				verified, ret);
			mds_file_close(&ms, &mf);
			mds_session_destroy(&ms);
			free(buf);
			return 1;
		}
		if (nread == 0) {
			fprintf(stderr,
				"ec_demo: short read at offset %zu"
				" (got 0 bytes)\n",
				verified);
			break;
		}

		for (uint32_t i = 0; i < nread; i++) {
			uint64_t off = (uint64_t)(verified + i);
			uint8_t expected = (uint8_t)('a' + ((off / 8192) % 26));

			if (buf[i] != expected) {
				fprintf(stderr,
					"ec_demo: MISMATCH at offset %llu:"
					" expected '%c' (0x%02x),"
					" got '%c' (0x%02x)\n",
					(unsigned long long)off, (char)expected,
					expected, (char)buf[i], buf[i]);
				mismatches++;
				if (mismatches >= 10) {
					fprintf(stderr,
						"ec_demo: too many mismatches,"
						" stopping\n");
					mds_file_close(&ms, &mf);
					mds_session_destroy(&ms);
					free(buf);
					return 1;
				}
			}
		}
		verified += nread;
	}

	mds_file_close(&ms, &mf);
	mds_session_destroy(&ms);
	free(buf);

	if (mismatches > 0) {
		fprintf(stderr, "ec_demo: FAILED -- %d mismatch(es)\n",
			mismatches);
		return 1;
	}

	fprintf(stderr, "ec_demo: bigfile PASSED -- %zu bytes verified\n",
		verified);
	return 0;
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
		"  Inband MDS test:\n"
		"  ec_demo bigfile --mds HOST --file NAME"
		" [--size N] [--chunk N] [--delete-first]\n"
		"\n"
		"  Erasure-coded:\n"
		"  ec_demo write        --mds HOST --file NAME --input FILE"
		" [--k K] [--m M]\n"
		"  ec_demo read         --mds HOST --file NAME --output FILE"
		" [--k K] [--m M] [--size N]\n"
		"  ec_demo verify       --mds HOST --file NAME --input FILE"
		" [--k K] [--m M]\n"
		"  ec_demo write_verify --mds HOST --file NAME --input FILE"
		" [--k K] [--m M]\n"
		"                       (write then verify on ONE session;\n"
		"                       half the reserved-port footprint of\n"
		"                       running write + verify as separate\n"
		"                       processes -- intended for stress\n"
		"                       drivers against strict-port servers.)\n"
		"\n"
		"Options:\n"
		"  --mds HOST       MDS hostname or IP\n"
		"  --file NAME      NFS filename (in root of MDS export)\n"
		"  --input FILE     Local file to write/verify\n"
		"  --output FILE    Local file to write read data to\n"
		"  --k K            Data shards for EC (default: 4)\n"
		"  --m M            Parity shards for EC (default: 2)\n"
		"  --size N         File/read size in bytes"
		" (bigfile default: 30M)\n"
		"  --chunk N        Chunk size for bigfile I/O"
		" (default: 1M)\n"
		"  --delete-first   Remove file before bigfile write"
		" (forces fresh inode)\n"
		"  --codec TYPE     Codec: rs (default), mojette-sys,"
		" mojette-nonsys, stripe, mirror\n"
		"  --id ID          Client identity (default: PID)."
		" Unique per concurrent instance.\n"
		"  --layout TYPE    Layout: v1 (default, NFSv3 DS),"
		" v2 (CHUNK ops)\n"
		"  --skip-ds LIST   Comma-separated DS indices to skip"
		" on read (degraded mode)\n"
		"  --force-scalar   Disable SIMD in Mojette forward"
		" transform (benchmark scalar path)\n"
		"  --force-gd       Use geometry-driven Mojette inverse"
		" instead of corner-peeling (benchmark)\n"
		"  --shard-size N   Per-data-shard byte size for EC"
		" (default: 4096; must be a multiple of 8).\n"
		"                   Use 24576 for the Mojette 24 KiB"
		" demo at 96 KiB / k=4 payloads.\n"
		"  --offset OFF     Partial-range write / verify (Track 1b):\n"
		"                   start byte offset in the MDS file.\n"
		"                   Default 0.  --length defaults to the\n"
		"                   input file size if omitted.\n"
		"  --length LEN     Partial-range byte count.  Both --offset\n"
		"                   and --length apply only to write /\n"
		"                   verify; ignored elsewhere.\n"
		"  --sec FLAVOR     Security flavor: sys (default), krb5,\n"
		"                   krb5i, krb5p.\n"
		"  --spn NAME       Target Kerberos service principal name\n"
		"                   override.  Accepted forms:\n"
		"                     nfs/host.example.com\n"
		"                     nfs/host.example.com@REALM\n"
		"                     nfs@host.example.com (default shape)\n"
		"                   Only meaningful with --sec krb5*.\n"
		"                   When omitted, the krb5 library defaults\n"
		"                   to nfs/<server>@<REALM>.\n"
		"  --spn-list L     Comma-separated list of SPNs to rotate\n"
		"                   across burst workers (e.g.\n"
		"                   nfs/h0,nfs/h1,nfs/h2).  When set,\n"
		"                   worker i uses list[i %% N] as its target.\n"
		"                   Overrides --spn.  List length need not\n"
		"                   match --nsessions (modular).  Drives the\n"
		"                   server's SPN-resolution path with a fan\n"
		"                   of distinct principals from one process.\n"
		"  --nsessions N    Number of parallel mds_sessions for the\n"
		"                   `burst` subcommand (default: 1).  Each\n"
		"                   session is an independent EXCHANGE_ID +\n"
		"                   CREATE_SESSION + GSS context establishment,\n"
		"                   driving N concurrent handshakes from one\n"
		"                   process.  Closest in-process analogue to\n"
		"                   the multi-mount load that motivates the\n"
		"                   krb5 stress reproducer.  The kernel /\n"
		"                   pd-protod nconnect knob (N TCP transports\n"
		"                   under one session) is exposed separately\n"
		"                   as --nconnect.\n");

	/*
	 * Second fprintf to keep each string literal under C99's
	 * 4095-char minimum-mandated length (-Woverlength-strings).
	 */
	fprintf(stderr,
		"  --nconnect M     Kernel-style TCP transports per\n"
		"                   mds_session (default: 1).  Transport 0\n"
		"                   carries EXCHANGE_ID + CREATE_SESSION;\n"
		"                   transports 1..M-1 are bound to the same\n"
		"                   sessionid via BIND_CONN_TO_SESSION.  Each\n"
		"                   transport carries its own RPCSEC_GSS\n"
		"                   context, so M transports = M parallel\n"
		"                   GSS_INIT exchanges per session.  Total\n"
		"                   wire transports for `burst` = nsessions x\n"
		"                   nconnect.  Only meaningful with\n"
		"                   --sec krb5*; ignored for sec=sys.\n"
		"  --ccache-dir D   Per-worker krb5 ccache rotation: scan\n"
		"                   directory D for regular files, and use\n"
		"                   ccaches[i %% N] as KRB5CCNAME for burst\n"
		"                   worker i.  Implies forked-worker mode\n"
		"                   (each child carries its own KRB5CCNAME\n"
		"                   and libkrb5 context).  Worker i also\n"
		"                   decorates --id as \"id.i\" so the server\n"
		"                   sees N distinct NFSv4 clientowners --\n"
		"                   drives the multi-identity load shape.\n"
		"                   Default: unset (threaded burst, one\n"
		"                   ccache from the inherited environment).\n"
		"  --source-ip A    Local IPv4 source address (dotted-quad) to\n"
		"                   bind MDS- and DS-side TCP sockets to before\n"
		"                   connect.  The address must already be\n"
		"                   assigned to a local interface on this host\n"
		"                   (bind fails with EADDRNOTAVAIL otherwise).\n"
		"                   Use to drive multi-client stress from a\n"
		"                   single host: each parallel ec_demo gets a\n"
		"                   different --source-ip and --id so the MDS\n"
		"                   sees them as independent clients.\n"
		"                   Default: unset (kernel-assigned source).\n"
		"\n"
		"Subcommands:\n"
		"  burst            Open --nsessions N parallel mds_sessions\n"
		"                   to --mds HOST, optionally under --sec\n"
		"                   krb5 with --spn NAME, then close.\n"
		"                   Prints per-handshake min/max/avg ms and\n"
		"                   total elapsed.  No file I/O -- this is a\n"
		"                   handshake-burst-only driver for the\n"
		"                   krb5 stress reproducer.\n");
}

static struct option long_options[] = {
	{ "mds", required_argument, NULL, 'h' },
	{ "file", required_argument, NULL, 'f' },
	{ "input", required_argument, NULL, 'i' },
	{ "output", required_argument, NULL, 'o' },
	{ "k", required_argument, NULL, 'k' },
	{ "m", required_argument, NULL, 'm' },
	{ "size", required_argument, NULL, 's' },
	{ "chunk", required_argument, NULL, 'C' },
	{ "delete-first", no_argument, NULL, 'D' },
	{ "codec", required_argument, NULL, 'c' },
	{ "id", required_argument, NULL, 'd' },
	{ "layout", required_argument, NULL, 'l' },
	{ "skip-ds", required_argument, NULL, 'S' },
	{ "force-scalar", no_argument, NULL, 'F' },
	{ "force-gd", no_argument, NULL, 'G' },
	{ "sec", required_argument, NULL, 'x' },
	{ "spn", required_argument, NULL, 'p' },
	{ "spn-list", required_argument, NULL, 'P' },
	{ "ccache-dir", required_argument, NULL, 257 },
	{ "nsessions", required_argument, NULL, 'n' },
	/*
	 * --nconnect kept as a deprecated alias.  Uses 256 (out of
	 * ASCII range) as its return value so the handler can detect
	 * the alias path and emit a one-line deprecation warning,
	 * without colliding with the canonical -n short option.
	 */
	{ "nconnect", required_argument, NULL, 256 },
	{ "shard-size", required_argument, NULL, 'Z' },
	{ "offset", required_argument, NULL, 'O' },
	{ "length", required_argument, NULL, 'L' },
	/*
	 * Optional local IPv4 source address to bind MDS- and DS-side
	 * TCP sockets to before connect.  Use to run multiple parallel
	 * ec_demo instances from one host, each presenting as a
	 * different client (combined with a unique --id).  The address
	 * must already be assigned to a local interface on this host.
	 */
	{ "source-ip", required_argument, NULL, 258 },
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
	size_t chunk_size = 1024 * 1024; /* 1 MB default for bigfile */
	bool delete_first = false;
	enum ec_codec_type codec_type = EC_CODEC_RS;
	layouttype4 layout_type = LAYOUT4_FLEX_FILES;
	const char *client_id = NULL;
	uint64_t skip_ds_mask = 0;
	size_t shard_size = EC_SHARD_SIZE_DEFAULT;
	uint64_t range_offset = 0;
	size_t range_length = 0;
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

	while ((opt = getopt_long(argc, argv,
				  "h:f:i:o:k:m:s:C:c:d:l:S:x:p:P:n:Z:O:L:DFG?",
				  long_options, NULL)) != -1) {
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
		case 'C':
			chunk_size = (size_t)atol(optarg);
			if (chunk_size == 0) {
				fprintf(stderr,
					"ec_demo: --chunk must be > 0\n");
				return 1;
			}
			break;
		case 'D':
			delete_first = true;
			break;
		case 'd':
			client_id = optarg;
			break;
		case 'l':
			if (strcmp(optarg, "v1") == 0)
				layout_type = LAYOUT4_FLEX_FILES;
			else if (strcmp(optarg, "v2") == 0)
				layout_type = LAYOUT4_FLEX_FILES_V2;
			else {
				fprintf(stderr,
					"ec_demo: unknown layout '%s'\n",
					optarg);
				return 1;
			}
			break;
		case 'c':
			if (strcmp(optarg, "rs") == 0)
				codec_type = EC_CODEC_RS;
			else if (strcmp(optarg, "mojette-sys") == 0)
				codec_type = EC_CODEC_MOJETTE_SYS;
			else if (strcmp(optarg, "mojette-nonsys") == 0)
				codec_type = EC_CODEC_MOJETTE_NONSYS;
			else if (strcmp(optarg, "stripe") == 0)
				codec_type = EC_CODEC_STRIPE;
			else if (strcmp(optarg, "mirror") == 0)
				codec_type = EC_CODEC_MIRROR;
			else {
				fprintf(stderr, "ec_demo: unknown codec '%s'\n",
					optarg);
				return 1;
			}
			break;
		case 'S': {
			char *copy = strdup(optarg);
			char *tok, *end;

			if (!copy)
				return 1;

			for (tok = strtok(copy, ","); tok;
			     tok = strtok(NULL, ",")) {
				long idx = strtol(tok, &end, 10);

				if (*end != '\0' || end == tok || idx < 0 ||
				    idx > 63) {
					fprintf(stderr,
						"ec_demo: invalid DS"
						" index %ld\n",
						idx);
					free(copy);
					return 1;
				}
				skip_ds_mask |= (1ULL << idx);
			}
			free(copy);
			break;
		}
		case 'F':
			moj_force_scalar(true);
			break;
		case 'G':
			moj_force_gd(true);
			break;
		case 'Z':
			shard_size = (size_t)atol(optarg);
			if (shard_size == 0 ||
			    (shard_size % sizeof(uint64_t)) != 0) {
				fprintf(stderr, "ec_demo: --shard-size must be"
						" a non-zero multiple of 8\n");
				return 1;
			}
			break;
		case 'O':
			range_offset = (uint64_t)strtoull(optarg, NULL, 0);
			break;
		case 'L':
			range_length = (size_t)strtoull(optarg, NULL, 0);
			break;
		case 'x':
			if (!strcasecmp(optarg, "sys"))
				g_sec = EC_SEC_SYS;
			else if (!strcasecmp(optarg, "krb5"))
				g_sec = EC_SEC_KRB5;
			else if (!strcasecmp(optarg, "krb5i"))
				g_sec = EC_SEC_KRB5I;
			else if (!strcasecmp(optarg, "krb5p"))
				g_sec = EC_SEC_KRB5P;
			else {
				fprintf(stderr, "ec_demo: unknown sec: %s\n",
					optarg);
				return 1;
			}
			break;
		case 'p':
			g_spn = optarg;
			break;
		case 'P': {
			/*
			 * Parse comma-separated SPN list into g_spn_list[]
			 * for round-robin rotation across burst workers.
			 * The optarg buffer survives until exit (argv-based),
			 * so we can safely strdup once and NUL-terminate
			 * the comma boundaries in-place via strtok.
			 */
			char *dup = strdup(optarg);

			if (!dup) {
				fprintf(stderr,
					"ec_demo: --spn-list strdup failed\n");
				return 1;
			}
			/* First pass: count comma-separated tokens. */
			int n = 1;

			for (const char *q = dup; *q; q++)
				if (*q == ',')
					n++;
			g_spn_list = calloc(n, sizeof(*g_spn_list));
			if (!g_spn_list) {
				free(dup);
				fprintf(stderr,
					"ec_demo: --spn-list alloc failed\n");
				return 1;
			}
			char *save = NULL;
			char *tok = strtok_r(dup, ",", &save);
			int i = 0;

			while (tok && i < n) {
				g_spn_list[i++] = tok;
				tok = strtok_r(NULL, ",", &save);
			}
			g_spn_list_n = i;
			break;
		}
		case 'n':
			g_nsessions = atoi(optarg);
			if (g_nsessions < 1) {
				fprintf(stderr,
					"ec_demo: --nsessions must be >= 1\n");
				return 1;
			}
			break;
		case 256:
			/*
			 * --nconnect M: kernel-style TCP transports per
			 * mds_session (was a deprecated alias for one cycle;
			 * now reclaimed for the kernel meaning).  One
			 * EXCHANGE_ID + CREATE_SESSION on transport 0, then
			 * M-1 BIND_CONN_TO_SESSION on transports 1..M-1, each
			 * carrying its own RPCSEC_GSS context.  Total wire
			 * transports for `burst` = nsessions x nconnect.
			 */
			g_nconnect = atoi(optarg);
			if (g_nconnect < 1) {
				fprintf(stderr,
					"ec_demo: --nconnect must be >= 1\n");
				return 1;
			}
			break;
		case 257:
			/*
			 * --ccache-dir DIR: per-worker krb5 ccache rotation.
			 * Switches burst to forked-worker mode so each child
			 * carries its own KRB5CCNAME / libkrb5 context, and
			 * each NFSv4 EXCHANGE_ID carries a distinct
			 * clientowner.  Drives the multi-identity load shape
			 * the threaded burst (one-ccache by design) cannot.
			 */
			g_ccache_dir = optarg;
			if (load_ccache_dir(g_ccache_dir) < 0)
				return 1;
			break;
		case 258:
			/*
			 * --source-ip ADDR: bind MDS- and DS-side TCP sockets
			 * to this local IPv4 source address before connect.
			 * The address must already be assigned to a local
			 * interface (bind returns EADDRNOTAVAIL otherwise).
			 * Use with a unique --id per parallel ec_demo
			 * instance to drive multi-client load from a single
			 * host.
			 */
			g_source_ip = optarg;
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

	g_client_id = client_id;

	/* Plain (non-EC) commands. */
	if (strcmp(cmd, "put") == 0) {
		if (!local_input) {
			fprintf(stderr, "ec_demo: put requires --input\n");
			return 1;
		}
		return cmd_put(mds_host, nfs_file, local_input, layout_type);
	}

	if (strcmp(cmd, "get") == 0) {
		if (!local_output) {
			fprintf(stderr, "ec_demo: get requires --output\n");
			return 1;
		}
		return cmd_get(mds_host, nfs_file, local_output, read_size,
			       layout_type);
	}

	if (strcmp(cmd, "bigfile") == 0) {
		/* Default to 30 MB if --size not given (CTHON04 default). */
		size_t file_size = (read_size > 0) ? read_size :
						     30 * 1024 * 1024;

		return cmd_bigfile(mds_host, nfs_file, file_size, chunk_size,
				   delete_first);
	}

	if (strcmp(cmd, "getowner") == 0) {
		return cmd_getowner(mds_host, nfs_file);
	}

	if (strcmp(cmd, "setowner") == 0) {
		/* --owner <user@domain> via the --input flag (reused). */
		if (!local_input) {
			fprintf(stderr,
				"ec_demo: setowner requires --input <owner>\n");
			return 1;
		}
		return cmd_setowner(mds_host, nfs_file, local_input);
	}

	if (strcmp(cmd, "check") == 0) {
		if (!local_input) {
			fprintf(stderr, "ec_demo: check requires --input\n");
			return 1;
		}
		return cmd_check(mds_host, nfs_file, local_input, layout_type);
	}

	/* EC commands need valid k/m.  Stripe and mirror allow m=0. */
	int m_min = (codec_type == EC_CODEC_STRIPE ||
		     codec_type == EC_CODEC_MIRROR) ?
			    0 :
			    1;

	if (k < 1 || m < m_min || k + m > 255) {
		fprintf(stderr, "ec_demo: invalid k=%d m=%d\n", k, m);
		return 1;
	}

	if (strcmp(cmd, "write") == 0) {
		if (!local_input) {
			fprintf(stderr, "ec_demo: write requires --input\n");
			return 1;
		}
		return cmd_write(mds_host, nfs_file, local_input, k, m,
				 codec_type, layout_type, shard_size,
				 range_offset, range_length);
	}

	if (strcmp(cmd, "read") == 0) {
		if (!local_output) {
			fprintf(stderr, "ec_demo: read requires --output\n");
			return 1;
		}
		return cmd_read(mds_host, nfs_file, local_output, k, m,
				read_size, codec_type, layout_type,
				skip_ds_mask, shard_size);
	}

	if (strcmp(cmd, "verify") == 0) {
		if (!local_input) {
			fprintf(stderr, "ec_demo: verify requires --input\n");
			return 1;
		}
		return cmd_verify(mds_host, nfs_file, local_input, k, m,
				  codec_type, layout_type, skip_ds_mask,
				  shard_size, range_offset, range_length);
	}

	if (strcmp(cmd, "write_verify") == 0) {
		if (!local_input) {
			fprintf(stderr,
				"ec_demo: write_verify requires --input\n");
			return 1;
		}
		return cmd_write_verify(mds_host, nfs_file, local_input, k, m,
					codec_type, layout_type, skip_ds_mask,
					shard_size, range_offset, range_length);
	}

	if (strcmp(cmd, "burst") == 0)
		return cmd_burst(mds_host, g_nsessions);

	fprintf(stderr, "ec_demo: unknown command '%s'\n", cmd);
	usage();
	return 1;
}
