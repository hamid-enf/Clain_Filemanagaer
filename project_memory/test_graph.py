#!/usr/bin/env python3
"""Regression checks for the inventory and drift detector; no repository writes."""
import tempfile
import unittest
from pathlib import Path

import graph


class GraphTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.lib = self.root / graph.LIB
        (self.lib / 'src').mkdir(parents=True)
        (self.lib / 'include').mkdir()
        (self.lib / 'third_party' / 'vendor').mkdir(parents=True)
        (self.lib / 'test' / 'vectors').mkdir(parents=True)
        (self.lib / 'include' / 'hello.h').write_text('int hello(void);\n')
        (self.lib / 'src' / 'hello.c').write_text('#include "hello.h"\nint hello(void) { return 1; }\n')
        (self.lib / 'third_party' / 'vendor' / 'external.c').write_text('void vendor(void) {}')
        (self.lib / 'test' / 'vectors' / 'asset.c').write_text('fixture')

    def tearDown(self):
        self.tmp.cleanup()

    def test_inventory_and_include_candidates(self):
        snap = graph.snapshot(self.root)
        self.assertEqual(len(snap['files']), 2)
        self.assertEqual(snap['vendor_directories'], ['vendor'])
        item = next(x for x in snap['files'] if x['path'].endswith('hello.c'))
        self.assertEqual(item['includes'][0]['candidates'], [f'{graph.LIB}/include/hello.h'])
        self.assertEqual(item['definitions_hint'][0]['name'], 'hello')
        self.assertEqual(item['definitions_hint'][0]['line'], 2)
        self.assertIn('hello.c', graph.module_index(snap))

    def test_drift_add_remove_modify(self):
        before = graph.snapshot(self.root)
        (self.lib / 'src' / 'hello.c').write_text('int hello(void) { return 2; }\n')
        (self.lib / 'include' / 'hello.h').unlink()
        (self.lib / 'src' / 'new.c').write_text('void new_fn(void) {}\n')
        (self.lib / 'third_party' / 'other').mkdir()
        changes = graph.diff_snapshot(before, graph.snapshot(self.root))
        self.assertTrue(any(x.startswith('changed: ') and x.endswith('hello.c') for x in changes))
        self.assertTrue(any(x.startswith('removed: ') and x.endswith('hello.h') for x in changes))
        self.assertTrue(any(x.startswith('added: ') and x.endswith('new.c') for x in changes))
        self.assertIn('vendor directory list changed', changes)

    def test_local_link_validation(self):
        docs = self.root / 'project_memory'
        docs.mkdir()
        (docs / 'one.md').write_text('[good](../Clain_FileManager/FileManager_lib/include/hello.h)\n[bad](missing.md)\n[external](https://example.com)\n')
        errors = graph.link_errors(self.root)
        self.assertEqual(len(errors), 1)
        self.assertIn('missing.md', errors[0])

    def test_conditional_includes_are_not_treated_as_active(self):
        (self.lib / 'src' / 'conditional.c').write_text('#if 0\n#include "hello.h"\n#endif\n')
        item = next(x for x in graph.snapshot(self.root)['files'] if x['path'].endswith('conditional.c'))
        self.assertEqual(item['includes'][0]['spelling'], 'hello.h')
        self.assertIn('lexical', graph.snapshot(self.root)['limitations'])


if __name__ == '__main__':
    unittest.main()
