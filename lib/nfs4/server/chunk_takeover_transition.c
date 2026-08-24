/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "nfs4/chunk_epoch.h"
#include "nfs4/chunk_takeover_transition.h"
#include "reffs/posix_shims.h"

#define CHUNK_TAKEOVER_JOURNAL_MAGIC 0x43544A52U /* "CTJR" */
#define CHUNK_TAKEOVER_JOURNAL_VERSION 1
#define CHUNK_TAKEOVER_JOURNAL_PENDING 0
#define CHUNK_TAKEOVER_JOURNAL_ABORTED 1
#define CHUNK_TAKEOVER_JOURNAL_COMPLETED 2

struct takeover_journal {
	uint32_t magic;
	uint32_t version;
	uint32_t profile;
	uint32_t reserved;
	uint64_t token_expires_at;
	uint64_t expected_prior_epoch;
	uint64_t new_epoch;
	uint64_t new_expires_at_ns;
	uint64_t issuer_clientid;
	char principal[REFFS_CONFIG_MAX_PRINCIPAL];
	uint8_t token_id[CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN];
};

static int transition_paths(char *journal, size_t journal_len, char *lock,
			    size_t lock_len, const char *state_dir)
{
	int n;

	if (!state_dir)
		return -EINVAL;
	n = snprintf(journal, journal_len, "%s/chunk_takeover_journal",
		     state_dir);
	if (n < 0 || (size_t)n >= journal_len)
		return -ENAMETOOLONG;
	n = snprintf(lock, lock_len, "%s.lock", journal);
	if (n < 0 || (size_t)n >= lock_len)
		return -ENAMETOOLONG;
	return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = read(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static void journal_from_transition(struct takeover_journal *journal,
				    const struct chunk_takeover_transition *t)
{
	memset(journal, 0, sizeof(*journal));
	journal->magic = CHUNK_TAKEOVER_JOURNAL_MAGIC;
	journal->version = CHUNK_TAKEOVER_JOURNAL_VERSION;
	journal->profile = t->profile;
	journal->token_expires_at = t->token_expires_at;
	journal->expected_prior_epoch = t->expected_prior_epoch;
	journal->new_epoch = t->new_epoch;
	journal->new_expires_at_ns = t->new_expires_at_ns;
	journal->issuer_clientid = t->issuer_clientid;
	strcpy(journal->principal, t->principal);
	memcpy(journal->token_id, t->token_id,
	       CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN);
}

static bool journal_matches(const struct takeover_journal *journal,
			    const struct chunk_takeover_transition *t)
{
	/* The monotonic deadline is recomputed from live clocks on reissue. */
	return journal->profile == t->profile &&
	       journal->token_expires_at == t->token_expires_at &&
	       journal->expected_prior_epoch == t->expected_prior_epoch &&
	       journal->new_epoch == t->new_epoch &&
	       journal->issuer_clientid == t->issuer_clientid &&
	       strcmp(journal->principal, t->principal) == 0 &&
	       memcmp(journal->token_id, t->token_id,
		      CHUNK_TAKEOVER_REPLAY_TOKEN_ID_LEN) == 0;
}

static int journal_load(const char *path, struct takeover_journal *journal,
			bool *present)
{
	int fd, ret;

	*present = false;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return errno == ENOENT ? 0 : -errno;
	ret = read_full(fd, journal, sizeof(*journal));
	if (!ret) {
		uint8_t extra;

		if (read(fd, &extra, sizeof(extra)) != 0)
			ret = -EPROTO;
	}
	if (close(fd) && !ret)
		ret = -errno;
	if (ret)
		return ret;
	if (journal->magic != CHUNK_TAKEOVER_JOURNAL_MAGIC ||
	    journal->version != CHUNK_TAKEOVER_JOURNAL_VERSION ||
	    journal->reserved > CHUNK_TAKEOVER_JOURNAL_COMPLETED ||
	    !journal->profile ||
	    !memchr(journal->principal, '\0', sizeof(journal->principal)))
		return -EPROTO;
	*present = true;
	return 0;
}

static int journal_save(const char *path,
			const struct takeover_journal *journal)
{
	char tmp[512];
	int fd, ret;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -errno;
	ret = write_full(fd, journal, sizeof(*journal));
	if (!ret)
		ret = reffs_fdatasync(fd);
	if (close(fd) && !ret)
		ret = -errno;
	if (ret) {
		unlink(tmp);
		return ret;
	}
	if (rename(tmp, path)) {
		ret = -errno;
		unlink(tmp);
		return ret;
	}
	return 0;
}

static int journal_remove(const char *path)
{
	if (unlink(path) && errno != ENOENT)
		return -errno;
	return 0;
}

int chunk_takeover_transition_apply(const char *state_dir,
				    const struct chunk_takeover_transition *t,
				    uint64_t now_sec)
{
	char journal_path[512], lock_path[520];
	struct takeover_journal journal;
	struct chunk_mds_epoch epoch;
	bool journal_present, journal_aborted = false;
	bool journal_completed = false, replay_seen, resuming;
	int lock_fd, ret;

	if (!t || !t->profile || !t->principal || !t->principal[0] ||
	    strlen(t->principal) >= REFFS_CONFIG_MAX_PRINCIPAL ||
	    !t->token_id || t->token_expires_at <= now_sec ||
	    !t->new_expires_at_ns || t->new_epoch < t->expected_prior_epoch ||
	    transition_paths(journal_path, sizeof(journal_path), lock_path,
			     sizeof(lock_path), state_dir))
		return -EINVAL;
	lock_fd = open(lock_path, O_RDWR | O_CREAT, 0600);
	if (lock_fd < 0)
		return -errno;
	if (flock(lock_fd, LOCK_EX)) {
		ret = -errno;
		close(lock_fd);
		return ret;
	}
	ret = journal_load(journal_path, &journal, &journal_present);
	if (ret)
		goto out;
	if (journal_present &&
	    journal.reserved == CHUNK_TAKEOVER_JOURNAL_ABORTED) {
		ret = journal_remove(journal_path);
		if (ret)
			goto out;
		journal_present = false;
		journal_aborted = true;
	}
	if (journal_present &&
	    journal.reserved == CHUNK_TAKEOVER_JOURNAL_COMPLETED) {
		journal_completed = true;
		if (!journal_matches(&journal, t))
			journal_present = false;
	}
	if (journal_present && !journal_completed &&
	    !journal_matches(&journal, t)) {
		ret = -EBUSY;
		goto out;
	}
	ret = chunk_takeover_replay_contains(state_dir, t->profile,
					     t->principal, t->token_id, now_sec,
					     &replay_seen);
	if (ret)
		goto out;
	ret = chunk_mds_epoch_load(state_dir, &epoch);
	if (ret)
		goto out;
	if (journal_aborted) {
		ret = -EALREADY;
		goto out;
	}
	if (journal_completed && journal_present) {
		if (epoch.epoch == t->new_epoch &&
		    epoch.issuer_clientid == t->issuer_clientid)
			ret = 0;
		else
			ret = -EIO;
		goto out;
	}
	if (replay_seen && !journal_present) {
		if (epoch.epoch == t->new_epoch &&
		    epoch.issuer_clientid == t->issuer_clientid)
			ret = 0;
		else
			ret = -EALREADY;
		goto out;
	}
	if (epoch.epoch > t->expected_prior_epoch) {
		if (t->new_epoch > t->expected_prior_epoch &&
		    epoch.epoch == t->new_epoch) {
			if (!journal_present) {
				ret = -ESTALE;
				goto out;
			}
			journal.reserved = CHUNK_TAKEOVER_JOURNAL_COMPLETED;
			ret = journal_save(journal_path, &journal);
			goto out;
		}
		ret = -ESTALE;
		goto out;
	}
	if (epoch.epoch != t->expected_prior_epoch) {
		ret = -ESTALE;
		goto out;
	}
	if (!journal_present) {
		journal_from_transition(&journal, t);
		ret = journal_save(journal_path, &journal);
		if (ret)
			goto out;
	}
	resuming = journal_present;
	if (!replay_seen)
		ret = chunk_takeover_replay_claim(state_dir, t->profile,
						  t->principal, t->token_id,
						  t->token_expires_at, now_sec);
	else
		ret = 0;
	if (ret == -EALREADY && !resuming) {
		journal.reserved = CHUNK_TAKEOVER_JOURNAL_ABORTED;
		if (journal_save(journal_path, &journal))
			ret = -EIO;
		goto out;
	}
	if (ret && ret != -EALREADY)
		goto out;
	epoch.epoch = t->new_epoch;
	epoch.expires_at_ns = t->new_expires_at_ns;
	epoch.issuer_clientid = t->issuer_clientid;
	ret = chunk_mds_epoch_persist(state_dir, &epoch);
	if (!ret) {
		journal.reserved = CHUNK_TAKEOVER_JOURNAL_COMPLETED;
		ret = journal_save(journal_path, &journal);
	}
out:
	if (flock(lock_fd, LOCK_UN) && !ret)
		ret = -errno;
	if (close(lock_fd) && !ret)
		ret = -errno;
	return ret;
}
