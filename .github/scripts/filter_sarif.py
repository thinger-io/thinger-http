#!/usr/bin/env python3
"""Drop from SARIF files the results located outside the project's own code.

Usage: filter_sarif.py <sarif-dir>

CodeQL extracts every C++ file compiled, so the dependencies CMake downloads
(build/_deps) are analyzed too, and the paths-ignore of the CodeQL config does
not apply to compiled languages. Results whose primary location matches an
excluded path are removed; every other result is kept. Files are rewritten in
place.
"""
import fnmatch
import json
import os
import sys

# Same paths as paths-ignore in .github/codeql/codeql-config.yml, plus the
# dependencies wherever they are fetched
EXCLUDED = [
    'build/*',
    'build-*/*',
    '*/_deps/*',
    'tests/*',
    'benchmark/*',
    'examples/*',
]


def excluded(uri):
    return any(fnmatch.fnmatch(uri, pattern) for pattern in EXCLUDED)


def location_uri(result):
    for location in result.get('locations', []):
        uri = location.get('physicalLocation', {}).get('artifactLocation', {}).get('uri')
        if uri:
            return uri
    return None


def filter_file(path):
    with open(path, encoding='utf-8') as f:
        sarif = json.load(f)
    removed = 0
    for run in sarif.get('runs', []):
        results = run.get('results', [])
        kept = [r for r in results if not (location_uri(r) and excluded(location_uri(r)))]
        removed += len(results) - len(kept)
        run['results'] = kept
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(sarif, f)
    return removed


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    directory = sys.argv[1]
    for name in sorted(os.listdir(directory)):
        if name.endswith('.sarif'):
            removed = filter_file(os.path.join(directory, name))
            print(f'{name}: removed {removed} results outside the project code')
    return 0


if __name__ == '__main__':
    sys.exit(main())
