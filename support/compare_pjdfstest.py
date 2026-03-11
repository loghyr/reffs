#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

"""
compare_pjdfstest.py - Compare two pjdfstest run output files (script/typescript format).

Usage:
    python3 compare_pjdfstest.py <run_before.txt> <run_after.txt>

Produces a human-readable summary of:
  - Tests fixed (were failing, now passing)
  - New regressions (were passing, now failing)
  - Still failing (failing in both runs)
  - Overall statistics
"""

import re
import sys
from collections import defaultdict


def strip_ansi(text: str) -> str:
    """Remove ANSI/VT escape sequences from text."""
    return re.sub(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])', '', text)


def clean_typescript(raw: bytes) -> str:
    """Decode, strip ANSI codes, normalize line endings."""
    text = raw.decode('utf-8', errors='replace')
    text = strip_ansi(text)
    text = text.replace('\r\n', '\n').replace('\r', '\n')
    return text


def expand_ranges(range_str: str) -> set[int]:
    """
    Expand a prove-style range string like "70-71, 74, 122-123" into a set of ints.
    """
    result = set()
    for part in re.split(r'[\s,]+', range_str.strip()):
        part = part.strip()
        if not part:
            continue
        m = re.match(r'^(\d+)-(\d+)$', part)
        if m:
            result.update(range(int(m.group(1)), int(m.group(2)) + 1))
        elif re.match(r'^\d+$', part):
            result.add(int(part))
    return result


def find_last_summary_block(lines: list[str]) -> list[str]:
    """
    Locate the *last* 'Test Summary Report' block (the final prove run,
    ignoring any earlier partial runs from the same session).
    """
    last_idx = -1
    for i, line in enumerate(lines):
        if line.strip() == 'Test Summary Report':
            last_idx = i
    if last_idx < 0:
        return []
    # Collect until the "Files=N, Tests=N" totals line
    block = []
    for line in lines[last_idx:]:
        block.append(line)
        if line.startswith('Files=') and 'Tests=' in line:
            break
    return block


def parse_summary_block(lines: list[str]) -> dict:
    """
    Parse a prove Test Summary Report block.

    Returns a dict:
        {
            'test_file': {
                'total': int,
                'failed_count': int,
                'failed_set': set[int],   # individual sub-test numbers
                'todo_passed': set[int],  # TODO-passed sub-tests (bonus info)
                'parse_errors': bool,
            },
            ...
        }
    And a 'totals' key:
        {'files': int, 'tests': int}
    """
    # Patterns
    test_file_pat = re.compile(
        r'(/\S+\.t)\s+\(Wstat:\s*\d+.*?Tests:\s*(\d+)\s+Failed:\s*(\d+)\)'
    )
    failed_pat   = re.compile(r'Failed tests?:\s*(.*)')
    todo_pat     = re.compile(r'TODO passed:\s*(.*)')
    parse_err_pat = re.compile(r'Parse errors')
    totals_pat   = re.compile(r'Files=(\d+),\s*Tests=(\d+)')

    results = {}
    totals  = {'files': 0, 'tests': 0}

    current = None

    for line in lines:
        m = test_file_pat.search(line)
        if m:
            path        = m.group(1)
            # Normalize path: strip leading directory down to tests/category/file
            rel = re.sub(r'^.*/pjdfstest/', 'pjdfstest/', path)
            current = rel
            results[current] = {
                'total':        int(m.group(2)),
                'failed_count': int(m.group(3)),
                'failed_set':   set(),
                'todo_passed':  set(),
                'parse_errors': False,
            }
            continue

        if current is None:
            continue

        mf = failed_pat.search(line)
        if mf:
            results[current]['failed_set'] |= expand_ranges(mf.group(1))
            continue

        mt = todo_pat.search(line)
        if mt:
            results[current]['todo_passed'] |= expand_ranges(mt.group(1))
            continue

        if parse_err_pat.search(line):
            results[current]['parse_errors'] = True
            continue

        # Continuation lines (indented numbers for multi-line failed/todo lists)
        # These are lines starting with whitespace containing only numbers, ranges, commas
        stripped = line.strip()
        if stripped and re.match(r'^[\d,\s\-]+$', stripped):
            # Attribute to whichever was last seen: failed_set vs todo_passed
            # We can't easily know which, so peek at the last non-empty state.
            # Simple heuristic: if failed_count > len(failed_set), keep filling failed_set
            if results[current]['failed_count'] > len(results[current]['failed_set']):
                results[current]['failed_set'] |= expand_ranges(stripped)
            else:
                results[current]['todo_passed'] |= expand_ranges(stripped)
            continue

        tm = totals_pat.search(line)
        if tm:
            totals = {'files': int(tm.group(1)), 'tests': int(tm.group(2))}
            current = None

    return results, totals


def load_run(path: str):
    with open(path, 'rb') as f:
        raw = f.read()
    text  = clean_typescript(raw)
    lines = text.split('\n')
    block = find_last_summary_block(lines)
    if not block:
        print(f"WARNING: No 'Test Summary Report' found in {path}", file=sys.stderr)
        return {}, {'files': 0, 'tests': 0}
    return parse_summary_block(block)


def fmt_set(s: set[int]) -> str:
    """Format a set of ints as a compact range string."""
    if not s:
        return ''
    nums = sorted(s)
    ranges = []
    start = end = nums[0]
    for n in nums[1:]:
        if n == end + 1:
            end = n
        else:
            ranges.append(f'{start}' if start == end else f'{start}-{end}')
            start = end = n
    ranges.append(f'{start}' if start == end else f'{start}-{end}')
    return ', '.join(ranges)


def compare(before: dict, after: dict, before_totals: dict, after_totals: dict):
    all_files = sorted(set(before) | set(after))

    fixed       = {}   # file -> set of sub-tests fixed
    regressions = {}   # file -> set of sub-tests newly failing
    still_fail  = {}   # file -> set of sub-tests still failing
    new_files   = {}   # file -> info (appeared only in after, with failures)
    gone_files  = {}   # file -> info (appeared only in before, with failures)

    for f in all_files:
        b = before.get(f)
        a = after.get(f)

        b_fail = b['failed_set'] if b else set()
        a_fail = a['failed_set'] if a else set()

        fx  = b_fail - a_fail
        reg = a_fail - b_fail
        sf  = b_fail & a_fail

        if f not in before and a_fail:
            new_files[f] = a
        elif f not in after and b_fail:
            gone_files[f] = b
        else:
            if fx:  fixed[f]       = fx
            if reg: regressions[f] = reg
            if sf:  still_fail[f]  = sf

    # ------------------------------------------------------------------ output
    print('=' * 70)
    print('  pjdfstest Run Comparison')
    print('=' * 70)

    # Totals
    before_total_fail = sum(d['failed_count'] for d in before.values())
    after_total_fail  = sum(d['failed_count'] for d in after.values())
    delta = after_total_fail - before_total_fail
    delta_str = f'+{delta}' if delta > 0 else str(delta)
    print(f'\nOverall sub-test failures:  {before_total_fail} → {after_total_fail}  ({delta_str})')
    print(f'Files with failures:        {sum(1 for d in before.values() if d["failed_count"])} → '
          f'{sum(1 for d in after.values() if d["failed_count"])}')
    if before_totals["tests"] or after_totals["tests"]:
        print(f'Total sub-tests run:        {before_totals["tests"]} → {after_totals["tests"]}')

    # Fixed
    print()
    if fixed:
        total_fixed = sum(len(v) for v in fixed.values())
        print(f'✓ FIXED  ({total_fixed} sub-tests across {len(fixed)} file(s))')
        print('-' * 70)
        for f, nums in sorted(fixed.items()):
            print(f'  {f}')
            print(f'    Fixed sub-tests: {fmt_set(nums)}')
    else:
        print('✓ FIXED  (none)')

    # New regressions
    print()
    if regressions:
        total_reg = sum(len(v) for v in regressions.values())
        print(f'✗ NEW REGRESSIONS  ({total_reg} sub-tests across {len(regressions)} file(s))')
        print('-' * 70)
        for f, nums in sorted(regressions.items()):
            print(f'  {f}')
            print(f'    Newly failing: {fmt_set(nums)}')
    else:
        print('✗ NEW REGRESSIONS  (none)')

    # New test files with failures (appeared only in "after")
    if new_files:
        print()
        print(f'? NEW FAILING FILES  ({len(new_files)} file(s) not present in baseline)')
        print('-' * 70)
        for f, info in sorted(new_files.items()):
            print(f'  {f}  ({info["failed_count"]} failed / {info["total"]} total)')
            if info['failed_set']:
                print(f'    Failed sub-tests: {fmt_set(info["failed_set"])}')

    # Files that disappeared from "after" (were failing before)
    if gone_files:
        print()
        print(f'- GONE FILES  ({len(gone_files)} file(s) that had failures but are absent in after run)')
        print('-' * 70)
        for f, info in sorted(gone_files.items()):
            print(f'  {f}  (had {info["failed_count"]} failures)')

    # Still failing
    print()
    if still_fail:
        total_sf = sum(len(v) for v in still_fail.values())
        print(f'~ STILL FAILING  ({total_sf} sub-tests across {len(still_fail)} file(s))')
        print('-' * 70)
        for f, nums in sorted(still_fail.items()):
            b_info = before[f]
            a_info = after[f]
            print(f'  {f}')
            print(f'    Sub-tests: {fmt_set(nums)}')
            # Show if the count changed (partial improvement within file)
            if b_info['failed_count'] != a_info['failed_count']:
                print(f'    (failure count: {b_info["failed_count"]} → {a_info["failed_count"]})')
    else:
        print('~ STILL FAILING  (none — all prior failures resolved!)')

    print()
    print('=' * 70)


def main():
    if len(sys.argv) != 3:
        print(f'Usage: {sys.argv[0]} <before_run.txt> <after_run.txt>')
        sys.exit(1)

    before_path = sys.argv[1]
    after_path  = sys.argv[2]

    print(f'Before: {before_path}')
    print(f'After:  {after_path}')

    before, before_totals = load_run(before_path)
    after,  after_totals  = load_run(after_path)

    compare(before, after, before_totals, after_totals)


if __name__ == '__main__':
    main()
