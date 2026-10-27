#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Vertex Animation crowd benchmark: N heroes (default 2,000) playing 3 clips out of sync, as
skinned heroes with Animation Players and as one Vertex Animation crowd (Vertices and Bones
bakes), each measured playing with the editor's benchmark CLI (`jasperengine-editor benchmark
--play`). Not gated: it prints a table and writes the JSON results; the numbers go into
tools/performance/vat-crowd.md by hand.

The baked hero comes from the end-to-end bundle of the editor tests (VatEndToEndTest bakes
tools/fixtures/pose-animation hero_animated.gltf's Walk, Idle and Wave in both modes through the
real baker): the script runs them into its work directory unless --bundle names one.

    python3 tools/performance/vat_crowd.py [--editor build/jasperengine-editor]
        [--editor-tests build/jasperengine-editor-tests] [--bundle DIR] [--count 2000]
        [--frames 60] [--warmup 30] [--cases skinned,vat_vertices,vat_bones] [--output DIR]

`--count 1000,2000` runs both sizes. The table's `drawn` column is the heroes the frame drew
(instances / 3 materials): a skinned crowd stops drawing past ~1,000 heroes on a desktop GL
build (each skinned Model creates its own GPU buffers, 7 for the hero, and sokol's buffer pool
holds 8,192), see tools/performance/vat-crowd.md.

Without a DISPLAY the editor runs under xvfb-run with software GL (llvmpipe: the numbers are
relative to each other, not to a GPU).
"""
import argparse
import json
import math
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / 'tools/performance/vat-crowd'
HERO = ROOT / 'tools/fixtures/pose-animation/assets/models/hero_animated.gltf'

CASES = {
    'skinned': {'mode': 'skinned', 'vat': ''},
    'vat_vertices': {'mode': 'vat', 'vat': 'vat/hero_vertices.vat'},
    'vat_bones': {'mode': 'vat', 'vat': 'vat/hero_bones.vat'},
}

IDENTITY = '[[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]'


def transform(position):
    x, y, z = position
    return (f"      transform:\n        position: [{x}, {y}, {z}]\n        rotation: [1, 0, 0, 0]\n        scale: [1, 1, 1]\n"
            f"        localMatrix: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [{x}, {y}, {z}, 1]]\n"
            f"        modelMatrix: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [{x}, {y}, {z}, 1]]\n"
            f"        normalMatrix: {IDENTITY}\n        visible: true\n")


def scene_text(name, index, case, count):
    """a 3D scene: a directional light, a camera seeing the whole grid, the Crowd script"""
    side = math.ceil(math.sqrt(count))
    extent = side * 1.6
    # high and far enough that every hero is in view (both cases draw all of them: the skinned
    # heroes are culled one by one, the instances of an Instanced Mesh are not by default)
    eye = (0, round(extent * 1.0 + 4, 2), round(extent * 1.2 + 4, 2))
    props = case
    return f"""id: {4100 + index}
name: {name}
scene:
  backgroundColor: [0.25, 0.45, 0.65, 1]
  shadowQuality: low
  lightState: on
  globalIlluminationIntensity: 0.3
  globalIlluminationColor: [1, 1, 1]
sceneType: scene_3d
mainCamera: 1002
entities:
  - type: Entity
    entity: 1001
    name: Directional Light
    components:
{transform((0, 10, 0))}      light:
        enabled: true
        type: directional
        direction: [0.35, -0.6, -0.72]
        color: [1, 1, 1]
        intensity: 3
        shadows: false
  - type: Entity
    entity: 1002
    name: Main Camera
    components:
{transform(eye)}      camera:
        enabled: true
        type: camera_perspective
        target: [0, 0, 0]
        up: [0, 1, 0]
        yfov: 0.75
        nearClip: 0.5
        farClip: 500
        useTarget: true
        autoResize: true
  - type: Entity
    entity: 1003
    name: Crowd
    components:
{transform((0, 0, 0))}      script:
        scripts:
          - type: lua
            path: scripts/Crowd.lua
            className: Crowd
            enabled: true
            properties:
              - name: mode
                displayName: Mode
                type: String
                value: {props['mode']}
                defaultValue: vat
              - name: count
                displayName: Count
                type: Int
                value: {count}
                defaultValue: 2000
              - name: vat
                displayName: VAT
                type: String
                value: "{props['vat']}"
                defaultValue: vat/hero_vertices.vat
"""


def make_project(work, bundle, count, cases):
    project = work / 'project'
    shutil.rmtree(project, ignore_errors=True)
    (project / 'assets/models').mkdir(parents=True)
    (project / 'assets/vat').mkdir(parents=True)
    shutil.copytree(TEMPLATE / 'scripts', project / 'scripts')
    shutil.copyfile(HERO, project / 'assets/models/hero.gltf')
    for name in ('hero_vertices', 'hero_bones'):
        for ext in ('.vat', '.vat.glb'):
            shutil.copyfile(bundle / (name + ext), project / 'assets/vat' / (name + ext))
    scenes = []
    for index, key in enumerate(cases):
        scene = f'{key}.scene'
        (project / scene).write_text(scene_text(key, index, CASES[key], count))
        scenes.append(scene)
    (project / 'project.yaml').write_text(
        'name: VatCrowd\nassetsDir: assets\nluaDir: .\ncanvasWidth: 1280\ncanvasHeight: 720\nscalingMode: fitwidth\n'
        'scenes:\n' + ''.join(f'  - filepath: {s}\n' for s in scenes))
    return project


def editor_env():
    env = dict(os.environ)
    if not env.get('DISPLAY'):
        env['LIBGL_ALWAYS_SOFTWARE'] = '1'
    return env


def run_editor(editor, args, timeout, cwd):
    command = [str(editor), *args]
    if not os.environ.get('DISPLAY'):
        command = ['xvfb-run', '-a', '-s', '-screen 0 1600x900x24', *command]
    # the editor's file dialogs (NFD) need a session bus
    if not os.environ.get('DBUS_SESSION_BUS_ADDRESS') and shutil.which('dbus-run-session'):
        command = ['dbus-run-session', '--', *command]
    # in the work directory: the editor writes its layout file (editor_layout_v2.ini) there
    return subprocess.run(command, env=editor_env(), capture_output=True, text=True, timeout=timeout, cwd=str(cwd))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--editor', type=Path, default=ROOT / 'build/jasperengine-editor')
    parser.add_argument('--editor-tests', type=Path, default=ROOT / 'build/jasperengine-editor-tests')
    parser.add_argument('--bundle', type=Path, help='an end-to-end bundle (tools/vat_e2e.sh KEEP_BUNDLE=1); default: baked here')
    parser.add_argument('--count', default='2000', help='heroes; a comma-separated list runs each (e.g. 1000,2000)')
    parser.add_argument('--frames', type=int, default=60)
    parser.add_argument('--warmup', type=int, default=30)
    parser.add_argument('--cases', default='skinned,vat_vertices,vat_bones')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/performance-vat-crowd')
    args = parser.parse_args()
    cases = [c for c in args.cases.split(',') if c]
    for c in cases:
        if c not in CASES:
            parser.error(f'unknown case {c} (one of {", ".join(CASES)})')
    counts = [int(c) for c in args.count.split(',') if c]

    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=True)
    bundle = args.bundle.resolve() if args.bundle else work / 'bundle'
    if not args.bundle:
        bundle.mkdir(exist_ok=True)
        env = dict(os.environ, JASPER_VAT_E2E_DIR=str(bundle))
        subprocess.run([str(args.editor_tests), '--gtest_filter=VatEndToEnd.BakesTheRenderParityBundle'], env=env, check=True,
                       stdout=subprocess.DEVNULL)

    results = []
    for count in counts:
        directory = work / str(count)
        directory.mkdir(exist_ok=True)
        project = make_project(directory, bundle, count, cases)
        for key in cases:
            out = directory / f'{key}.json'
            r = run_editor(args.editor, ['benchmark', '--project', str(project), '--scene', f'{key}.scene', '--play',
                                         '--frames', str(args.frames), '--warmup', str(args.warmup), '--name', key, '--out', str(out)],
                           timeout=3600, cwd=directory)
            if r.returncode != 0 or not out.exists():
                print(r.stdout[-3000:])
                print(r.stderr[-3000:])
                raise SystemExit(f'benchmark {key} ({count}) failed (exit {r.returncode})')
            result = json.loads(out.read_text())
            result['count'] = count
            # every hero draws its 3 materials: the heroes the frame drew
            result['heroesDrawn'] = round(result['avgInstances'] / 3)
            results.append(result)

    (work / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(f'{"case":14} {"count":>6} {"drawn":>6} {"avg ms":>9} {"wall fps":>9} {"draws":>8} {"instances":>10} {"triangles":>12}')
    for r in results:
        print(f'{r["name"]:14} {r["count"]:6d} {r["heroesDrawn"]:6d} {r["avgMs"]:9.2f} {r["wallFps"]:9.2f} {r["avgDrawCalls"]:8.0f} '
              f'{r["avgInstances"]:10.0f} {r["avgTriangles"]:12.0f}')
    print(f'results: {work / "results.json"} (viewport {results[0]["viewport"] if results else "-"})')


if __name__ == '__main__':
    main()
