#!/usr/bin/env python3
"""Summarize lizard, jscpd and clang-tidy reports as Markdown for the job summary.

Usage: quality_summary.py <reports-dir> <source-root>

Expects in <reports-dir>: lizard.csv, jscpd/jscpd-report.json and clang-tidy.txt.
Missing reports are listed as unavailable. Findings never make the script fail.
"""
import csv
import json
import os
import re
import sys
from collections import Counter

# Functions above any of these limits are listed
MAX_CCN = 15
MAX_LENGTH = 100
MAX_PARAMS = 6

TIDY_LINE = re.compile(r'^(?P<file>/[^:]+):(?P<line>\d+):(?P<col>\d+): warning: (?P<msg>.*) \[(?P<check>[^\]]+)\]$')


def relative(path, root):
    path = os.path.normpath(path)
    return os.path.relpath(path, root) if os.path.isabs(path) else path


def lizard_section(path, root):
    out = ['## Complexity (lizard)', '']
    if not os.path.exists(path):
        return out + ['_Report not available._', '']
    functions = []
    with open(path, newline='') as f:
        # nloc, ccn, tokens, params, length, location, file, name, long_name, start, end
        for row in csv.reader(f):
            if len(row) < 11 or not row[0].isdigit():
                continue
            functions.append({
                'nloc': int(row[0]), 'ccn': int(row[1]), 'params': int(row[3]), 'length': int(row[4]),
                'file': relative(row[6], root), 'name': row[7], 'line': int(row[9]),
            })
    if not functions:
        return out + ['_No functions found._', '']
    over = [fn for fn in functions
            if fn['ccn'] > MAX_CCN or fn['length'] > MAX_LENGTH or fn['params'] > MAX_PARAMS]
    over.sort(key=lambda fn: fn['ccn'], reverse=True)
    avg_ccn = sum(fn['ccn'] for fn in functions) / len(functions)
    out += [f'{len(functions)} functions, average CCN **{avg_ccn:.1f}**, '
            f'**{len(over)}** above the limits (CCN > {MAX_CCN}, length > {MAX_LENGTH} lines, '
            f'more than {MAX_PARAMS} parameters).', '']
    if over:
        out += ['| Function | Location | CCN | Lines | Params |', '|---|---|---:|---:|---:|']
        out += [f"| `{fn['name']}` | {fn['file']}:{fn['line']} | {fn['ccn']} | {fn['length']} | {fn['params']} |"
                for fn in over]
        out.append('')
    return out


def jscpd_section(path, root):
    out = ['## Duplication (jscpd)', '']
    if not os.path.exists(path):
        return out + ['_Report not available._', '']
    with open(path) as f:
        report = json.load(f)
    total = report['statistics']['total']
    out += [f"**{total['percentage']} %** duplicated lines ({total['duplicatedLines']} of {total['lines']}) "
            f"in {total['clones']} clones across {total['sources']} files.", '']
    clones = sorted(report.get('duplicates', []), key=lambda d: d['lines'], reverse=True)
    if clones:
        out += ['| Lines | First copy | Second copy |', '|---:|---|---|']
        for d in clones:
            a, b = d['firstFile'], d['secondFile']
            out.append(f"| {d['lines']} | {relative(a['name'], root)}:{a['start']}-{a['end']} "
                       f"| {relative(b['name'], root)}:{b['start']}-{b['end']} |")
        out.append('')
    return out


def tidy_section(path, root):
    out = ['## Static analysis (clang-tidy)', '']
    if not os.path.exists(path):
        return out + ['_Report not available._', '']
    source_dir = os.path.join(root, 'thinger') + os.sep
    warnings = set()
    with open(path, errors='replace') as f:
        for line in f:
            m = TIDY_LINE.match(line.rstrip('\n'))
            if not m:
                continue
            file = os.path.normpath(m['file'])
            # Analyzer paths can end inside third-party headers: keep project code only
            if not file.startswith(source_dir):
                continue
            warnings.add((relative(file, root), int(m['line']), m['check'], m['msg']))
    by_check = Counter(w[2] for w in warnings)
    out += [f'**{len(warnings)}** warnings in project code.', '']
    if warnings:
        out += ['| Check | Count |', '|---|---:|']
        out += [f'| `{check}` | {count} |' for check, count in by_check.most_common()]
        out += ['', '<details><summary>All warnings</summary>', '']
        out += [f'- {file}:{line} `{check}`: {msg}' for file, line, check, msg in sorted(warnings)]
        out += ['', '</details>', '']
    return out


def main():
    reports, root = sys.argv[1], os.path.abspath(sys.argv[2])
    lines = ['# Code quality', '', '_Reported only: these findings do not fail the build._', '']
    lines += lizard_section(os.path.join(reports, 'lizard.csv'), root)
    lines += jscpd_section(os.path.join(reports, 'jscpd', 'jscpd-report.json'), root)
    lines += tidy_section(os.path.join(reports, 'clang-tidy.txt'), root)
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
