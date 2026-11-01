#!/usr/bin/env python3
"""Self-test of structure_budget.py: guardrail H3 (fire-and-forget spawns) on synthetic sources.

Usage: structure_budget_test.py (unittest options apply, e.g. -v)
"""
import re
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True    # ctest runs it in the source tree
sys.path.insert(0, str(Path(__file__).resolve().parent))
import structure_budget as budget  # noqa: E402

PATH = 'thinger/http/server/example.cpp'


class Sources:
    """A tree of synthetic sources, comments stripped as Tree.code() does"""
    def __init__(self, files):
        self.files = {path: budget.strip_comments(text) for path, text in files.items()}

    def code(self):
        return list(self.files.items())


def outside(text, allow_list=()):
    return budget.detached_outside(Sources({PATH: text}), list(allow_list))


def kinds(text):
    return [kind for _, kind in budget.spawns(text)]


def literal_detached_rule(text):
    """H3 before 0.9 (review finding R2): the `detached` tokens not bound to a slot"""
    code = budget.strip_comments(text)
    return sum(1 for token in re.finditer(r'^(?!\s*(?:#\s*include|using\s)).*?\bdetached\b', code, re.M)
               if 'bind_cancellation_slot' not in code[max(code.rfind('co_spawn', 0, token.end()), 0):token.end()])


LAMBDA = 'void f() { co_spawn(ex, f(), [](std::exception_ptr) {}); }'


class FireAndForget(unittest.TestCase):
    def test_lambda_completion_counts(self):
        self.assertEqual(outside(LAMBDA), 1)

    def test_lambda_completion_escaped_the_literal_rule(self):
        # The case above fails if H3 goes back to counting the word `detached`
        self.assertEqual(literal_detached_rule(LAMBDA), 0)

    def test_detached_counts(self):
        self.assertEqual(outside('co_spawn(ex, loop(), detached);'), 1)
        self.assertEqual(outside('boost::asio::co_spawn(ex, loop(), boost::asio::detached);'), 1)

    def test_function_object_counts(self):
        self.assertEqual(outside('co_spawn(entry.executor, serve_tls(self, std::move(sock), gate, at),\n'
                                 '         log_serve_failure);'), 1)

    def test_multiline_lambda_with_nested_parentheses_and_commas_counts(self):
        text = '''
        co_spawn(strand,
            [this, req, memory]() -> awaitable<void> {
                co_await handle(req, memory, [](const response& res) { use(res, ",)"); });
            },
            [this, memory](const std::exception_ptr& error) {
                if (error) memory->send(make(status::error, "a, b)"), ')');
            });
        '''
        self.assertEqual(kinds(text), ['detached'])
        self.assertEqual(outside(text), 1)

    def test_each_call_counts_once(self):
        self.assertEqual(outside('co_spawn(a, x(), detached);\nco_spawn(b, y(), [](auto) {});'), 2)


class NotFireAndForget(unittest.TestCase):
    def test_awaited(self):
        for token in ('use_awaitable', 'boost::asio::use_future', 'as_tuple(use_awaitable)', 'use_nothrow_awaitable'):
            with self.subTest(token=token):
                self.assertEqual(kinds(f'auto r = co_spawn(ex, op(), {token});'), ['awaited'])
                self.assertEqual(outside(f'auto r = co_spawn(ex, op(), {token});'), 0)
        self.assertEqual(outside('co_return co_await co_spawn(executor, kept(k, std::move(op)), use_awaitable);'), 0)

    def test_bound_to_a_cancellation_slot(self):
        for text in ('co_spawn(ex, loop(), bind_cancellation_slot(signal_.slot(), detached));',
                     'co_spawn(ex, loop(),\n  boost::asio::bind_cancellation_slot(stream ? stream->slot() : slot{},\n'
                     '    [deadline](const std::exception_ptr&) { deadline->wake.set(); }));'):
            with self.subTest(text=text):
                self.assertEqual(kinds(text), ['bound'])
                self.assertEqual(outside(text), 0)

    def test_comments_literals_and_using(self):
        text = '''
        using boost::asio::co_spawn;
        // co_spawn(ex, loop(), detached);
        /* co_spawn(ex, loop(), [](auto) {}); */
        log("co_spawn(ex, loop(), detached)");
        // co_spawned with no slot
        '''
        self.assertEqual(kinds(text), [])
        self.assertEqual(outside(text), 0)


class AllowList(unittest.TestCase):
    def entry(self, spawns, file=PATH):
        return {'file': file, 'spawns': spawns, 'reason': 'test'}

    def test_entry_allows_a_lambda_completed_spawn(self):
        text = 'co_spawn(ex, ws_->close_graceful(code), [self](const std::exception_ptr&) { self->wake(); });'
        self.assertEqual(outside(text, [self.entry('close_graceful')]), 0)

    def test_entry_allows_one_spawn(self):
        text = 'co_spawn(a, run(), detached);\nco_spawn(b, run(), [](auto) {});'
        self.assertEqual(outside(text, [self.entry('run')]), 1)

    def test_entry_of_another_file_allows_nothing(self):
        with self.assertRaises(SystemExit):
            outside(LAMBDA, [self.entry('f', file='thinger/other.cpp')])

    def test_unused_entry_breaks_the_budget_file(self):
        with self.assertRaises(SystemExit):
            outside('co_spawn(ex, loop(), detached);', [self.entry('loop'), self.entry('missing')])

    def test_entry_naming_a_bound_spawn_is_unused(self):
        with self.assertRaises(SystemExit):
            outside('co_spawn(ex, loop(), bind_cancellation_slot(slot, detached));', [self.entry('loop')])


if __name__ == '__main__':
    unittest.main()
