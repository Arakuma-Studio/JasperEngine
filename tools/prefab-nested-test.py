#!/usr/bin/env python3
"""Scripted checks of nested prefabs in assets, stripped objects and the baked export
(editor/project/PREFABS.md, section 16).

    FIXTURE=<TappyPlane fixture project> tools/prefab-nested-test.py [work-dir]

Builds the `nested` fixture (tools/prefab-fixtures.py) and runs jasperengine-editor
scene-dump on it: Leaf, Branch, Tree and Forest made bottom-up from instances with overrides
of every kind and references into them (a fresh instance of each must equal what it was
made from), a reload (equal, member ids included) and a second one (the files written again
byte for byte), a multi-selection of instances made prefabs, Add to Prefab, Play's spawn
against an instantiated one, Apply of references into nested prefabs to the owning prefab
and to an outer one, undo / redo of Create Prefab from a selection holding an instance, a
nested prefab going missing and coming back, and the generated build_prefab_* sources.
Exits non-zero on a failure. Needs a Linux editor build (tools/build.sh linux).
"""
import os, shutil, subprocess, sys, tempfile, yaml

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
EDITOR = os.path.join(ROOT, 'build', 'linux', 'editor-install', 'bin', 'jasperengine-editor')
COMPARE = os.path.join(ROOT, 'tools', 'prefab-dump-compare.py')
WORK = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else tempfile.mkdtemp(prefix='prefab-nested-')
FAILS = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        FAILS.append(msg)


def editor(project, out, ops=None, save=False, name='run'):
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
    errors = [l for l in res.stdout.splitlines() if '[ERROR]' in l or '(ERROR)' in l]
    check(not errors, name + ': no error logged' + ('' if not errors else ' (%s)' % errors[0]))
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


def refs(entities, path):
    return (entities.get(path) or {}).get('refs') or {}


shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK)
FIX = os.path.join(WORK, 'nested')
subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'prefab-fixtures.py'), 'nested', FIX], check=True)
OPS = FIX + '.ops'
P = os.path.join(WORK, 'project')
shutil.copytree(FIX, P)

print('== Leaf, Branch, Tree, Forest made from instances with overrides, four levels deep')
editor(P, os.path.join(OPS, 'after-ops.yaml'), os.path.join(OPS, 'build.yaml'), name='build')
for made, fresh, a, b in (('0-leaf', '0-fresh', 'Forest/Tree/Branch/Leaf', 'Field/Leaf 2'),
                          ('2-branch', '2-fresh', 'Forest/Tree/Branch', 'Field/Branch 2'),
                          ('4-tree', '4-fresh', 'Forest/Tree', 'Field/Tree 2'),
                          ('6-forest', '6-fresh', 'Forest', 'Field/Forest 2')):
    check(compare(os.path.join(OPS, made + '.yaml'), os.path.join(OPS, fresh + '.yaml'), '--subtree=%s:%s' % (a, b)),
          'a fresh instance of %s is what it was made from (overrides, added / removed GameObjects, references)' % a.split('/')[-1])
for made, before in (('2-branch', '1-before-branch'), ('4-tree', '3-before-tree'), ('6-forest', '5-before-forest')):
    check(compare(os.path.join(OPS, before + '.yaml'), os.path.join(OPS, made + '.yaml'), '--ignore-prefab-info', '--ids'),
          'Create Prefab of %s changes nothing in the scene' % made.split('-')[1])
fresh = load(os.path.join(OPS, '6-fresh.yaml'))
F = 'Field/Forest 2'
check(refs(fresh, F + '/Tree/Branch/Screen').get('mesh.submeshes[0].material.baseColorTexture') == F + '/Tree/Branch/Leaf/LeafCam',
      'camera link into a nested prefab (Branch > Leaf)')
check(refs(fresh, F + '/Tree/Rope') == {'joint2d.bodyA': F + '/Tree/Branch/Twig', 'joint2d.bodyB': F + '/Tree/Branch/Leaf/Petal'},
      'joint bodies two and three levels down (Tree > Branch > Leaf)')
check(refs(fresh, F + '/Wind').get('action.target') == F + '/Tree/Branch/Leaf/Petal/Dew/Moss',
      'action target into GameObjects added at two levels (Forest > Tree > Branch > Leaf)')
check(refs(fresh, F + '/Tree/Branch/Leaf/Petal').get('action.target') == F + '/Tree/Branch/Leaf/LeafCam',
      'an added component of a nested instance, its reference into it')
check(refs(fresh, F + '/Tree/Branch/Eye').get('script.scripts[0].target') == F + '/Tree/Branch/Leaf/Petal' and
      refs(fresh, F + '/Tree/Lens').get('script.scripts[0].target') == F + '/Tree/Branch/Leaf/LeafCam',
      'script properties into nested prefabs, one and two levels down')
check(F + '/Tree/Branch/Leaf/Thorn' not in fresh and F + '/Tree/Branch/Leaf/Bud' not in fresh,
      'removed GameObjects at two levels stay out')
check(F + '/Tree/Branch/Twig/Knot' not in fresh and F + '/Tree/Branch/Leaf/Nest' in fresh,
      'added GameObject removed by the level above, added below a nested nested root')

print('== save and reload')
shutil.copytree(P, os.path.join(WORK, 'save1'))
editor(P, os.path.join(WORK, 'reload1.yaml'), save=True, name='reload1')
shutil.copytree(P, os.path.join(WORK, 'save2'))
editor(P, os.path.join(WORK, 'reload2.yaml'), save=True, name='reload2')
check(compare(os.path.join(OPS, 'after-ops.yaml'), os.path.join(WORK, 'reload1.yaml'), '--ids'),
      'reloaded: every GameObject, value, override, reference and member id as before')
check(compare(os.path.join(WORK, 'reload1.yaml'), os.path.join(WORK, 'reload2.yaml'), '--ids'), 'reloaded again: the same')
same = True
for base, dirs, files in os.walk(os.path.join(WORK, 'save2')):
    for f in files:
        if f.endswith(('.prefab', '.scene')):
            a = os.path.join(base, f)
            b = os.path.join(P, os.path.relpath(a, os.path.join(WORK, 'save2')))
            if open(a, 'rb').read() != open(b, 'rb').read():
                same = False
                print('  differs: ' + os.path.relpath(a, WORK))
check(same, 'saved twice: the scene and prefab files are byte for byte the same')
final = load(os.path.join(WORK, 'reload1.yaml'))
check(refs(final, 'Pointer').get('action.target') == 'Forest/Tree/Branch/Leaf/Petal', 'a scene reference into a deep member survives a reload')
check(refs(final, 'Cross').get('action.target') == 'Field/Forest 2/Tree/Branch/Twig', 'a reference into another instance survives a reload')
check(refs(final, 'Forest/Wind').get('joint2d.bodyA') == 'Field/Forest 2/Tree/Trunk' and
      refs(final, 'Forest/Wind').get('joint2d.bodyB') == 'Field/Forest 2/Tree/Branch/Twig', 'between instances, from an added component')
check(refs(final, 'Spy').get('script.scripts[0].target') == 'Field/Forest 2/Tree/Branch/Leaf/Petal/Dew/Moss2' and
      refs(final, 'Spy2').get('script.scripts[0].target') == 'Forest/Tree/Leaf 3/Petal/Drop',
      'scene scripts into GameObjects nested prefabs add, deep in instances, survive a reload')
check('Forest/Tree/Leaf 3/Petal/Drop' in final and final['Forest/Tree/Leaf 3'].get('prefab') == 'prefabs/Tree.prefab',
      'Add to Prefab: the instance is nested in Tree with its added GameObject')
check(final.get('Branch 2', {}).get('prefab') == 'prefabs/multi/Branch 2.prefab' and
      final.get('Branch 2/Leaf/Petal', {}).get('components', {}).get('mesh', {}).get('submeshes', [{}])[0]
      .get('material', {}).get('baseColorFactor') == [1, 1, 1, 0.5],
      'multi-selection: an instance made an original prefab keeps its nested overrides')

print('== Play spawn, Apply of references into nested prefabs, undo of Create Prefab, Missing and back')
Q = os.path.join(WORK, 'extra')
shutil.copytree(P, Q)
ex = os.path.join(WORK, 'extra-dumps')
os.makedirs(ex)
T = 'Forest 3/Tree/Branch'
ops = [
    {'op': 'spawn', 'entity': 'Field', 'prefab': 'prefabs/Forest.prefab', 'name': 'Spawned'},
    {'op': 'instantiate', 'entity': 'Field', 'prefab': 'prefabs/Forest.prefab'},
    {'op': 'dump', 'out': ex + '/spawn.yaml'},
    # a reference into a nested prefab applied to an outer prefab, then to the owning one
    {'op': 'setRef', 'entity': T + '/Twig', 'component': 'action', 'property': 'target', 'target': T + '/Leaf/LeafCam'},
    {'op': 'applyProperty', 'entity': T + '/Twig', 'component': 'action', 'properties': ['target'], 'target': 'prefabs/Forest.prefab'},
    {'op': 'instantiate', 'entity': 'Field', 'prefab': 'prefabs/Forest.prefab', 'below': True},
    {'op': 'dump', 'out': ex + '/apply-outer.yaml'},
    {'op': 'delete', 'entity': 'Field/Forest 4'},
    {'op': 'setRef', 'entity': 'Branch 2/Twig', 'component': 'action', 'property': 'target', 'target': 'Branch 2/Leaf/Bud'},
    {'op': 'applyProperty', 'entity': 'Branch 2/Twig', 'component': 'action', 'properties': ['target']},
    {'op': 'instantiate', 'entity': 'Field', 'prefab': 'prefabs/multi/Branch 2.prefab', 'below': True},
    {'op': 'dump', 'out': ex + '/apply-owning.yaml'},
    # Create Prefab of a selection holding an instance with overrides, undone and redone
    {'op': 'create', 'entity': '', 'name': 'Holder'},
    {'op': 'instantiate', 'entity': 'Holder', 'prefab': 'prefabs/Leaf.prefab', 'below': True},
    {'op': 'set', 'entity': 'Holder/Leaf 4/Petal', 'component': 'transform', 'property': 'scale', 'value': [4, 4, 1]},
    {'op': 'create', 'entity': 'Holder/Leaf 4/Petal', 'name': 'Bead'},
    {'op': 'dump', 'out': ex + '/holder-before.yaml'},
    {'op': 'createPrefab', 'entity': 'Holder', 'path': 'prefabs/Holder.prefab'},
    {'op': 'dump', 'out': ex + '/holder-made.yaml'},
    {'op': 'undo'},
    {'op': 'dump', 'out': ex + '/holder-undone.yaml'},
    {'op': 'redo'},
    {'op': 'dump', 'out': ex + '/holder-redone.yaml'},
    {'op': 'save'},
    # Leaf (nested everywhere) goes missing, then comes back
    {'op': 'copy', 'entity': 'Field', 'from': 'prefabs/Leaf.prefab', 'to': 'Leaf.prefab.keep'},
    {'op': 'remove', 'entity': 'Field', 'path': 'prefabs/Leaf.prefab'},
    {'op': 'refreshAssets', 'entity': 'Field'},
    {'op': 'dump', 'out': ex + '/missing.yaml'},
    {'op': 'copy', 'entity': 'Field', 'from': 'Leaf.prefab.keep', 'to': 'prefabs/Leaf.prefab'},
    {'op': 'refreshAssets', 'entity': 'Field'},
    {'op': 'dump', 'out': ex + '/relinked.yaml'},
    # an added GameObject below a nested instance applied to the outer prefab
    {'op': 'create', 'entity': 'Forest 3/Tree/Branch/Leaf/Petal', 'name': 'Sprout'},
    {'op': 'addToPrefab', 'entity': 'Forest 3/Tree/Branch/Leaf/Petal/Sprout', 'parent': 'Forest 3/Tree/Branch/Leaf/Petal',
     'target': 'prefabs/Forest.prefab'},
    {'op': 'instantiate', 'entity': 'Field', 'prefab': 'prefabs/Forest.prefab', 'below': True},
    {'op': 'dump', 'out': ex + '/apply-added.yaml'},
    {'op': 'delete', 'entity': 'Field/Forest 4'},
    # Revert on a nested prefab's root after that prefab changed: the layered value
    {'op': 'instantiate', 'entity': 'Field', 'prefab': 'prefabs/Leaf.prefab'},
    {'op': 'set', 'entity': 'Leaf 5', 'component': 'transform', 'property': 'scale', 'value': [1.5, 1.5, 1]},
    {'op': 'applyProperty', 'entity': 'Leaf 5', 'component': 'transform', 'properties': ['scale']},
    {'op': 'set', 'entity': 'Forest/Tree/Branch/Leaf', 'component': 'transform', 'property': 'scale', 'value': [3, 3, 1]},
    {'op': 'revertProperty', 'entity': 'Forest/Tree/Branch/Leaf', 'component': 'transform', 'properties': ['scale']},
    {'op': 'dump', 'out': ex + '/revert-nested-root.yaml'},
    {'op': 'bake', 'entity': 'Field', 'out': ex + '/bake'},
]
editor(Q, os.path.join(ex, 'final.yaml'), ops_file('extra', ops), name='extra')
check(compare(ex + '/spawn.yaml', ex + '/spawn.yaml', '--subtree=Spawned:Forest 3'),
      'Play: what a script spawns is what the editor instantiates (every level)')
e = load(ex + '/apply-outer.yaml')
check(refs(e, 'Field/Forest 4/Tree/Branch/Twig').get('action.target') == 'Field/Forest 4/Tree/Branch/Leaf/LeafCam',
      'Apply to an outer prefab of a reference into a nested prefab: a new instance has it')
check(refs(e, 'Field/Forest 2/Tree/Branch/Twig').get('action.target') == 'Field/Forest 2/Tree/Branch/Leaf/LeafCam',
      'and every instance of it follows')
check(not (e.get(T + '/Twig', {}).get('overrides')), 'and the override is cleared')
e = load(ex + '/apply-owning.yaml')
check(refs(e, 'Field/Branch 3/Twig').get('action.target') == 'Field/Branch 3/Leaf/Bud',
      'Apply to the owning prefab of a reference into a nested prefab')
check(compare(ex + '/holder-before.yaml', ex + '/holder-undone.yaml', '--ids'),
      'undo of Create Prefab: the nested instance gets its overrides and added GameObject back')
check(compare(ex + '/holder-made.yaml', ex + '/holder-redone.yaml'), 'redo of Create Prefab: as made')
e = load(ex + '/holder-made.yaml')
check(e.get('Holder/Leaf 4/Petal/Bead', {}).get('prefab') == 'prefabs/Holder.prefab' and
      not e.get('Holder/Leaf 4/Petal', {}).get('overrides'),
      "Create Prefab: the nested instance's overrides and added GameObject are the new prefab's")
e = load(ex + '/missing.yaml')
check(e.get('Forest/Tree/Branch/Leaf', {}).get('missingPrefab') is True and 'Forest/Tree/Branch/Leaf/Petal' not in e,
      'a nested prefab gone: its instances are Missing Prefabs')
check(compare(ex + '/holder-redone.yaml', ex + '/relinked.yaml'), 'it comes back: everything as before (added GameObjects in their nested parents)')
e = load(ex + '/apply-added.yaml')
check(e.get('Field/Forest 4/Tree/Branch/Leaf/Petal/Sprout', {}).get('prefab') == 'prefabs/Forest.prefab' and
      e.get('Forest 3/Tree/Branch/Leaf/Petal/Sprout', {}).get('prefab') == 'prefabs/Forest.prefab' and
      'Holder/Leaf 4/Petal/Sprout' not in e and 'Holder/Leaf 4/Petal' in e and 'Forest/Tree/Branch/Leaf/Petal/Sprout' in e,
      'Apply of an added GameObject below a nested instance to the outer prefab: every instance of it, not of the inner one')
e = load(ex + '/revert-nested-root.yaml')
check(e['Forest/Tree/Branch/Leaf']['components']['transform']['scale'] == [1.5, 1.5, 1] and
      not e['Forest/Tree/Branch/Leaf'].get('overrides'),
      "Revert on a nested root takes its prefab's current value, not the holder's stored copy")
src = open(os.path.join(ex, 'bake', 'prefab_prefabs_Forest.cpp')).read()
check('joint2d.bodyA = e' in src and 'joint2d.bodyB = e' in src, 'export: joint bodies are member variables')
check('.setFramebuffer(scene->getComponent<CameraComponent>(e' in src, 'export: a camera link into a nested prefab is bound')
check('members.push_back(NULL_ENTITY);' in src, 'export: a GameObject a layer removed keeps its slot empty')

print('\n%d failure(s)' % len(FAILS))
for f in FAILS:
    print('  - ' + f)
sys.exit(1 if FAILS else 0)
