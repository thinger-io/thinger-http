#!/usr/bin/env python3
"""Complexity ratchet: flag functions added or changed since a base commit that are too complex.

Usage: complexity_ratchet.py [--base SHA] [--fail] [--ccn N] [--length N] [--lizard CMD] [paths...]

Only the functions whose lines the diff from the base to HEAD touches are checked, so
existing functions are not reported until someone changes them. Each one above the limits
gets a GitHub annotation: an error with --fail (and the exit status is 1), a warning
otherwise. Without a usable base (none given, all zeros on the first push of a branch, or
not in the clone) it falls back to HEAD~1, or checks nothing if there is no parent.

The parser state machines and the lookup tables are exempt (EXEMPT); a function can also be
exempted where it is defined with lizard's "#lizard forgives" comment.
"""
import argparse
import csv
import io
import os
import re
import shlex
import subprocess
import sys

SOURCE_EXTENSIONS = ('.cpp', '.hpp', '.h', '.c', '.cc', '.ipp')

# Long by nature: byte-by-byte state machines and tables, kept flat on purpose
EXEMPT = (
    'request_factory::consume',
    'chunked_decoder::consume',
    'utf8::is_valid',
    'mime_types::extension_to_type',
    'status_strings::get_status_string',
    'stock_replies::to_string',
)

HUNK = re.compile(r'^@@ -\d+(?:,\d+)? \+(?P<start>\d+)(?:,(?P<count>\d+))? @@')


def git(*args):
    return subprocess.run(['git', *args], check=True, capture_output=True, text=True).stdout


def is_commit(sha):
    return subprocess.run(['git', 'cat-file', '-e', f'{sha}^{{commit}}'], capture_output=True).returncode == 0


def resolve_base(base):
    if base and set(base) != {'0'} and is_commit(base):
        return base
    if base:
        print(f'base {base} is not usable, comparing with the previous commit')
    return 'HEAD~1' if is_commit('HEAD~1') else None


def changed_lines(base, paths):
    """{file: (added or modified lines, lines followed by a deletion)} in HEAD"""
    diff = git('diff', '--no-color', '--no-ext-diff', '-U0', '--diff-filter=AMR', base, 'HEAD', '--', *paths)
    changes = {}
    current = None
    for line in diff.splitlines():
        if line.startswith('+++ '):
            name = line[4:]
            current = name[2:] if name.startswith('b/') else None
            if current and not current.endswith(SOURCE_EXTENSIONS):
                current = None
            if current:
                changes.setdefault(current, (set(), set()))
            continue
        match = HUNK.match(line)
        if not match or not current:
            continue
        start = int(match.group('start'))
        count = int(match.group('count') or 1)
        if count:
            changes[current][0].update(range(start, start + count))
        else:
            # Lines removed after line `start`
            changes[current][1].add(start)
    return changes


def analyze(lizard, files):
    """Functions of the files: (name, ccn, length, file, start, end)"""
    output = subprocess.run([*shlex.split(lizard), '-l', 'cpp', '--csv', *files],
                            capture_output=True, text=True).stdout
    functions = []
    # nloc, ccn, tokens, params, length, location, file, name, long_name, start, end
    for row in csv.reader(io.StringIO(output)):
        if len(row) < 11 or not row[0].isdigit():
            continue
        functions.append((row[7], int(row[1]), int(row[4]), os.path.normpath(row[6]), int(row[9]), int(row[10])))
    return functions


def touched(function, changes):
    _, _, _, path, start, end = function
    modified, deleted_after = changes.get(path, (set(), set()))
    return any(start <= line <= end for line in modified) or any(start <= line < end for line in deleted_after)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--base', default='', help='commit to compare HEAD with')
    parser.add_argument('--fail', action='store_true', help='report errors and exit 1 on findings')
    parser.add_argument('--ccn', type=int, default=15, help='maximum cyclomatic complexity')
    parser.add_argument('--length', type=int, default=100, help='maximum function length, in lines')
    parser.add_argument('--lizard', default='lizard', help='lizard command')
    parser.add_argument('paths', nargs='*', default=['thinger'])
    args = parser.parse_args()

    base = resolve_base(args.base)
    if not base:
        print('no base commit to compare with: nothing to check')
        return 0
    changes = changed_lines(base, args.paths)
    files = sorted(path for path in changes if os.path.exists(path))
    if not files:
        print(f'no source changes since {base}')
        return 0

    level = 'error' if args.fail else 'warning'
    findings = 0
    for function in analyze(args.lizard, files):
        name, ccn, length, path, start, _ = function
        if ccn <= args.ccn and length <= args.length:
            continue
        if name.endswith(EXEMPT) or not touched(function, changes):
            continue
        findings += 1
        print(f'::{level} file={path},line={start},title=Function too complex::'
              f'{name} has CCN {ccn} (max {args.ccn}) and {length} lines (max {args.length}): '
              f'split it, or keep it below the limits')

    checked = f'{len(files)} changed file(s) since {base}'
    if findings:
        print(f'{findings} new or changed function(s) above the limits in {checked}')
    else:
        print(f'no new or changed function above the limits in {checked}')
    return 1 if findings and args.fail else 0


if __name__ == '__main__':
    sys.exit(main())
