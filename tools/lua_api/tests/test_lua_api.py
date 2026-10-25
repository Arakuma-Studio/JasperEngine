# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""tools/lua_api (S23 AC19): the JSON is well formed and deterministic, every binding resolves,
the emitters read only it, the completion keeps what the regex parser knew, and the Lua doc test
passes on the repository's docs and fails with file:line on a bad one.

    cd tools && python3 -m unittest discover -s lua_api/tests -t .      (or python3 -m pytest lua_api/tests)
"""

import json
import os
import tempfile
import unittest

from lua_api import doctest, extract, markdown, stubs, suggestions

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
BINDINGS = os.path.join(REPO, 'engine', 'core', 'script', 'binding')
CORE = os.path.join(REPO, 'engine', 'core')
FIXTURES = os.path.join(os.path.dirname(__file__), 'fixtures')

_API = None


def api():
    global _API
    if _API is None:
        _API = extract.extract(BINDINGS, CORE, 'test')
    return _API


def check_schema(test, data):
    """The fields of S23 R29's schema, with their types"""
    test.assertEqual(data['format'], 'jasper-lua-api')
    test.assertEqual(data['formatVersion'], 1)
    for key, kind in (('engineVersion', str), ('hash', str), ('classes', list), ('enums', list), ('globals', list),
                      ('events', list), ('unresolved', list)):
        test.assertIsInstance(data[key], kind, key)
    test.assertRegex(data['hash'], r'^[0-9a-f]{64}$')
    for c in data['classes']:
        for key, kind in (('name', str), ('cpp', str), ('file', str), ('line', int), ('doc', list), ('constructors', list),
                          ('methods', list), ('properties', list), ('cppOnlyMethods', list)):
            test.assertIsInstance(c[key], kind, f'{c.get("name")}.{key}')
        test.assertTrue(c['base'] is None or isinstance(c['base'], str))
        for m in c['methods']:
            test.assertIsInstance(m['name'], str)
            test.assertIsInstance(m['static'], bool)
            for o in m['overloads']:
                test.assertIsInstance(o['returns'], list)
                for p in o['params']:
                    test.assertEqual(set(p) - {'description'}, {'name', 'type', 'optional'})
        for p in c['properties']:
            for key in ('name', 'type'):
                test.assertIsInstance(p[key], str)
            for key in ('readonly', 'static', 'copy'):
                test.assertIsInstance(p[key], bool)
    for en in data['enums']:
        test.assertIsInstance(en['name'], str)
        for v in en['values']:
            test.assertIsInstance(v['name'], str)


class LuaApiJson(unittest.TestCase):

    def test_schema(self):
        check_schema(self, api())
        names = {c['name'] for c in api()['classes']}
        for expected in ('Vector3', 'Engine', 'GameObject', 'Scene', 'UIElement', 'Timer'):
            self.assertIn(expected, names)

    def test_deterministic(self):
        again = extract.extract(BINDINGS, CORE, 'test')
        self.assertEqual(api()['hash'], again['hash'])
        self.assertEqual(extract.dumps(api()), extract.dumps(again))
        # the hash names the API, not the editor build
        other = extract.extract(BINDINGS, CORE, 'another-version')
        self.assertEqual(api()['hash'], other['hash'])

    def test_nothing_unresolved(self):
        self.assertEqual(api()['unresolved'], [])

    def test_round_trip_through_a_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'lua-api.json')
            extract.write_if_changed(path, extract.dumps(api()))
            loaded = extract.load(path)
            self.assertEqual(loaded['hash'], api()['hash'])
            self.assertEqual(extract.content_hash(loaded), api()['hash'])

    def test_vector3(self):
        v3 = next(c for c in api()['classes'] if c['name'] == 'Vector3')
        self.assertTrue(v3['constructors'])
        self.assertIn('length', {m['name'] for m in v3['methods']})
        self.assertIn('x', {p['name'] for p in v3['properties']})
        self.assertTrue(v3['file'].endswith('.cpp'))
        self.assertGreater(v3['line'], 0)


class Emitters(unittest.TestCase):

    def test_stubs(self):
        with tempfile.TemporaryDirectory() as tmp:
            files, emitter = stubs.write_stubs(api(), os.path.join(tmp, 'stubs.h'), os.path.join(tmp, 'lua'))
            self.assertIn('Vector3.lua', files)
            self.assertIn('Globals.lua', files)
            self.assertIn('onReload', files['Script.lua'])
            self.assertIn('ScriptReloadInfo', files['Script.lua'])
            self.assertIn('function Editor.call(tool, args, callback) end', files['EditorScript.lua'])
            self.assertIn('HtmlUITypes.lua', files)
            self.assertFalse(any('-- unresolved:' in text for text in files.values()))
            self.assertTrue(os.path.exists(os.path.join(tmp, 'lua', stubs.LUALS_PLUGIN_NAME)))
            self.assertIn('GameObject', emitter.entity_handle_classes())
            with open(os.path.join(tmp, 'stubs.h'), encoding='utf-8') as f:
                header = f.read()
            self.assertIn('stubFiles[]', header)
            self.assertIn('entityHandleClasses[]', header)

    def test_suggestions_keep_what_the_regex_parser_knew(self):
        """generate_api_suggestions.py's symbols at its deletion (fixtures/regex_symbols.txt: kind
        parent name): the one parser's completion has every one of them"""
        new = {(k, parent, name) for name, k, _, parent in suggestions.symbols(api())}
        missing = []
        with open(os.path.join(FIXTURES, 'regex_symbols.txt'), encoding='utf-8') as f:
            for line in f:
                line = line.rstrip('\n')
                if not line or line.startswith('#'):
                    continue
                kind, parent, name = line.split('\t')
                if (kind, parent, name) not in new:
                    missing.append(line)
        self.assertEqual(missing, [], 'symbols the regex parser listed and the one parser does not')

    def test_suggestions_header(self):
        text = suggestions.header_text(api())
        self.assertIn('getEngineAPISymbols', text)
        self.assertIn('{"Vector3", "Class", "class Vector3", ""}', text)
        self.assertIn('"CppMethod"', text)

    def test_markdown(self):
        text = markdown.reference(api())
        self.assertTrue(text.startswith('# Lua API reference'))
        self.assertIn('### Vector3', text)
        self.assertIn('## Enums', text)


class DocTest(unittest.TestCase):

    def test_repository_docs_pass(self):
        files = doctest.doc_files(REPO)
        self.assertTrue(any(f.endswith('lua-api.md') for f in files))
        failures = doctest.check(files, api(), REPO, compiler=None)
        self.assertEqual(failures, [])

    def test_bad_doc_names_file_line_and_symbol(self):
        path = os.path.join(FIXTURES, 'bad_doc.md')
        failures = doctest.check([path], api(), REPO, compiler=None)
        self.assertTrue(any(f.endswith('bad_doc.md:7: unknown symbol Vector3:nope') for f in failures), failures)
        self.assertTrue(any('unknown symbol Engine.nopeField' in f for f in failures), failures)
        self.assertEqual(len(failures), 2, failures)

    def test_runtime_blocks(self):
        blocks = doctest.runtime_blocks(doctest.doc_files(REPO), REPO)
        self.assertTrue(blocks, 'the docs have ```lua run blocks (the MCP test runs them)')


if __name__ == '__main__':
    unittest.main()
