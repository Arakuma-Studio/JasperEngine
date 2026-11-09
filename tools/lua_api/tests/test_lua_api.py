# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""tools/lua_api (S23 AC19): the JSON is well formed and deterministic, every binding resolves,
the emitters read only it, the completion keeps what the regex parser knew, and the Lua doc test
passes on the repository's docs and fails with file:line on a bad one.

    cd tools && python3 -m unittest discover -s lua_api/tests -t .      (or python3 -m pytest lua_api/tests)
"""

import json
import os
import re
import tempfile
import unittest

from lua_api import doctest, extract, stubs, suggestions

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
        for expected in ('Vector3', 'Engine', 'GameObject', 'Scene', 'UIElement', 'Input'):
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

    def test_modules(self):
        """S34 D4: each binding names the optional modules it is compiled under"""
        classes = {c['name']: c for c in api()['classes']}

        def member(cls, name):
            return next(m for m in classes[cls]['methods'] + classes[cls]['properties'] if m['name'] == name)

        self.assertEqual(classes['UIDocument']['modules'], ['htmlui'])
        self.assertEqual(classes['anime']['modules'], ['htmlui'])
        self.assertEqual(classes['Rigidbody']['modules'], ['physics3d'])
        self.assertEqual(classes['Rigidbody2D']['modules'], ['physics2d'])
        self.assertEqual(classes['Vector3']['modules'], [])
        self.assertEqual(classes['GameObject']['modules'], [])
        # members under a module's condition on classes outside it
        self.assertEqual(member('GameObject', 'getRigidbody')['modules'], ['physics3d'])
        self.assertEqual(member('InputActions', 'bindVirtualStick')['modules'], ['htmlui'])
        self.assertEqual(member('UIElement', 'canPlayType')['modules'], ['video'])
        self.assertEqual(member('GameObject', 'setPosition')['modules'], [])
        enums = {e['name']: e for e in api()['enums']}
        self.assertEqual(enums['Joint3DType']['modules'], ['physics3d'])
        # the sound layer's names, which ModuleUsage reads to keep the module in a Compile build
        self.assertEqual(classes['Audio']['modules'], ['soundlayer'])
        self.assertEqual(classes['SoundHandle']['modules'], ['soundlayer'])
        self.assertEqual(enums['AudioPlayOn']['modules'], ['soundlayer'])
        # the music (S39 R19): every name on the soundlayer class
        audio = {m['name'] for m in classes['Audio']['methods'] + classes['Audio']['properties']}
        for name in ('playMusic', 'stopMusic', 'advanceMusic', 'playStinger', 'getMusic', 'getMusicTime', 'getMusicPosition',
                     'onMusicBeat', 'onMusicSection'):
            self.assertIn(name, audio)
            self.assertTrue(set(member('Audio', name)['modules']) <= {'soundlayer'}, name)
        for c in classes.values():
            for key in ('modules',):
                self.assertIsInstance(c[key], list)
                for m in c['methods'] + c['properties']:
                    self.assertTrue(set(m['modules']) <= {'htmlui', 'video', 'physics2d', 'physics3d', 'soundlayer'}, m['name'])


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

    # What the regex parser listed wrongly, so the one parser has it differently or not at all:
    #  - static properties bound with a lambda (Input.KEY_A, AABB.BoxType) were 'Event's; they are
    #    constants ('Constant'), the one parser's real events stay 'Event';
    #  - its C++ methods came from the first C++ class of the same name in any header: the Lua
    #    AssetRef is ScriptAssetRef (not asset/AssetRef.h), Image and Text are the scene UI classes
    #    (not htmlui's DecoratorCommon Image or CssAst Text).
    REGEX_FALSE_CPP_METHODS = {
        'AssetRef': {'clear', 'empty', 'isSet', 'path', 'set'},
        'Image': {'colour', 'hasIntrinsicSize', 'intrinsicSize', 'isSvg', 'texture', 'tintsWithColor'},
        'Text': {'compare', 'str', 'substr'},
    }

    # What later specs changed on purpose since the snapshot (the API's history, not the parser's):
    #  - S20 replaced the particle system: the legacy component's fields and the Lua classes of its
    #    initializers, modifiers and colour gradient are gone (deprecated setters stay on the
    #    ParticleSystem object, S20 D9), and so is ActionSystem::particleActionReset;
    #  - the lattice merge moved setTimes/setEasings/setEasing to KeyframeTracks, where every
    #    tracks class inherits them (a base class's member counts for the derived one);
    #  - mobile-shipping retired built-in AdMob (ads go through the native bridge).
    ADMOB_SYSTEM_STATICS = {'initializeAdMob', 'setMaxAdContentRating', 'loadInterstitialAd', 'isInterstitialAdLoaded',
                            'showInterstitialAd'}
    S20_LEGACY_PARTICLE_FIELDS = {
        'accelerationInitializer', 'accelerationModifier', 'alphaInitializer', 'alphaModifier', 'bursts',
        'colorGradient', 'colorInitializer', 'colorModifier', 'currentBurst', 'emitter', 'lastUsedParticle',
        'lifeInitializer', 'localSpace', 'loop', 'maxPerUpdate', 'newParticlesCount', 'positionInitializer',
        'positionModifier', 'rate', 'rotationInitializer', 'rotationModifier', 'scaleInitializer',
        'scaleModifier', 'sizeInitializer', 'sizeModifier', 'spriteInitializer', 'spriteModifier',
        'velocityInitializer', 'velocityModifier'}

    @classmethod
    def removed_on_purpose(cls, kind, parent, name):
        owner = parent or name
        if re.fullmatch(r'Particle\w*(Initializer|Modifier)|ParticleColorGradient', owner):
            return True
        if parent == 'ParticleSystemComponent' and kind == 'Property' and name in cls.S20_LEGACY_PARTICLE_FIELDS:
            return True
        if owner == 'AdMobRating' or (parent == 'System' and name in cls.ADMOB_SYSTEM_STATICS):
            return True
        # SYNC#4: upstream's collider vectors replaced PhysicsSystem::loadCollider2D
        return (kind, parent, name) in (('CppMethod', 'ActionSystem', 'particleActionReset'),
                                        ('CppMethod', 'PhysicsSystem', 'loadCollider2D'))

    def test_suggestions_keep_what_the_regex_parser_knew(self):
        """generate_api_suggestions.py's symbols at its deletion (fixtures/regex_symbols.txt: kind
        parent name): the one parser's completion has every one of them, but for the regex
        parser's mistakes and the later API changes above"""
        new = {(k, parent, name) for name, k, _, parent in suggestions.symbols(api())}
        bases = {c['name']: c.get('base') for c in api()['classes']}

        def known(kind, parent, name):
            seen = set()
            while parent and parent not in seen:
                if (kind, parent, name) in new:
                    return True
                seen.add(parent)
                parent = bases.get(parent)
            return False
        missing = []
        with open(os.path.join(FIXTURES, 'regex_symbols.txt'), encoding='utf-8') as f:
            for line in f:
                line = line.rstrip('\n')
                if not line or line.startswith('#'):
                    continue
                kind, parent, name = line.split('\t')
                if (kind, parent, name) in new or known(kind, parent, name):
                    continue
                if self.removed_on_purpose(kind, parent, name):
                    continue
                if kind == 'Event' and ('Constant', parent, name) in new:
                    continue
                if kind == 'CppMethod' and name in self.REGEX_FALSE_CPP_METHODS.get(parent, ()):
                    continue
                # a C++-only method since bound to Lua (SYNC#4: Rigidbody.setType, System.syncFileSystem...)
                if kind == 'CppMethod' and any(known(k, parent, name) for k in ('Method', 'StaticMethod', 'Property', 'StaticProperty')):
                    continue
                missing.append(line)
        self.assertEqual(missing, [], 'symbols the regex parser listed and the one parser does not')

    def test_sprite_sheets_and_atlases(self):
        """S45 R36 (AC17): the API lists Sprite.drawMode and its SpriteDrawMode values, and the sheet
        members an atlas is used through (Sprite.sheet/frameName/frameCount, SpriteSheet.load/getFrame)"""
        classes = {c['name']: c for c in api()['classes']}
        enums = {e['name']: e for e in api()['enums']}
        sprite = {p['name'] for p in classes['Sprite']['properties']}
        self.assertTrue({'drawMode', 'sheet', 'frame', 'frameName', 'frameCount'} <= sprite, sprite)
        self.assertEqual([v['name'] for v in enums['SpriteDrawMode']['values']], ['SIMPLE', 'SLICED'])
        sheet = classes['SpriteSheet']
        self.assertTrue({'load', 'getFrame', 'findFrame'} <= {m['name'] for m in sheet['methods']})
        self.assertIn('frameCount', {p['name'] for p in sheet['properties']})
        self.assertTrue(any(m['name'] == 'load' and m['static'] for m in sheet['methods']), 'SpriteSheet.load is static')

    def test_pixel_particles(self):
        """S49 R40 (AC12): ps.pixelArt, ps.simulationMode, ps:getAnalyticParticle, and setRamp on a
        module's colour (ParticleSystemGradient, live) and on a MinMaxGradient value"""
        classes = {c['name']: c for c in api()['classes']}
        ps = classes['ParticleSystem']
        props = {p['name']: p for p in ps['properties']}
        self.assertEqual(props['pixelArt']['type'], 'ParticleSystemPixelArtModule')
        self.assertIn('simulationMode', props)
        self.assertIn('getAnalyticParticle', {m['name'] for m in ps['methods']})
        self.assertIn('setRamp', {m['name'] for m in classes['ParticleSystemGradient']['methods']})
        gradient = classes['MinMaxGradient']
        self.assertIn('setRamp', {m['name'] for m in gradient['methods']})
        self.assertTrue({'ramp', 'rampSteps', 'rampReverse', 'rampBias'} <= {p['name'] for p in gradient['properties']})

    def test_lua_only_marked(self):
        """C++ completion offers Engine::deltatime only if C++ has it (Doriax 755972f92): a static
        property backed by an accessor or a lambda, and a method bound to a helper, are Lua only"""
        classes = {c['name']: c for c in api()['classes']}

        def prop(cls, name):
            return next(p for p in classes[cls]['properties'] if p['name'] == name)

        def method(cls, name):
            return next(m for m in classes[cls]['methods'] if m['name'] == name)
        self.assertTrue(prop('Engine', 'deltatime').get('luaOnly'))
        self.assertTrue(prop('Input', 'KEY_SPACE').get('luaOnly'))
        self.assertFalse(prop('Vector3', 'ZERO').get('luaOnly'))
        self.assertTrue(method('GameObject', 'getParticleSystemsInChildren').get('luaOnly'))
        self.assertFalse(method('Vector3', 'lerp').get('luaOnly'))
        only = suggestions.lua_only(api())
        self.assertIn(('Engine', 'deltatime', 'Constant'), only)
        self.assertIn('{"deltatime", "Constant", "Engine.deltatime", "Engine", true}', suggestions.header_text(api()))

    def test_no_bool_true_default_lost(self):
        """LuaBridge reads a missing bool as false: a method bound straight to a C++ function whose
        bool parameter defaults to true loses the default from Lua (actionStart's newRun, loadGLTF's
        changeRootTransform). Such a binding takes std::optional<bool> and value_or(true)."""
        lost = []
        for c in api()['classes']:
            for m in c['methods']:
                # an overload set picks by argument count: its full form gets every argument
                if len(m['overloads']) != 1:
                    continue
                cpp = m['overloads'][0].get('cpp') or {}
                for q in cpp.get('params', []):
                    if q['type'] == 'bool' and q.get('default') == 'true':
                        lost.append(f"{c['name']}.{m['name']}({q['name']})")
        self.assertEqual(lost, [])

    def test_suggestions_header(self):
        text = suggestions.header_text(api())
        self.assertIn('getEngineAPISymbols', text)
        self.assertIn('{"Vector3", "Class", "class Vector3", ""}', text)
        self.assertIn('"CppMethod"', text)


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
