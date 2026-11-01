#!/usr/bin/env python3
"""Structure budget: measure the guardrails H1-H10 of the library with greps, against structure-budget.json.

Usage: structure_budget.py [--warn-only] [--root DIR] [--budget FILE]

Budgets only ratchet down. A value over its budget fails; so does a value more than 5 % under a
budget that was not lowered (any value under a budget of 20 or less), so that a change that
improves a value also lowers its budget. H6 counts what must grow, so its rule is the mirror. H10
(file sizes) only warns. Lower a budget by editing its number in structure-budget.json; raising
one is a decision reviewed by the code owners (.github/CODEOWNERS). With --warn-only (pushes,
local use) findings are warnings and the exit status is 0; a broken budget file or manifest
always fails.
A table goes to stdout, and to $GITHUB_STEP_SUMMARY when it is set.

Each metric is a row of the target design's §6. Sources are thinger/**/*.hpp and *.cpp, with
comments ignored. "Public headers" are every header while public-headers.txt is a draft, and the
manifest's entries afterwards.
H1  headers            every header
    public             the public headers
    dead               headers that no file of thinger/, tests/, examples/ or benchmark/ includes,
                       except APPLICATION_ONLY (headers only the applications include)
H2  singletons         `static T& instance()` declarations
    singleton_uses     other `instance()` calls (not the declaration or the definition)
    get_workers        `get_workers()` calls (not the declaration or the definition)
    namespace_mutables mutable variables at namespace scope in .cpp files: the linger atomics
    static_counters    `static std::atomic<...>` data members (the logger's sink is not atomic)
H3  max_timers         `time_point::max()`
    generation_counters  integer members named *generation* declared in headers
    detached           fire-and-forget `co_spawn(` calls not allowed by an entry {file, spawns, reason}
                       of the budget file's _detached_allow_list. A call is classified by its completion
                       token, its last argument: awaited (use_awaitable, use_future, possibly wrapped)
                       and bound to a cancellation slot of its owner (bind_cancellation_slot(...))
                       are not counted; anything else (detached, a lambda, a function or function
                       object) is fire-and-forget. An entry allows one fire-and-forget call of `file`
                       naming `spawns` between `co_spawn(` and its closing parenthesis (an entry that
                       allows none breaks the budget file). Self-test: structure_budget_test.py
    detached_allowed   the entries of the allow-list (owned writers, loops and closing handshakes,
                       each with its owner and how it ends; measured at 0.9: 13)
H4  socket_casts       dynamic_cast/*_pointer_cast under http/ to a *_socket type, or of a
                       socket's get_attachment()
    frame_casts        the same casts to frame types (http_frame, http_response, http_request,
                       headers, http_data, out_*)
H5  (deleted in 9.7: http_stream owns the disposition and the close_reason table test keeps the rule)
H6  matrix_cases       TEST_CASEs tagged [matrix] in tests/ (must grow: the mirror rule)
    matrix_suites      the suites (files) they are in, sse_framing_test part of sse_test (§7.1)
H7  using_namespace    `using namespace` in public headers
    logger_includes    public headers that include util/logger.hpp (its macros)
    camel_case         camelCase function declarations in public headers
    (each public header compiling alone is a CMake target, not measured here: "not checked")
H8  error_fields       `std::string error` fields of `*_result` types in public headers
    last_error         `last_error()` accessors declared in public headers
    optional_returns   overload sets in public headers returning [awaitable<]std::optional<*_client
                       or *_connection>[>] (today the four websocket()/upgrade_websocket())
H9  server_client      includes between http/server/ and http/client/, either way (N6)
    asio_http          includes of http/ from asio/
    common_up          includes of http/server/ or http/client/ from http/common/ (a §3.2 rule
                       that the "Today" column of §6 does not count)
    public_detail      includes of a detail/ header from a public header
H10 <file>             lines of each file of SIZE_FILES, found by name under thinger/
"""
import argparse
import bisect
import json
import os
import re
import sys
from pathlib import Path

SOURCE_DIRS = ('thinger', 'tests', 'examples', 'benchmark')
SOURCE_SUFFIXES = ('.hpp', '.cpp', '.h', '.cc', '.ipp')
SKIPPED_DIRS = ('build', 'cmake-build', '_deps')         # prefixes of directory names
APPLICATION_ONLY = {'util/hex.hpp'}                       # the core includes it (§2 N7, §3.1)
# H10 metric: its file names, the first found wins (websocket_session.cpp is websocket.cpp today)
SIZE_FILES = {'server_connection.cpp': ('server_connection.cpp',), 'response.cpp': ('response.cpp',),
              'websocket_session.cpp': ('websocket_session.cpp', 'websocket.cpp'),
              'http_client_base.cpp': ('http_client_base.cpp',), 'http_application.cpp': ('http_application.cpp',)}
STALE_PERCENT = 5
SMALL_BUDGET = 20                    # at or under it, any value under the budget is stale

LEXEMES = re.compile(r'R"(\w*)\(.*?\)\1"|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])+\'|//[^\n]*|/\*.*?\*/', re.S)
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
CAST = re.compile(r'(?:dynamic_cast|\w+_pointer_cast)\s*<\s*(?:const\s+)?([\w:]+)[^>]*>\s*\(([^;]*)')
MATRIX_CASE = re.compile(r'^\s*(?:TEST_CASE|TEMPLATE_TEST_CASE)\w*\s*\([^{;]*\[matrix\]', re.M)   # tags may wrap
# A completion token that its caller awaits, possibly wrapped (as_tuple(use_awaitable))
AWAITED = re.compile(r'(?:[\w:]+\s*\(\s*)*(?:[\w:]*::)?(?:use_awaitable|use_nothrow_awaitable|use_future)\b')
FRAME_TYPES = re.compile(r'(?:^|::)(?:http_frame|http_response|http_request|headers|http_data|out_\w+)$')


def strip_comments(text, literals=False):
    """The text with comments blanked out (line numbers kept), and string and character literals
    kept, or replaced with 0 if literals is set"""
    def blank(match):
        lexeme = match.group()
        if lexeme[0] in '"\'R':
            return '0' if literals else lexeme
        return re.sub(r'[^\n]', ' ', lexeme)
    return LEXEMES.sub(blank, text)


class Tree:
    def __init__(self, root, manifest):
        self.files = {}
        for top in SOURCE_DIRS:
            for dirpath, dirnames, filenames in os.walk(root / top):
                dirnames[:] = [d for d in dirnames if not d.startswith(SKIPPED_DIRS)]
                for name in filenames:
                    if name.endswith(SOURCE_SUFFIXES):
                        path = Path(dirpath, name)
                        text = path.read_text(errors='replace')
                        self.files[path.relative_to(root).as_posix()] = (text, strip_comments(text))
        self.headers = sorted(p for p in self.files if p.startswith('thinger/') and p.endswith('.hpp'))
        self.public = [p for p in self.headers if manifest is None or p[len('thinger/'):] in manifest]

    def code(self, prefix='thinger/', suffixes=('.hpp', '.cpp')):
        return [(p, c) for p, (_, c) in self.files.items() if p.startswith(prefix) and p.endswith(suffixes)]

    def public_code(self):
        return [(p, c) for p, c in self.code(suffixes=('.hpp',)) if p in self.public]

    def count(self, pattern, prefix='thinger/', suffixes=('.hpp', '.cpp'), exclude=None):
        regex = re.compile(pattern, re.M)
        return sum(len(regex.findall(line)) for _, code in self.code(prefix, suffixes) for line in code.splitlines()
                   if not (exclude and re.search(exclude, line)))

    def includes(self, path):
        """The files of the tree that path includes (relative to it, or to the root)"""
        found = []
        for name in INCLUDE.findall(self.files[path][0]):
            for candidate in (os.path.normpath(os.path.join(os.path.dirname(path), name)), os.path.normpath(name)):
                if candidate in self.files and candidate != path:
                    found.append(candidate)
                    break
        return found


def namespace_scope_statements(code):
    """The statements of code that are at namespace scope (outside classes, functions, initializers)"""
    code = strip_comments(code, literals=True)
    stack, start, statements = [], 0, []
    for match in re.finditer(r'[{};]', code):
        char = match.group()
        if char == '{':
            namespace = all(stack) and bool(re.search(r'\bnamespace\b[\w\s:]*$', code[start:match.start()]))
            stack.append(namespace)
            start = match.end() if namespace else start
        elif char == '}':
            namespace = stack.pop() if stack else True
            # A function body ends its statement; a brace initializer or a class does not
            if all(stack) and (namespace or not re.match(r'\s*[;,=)]', code[match.end():])):
                start = match.end()
        elif all(stack):
            statements.append(' '.join(code[start:match.end()].split()))
            start = match.end()
    return statements


def is_mutable_variable(statement):
    if re.match(r'(#|using |typedef |template\b|class |struct |enum |friend |extern |namespace\b)', statement):
        return False
    declarator = re.split(r'[={]', statement, maxsplit=1)[0]
    # Constants, function declarations, and static members defined out of their class
    return not re.search(r'\b(const|constexpr|constinit)\b|\(|\w::\w+\s*$', declarator)


def overload_sets(tree, pattern):
    return len({(p, m) for p, code in tree.public_code() for m in re.findall(pattern, code, re.M)})


def result_error_fields(tree):
    count = 0
    for path, code in tree.public_code():
        types = [(m.start(), m.group(1)) for m in re.finditer(r'\b(?:struct|class)\s+(\w+)\s*(?:final\s*)?[:{]', code)]
        for field in re.finditer(r'^\s*std::string\s+error\s*[;={]', code, re.M):
            index = bisect.bisect([start for start, _ in types], field.start()) - 1
            count += index >= 0 and types[index][1].endswith('_result')
    return count


def casts(tree, frame):
    count = 0
    for _, code in tree.code('thinger/http/'):
        for target, operand in CAST.findall(code):
            is_frame = bool(FRAME_TYPES.search(target))
            is_socket = target.endswith('_socket') or 'get_attachment(' in operand
            count += is_frame if frame else is_socket and not is_frame
    return count


def layer_includes(tree, sources, targets):
    return sum(1 for path in tree.files if path.startswith(sources)
               for include in tree.includes(path) if include.startswith(targets))


def spawns(code):
    """(call, kind) of each co_spawn( call of code: its text up to its closing parenthesis, and
    awaited, bound or detached (fire-and-forget) by its completion token, its last argument"""
    code = strip_comments(code, literals=True)
    for call in re.finditer(r'\bco_spawn\s*\(', code):
        depth, last = 0, call.end()
        for match in re.compile(r'[()\[\]{},]').finditer(code, call.end()):
            char = match.group()
            if char in '([{':
                depth += 1
            elif char in ')]}':
                if depth == 0:
                    break
                depth -= 1
            elif depth == 0:
                last = match.end()
        else:
            continue  # unbalanced (a macro, a partial file): not a call
        token = code[last:match.start()].strip()
        if AWAITED.match(token):
            kind = 'awaited'
        elif re.match(r'(?:[\w:]*::)?bind_cancellation_slot\s*\(', token):
            kind = 'bound'
        else:
            kind = 'detached'
        yield code[call.start():match.end()], kind


def detached_outside(tree, allow_list):
    """The fire-and-forget spawns not allowed; fails on an unused entry"""
    unused, outside = list(allow_list), 0
    for path, code in tree.code():
        for statement, kind in spawns(code):
            if kind != 'detached':
                continue
            entry = next((e for e in unused if e['file'] == path and re.search(rf'\b{re.escape(e["spawns"])}\b', statement)), None)
            if entry:
                unused.remove(entry)
            outside += entry is None
    if unused:
        names = ', '.join(f'{e["file"]} ({e["spawns"]})' for e in unused)
        raise SystemExit(f'::error title=Structure budget::_detached_allow_list allows no spawn of: {names}')
    return outside


def measure(tree, allow_list):
    included = {include for path in tree.files for include in tree.includes(path)}
    matrix = {p: len(MATRIX_CASE.findall(code)) for p, code in tree.code('tests/', ('.cpp',))}
    m = {
        'H1.headers': len(tree.headers),
        'H1.public': len(tree.public),
        'H1.dead': sum(1 for h in tree.headers if h not in included and h[len('thinger/'):] not in APPLICATION_ONLY),
        'H2.singletons': tree.count(r'\bstatic\s+[\w:<>]+\s*&\s*instance\s*\(\s*\)', suffixes=('.hpp',)),
        'H2.singleton_uses': tree.count(r'\binstance\s*\(\s*\)', exclude=r'&\s*(\w+::)?instance\s*\(\s*\)\s*[{;]'),
        'H2.get_workers': tree.count(r'\bget_workers\s*\(\s*\)', exclude=r'workers\s*&\s*get_workers\s*\(\s*\)\s*[{;]'),
        'H2.namespace_mutables': sum(1 for _, code in tree.code(suffixes=('.cpp',))
                                     for s in namespace_scope_statements(code) if is_mutable_variable(s)),
        'H2.static_counters': tree.count(r'^\s*static\s+std::atomic\s*<', suffixes=('.hpp',)),
        'H3.max_timers': tree.count(r'time_point::max\s*\(\s*\)'),
        'H3.generation_counters': tree.count(r'^\s*(?:std::)?u?int\w*_t\s+\w*generation\w*\s*[;={]', suffixes=('.hpp',)),
        'H3.detached': detached_outside(tree, allow_list),
        'H3.detached_allowed': len(allow_list),
        'H4.socket_casts': casts(tree, frame=False),
        'H4.frame_casts': casts(tree, frame=True),
        'H6.matrix_cases': sum(matrix.values()),
        'H6.matrix_suites': len({Path(p).stem.replace('sse_framing', 'sse') for p, n in matrix.items() if n}),
        'H7.using_namespace': sum(len(re.findall(r'\busing\s+namespace\b', c)) for _, c in tree.public_code()),
        'H7.logger_includes': sum(1 for p in tree.public
                                  if any(re.search(r'(^|/)util/logger\.hpp$|^logger\.hpp$', i) for i in INCLUDE.findall(tree.files[p][0]))),
        'H7.camel_case': sum(1 for _, c in tree.public_code() for _ in re.finditer(r'^\s*(?:(?:virtual|static|inline|explicit)\s+)*[\w:<>]+[\s*&]+[a-z][a-z0-9]*[A-Z]\w*\s*\(', c, re.M)),
        'H8.error_fields': result_error_fields(tree),
        'H8.last_error': overload_sets(tree, r'\b(last_error)\s*\(\s*\)\s*(?:const\s*)?(?:noexcept\s*)?[{;]'),
        'H8.optional_returns': overload_sets(tree, r'^\s*(?:awaitable<\s*)?std::optional<\s*\w+_(?:client|connection)\s*>\s*>?\s+(\w+)\s*\('),
        'H9.server_client': layer_includes(tree, 'thinger/http/server/', 'thinger/http/client/')
                            + layer_includes(tree, 'thinger/http/client/', 'thinger/http/server/'),
        'H9.asio_http': layer_includes(tree, 'thinger/asio/', 'thinger/http/'),
        'H9.common_up': layer_includes(tree, 'thinger/http/common/', ('thinger/http/server/', 'thinger/http/client/')),
        'H9.public_detail': sum(1 for p in tree.public for i in tree.includes(p) if '/detail/' in i),
    }
    for metric, names in SIZE_FILES.items():
        found = [p for name in names for p in sorted(tree.files) if p.startswith('thinger/') and p.endswith('/' + name)]
        m['H10.' + metric] = tree.files[found[0]][0].count('\n') if found else 0
    return m


def load_budget(path):
    """The budgets and the allow-list of detached spawns"""
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError) as error:
        raise SystemExit(f'::error title=Structure budget::cannot read {path.name}: {error}')
    data = data if isinstance(data, dict) else {}
    allow_list = data.get('_detached_allow_list')
    if not isinstance(allow_list, list) or not all(isinstance(e, dict) and all(isinstance(e.get(k), str) and e[k]
                                                   for k in ('file', 'spawns', 'reason')) for e in allow_list):
        raise SystemExit(f'::error title=Structure budget::{path.name} needs _detached_allow_list, a list of {{file, spawns, reason}}')
    return {k: v for k, v in data.items() if not k.startswith('_')}, allow_list


def check_budget(path, budget, metrics):
    missing = sorted(set(metrics) - set(budget))
    unknown = sorted(set(budget) - set(metrics))
    invalid = sorted(k for k, v in budget.items() if not isinstance(v, int) or isinstance(v, bool) or v < 0)
    for keys, problem in ((missing, 'has no budget for'), (unknown, 'has a budget for an unknown metric'),
                          (invalid, 'needs a whole number >= 0 for')):
        if keys:
            raise SystemExit(f'::error title=Structure budget::{path.name} {problem}: {", ".join(keys)}')


def load_manifest(path, root):
    """The public headers (relative to thinger/), or None while the manifest is a draft"""
    if not path.is_file():
        raise SystemExit(f'::error title=Structure budget::{path.name} is missing')
    lines = [line.strip() for line in path.read_text().splitlines()]
    entries = {line for line in lines if line and not line.startswith('#')}
    missing = sorted(e for e in entries if not (root / 'thinger' / e).is_file())
    if missing:
        raise SystemExit(f'::error title=Structure budget::{path.name} lists missing headers: {", ".join(missing)}')
    return None if any(re.match(r'#\s*status:\s*draft\b', line) for line in lines) else entries


def verdict(metric, value, budget):
    """(level, message) of a value against its budget: level is ok, warn or fail"""
    gap = abs(value - budget)
    stale = gap > 0 and (budget <= SMALL_BUDGET or gap * 100 > STALE_PERCENT * budget)
    if metric.startswith('H10.'):
        if value > budget:
            return 'warn', f'grew from {budget} to {value} lines: review the growth in the change, then raise its ratchet'
        return ('warn', f'shrank to {value} lines: lower its ratchet from {budget}') if stale else ('ok', '')
    grows = metric.startswith('H6.')
    if (value < budget) if grows else (value > budget):
        return 'fail', f'reads {value}, beyond its budget of {budget}: fix the change, or edit the budget (reviewed by the code owners)'
    if stale:
        return 'fail', f'reads {value}, better than its budget of {budget}: set its budget to {value} in structure-budget.json'
    return 'ok', ''


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--warn-only', action='store_true', help='report findings as warnings and exit 0')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2], help='repository root')
    parser.add_argument('--budget', type=Path, help='budget file (default: ROOT/structure-budget.json)')
    args = parser.parse_args()

    root = args.root.resolve()
    tree = Tree(root, load_manifest(root / 'public-headers.txt', root))
    budget_path = args.budget or root / 'structure-budget.json'
    budget, allow_list = load_budget(budget_path)
    metrics = measure(tree, allow_list)
    check_budget(budget_path, budget, metrics)

    rows, failures = [], 0
    for metric, value in metrics.items():
        level, message = verdict(metric, value, budget[metric])
        failures += level == 'fail'
        level = 'warn' if level == 'fail' and args.warn_only else level
        if level != 'ok':
            print(f'::{"error" if level == "fail" else "warning"} title=Structure budget::{metric} {message}')
        rows.append((metric, value, budget[metric], f'{level}: {message}' if message else level))
    rows.insert(rows.index(next(r for r in rows if r[0].startswith('H8.'))),
                ('H7.compiles_alone', '-', '-', 'not checked (a CMake target, task 11.4)'))

    width = max(len(r[0]) for r in rows)
    print(f'{"metric".ljust(width)}  {"value":>5}  {"budget":>6}  status')
    for metric, value, limit, status in rows:
        print(f'{metric.ljust(width)}  {value!s:>5}  {limit!s:>6}  {status}')
    print(f'{len(metrics)} metrics, {failures} over or stale budget(s)' + (' (warnings only)' if args.warn_only else ''))

    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as summary:
            summary.write('### Structure budget\n\n| Metric | Value | Budget | Status |\n|---|---:|---:|---|\n')
            summary.write(''.join(f'| {m} | {v} | {b} | {s} |\n' for m, v, b, s in rows) + '\n')
    return 1 if failures and not args.warn_only else 0


if __name__ == '__main__':
    sys.exit(main())
