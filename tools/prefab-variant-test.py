#!/usr/bin/env python3
"""Scripted checks of Prefab Variants (editor/project/PREFABS.md, section 19).

    FIXTURE=<TappyPlane fixture project> tools/prefab-variant-test.py [work-dir]

Builds the `variant` fixture (tools/prefab-fixtures.py) and runs jasperengine-editor
scene-dump on it: Engine, Ship (Engine nested in it), Ship Variant made from an instance of
Ship with overrides of every kind, Ship Variant 2 (a variant of the variant) made from an
instance of that one with overrides at both of its levels, a variant made from the Project
window; then base edits in Prefab Mode reaching the variants except where they override,
the apply targets of a chain (to a variant, to the base through a variant that overrides
the value), Revert in a variant's Prefab Mode, Unpack (one level: an instance of the base;
completely), Apply of removed and added GameObjects as variant overrides, duplicate, Play's
spawn, Replace between a prefab and its variants, a save and reload (byte for byte), the
base renamed in the editor and moved with the scene closed, the base deleted (Missing
Prefab instances keeping their data, the variant's Prefab Mode keeping its file) and put
back, and the generated build_prefab_* sources. Exits non-zero on a failure. Needs a Linux
editor build (tools/build.sh linux).
"""
import os, shutil, subprocess, sys, tempfile, yaml

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
EDITOR = os.path.join(ROOT, 'build', 'linux', 'editor-install', 'bin', 'jasperengine-editor')
COMPARE = os.path.join(ROOT, 'tools', 'prefab-dump-compare.py')
WORK = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else tempfile.mkdtemp(prefix='prefab-variant-')
FAILS = []

COLOR = 'submeshes[0].material.baseColorFactor'
SHIP = 'prefabs/Ship.prefab'
V1 = 'prefabs/Ship Variant.prefab'
V2 = 'prefabs/Ship Variant 2.prefab'


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        FAILS.append(msg)


def editor(project, out, ops=None, save=False, name='run', allow_errors=False):
    args = [EDITOR, 'scene-dump', '--project', project, '--out', out]
    if ops:
        args += ['--ops', ops]
    if save:
        args.append('--save')
    res = subprocess.run(args, capture_output=True, text=True, timeout=1800)
    open(os.path.join(WORK, name + '.log'), 'w').write(res.stdout + '\n----\n' + res.stderr)
    if res.returncode != 0:
        print(res.stdout[-3000:], res.stderr[-3000:])
        raise SystemExit('editor failed: ' + name)
    if not allow_errors:
        errors = [l for l in res.stdout.splitlines() if '[ERROR]' in l or '(ERROR)' in l]
        check(not errors, name + ': no error logged' + ('' if not errors else ' (%s)' % errors[0]))
    failed = [l for l in res.stdout.splitlines() if l.strip() == '(failed)']
    check(not failed, name + ': every op ran')
    return res.stdout


def compare(a, b, *flags):
    res = subprocess.run([sys.executable, COMPARE, a, b] + list(flags), capture_output=True, text=True)
    if res.returncode != 0:
        print('\n'.join(res.stdout.splitlines()[-12:]))
    return res.returncode == 0


def load(path):
    return yaml.safe_load(open(path))['scenes'][0]['entities']


def ops_file(name, ops):
    path = os.path.join(WORK, name + '.ops.yaml')
    yaml.safe_dump(ops, open(path, 'w'), sort_keys=False)
    return path


def comp(entities, path, component, field):
    node = (entities.get(path) or {}).get('components', {}).get(component, {})
    if component == 'mesh' and field == 'color':
        try:
            return node['submeshes'][0]['material']['baseColorFactor']
        except (KeyError, IndexError, TypeError):
            return None
    return node.get(field)


def refs(entities, path):
    return (entities.get(path) or {}).get('refs') or {}


def ship_block(path):
    for node in yaml.safe_load(open(path))['entities']:
        if node.get('name') == 'Ship':
            return {k: v for k, v in node.items() if k not in ('children',)}
    return None


def root_block(prefab_file):
    """What a variant overrides of its base: its root's prefabInstance block (keyed by the base's ids)."""
    data = yaml.safe_load(open(prefab_file))
    root = [m for m in data['members'] if m['entity'] == data['root']][0]
    return root.get('prefabInstance') or {}


SHIP_HULL = 1002   # Hull's registry entity in Ship.prefab (the fixture's ids)


def lines_of(log, prefix):
    return [l.strip()[len(prefix):].strip() for l in log.splitlines() if l.strip().startswith(prefix)]


def files_equal(a_dir, b_dir, exts=('.prefab', '.scene')):
    same = True
    for base, dirs, files in os.walk(a_dir):
        for f in files:
            if f.endswith(exts):
                a = os.path.join(base, f)
                b = os.path.join(b_dir, os.path.relpath(a, a_dir))
                if not os.path.exists(b) or open(a, 'rb').read() != open(b, 'rb').read():
                    same = False
                    print('  differs: ' + os.path.relpath(a, a_dir))
    return same


shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK)
FIX = os.path.join(WORK, 'variant')
subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'prefab-fixtures.py'), 'variant', FIX], check=True)
OPS = FIX + '.ops'
P = os.path.join(WORK, 'project')
shutil.copytree(FIX, P)

print('== Ship, Ship Variant and Ship Variant 2 made from instances with overrides')
out = editor(P, os.path.join(OPS, 'after-ops.yaml'), os.path.join(OPS, 'build.yaml'), name='build')
levels = lines_of(out, 'levels:')
check(levels[:2] == ['prefabs/Ship Variant.prefab prefabs/Ship.prefab',
                     'prefabs/Ship Variant 2.prefab prefabs/Ship Variant.prefab prefabs/Ship.prefab'],
      'an instance of a variant roots one instance per level of its chain')
check(compare(os.path.join(OPS, '1-before-variant.yaml'), os.path.join(OPS, '2-variant.yaml'), '--ignore-prefab-info', '--ids'),
      'Create Variant changes nothing in the scene (values, references, ids)')
check(compare(os.path.join(OPS, '2-variant.yaml'), os.path.join(OPS, '2-fresh.yaml'), '--subtree=Ship:Field/Ship 2'),
      'a fresh instance of Ship Variant is what it was made from (overrides of every kind, nested, references)')
check(compare(os.path.join(OPS, '3-before-variant2.yaml'), os.path.join(OPS, '4-variant2.yaml'), '--ignore-prefab-info', '--ids'),
      'Create Variant of a variant instance changes nothing in the scene')
check(compare(os.path.join(OPS, '4-variant2.yaml'), os.path.join(OPS, '4-fresh.yaml'), '--subtree=Ship:Field/Ship 2'),
      'a fresh instance of Ship Variant 2 is what it was made from (overrides at both levels)')
check(compare(os.path.join(OPS, '5-plain.yaml'), os.path.join(OPS, '5-plain.yaml'), '--subtree=Field/Ship 2:Field/Ship 3', '--ignore-prefab-info'),
      'a variant made from the Project window instantiates as its base')
final = load(os.path.join(OPS, '6-final.yaml'))
check(final['Ship'].get('prefab') == V2 and final['Ship/Hull'].get('prefab') == SHIP and final['Ship/Wing/Flag'].get('prefab') == V1
      and final['Ship/Wing/Flag/Pole'].get('prefab') == V2,
      'the GameObjects of a variant instance belong to the level that has them')
check(refs(final, 'Pointer').get('action.target') == 'Ship/Engine/Flame' and
      refs(final, 'Spy').get('script.scripts[0].target') == 'Ship/Wing/Flag/Pole' and
      refs(final, 'Ship/Gun').get('action.target') == 'Ship/Wing/Flag' and
      refs(final, 'Ship/Hull').get('action.target') == 'Ship/Engine/Flame',
      'references: scene into every level, a base GameObject to the variant\'s, into the nested Engine')
check('Ship/Cam' not in final and 'Ship/Wing/Tip' not in final, 'removed GameObjects at two levels stay out')
check(comp(final, 'Ship/Hull', 'mesh', 'color') == [1, 0, 0, 1] and comp(final, 'Ship/Hull', 'transform', 'scale') == [3, 1, 1] and
      comp(final, 'Ship', 'transform', 'scale') == [1.5, 1.5, 1] and final['Ship/Wing'].get('tag') == 'Enemy' and
      'action' in final['Ship/Hull']['components'] and 'camera' not in (final.get('Ship/Cam') or {}).get('components', {}),
      'the overrides of each level are on the instance')
info = lines_of(out, 'chain')
check(any(l.endswith(': prefabs/Ship Variant 2.prefab prefabs/Ship Variant.prefab prefabs/Ship.prefab') for l in info),
      'variant chain of Ship Variant 2')
usage = [l for l in out.splitlines() if l.strip().startswith('usage prefabs/Ship.prefab')]
check(usage and 'variant prefabs/Ship Variant.prefab' in usage[0] and 'variant prefabs/Ship Plain Variant.prefab' in usage[0],
      'Prefabs dialog: the base lists its variants')
v2file = yaml.safe_load(open(os.path.join(P, V2)))
root = [m for m in v2file['members'] if m['entity'] == v2file['root']][0]
check(root['components']['prefab']['path'] == V1 and 'prefabInstance' in root and
      any(n.get('prefabRootRegistryEntity') == v2file['root'] for n in root['prefabInstance'].get('nested', [])),
      'Ship Variant 2 file: its root an instance of Ship Variant, its overrides of both levels')
v1hull = [o for o in root_block(os.path.join(P, V1)).get('overrides', []) if o.get('registryEntity') == SHIP_HULL]
check(v1hull and v1hull[0].get('properties', {}).get('mesh') == [COLOR] and v1hull[0].get('addedComponents') == ['action'],
      'Ship Variant file: what the instance overrode of the base is its override (by property, added component)')
bake = os.path.join(OPS, 'bake')
check(os.path.isfile(os.path.join(bake, 'prefabs_Ship_Variant_2.cpp')) or any('Variant_2' in f for f in os.listdir(bake)),
      'export: a build_prefab_* source for each variant')

print('== save and reload')
shutil.copytree(P, os.path.join(WORK, 'save1'))
editor(P, os.path.join(WORK, 'reload1.yaml'), save=True, name='reload1')
shutil.copytree(P, os.path.join(WORK, 'save2'))
editor(P, os.path.join(WORK, 'reload2.yaml'), save=True, name='reload2')
check(compare(os.path.join(OPS, '6-final.yaml'), os.path.join(WORK, 'reload1.yaml'), '--ids'),
      'reloaded: every GameObject, value, override, reference and member id as before')
check(compare(os.path.join(WORK, 'reload1.yaml'), os.path.join(WORK, 'reload2.yaml'), '--ids'), 'reloaded again: the same')
check(files_equal(os.path.join(WORK, 'save2'), P), 'saved twice: the scene and prefab files are byte for byte the same')

print('== base edits, apply targets, Revert, Unpack, removed / added GameObjects, duplicate, spawn')
E = os.path.join(WORK, 'edit')
shutil.copytree(P, E)
ed = os.path.join(WORK, 'edit-dumps')
os.makedirs(ed)
ops = [
    # the base edited in Prefab Mode: what no level overrides reaches every variant
    {'op': 'openPrefab', 'prefab': SHIP},
    {'op': 'setCmd', 'stage': True, 'entity': 'Ship/Hull', 'property': 'position', 'value': [10, 0, 0]},
    {'op': 'set', 'stage': True, 'entity': 'Ship/Hull', 'component': 'mesh', 'property': COLOR, 'value': [0, 0, 1, 1]},
    {'op': 'set', 'stage': True, 'entity': 'Ship/Hull', 'component': 'transform', 'property': 'scale', 'value': [7, 7, 1]},
    {'op': 'savePrefab'},
    {'op': 'closePrefab', 'choice': 'save'},
    {'op': 'instantiate', 'entity': 'Field', 'prefab': SHIP, 'below': True},
    {'op': 'instantiate', 'entity': 'Field', 'prefab': V1, 'below': True},
    {'op': 'instantiate', 'entity': 'Field', 'prefab': V2, 'below': True},
    {'op': 'dump', 'out': ed + '/A.yaml'},
    # apply targets of a chain: to the variant in the middle
    {'op': 'set', 'entity': 'Ship/Wing', 'component': 'transform', 'property': 'scale', 'value': [4, 4, 1]},
    {'op': 'canApply', 'entity': 'Ship/Wing', 'component': 'transform', 'properties': ['scale']},
    {'op': 'applyProperty', 'entity': 'Ship/Wing', 'component': 'transform', 'properties': ['scale'], 'target': V1},
    {'op': 'dump', 'out': ed + '/B.yaml'},
    # to the base, through a variant that overrides the value: its override goes
    {'op': 'set', 'entity': 'Ship/Hull', 'component': 'mesh', 'property': COLOR, 'value': [1, 1, 0, 1]},
    {'op': 'applyProperty', 'entity': 'Ship/Hull', 'component': 'mesh', 'properties': [COLOR], 'target': SHIP},
    {'op': 'dump', 'out': ed + '/C.yaml'},
    # the root of a variant instance: to the base's root, through the variants
    {'op': 'setStateLocal', 'entity': 'Ship', 'layer': 3},
    {'op': 'recordState', 'entity': 'Ship', 'state': 8},
    {'op': 'canApply', 'entity': 'Ship', 'component': 'transform', 'properties': ['scale']},
    {'op': 'applyState', 'entity': 'Ship', 'state': 8, 'target': V1},
    {'op': 'dump', 'out': ed + '/C2.yaml'},
    # Revert in the variant's Prefab Mode: the base's value comes back; an edit there is the
    # variant's override
    {'op': 'openPrefab', 'prefab': V1},
    {'op': 'dump', 'out': ed + '/D-stage.yaml'},
    {'op': 'revertProperty', 'stage': True, 'entity': 'Ship/Wing', 'component': 'transform', 'properties': ['scale']},
    {'op': 'set', 'stage': True, 'entity': 'Ship/Engine/Flame', 'component': 'mesh', 'property': COLOR, 'value': [0, 0, 1, 1]},
    {'op': 'savePrefab'},
    {'op': 'closePrefab', 'choice': 'save'},
    {'op': 'dump', 'out': ed + '/D.yaml'},
    # Unpack: an instance of the base (Unity), completely: plain GameObjects
    {'op': 'unpack', 'entity': 'Ship'},
    {'op': 'levels', 'entity': 'Ship'},
    {'op': 'dump', 'out': ed + '/F1.yaml'},
    {'op': 'undo'},
    {'op': 'levels', 'entity': 'Ship'},
    {'op': 'unpack', 'entity': 'Ship', 'completely': True},
    {'op': 'levels', 'entity': 'Ship'},
    {'op': 'dump', 'out': ed + '/F2.yaml'},
    {'op': 'undo'},
    {'op': 'levels', 'entity': 'Ship'},
    {'op': 'dump', 'out': ed + '/F3.yaml'},
    # a base GameObject removed in a variant instance, applied as the variant's override
    {'op': 'delete', 'entity': 'Field/Ship 3/Gun'},
    {'op': 'applyRemovals', 'entity': 'Field/Ship 3', 'target': V1},
    {'op': 'dump', 'out': ed + '/G1.yaml'},
    {'op': 'undo'},
    {'op': 'undo'},
    {'op': 'dump', 'out': ed + '/G2.yaml'},
    # GameObjects added to a variant instance applied: below a base GameObject to the variant
    # in the middle, below the root to the base
    {'op': 'addObject', 'entity': 'Ship/Hull', 'name': 'Rivet'},
    {'op': 'addToPrefab', 'entity': 'Ship/Hull/Rivet', 'parent': 'Ship/Hull', 'target': V1},
    {'op': 'addObject', 'entity': 'Ship', 'name': 'Decal'},
    {'op': 'addToPrefab', 'entity': 'Ship/Decal', 'parent': 'Ship', 'target': SHIP},
    {'op': 'dump', 'out': ed + '/G3.yaml'},
    # duplicate, Play's spawn
    {'op': 'duplicate', 'entity': 'Ship'},
    {'op': 'spawn', 'entity': 'Field', 'prefab': V2, 'name': 'Spawned'},
    {'op': 'dump', 'out': ed + '/H.yaml'},
    {'op': 'save'},
]
out = editor(E, os.path.join(ed, 'final.yaml'), ops_file('edit', ops), name='edit')
a = load(ed + '/A.yaml')
check(comp(a, 'Field/Ship 2/Hull', 'transform', 'position') == [10, 0, 0] and comp(a, 'Field/Ship 3/Hull', 'transform', 'position') == [10, 0, 0] and
      comp(a, 'Field/Ship 4/Hull', 'transform', 'position') == [10, 0, 0] and comp(a, 'Ship/Hull', 'transform', 'position') == [10, 0, 0],
      'base edited in Prefab Mode: a value no level overrides reaches the base, the variants and their instances')
check(comp(a, 'Field/Ship 2/Hull', 'mesh', 'color') == [0, 0, 1, 1] and comp(a, 'Field/Ship 3/Hull', 'mesh', 'color') == [1, 0, 0, 1] and
      comp(a, 'Field/Ship 4/Hull', 'mesh', 'color') == [1, 0, 0, 1] and comp(a, 'Ship/Hull', 'mesh', 'color') == [1, 0, 0, 1],
      'a value Ship Variant overrides stays the variant\'s (and its variant\'s)')
check(comp(a, 'Field/Ship 3/Hull', 'transform', 'scale') == [7, 7, 1] and comp(a, 'Field/Ship 4/Hull', 'transform', 'scale') == [3, 1, 1] and
      comp(a, 'Ship/Hull', 'transform', 'scale') == [3, 1, 1],
      'a value only Ship Variant 2 overrides reaches Ship Variant, not Ship Variant 2')
targets = lines_of(out, 'targets:')
check(targets and targets[0] == ' '.join([SHIP, V1, V2]), 'apply targets of a GameObject of the base in a variant of a variant: base, variant, variant')
b = load(ed + '/B.yaml')
check(comp(b, 'Field/Ship 3/Wing', 'transform', 'scale') == [4, 4, 1] and comp(b, 'Field/Ship 4/Wing', 'transform', 'scale') == [4, 4, 1] and
      comp(b, 'Field/Ship 2/Wing', 'transform', 'scale') == [1, 1, 1] and not (b['Ship/Wing'].get('overrides') or {}).get('properties'),
      'Apply as Override in Prefab Variant: its instances and its variants\' follow, not the base\'s; the override is cleared')
c = load(ed + '/C.yaml')
check(all(comp(c, p, 'mesh', 'color') == [1, 1, 0, 1] for p in ('Ship/Hull', 'Field/Ship 2/Hull', 'Field/Ship 3/Hull', 'Field/Ship 4/Hull')),
      'Apply to the base through a variant overriding the value: the variant\'s override goes, every instance shows it')
hull = [o for o in root_block(os.path.join(E, V1)).get('overrides', []) if o.get('registryEntity') == SHIP_HULL]
check(not hull or 'mesh' not in (hull[0].get('properties') or {}), 'and Ship Variant\'s file no longer overrides it')
c2 = load(ed + '/C2.yaml')
check(c2['Ship'].get('layer') == 3 and c2['Field/Ship 3'].get('layer') == 3 and c2['Field/Ship 4'].get('layer') == 3 and
      not c2['Field/Ship 2'].get('layer'), 'the root of a variant instance applied to the variant in the middle (its root\'s state)')
check(len(targets) > 1 and targets[1] == ' '.join([SHIP, V1, V2]), 'apply targets of a variant instance root: base, variant, variant')
d = load(ed + '/D.yaml')
check(comp(d, 'Field/Ship 3/Wing', 'transform', 'scale') == [1, 1, 1] and comp(d, 'Ship/Wing', 'transform', 'scale') == [1, 1, 1],
      'Revert in the variant\'s Prefab Mode: the base\'s value, in the variant and its variant')
check(comp(d, 'Field/Ship 3/Engine/Flame', 'mesh', 'color') == [0, 0, 1, 1] and comp(d, 'Field/Ship 2/Engine/Flame', 'mesh', 'color') == [1, 0.5, 0, 1]
      and comp(d, 'Ship/Engine/Flame', 'mesh', 'color') == [0, 1, 0, 1],
      'an edit of a nested GameObject in the variant\'s Prefab Mode is its override (Ship Variant 2 keeps its own)')
stage = yaml.safe_load(open(ed + '/D-stage.yaml'))['scenes']
stage = [s for s in stage if s['name'].startswith('Prefab Mode')][0]['entities']
check(stage['Ship'].get('prefab') == SHIP and (stage['Ship/Wing'].get('overrides') or {}).get('properties') == {'transform': ['scale']} and
      (stage['Ship/Hull'].get('overrides') or {}).get('added') == ['action'] and stage['Ship/Wing/Flag'].get('prefab') is None,
      'Prefab Mode of a variant: its root an instance of the base, the variant\'s overrides marked')
lv = lines_of(out, 'levels:')
check(lv[:4] == [V1 + ' ' + SHIP, ' '.join([V2, V1, SHIP]), '', ' '.join([V2, V1, SHIP])],
      'Unpack: an instance of the base; Unpack Completely: plain; undo: as before')
check(compare(ed + '/D.yaml', ed + '/F1.yaml', '--ignore-prefab-info', '--ids') and compare(ed + '/D.yaml', ed + '/F2.yaml', '--ignore-prefab-info', '--ids')
      and compare(ed + '/D.yaml', ed + '/F3.yaml', '--ids'), 'Unpack keeps every value and id; undo gives back everything')
g1 = load(ed + '/G1.yaml')
check('Field/Ship 3/Gun' not in g1 and 'Field/Ship 4/Gun' not in g1 and 'Ship/Gun' not in g1 and 'Field/Ship 2/Gun' in g1,
      'a removed GameObject applied as the variant\'s override: its instances and variants lose it, the base keeps it')
check(compare(ed + '/D.yaml', ed + '/G2.yaml', '--ids'), 'undo: as before')
g3 = load(ed + '/G3.yaml')
check('Field/Ship 3/Hull/Rivet' in g3 and 'Field/Ship 4/Hull/Rivet' in g3 and 'Field/Ship 2/Hull/Rivet' not in g3 and
      g3['Ship/Hull/Rivet'].get('prefab') == V1, 'a GameObject added below a base GameObject applied to the variant: the variant adds it')
check('Field/Ship 2/Decal' in g3 and 'Field/Ship 3/Decal' in g3 and g3['Ship/Decal'].get('prefab') == SHIP,
      'a GameObject added below a variant instance\'s root applied to the base: every instance has it')
h = load(ed + '/H.yaml')
dup = [k for k in h if k.startswith('Ship ') and '/' not in k]
check(dup and compare(ed + '/H.yaml', ed + '/H.yaml', '--subtree=Ship:' + dup[0]), 'duplicate of a variant instance: the same')
check(compare(ed + '/H.yaml', ed + '/H.yaml', '--subtree=Field/Ship 4:Spawned'), 'Play: what a script spawns of a variant is what the editor instantiates')
editor(E, os.path.join(ed, 'reload.yaml'), name='edit-reload')
check(compare(ed + '/final.yaml', ed + '/reload.yaml', '--ids'), 'all of it saved and reloaded the same')

print('== undo / redo of Create Variant, Apply All and Revert All on a variant instance, copy and paste')
U = os.path.join(WORK, 'undo')
shutil.copytree(P, U)
ud = os.path.join(WORK, 'undo-dumps')
os.makedirs(ud)
ops = [
    {'op': 'instantiate', 'entity': 'Field', 'prefab': V1, 'below': True},
    {'op': 'set', 'entity': 'Field/Ship 2/Hull', 'component': 'transform', 'property': 'scale', 'value': [6, 6, 1]},
    {'op': 'addObject', 'entity': 'Field/Ship 2/Wing/Flag', 'name': 'Ribbon'},
    {'op': 'dump', 'out': ud + '/before.yaml'},
    {'op': 'createVariant', 'entity': 'Field/Ship 2', 'path': 'prefabs/Ship Variant 3.prefab'},
    {'op': 'dump', 'out': ud + '/made.yaml'},
    {'op': 'undo'},
    {'op': 'dump', 'out': ud + '/undone.yaml'},
    {'op': 'redo'},
    {'op': 'dump', 'out': ud + '/redone.yaml'},
    # Apply All on an instance of the variant of a variant: overrides at every level, an
    # added and a removed GameObject, all of them the variant's (its prefab's) now
    {'op': 'instantiate', 'entity': 'Field', 'prefab': V2, 'below': True},
    {'op': 'set', 'entity': 'Field/Ship 3/Hull', 'component': 'transform', 'property': 'position', 'value': [0, -80, 0]},
    {'op': 'set', 'entity': 'Field/Ship 3/Wing/Flag', 'component': 'transform', 'property': 'scale', 'value': [2, 2, 1]},
    {'op': 'set', 'entity': 'Field/Ship 3/Wing/Flag/Pole', 'component': 'transform', 'property': 'scale', 'value': [3, 3, 1]},
    {'op': 'set', 'entity': 'Field/Ship 3/Engine/Flame', 'component': 'transform', 'property': 'position', 'value': [7, 7, 0]},
    {'op': 'set', 'entity': 'Field/Ship 3', 'component': 'transform', 'property': 'scale', 'value': [2, 2, 1]},
    {'op': 'addObject', 'entity': 'Field/Ship 3/Hull', 'name': 'Bolt'},
    {'op': 'delete', 'entity': 'Field/Ship 3/Eye'},
    {'op': 'dump', 'out': ud + '/apply-before.yaml'},
    {'op': 'applyAll', 'entity': 'Field/Ship 3'},
    {'op': 'instantiate', 'entity': 'Field', 'prefab': V2, 'below': True},
    {'op': 'dump', 'out': ud + '/apply-after.yaml'},
    {'op': 'undo'},
    {'op': 'undo'},
    {'op': 'dump', 'out': ud + '/apply-undone.yaml'},
    {'op': 'revertAll', 'entity': 'Field/Ship 3'},
    {'op': 'instantiate', 'entity': 'Field', 'prefab': V2, 'below': True},
    {'op': 'dump', 'out': ud + '/reverted.yaml'},
    {'op': 'copyPaste', 'entity': 'Ship'},
    {'op': 'dump', 'out': ud + '/pasted.yaml'},
]
undo_log = editor(U, os.path.join(ud, 'final.yaml'), ops_file('undo', ops), name='undo')
check(compare(ud + '/before.yaml', ud + '/made.yaml', '--ignore-prefab-info', '--ids'), 'Create Variant (with an added GameObject): nothing changes in the scene')
check(compare(ud + '/before.yaml', ud + '/undone.yaml', '--ids') and 'Removed prefab file' in undo_log,
      'undo of Create Variant: the instance is one of the base again, as it was; the file is gone')
check(compare(ud + '/made.yaml', ud + '/redone.yaml', '--ids'), 'redo: as made')
check(compare(ud + '/apply-before.yaml', ud + '/apply-after.yaml', '--subtree=Field/Ship 3:Field/Ship 4', '--ignore-prefab-info'),
      'Apply All on an instance of a variant of a variant: a new instance of it is the same (every level, added and removed GameObjects)')
aa = load(ud + '/apply-after.yaml')
check(not any((aa.get(p) or {}).get('overrides') for p in ('Field/Ship 3/Hull', 'Field/Ship 3/Wing/Flag', 'Field/Ship 3/Engine/Flame')),
      'and the instance has no override left')
check(compare(ud + '/apply-before.yaml', ud + '/apply-undone.yaml', '--ids'), 'undo of Apply All: as before')
check(compare(ud + '/reverted.yaml', ud + '/reverted.yaml', '--subtree=Field/Ship 3:Field/Ship 4'),
      'Revert All on a variant instance: it is a plain instance of the variant again')
pasted = load(ud + '/pasted.yaml')
copies = [k for k in pasted if k.startswith(('Ship ', 'Ship#')) and '/' not in k]
check(copies and compare(ud + '/pasted.yaml', ud + '/pasted.yaml', '--subtree=Ship:' + copies[0]), 'copy and paste of a variant instance: the same')

print('== Replace between a prefab and its variants')
R = os.path.join(WORK, 'replace')
shutil.copytree(P, R)
rd = os.path.join(WORK, 'replace-dumps')
os.makedirs(rd)
ops = [
    {'op': 'instantiate', 'entity': 'Field', 'prefab': SHIP, 'below': True},
    {'op': 'set', 'entity': 'Field/Ship 2/Hull', 'component': 'transform', 'property': 'scale', 'value': [9, 9, 1]},
    {'op': 'set', 'entity': 'Field/Ship 2/Engine/Flame', 'component': 'transform', 'property': 'position', 'value': [1, 2, 0]},
    {'op': 'addObject', 'entity': 'Field/Ship 2/Wing', 'name': 'Sticker'},
    {'op': 'replace', 'entity': 'Field/Ship 2', 'prefab': V1, 'renameRoot': False},
    {'op': 'dump', 'out': rd + '/to-variant.yaml'},
    {'op': 'replace', 'entity': 'Field/Ship 2', 'prefab': V2, 'renameRoot': False},
    {'op': 'dump', 'out': rd + '/to-variant2.yaml'},
    {'op': 'replace', 'entity': 'Field/Ship 2', 'prefab': SHIP, 'renameRoot': False},
    {'op': 'dump', 'out': rd + '/to-base.yaml'},
    {'op': 'undo'},
    {'op': 'undo'},
    {'op': 'undo'},
    {'op': 'dump', 'out': rd + '/undone.yaml'},
]
editor(R, os.path.join(rd, 'final.yaml'), ops_file('replace', ops), name='replace')
for name, prefab in (('to-variant', V1), ('to-variant2', V2), ('to-base', SHIP)):
    r = load(rd + '/' + name + '.yaml')
    check(r['Field/Ship 2'].get('prefab') == prefab and comp(r, 'Field/Ship 2/Hull', 'transform', 'scale') == [9, 9, 1] and
          comp(r, 'Field/Ship 2/Engine/Flame', 'transform', 'position') == [1, 2, 0] and 'Field/Ship 2/Wing/Sticker' in r,
          'Replace with %s: the overrides of the base\'s GameObjects, of the nested instance and the added GameObject kept' % prefab)
r = load(rd + '/to-variant.yaml')
check(comp(r, 'Field/Ship 2/Hull', 'mesh', 'color') == [1, 0, 0, 1] and 'Field/Ship 2/Wing/Flag' in r,
      'Replace with a variant: what the variant overrides and adds is there')

print('== the base renamed, moved with the scene closed, deleted and put back')
M = os.path.join(WORK, 'move')
shutil.copytree(P, M)
md = os.path.join(WORK, 'move-dumps')
os.makedirs(md)
ops = [
    {'op': 'renameFile', 'from': SHIP, 'to': 'prefabs/Hull Base.prefab'},
    {'op': 'dump', 'out': md + '/renamed.yaml'},
    {'op': 'save'},
]
editor(M, os.path.join(md, 'r.yaml'), ops_file('rename', ops), name='rename')
check(compare(os.path.join(OPS, '6-final.yaml'), md + '/renamed.yaml', '--ignore-prefab-info', '--ids'),
      'base renamed in the editor: every variant instance as before')
# moved outside the editor, the project closed
os.makedirs(os.path.join(M, 'prefabs', 'base'), exist_ok=True)
shutil.move(os.path.join(M, 'prefabs', 'Hull Base.prefab'), os.path.join(M, 'prefabs', 'base', 'Hull Base.prefab'))
editor(M, md + '/moved.yaml', name='moved', save=True)
check(compare(os.path.join(OPS, '6-final.yaml'), md + '/moved.yaml', '--ignore-prefab-info', '--ids'),
      'base moved with the project closed: found by its GUID, every variant instance as before')
before_missing = open(os.path.join(M, V1), 'rb').read()
before_scene = ship_block(os.path.join(M, 'Variant Scene.scene'))
ops = [
    {'op': 'remove', 'path': 'prefabs/base/Hull Base.prefab'},
    {'op': 'refreshAssets'},
    {'op': 'dump', 'out': md + '/missing.yaml'},
    {'op': 'save'},
]
editor(M, os.path.join(md, 'm.yaml'), ops_file('missing', ops), name='missing', allow_errors=True)
miss = load(md + '/missing.yaml')
check(miss['Ship'].get('missingPrefab') and 'Ship/Hull' not in miss and 'Ship/Wing/Flag' not in miss,
      'base deleted: the variant instance is a Missing Prefab (its root only)')
check(ship_block(os.path.join(M, 'Variant Scene.scene')) == before_scene and
      'prefabInstance' in ship_block(os.path.join(M, 'Variant Scene.scene')),
      'its data stays in the scene file as it was (instance block, member ids)')
ops = [
    {'op': 'openPrefab', 'prefab': V1},
    {'op': 'dump', 'out': md + '/missing-stage.yaml'},
    {'op': 'savePrefab'},
    {'op': 'closePrefab', 'choice': 'save'},
]
editor(M, os.path.join(md, 'ms.yaml'), ops_file('missing-stage', ops), name='missing-stage', allow_errors=True)
ms = yaml.safe_load(open(md + '/missing-stage.yaml'))['scenes']
ms = [s for s in ms if s['name'].startswith('Prefab Mode')][0]['entities']
check(ms['Ship'].get('missingPrefab'), 'Prefab Mode of a variant whose base is missing: its root a Missing Prefab')
v1_after = yaml.safe_load(open(os.path.join(M, V1)))
v1_before = yaml.safe_load(before_missing)
check(v1_after == v1_before, 'and saving it keeps the variant\'s data (its file the same)')
shutil.copy(os.path.join(WORK, 'save1', SHIP), os.path.join(M, 'prefabs', 'base', 'Hull Base.prefab'))
editor(M, md + '/back.yaml', name='back')
check(compare(os.path.join(OPS, '6-final.yaml'), md + '/back.yaml', '--ignore-prefab-info', '--ids'),
      'base back: every variant instance as before (ids included)')

print('== the variants renamed with the scene closed')
ops = [
    {'op': 'close', 'scene': 'Variant Scene'},
    {'op': 'usage'},
    {'op': 'renameFile', 'from': V1, 'to': 'prefabs/Ship Mid.prefab'},
    {'op': 'renameFile', 'from': V2, 'to': 'prefabs/Ship Top.prefab'},
    {'op': 'variantInfo', 'prefab': 'prefabs/Ship Top.prefab'},
]
out = editor(M, os.path.join(md, 'closed.yaml'), ops_file('closed', ops), name='closed')
usage = [l.strip() for l in out.splitlines() if l.strip().startswith('usage ')]
ship_usage = [l for l in usage if l.startswith('usage prefabs/base/Hull Base.prefab')]
top_usage = [l for l in usage if l.startswith('usage prefabs/Ship Variant 2.prefab')]
check(ship_usage and '[Variant Scene closed instances 0 nested 1' in ship_usage[0] and 'variant prefabs/Ship Variant.prefab' in ship_usage[0]
      and top_usage and '[Variant Scene closed instances 1 nested 0' in top_usage[0],
      'Prefabs dialog, the scene closed: the variant instance counts for the variant, and as nested for its bases')
chain = lines_of(out, 'chain')
check(chain and chain[-1].endswith(': prefabs/Ship Top.prefab prefabs/Ship Mid.prefab prefabs/base/Hull Base.prefab'),
      'renamed variants: the chain follows (by GUID)')
editor(M, md + '/renamed-closed.yaml', name='renamed-closed', save=True)
check(compare(os.path.join(OPS, '6-final.yaml'), md + '/renamed-closed.yaml', '--ignore-prefab-info', '--ids'),
      'variants renamed while the scene was closed: the scene loads every instance as before')
rc = load(md + '/renamed-closed.yaml')
check(rc['Ship'].get('prefab') == 'prefabs/Ship Top.prefab' and rc['Ship/Wing/Flag'].get('prefab') == 'prefabs/Ship Mid.prefab',
      'and its instance names the renamed variants')

print('== a cycle of bases (a file edited by hand)')
C = os.path.join(WORK, 'cycle')
shutil.rmtree(C, ignore_errors=True)
shutil.copytree(M, C)
top = yaml.safe_load(open(os.path.join(C, 'prefabs', 'Ship Top.prefab')))
base_file = os.path.join(C, 'prefabs', 'base', 'Hull Base.prefab')
base = yaml.safe_load(open(base_file))
base_root = [m for m in base['members'] if m['entity'] == base['root']][0]
base_root['components']['prefab'] = {'name': 'Ship Top', 'path': 'prefabs/Ship Top.prefab', 'guid': top['guid']}
yaml.safe_dump(base, open(base_file, 'w'), sort_keys=False)
editor(C, C + '-load.yaml', name='cycle', allow_errors=True)
cy_log = open(os.path.join(WORK, 'cycle.log')).read()
cy = load(C + '-load.yaml')
check('make a cycle' in cy_log, 'a cycle of bases: reported')
check(cy['Ship'].get('missingPrefab') and 'Ship/Hull' not in cy, 'and the variant instance is a Missing Prefab (no endless import)')
cy_after = yaml.safe_load(open(os.path.join(C, 'Variant Scene.scene')))
check(cy_after == yaml.safe_load(open(os.path.join(M, 'Variant Scene.scene'))), 'and its scene file is untouched')

print('== generated sources')
gen = open(os.path.join(bake, [f for f in os.listdir(bake) if 'Variant_2' in f and f.endswith('.cpp')][0])).read()
check(gen.count('scene->createEntity()') == 8 and all('"%s"' % n in gen for n in ('Pole', 'Flag', 'Hull', 'Wing', 'Gun', 'Engine', 'Eye', 'Flame'))
      and gen.count('members.push_back(NULL_ENTITY)') == 2,
      'build_prefab of the variant of a variant: its GameObjects and its bases\', the ones a level removed kept as empty slots')

print()
if FAILS:
    print('%d FAILED:' % len(FAILS))
    for f in FAILS:
        print('  ' + f)
    sys.exit(1)
print('all passed')
