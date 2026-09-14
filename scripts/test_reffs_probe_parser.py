#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

"""reffs-probe's argument parser, checked without running a server.

Three separable things live behind `reffs-probe.py <subcommand> <flags>`,
and only the last of them needs a reffsd:

  building the parser   argparse raises here on a duplicate option
                        string, a bad mutually-exclusive group or an
                        undefined type=.  Construction happens inside
                        parse_args(), so importing the script does not
                        notice; something has to call it.
  rendering the help    argparse defers help-string formatting until
                        asked, so a malformed help string builds fine
                        and raises only for the person who typed
                        --help.  Nothing but rendering finds it.
                        Measured, not assumed: '%(nope)s' raises
                        KeyError, a trailing '%' raises ValueError, and
                        a stray '%' raises whenever the character after
                        it is not a valid conversion ('% done' is a
                        '%d').  A stray '%' that happens to form a
                        valid one ('% reliably' is a '%r') renders as
                        nonsense rather than raising, so this catches
                        most of that class and not all of it.
  parsing an argv       returns a Namespace.  Every default, type= and
                        exclusion the handlers rely on can be asserted
                        straight off it.

  dispatching           main() calls args.func(Probe1Client(...), args)
                        -- that is the part that opens a socket, and
                        nothing here reaches it.

So this is a unit test, not an integration test: unlike test_sb_probe.py
next door it needs no running reffsd, and it is wired into `make check`
accordingly.

Help rendering is checked for every subcommand the parser offers rather
than a spot sample -- the list comes from the parser itself, so a
subcommand added tomorrow is covered without editing this file.  The
golden list below is the deliberate counterweight: adding or renaming a
subcommand costs one line there, and an accidental rename breaks the
build instead of reaching a user.
"""

import argparse
import contextlib
import re
import importlib.machinery
import importlib.util
import io
import os
import sys
import types
import unittest

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))


def _probe_path():
    """The BUILT reffs-probe.py -- never scripts/reffs-probe.py.in.

    Importing the .in writes scripts/reffs/__init__.py into the source
    tree: it does that on purpose so a source-tree run can find the
    generated package, but a test has no business dirtying the source
    tree to run.  AC_CONFIG_FILES copies the .in verbatim (there are no
    @substitutions@) and chmods +x, so the built copy is the same
    program, and it sits beside a reffs/ package whose __init__.py the
    build already made.

    REFFS_PROBE comes from AM_TESTS_ENVIRONMENT under `make check`.  The
    fallbacks are for running this by hand from a source checkout, and
    mirror the search test_sb_probe.py does.
    """
    candidates = [os.environ.get('REFFS_PROBE')]
    for build in ('build', 'build_asan'):
        candidates.append(os.path.join(TESTS_DIR, '..', build, 'scripts',
                                       'reffs-probe.py'))
    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return os.path.abspath(candidate)
    raise SystemExit(
        'cannot find a built reffs-probe.py (looked at REFFS_PROBE=%r and '
        'build/scripts, build_asan/scripts).  Build the tree first; this '
        'test deliberately will not load scripts/reffs-probe.py.in.'
        % os.environ.get('REFFS_PROBE'))


def _load_probe():
    path = _probe_path()
    # The generated reffs package sits next to the built script.  The
    # script adds this itself when it finds a reffs/ beside it, but do
    # not rely on that: it is the behaviour under test.
    sys.path.insert(0, os.path.dirname(path))
    # argcomplete is a runtime convenience the build does not require and
    # CI does not install; the parser is what is under test, so stub it
    # rather than making the test depend on it.
    try:
        # pylint: disable=unused-import,import-outside-toplevel
        import argcomplete  # noqa: F401
    except ImportError:
        stub = types.ModuleType('argcomplete')
        stub.autocomplete = lambda parser: None
        sys.modules['argcomplete'] = stub
    # reffs-probe wraps its own imports in a bare `except ImportError`
    # and reports "Could not import reffs package" for every one of
    # them, so a missing rpc module -- a different problem with a
    # different fix -- reads as a missing reffs package.  Say which.
    try:
        # pylint: disable=unused-import,import-outside-toplevel
        import rpc.rpc  # noqa: F401
    except ImportError as why:
        raise SystemExit(
            'the rpc module is missing (%s).  It comes from reply-xdr:\n'
            '  pip3 install reply-xdr@git+https://github.com/loghyr/reply.git\n'
            'This is a build dependency, not an optional extra, so this '
            'test fails rather than skipping.' % why)

    loader = importlib.machinery.SourceFileLoader('reffs_probe', path)
    spec = importlib.util.spec_from_loader('reffs_probe', loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


PROBE = _load_probe()


@contextlib.contextmanager
def _argv(*args):
    saved = sys.argv
    sys.argv = ['reffs-probe.py'] + list(args)
    try:
        yield
    finally:
        sys.argv = saved


def _render_help(*args):
    """Run the script's own parse_args() for `args --help`.

    Returns (exit_code, stdout, stderr).  argparse exits after printing
    help, so SystemExit is the success path.
    """
    out, err = io.StringIO(), io.StringIO()
    code = None
    with _argv(*(list(args) + ['--help'])):
        try:
            with contextlib.redirect_stdout(out), \
                    contextlib.redirect_stderr(err):
                PROBE.parse_args()
        except SystemExit as exit_request:
            code = exit_request.code
    return code, out.getvalue(), err.getvalue()


def _build_parser():
    """The finished ArgumentParser, borrowed on its way past argcomplete.

    reffs-probe builds its parser inside parse_args() and parses in the
    same breath, so there is no seam to take it by -- except that one
    line before parsing it hands the finished parser to
    argcomplete.autocomplete().  If that call ever moves, this raises
    IndexError rather than quietly testing nothing.
    """
    captured = []
    original = PROBE.argcomplete.autocomplete
    PROBE.argcomplete.autocomplete = captured.append
    try:
        _render_help()
    finally:
        PROBE.argcomplete.autocomplete = original
    return captured[0]


PARSER = _build_parser()


def _subcommands(parser):
    for action in parser._actions:  # pylint: disable=protected-access
        if isinstance(action, argparse._SubParsersAction):  # pylint: disable=protected-access
            return action.choices
    return {}


SUBCOMMANDS = _subcommands(PARSER)

# Every subcommand reffs-probe offers, spelled out.  Deriving the list
# from the parser proves each one renders, but cannot notice one quietly
# renamed or dropped -- which is a parsing bug that reaches users.
# Adding a subcommand costs one line here, on purpose: the CLI surface
# then changes visibly in the diff rather than silently.
EXPECTED_SUBCOMMANDS = frozenset((
    'context', 'dstore-drain', 'dstore-instance-count', 'dstore-list',
    'dstore-undrain', 'fd-infos-list', 'fs-usage', 'graceful-cleanup',
    'heartbeat', 'identity-domain-list', 'identity-map-list',
    'identity-map-remove', 'inode-layout-list', 'io-contexts-list',
    'layout-errors', 'nfs4-op-stats', 'null', 'ps-listener-list',
    'ps-write-buffer-stats', 'rpc-dump', 'sb-create', 'sb-destroy',
    'sb-get', 'sb-get-client-rules', 'sb-get-default-coding',
    'sb-lint-flavors', 'sb-list', 'sb-mount',
    'sb-set-checksum-algorithm', 'sb-set-client-rules',
    'sb-set-default-coding', 'sb-set-dstores', 'sb-set-flavors',
    'sb-set-layout-types', 'sb-set-stripe-unit', 'sb-unmount',
    'stats-gather', 'trace-set', 'traces-list', 'trust-stateid-stats',
))


class TopLevelHelp(unittest.TestCase):

    def test_help_renders(self):
        code, out, err = _render_help()
        self.assertEqual(code, 0)
        self.assertIn('usage:', out)
        self.assertEqual(err, '')

    def test_the_subcommand_list_was_found(self):
        # If this drops to zero the per-subcommand tests below stop
        # covering anything while still passing.
        self.assertGreater(len(SUBCOMMANDS), 20)

    def test_the_subcommand_list_is_the_expected_one(self):
        self.assertEqual(set(SUBCOMMANDS), set(EXPECTED_SUBCOMMANDS),
                         'the set of subcommands changed; if that was '
                         'deliberate, update EXPECTED_SUBCOMMANDS')


# A percent that is not part of a conversion has to be written '%%'.
# These are the only forms argparse's own expansion accepts.
_VALID_CONVERSION = re.compile(r'%%|%\((?:prog|default|choices|type|dest|'
                               r'metavar|const|nargs)\)[srd]')


def _all_help_strings(parser, prefix=''):
    """Every help string in the tree, as (where, text) pairs."""
    for action in parser._actions:  # pylint: disable=protected-access
        if action.help:
            yield ('%s%s' % (prefix, '/'.join(action.option_strings)
                             or action.dest), action.help)
        if isinstance(action, argparse._SubParsersAction):  # pylint: disable=protected-access
            for pseudo in action._choices_actions:  # pylint: disable=protected-access
                if pseudo.help:
                    yield ('%s<%s>' % (prefix, pseudo.dest), pseudo.help)
            for name, sub in action.choices.items():
                yield from _all_help_strings(sub, '%s%s ' % (prefix, name))


class HelpStringPercents(unittest.TestCase):
    """A literal percent in a help string must be written '%%'.

    Rendering the help catches the malformed conversions -- '%(nope)s',
    a trailing '%', '% done' which is a '%d' -- but not one that happens
    to form a VALID conversion: '% reliably' is a '%r', so argparse
    expands it against its params dict and prints nonsense rather than
    raising.  Nothing downstream notices.  So check the strings as well
    as render them.
    """

    def test_every_percent_is_escaped_or_a_known_conversion(self):
        for where, text in _all_help_strings(PARSER):
            with self.subTest(option=where):
                stripped = _VALID_CONVERSION.sub('', text)
                self.assertNotIn(
                    '%', stripped,
                    "%s: help text %r has a bare '%%'; write it '%%%%'"
                    % (where, text))


class SubcommandHelp(unittest.TestCase):
    """Every subcommand, rendered.

    argparse defers help-string formatting until something asks for it,
    so a subcommand whose help text is malformed parses fine and only
    breaks for the person typing --help.
    """

    def test_every_subcommand_renders_its_help(self):
        for name in sorted(SUBCOMMANDS):
            with self.subTest(subcommand=name):
                code, out, err = _render_help(name)
                self.assertEqual(code, 0, err)
                self.assertIn('usage:', out)
                self.assertIn(name, out)
                self.assertEqual(err, '')

    def test_every_subcommand_has_something_to_dispatch_to(self):
        # A subparser with no set_defaults(func=...) parses cleanly and
        # then dies in main() on args.func.  --help will not find it.
        for name, subparser in sorted(SUBCOMMANDS.items()):
            with self.subTest(subcommand=name):
                func = subparser.get_default('func')
                self.assertIsNotNone(func, '%s has no func' % name)
                self.assertTrue(callable(func))


def _parse(argv):
    """Parse an argv through the real parser.  Returns a Namespace, or
    None if the parser rejected it."""
    err = io.StringIO()
    try:
        with contextlib.redirect_stderr(err), \
                contextlib.redirect_stdout(io.StringIO()):
            return PARSER.parse_args(argv)
    except SystemExit:
        return None


class ParsedArguments(unittest.TestCase):
    """What an argv turns into, with nothing running.

    A default that quietly changes is a live bug, and a type= that stops
    converting hands the handler a string where it does arithmetic.
    None of this needs a server: parse_args() returns before anything
    dials one.
    """

    def test_global_defaults(self):
        args = _parse(['null'])
        self.assertEqual(args.port, PROBE.PROBE_PORT)
        self.assertEqual(args.host, '::1')
        self.assertFalse(args.json)
        self.assertFalse(args.human)
        self.assertFalse(args.time)

    def test_the_global_flags_come_before_the_subcommand(self):
        # `reffs-probe.py --json null`, not `reffs-probe.py null --json`.
        # The subparsers do not redeclare them, so the trailing form is
        # rejected rather than silently ignored.
        self.assertTrue(_parse(['--json', 'null']).json)
        self.assertTrue(_parse(['--human', 'null']).human)
        self.assertIsNone(_parse(['null', '--json']))

    def test_short_forms_match_their_long_ones(self):
        self.assertTrue(_parse(['-j', 'null']).json)
        self.assertTrue(_parse(['-H', 'null']).human)

    def test_port_arrives_as_a_number(self):
        args = _parse(['--port', '20491', 'null'])
        self.assertEqual(args.port, 20491)
        self.assertIsInstance(args.port, int)

    def test_a_non_numeric_port_is_rejected(self):
        self.assertIsNone(_parse(['--port', 'nope', 'null']))

    def test_a_subcommand_is_required(self):
        # subparsers.required = True in parse_args().
        self.assertIsNone(_parse([]))

    def test_an_unknown_subcommand_is_rejected(self):
        self.assertIsNone(_parse(['no-such-subcommand']))

    def test_stats_gather_wants_exactly_one_program_and_a_version(self):
        # --program-id and --program-name are a required, mutually
        # exclusive pair; --version is separately required.
        program_id = str(sorted(PROBE.PROGRAM_MAP)[0])
        program_name = PROBE.PROGRAM_MAP[int(program_id)]

        self.assertIsNotNone(_parse(['stats-gather', '--program-id',
                                     program_id, '--version', '4']))
        self.assertIsNone(_parse(['stats-gather', '--version', '4']),
                          'neither program given')
        self.assertIsNone(_parse(['stats-gather', '--program-id', program_id,
                                  '--program-name', program_name,
                                  '--version', '4']),
                          'both programs given')
        self.assertIsNone(_parse(['stats-gather', '--program-id',
                                  program_id]),
                          '--version omitted')

    def test_program_id_is_constrained_to_the_known_programs(self):
        # choices=PROGRAM_MAP.keys(), so a stray number is refused rather
        # than reaching the server as an unknown program.
        self.assertIsNone(_parse(['stats-gather', '--program-id', '1',
                                  '--version', '4']))


if __name__ == '__main__':
    unittest.main()
