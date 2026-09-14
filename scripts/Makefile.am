# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = reffs

dist_bin_SCRIPTS = \
	reffs-probe.py \
	bat_setup.sh \
	bat_client_setup.sh \
	bat_export_setup.sh \
	bat_krb5_setup.sh \
	bat_tls_setup.sh \
	ci_remote.sh \
	ci_space_test.sh \
	dstate_monitor.sh

# reffs-probe's parser, checked with nothing running.  Only main()'s
# args.func(Probe1Client(...), args) needs a reffsd; building the
# parser, rendering its help and parsing an argv do not, and all three
# have their own way of breaking.  test_sb_probe.py next door is the
# opposite case -- an integration test that does need a server -- which
# is why it is not in TESTS.
#
# REFFS_PROBE points the test at the BUILT script.  It must not load
# scripts/reffs-probe.py.in: importing that writes
# scripts/reffs/__init__.py into the source tree.
#
# PYTHONPATH carries the generated reffs package that lives beside the
# built script.  The rpc module comes from reply-xdr, installed
# system-wide (see .github/workflows/ci.yml).
#
# AM_PATH_PYTHON was given a ':' fallback, so $(PYTHON) is the no-op
# shell builtin when no interpreter was found -- which would run the
# test as ':' and report a pass having executed nothing.  Refuse that
# explicitly rather than letting a green tick mean nothing.
TEST_EXTENSIONS = .py
PY_LOG_COMPILER = $(srcdir)/run-python-test.sh
AM_TESTS_ENVIRONMENT = \
	PYTHON='$(PYTHON)'; export PYTHON; \
	REFFS_PROBE='$(abs_top_builddir)/scripts/reffs-probe.py'; \
	export REFFS_PROBE; \
	PYTHONPATH="$(abs_top_builddir)/scripts$${PYTHONPATH:+:$$PYTHONPATH}"; \
	export PYTHONPATH;

dist_check_SCRIPTS = \
	run-python-test.sh \
	test_reffs_probe_parser.py

TESTS = test_reffs_probe_parser.py
