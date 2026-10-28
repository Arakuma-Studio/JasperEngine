#!/usr/bin/env python3
"""Scripted checks of Unity's instance-edit semantics (editor/project/PREFABS.md, section 12).

    JASPER_EDITOR=<editor executable> tools/prefab-instance-edit-test.py

Builds the `legacy` fixture (tools/prefab-fixtures.py) in a temporary directory, then runs
jasperengine-editor scene-dump --ops on copies of it: instance edits recorded as overrides of
legacy default overrides, Apply to the owning / inner / outer prefab, Apply All /
Revert All, and deep state/component propagation. Generic flat edit/undo cases belong to
tools/mcp/tests/test_prefab.py. Play
mode edits gone after Stop, overrides kept across a save and a reload. Exits non-zero on a
failure. Needs a Linux editor build (tools/build.sh linux).
"""
import os, shlex, shutil, subprocess, sys, tempfile, yaml

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
EDITOR = shlex.split(os.environ.get('JASPER_EDITOR', os.path.join(ROOT, 'build', 'linux', 'editor-install', 'bin', 'jasperengine-editor')))
WORK = tempfile.mkdtemp(prefix='prefab-instance-edit-')


def run(project, ops, name):
    work = os.path.join(WORK, 'runs', name)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    proj = work + '/project'
    shutil.copytree(project, proj)
    dumps = []
    full = []
    for op in ops:
        if op.get('op') == 'dump':
            path = work + '/dump%d.yaml' % len(dumps)
            dumps.append(path)
            op = dict(op, out=path)
        full.append(op)
    opsPath = work + '/ops.yaml'
    yaml.safe_dump(full, open(opsPath, 'w'), sort_keys=False)
    res = subprocess.run([*EDITOR, 'scene-dump', '--project', proj, '--ops', opsPath, '--out', work + '/final.yaml'],
                         capture_output=True, text=True, timeout=120)
    open(work + '/log.txt', 'w').write(res.stdout + '\n----\n' + res.stderr)
    if res.returncode != 0:
        print(res.stdout[-3000:], res.stderr[-3000:])
        raise SystemExit('editor failed for ' + name)
    out = [yaml.safe_load(open(p)) for p in dumps] + [yaml.safe_load(open(work + '/final.yaml'))]
    return out, res.stdout, proj


def ents(dump, scene=0):
    return dump['scenes'][scene]['entities']


FAILS = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        FAILS.append(msg)


def done():
    print('\n%d failure(s)' % len(FAILS))
    for f in FAILS:
        print('  - ' + f)
    sys.exit(1 if FAILS else 0)


P = os.path.join(WORK, 'legacy')
subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'prefab-fixtures.py'), 'legacy', P], check=True)

print("== explicit overrides and legacy nested propagation")

RB = 'Rock B/Rock/Pebble'
RA = 'Rock A/Rock/Pebble'
TA = 'Tower A/Base/Rock/Rock/Pebble'
CA = 'Castle A/Keep/Tower/Base/Rock/Rock/Pebble'

def pos(e, path, key='position'):
    return e[path]['components']['transform'][key]

def ov(e, path):
    return e[path].get('overrides', {})

# An explicit override remains explicit when its value is set back to the base's value.
# Unlike an ordinary changed-value Revert, this must clear metadata without changing pixels.
ops = [
    {'op': 'set', 'entity': RB, 'component': 'transform', 'property': 'position', 'value': [7, 8, 0]},
    {'op': 'set', 'entity': RB, 'component': 'transform', 'property': 'position', 'value': [90, 0, 0]},
    {'op': 'dump'},
    {'op': 'revertProperty', 'entity': RB, 'component': 'transform', 'properties': ['position']},
    {'op': 'dump'},
]
d, out, proj = run(P, ops, 'explicit-equal')
explicit, reverted, final = d
check(pos(ents(explicit), RB) == [90, 0, 0] and ov(ents(explicit), RB).get('properties', {}).get('transform') == ['position'],
      'value equal to the prefab: still overridden')
check('overrides' not in ents(reverted)[RB], 'revert: override gone')

# ---- A/B/C: apply to the owning prefab, undo
ops = [
    {'op': 'set', 'entity': RB, 'component': 'transform', 'property': 'position', 'value': [7, 8, 0]},
    {'op': 'save'},
    {'op': 'dump'},
    {'op': 'applyProperty', 'entity': RB, 'component': 'transform', 'properties': ['position']},
    {'op': 'dump'},
    {'op': 'undo'},
    {'op': 'dump'},
]
d, out, proj = run(P, ops, 't2a')
saved, applied, unapplied, final = d
rock = yaml.safe_load(open(proj + '/prefabs/Rock.prefab'))
def findEntity(node, name):
    if isinstance(node, dict):
        if node.get('name') == name and node.get('type') == 'Entity':
            return node
        for v in node.values():
            r = findEntity(v, name)
            if r: return r
    if isinstance(node, list):
        for v in node:
            r = findEntity(v, name)
            if r: return r
    return None
peb = findEntity(rock, 'Pebble')
check(peb is not None and peb['components']['transform']['position'] == [90, 0, 0], 'saved: Rock.prefab Pebble unchanged by an instance edit')
e = ents(saved)
check(pos(e, TA) == [90, 0, 0], 'other scene instance (nested in Tower) unaffected')
e = ents(applied)
check('overrides' not in e[RB], 'apply: override cleared')
check(pos(e, RB) == [7, 8, 0], 'apply: instance keeps the value')
check(pos(e, TA) == [7, 8, 0], 'apply: Tower A nested Rock follows the prefab')
check(pos(e, CA) == [7, 8, 0], 'apply: Castle A (3 levels) follows the prefab')
check(pos(e, RA) == [90, 0, 0], 'apply: Rock A keeps its whole-transform override')
e = ents(unapplied)
check(ov(e, RB).get('properties', {}).get('transform') == ['position'], 'undo apply: override back')
check(pos(e, TA) == [90, 0, 0] and pos(e, CA) == [90, 0, 0], 'undo apply: other instances back')

# ---- D/E: nested apply targets
ops = [
    {'op': 'set', 'entity': TA, 'component': 'transform', 'property': 'scale', 'value': [2, 2, 1]},
    {'op': 'applyProperty', 'entity': TA, 'component': 'transform', 'properties': ['scale'], 'target': 'prefabs/Tower.prefab'},
    {'op': 'dump'},
    {'op': 'undo'},
    {'op': 'dump'},
    {'op': 'applyProperty', 'entity': TA, 'component': 'transform', 'properties': ['scale'], 'target': 'prefabs/Rock.prefab'},
    {'op': 'dump'},
    {'op': 'undo'},
    {'op': 'dump'},
]
d, out, proj = run(P, ops, 't2d')
toTower, undoTower, toRock, undoRock, final = d
e = ents(toTower)
check('overrides' not in e[TA] or 'transform' not in ov(e, TA).get('properties', {}), 'apply as override in Tower: scene override cleared')
check(pos(e, TA, 'scale') == [2, 2, 1], 'apply in Tower: Tower A keeps the value')
check(pos(e, CA, 'scale') == [2, 2, 1], 'apply in Tower: Castle (nesting Tower) gets it')
check(pos(e, RB, 'scale') == [1, 1, 1], 'apply in Tower: plain Rock instance does not')
e = ents(undoTower)
check(ov(e, TA).get('properties', {}).get('transform') == ['scale'], 'undo apply in Tower: override back')
check(pos(e, CA, 'scale') == [1, 1, 1], 'undo apply in Tower: Castle back')
e = ents(toRock)
check(pos(e, RB, 'scale') == [2, 2, 1], 'apply to Rock: plain Rock instance gets it')
check(pos(e, CA, 'scale') == [2, 2, 1], 'apply to Rock: Castle gets it')
e = ents(undoRock)
check(pos(e, RB, 'scale') == [1, 1, 1], 'undo apply to Rock')

# ---- J: Play
ops = [
    {'op': 'playStart'},
    {'op': 'set', 'entity': RB, 'component': 'transform', 'property': 'position', 'value': [5, 5, 5]},
    {'op': 'setName', 'entity': TA, 'name': 'PlayName'},
    {'op': 'addComponent', 'entity': RB, 'component': 'light'},
    {'op': 'removeComponent', 'entity': RB, 'component': 'sprite'},
    {'op': 'canApply', 'entity': RB, 'component': 'transform', 'properties': ['position']},
    {'op': 'applyProperty', 'entity': RB, 'component': 'transform', 'properties': ['position']},
    {'op': 'dump'},
    {'op': 'playStop'},
    {'op': 'dump'},
    {'op': 'save'},
]
d, out, proj = run(P, ops, 't2j')
playing, stopped, final = d
check('canApply: no' in out and 'play mode' in out.lower(), 'play: apply refused for the play-mode restriction')
e = ents(playing)
check(ov(e, RB).get('properties', {}).get('transform') == ['position'], 'play: edit recorded while playing')
check(pos(e, 'Tower A/Base/Rock/Rock/PlayName') == [90, 0, 0], 'play: other instance untouched')
e = ents(stopped)
check('overrides' not in e[RB] and pos(e, RB) == [90, 0, 0], 'stop: override and value back')
check('sprite' in e[RB]['components'] and 'light' not in e[RB]['components'], 'stop: components back')
check(TA in e and not (ov(e, TA).get('state', 0) & 1), 'stop: name back')
rock = yaml.safe_load(open(proj + '/prefabs/Rock.prefab'))
check(findEntity(rock, 'Pebble')['components']['transform']['position'] == [90, 0, 0], 'stop: prefab asset unchanged')

# ---- K: persistence across a reload
ops = [
    {'op': 'set', 'entity': RB, 'component': 'transform', 'property': 'position', 'value': [3, 3, 3]},
    {'op': 'setName', 'entity': TA, 'name': 'Renamed'},
    {'op': 'addComponent', 'entity': RB, 'component': 'light'},
    {'op': 'save'},
]
d, out, proj = run(P, ops, 't2k')
d2, out2, proj2 = run(proj, [], 't2k2')
e = ents(d2[0])
check(pos(e, RB) == [3, 3, 3] and ov(e, RB).get('properties', {}).get('transform') == ['position'], 'reload: property override kept')
check(ov(e, RB).get('added') == ['light'], 'reload: added component kept')
check(ov(e, 'Tower A/Base/Rock/Rock/Renamed').get('state') == 1, 'reload: name override kept')


print("== nested roots, state/component apply and revert, Apply All / Revert All")

TAROOT = 'Tower A/Base/Rock'
CAROOT = 'Castle A/Keep/Tower/Base/Rock'

# nested instance root: Apply to Prefab 'Rock' (its own) / as Override in Prefab 'Tower'
ops = [
    {'op': 'setName', 'entity': 'Rock B', 'name': 'Rock Renamed'},
    {'op': 'dump'},
    {'op': 'undo'},
    {'op': 'dump'},
    {'op': 'set', 'entity': TAROOT, 'component': 'transform', 'property': 'scale', 'value': [3, 3, 1]},
    {'op': 'applyProperty', 'entity': TAROOT, 'component': 'transform', 'properties': ['scale'], 'target': 'prefabs/Rock.prefab'},
    {'op': 'dump'},
    {'op': 'undo'},
    {'op': 'dump'},
    {'op': 'applyProperty', 'entity': TAROOT, 'component': 'transform', 'properties': ['scale'], 'target': 'prefabs/Tower.prefab'},
    {'op': 'dump'},
]
d, out, proj = run(P, ops, 't3a')
rootNamed, before, toRock, undone, toTower, final = d
e = ents(rootNamed)
check('Rock Renamed' in e and not (ov(e, 'Rock Renamed').get('state', 0) & 1),
      'rename instance root: no override (default override)')
e0 = ents(before)
e = ents(toRock)
print('  Rock A root scale', pos(e0, 'Rock A', 'scale'), '->', pos(e, 'Rock A', 'scale'))
check('transform' not in ov(e, TAROOT).get('properties', {}), 'apply nested root to Rock: override cleared')
check(pos(e, 'Rock A', 'scale') == [3, 3, 1], 'apply nested root to Rock: Rock instances follow')
e = ents(undone)
check(pos(e, 'Rock A', 'scale') == pos(e0, 'Rock A', 'scale'), 'undo nested root apply')
e = ents(toTower)
check(pos(e, CAROOT, 'scale') == [3, 3, 1], 'apply nested root in Tower: Castle follows')
check(pos(e, 'Rock A', 'scale') == pos(e0, 'Rock A', 'scale'), 'apply nested root in Tower: Rock instances do not')

# state, added and removed components: apply and revert
ops = [
    {'op': 'setState', 'entity': RB, 'active': False},
    {'op': 'applyState', 'entity': RB, 'state': 2},
    {'op': 'dump'},
    {'op': 'undo'},
    {'op': 'revertState', 'entity': RB, 'state': 2},
    {'op': 'dump'},
    {'op': 'addComponent', 'entity': RB, 'component': 'light'},
    {'op': 'applyAddedComponent', 'entity': RB, 'component': 'light'},
    {'op': 'dump'},
    {'op': 'undo'},
    {'op': 'revertAddedComponent', 'entity': RB, 'component': 'light'},
    {'op': 'dump'},
    {'op': 'removeComponent', 'entity': RB, 'component': 'sprite'},
    {'op': 'applyRemovedComponent', 'entity': RB, 'component': 'sprite'},
    {'op': 'dump'},
    {'op': 'undo'},
    {'op': 'revertRemovedComponent', 'entity': RB, 'component': 'sprite'},
    {'op': 'dump'},
    {'op': 'set', 'entity': RB, 'component': 'transform', 'property': 'scale', 'value': [4, 4, 4]},
    {'op': 'set', 'entity': RB, 'component': 'transform', 'property': 'position', 'value': [4, 4, 4]},
    {'op': 'revertComponent', 'entity': RB, 'component': 'transform'},
    {'op': 'dump'},
]
d, out, proj = run(P, ops, 't3b')
stApplied, stReverted, addApplied, addReverted, remApplied, remReverted, compReverted, final = d
e = ents(stApplied)
check(all(e[path].get('active', True) is False for path in (RB, RA, TA)),
      'apply state: the authored inactive state reaches the owning prefab instances')
check(not (ov(e, RB).get('state', 0) & 2), 'apply state: override cleared')
print('  active of Rock A pebble after apply', e[RA].get('active'), e[TA].get('active'))
e = ents(stReverted)
check(not (ov(e, RB).get('state', 0) & 2), 'revert state: override cleared')
e = ents(addApplied)
check('light' in e[RA]['components'] and 'light' in e[TA]['components'], 'apply added component: every instance gets it')
check('added' not in ov(e, RB), 'apply added component: override cleared')
e = ents(addReverted)
check('light' not in e[RB]['components'] and 'light' not in e[RA]['components'], 'revert added component (after undo of apply)')
e = ents(remApplied)
check('sprite' not in e[RA]['components'] and 'sprite' not in e[TA]['components'], 'apply removed component: gone from every instance')
e = ents(remReverted)
check('sprite' in e[RB]['components'] and 'sprite' in e[RA]['components'] and 'removed' not in ov(e, RB), 'revert removed component')
e = ents(compReverted)
check('overrides' not in e[RB] and pos(e, RB) == [90, 0, 0] and pos(e, RB, 'scale') == [1, 1, 1], 'revert component: every property back')

# Apply All / Revert All
ops = [
    {'op': 'dump'},
    {'op': 'revertAll', 'entity': 'Tower A'},
    {'op': 'dump'},
    {'op': 'undo'},
    {'op': 'dump'},
    {'op': 'set', 'entity': TA, 'component': 'transform', 'property': 'position', 'value': [6, 6, 0]},
    {'op': 'applyAll', 'entity': 'Tower A'},
    {'op': 'dump'},
    {'op': 'undo'},
    {'op': 'dump'},
]
d, out, proj = run(P, ops, 't3c')
before, revAll, revUndo, appAll, appUndo, final = d
e0 = ents(before)
print('  Tower A overrides before:', {k: v.get('overrides') for k, v in e0.items() if k.startswith('Tower A') and v.get('overrides')})
e = ents(revAll)
check(not any(v.get('overrides') for k, v in e.items() if k.startswith('Tower A/')), 'revert all: no overrides left below the root')
check('Tower A/Base/Rock/Rock/Leaf' not in e, 'revert all: added GameObject deleted')
e = ents(revUndo)
check({k: v.get('overrides') for k, v in e.items() if k.startswith('Tower A')} == {k: v.get('overrides') for k, v in e0.items() if k.startswith('Tower A')}, 'undo revert all')
check('Tower A/Base/Rock/Rock/Leaf' in e, 'undo revert all: added GameObject back')
e = ents(appAll)
left = {k: v.get('overrides') for k, v in e.items() if k.startswith('Tower A/') and v.get('overrides')}
print('  left after apply all:', left)
check(pos(e, CAROOT + '/Rock/Pebble') == [6, 6, 0], 'apply all: Castle (nests Tower) gets the nested override')
check(pos(e, RA) == [90, 0, 0] and pos(e, RB) == [90, 0, 0], 'apply all: plain Rock instances unaffected (applied as Tower overrides)')
e = ents(appUndo)
check(pos(e, CAROOT + '/Rock/Pebble') == [90, 0, 0], 'undo apply all')


done()
