#!/usr/bin/env python3
"""Paired CPU benchmark/equivalence checks for actual instance-view methods.

Extracts baseline and working-tree production methods without modifying their
bodies, plus the real instance layouts. Links engine math; omits Scene/GPU
initialization. Results measure this CPU kernel, not end-to-end frame time.
"""
import argparse
import csv
import os
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def source(path, revision=None):
    if revision:
        return subprocess.check_output(['git', 'show', f'{revision}:{path}'], cwd=ROOT, text=True)
    return (ROOT / path).read_text()

def definition(text, marker):
    start = text.index(marker)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def methods(revision, name):
    text = source('engine/core/subsystem/RenderSystem.cpp', revision)
    markers = ('unsigned int RenderSystem::selectMeshLod(',
               'bool RenderSystem::isInsideCamera(const float cameraFar, const Plane frustumPlanes[6], const Vector3& center, const float& radius)',
               'void RenderSystem::buildInstanceView(')
    return '\n'.join(definition(text, marker).replace('RenderSystem::', f'{name}::') for marker in markers)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', default='HEAD')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/performance-instance-views')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    template = (ROOT / 'tools/performance/instance_views.cpp').read_text()
    components = source('engine/core/component/InstancedMeshComponent.h')
    layouts = '\n'.join(definition(components, f'struct {name}' + '{') + ';' for name in ('InstanceRenderData', 'InstanceBounds', 'InstanceViewRange'))
    header = source('engine/core/subsystem/RenderSystem.h')
    views = '\n'.join(definition(header, marker) + ';' for marker in ('struct LodView{', 'enum class InstanceOrder{'))
    generated = template.replace('// INSTANCE_TYPES', layouts).replace('// VIEW_TYPES', views)
    generated = generated.replace('// BEFORE_METHODS', methods(args.baseline_ref, 'Before')).replace('// AFTER_METHODS', methods(None, 'After'))
    cpp = out / 'instance_views.cpp'; cpp.write_text(generated)
    binary = out / ('instance-views-sanitize' if args.sanitize else 'instance-views')
    flags = ['-std=c++17', '-O3', '-ffunction-sections', '-fdata-sections']
    if args.sanitize:
        flags = ['-std=c++17', '-O1', '-g', '-DCHECK_ONLY', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections']
    includes = ['engine/core', 'engine/core/script', 'engine/libs/lua', 'engine/libs/luabridge3', 'engine/renders', 'engine/libs/sokol']
    math_sources = ['Matrix4', 'Vector3', 'Vector4', 'Rect', 'Plane']
    subprocess.run([os.environ.get('CXX', 'c++'), *flags, *(f'-I{ROOT / path}' for path in includes), str(cpp),
                    *(str(ROOT / f'engine/core/math/{name}.cpp') for name in math_sources), '-Wl,--gc-sections', '-o', str(binary)], check=True)
    output = subprocess.check_output([str(binary)], text=True)
    (out / 'samples.csv').write_text(output)
    rows = list(csv.reader(output.splitlines()))
    for case, size in sorted({(r[0], r[1]) for r in rows}, key=lambda x: (x[0], int(x[1]))):
        samples = [r for r in rows if (r[0], r[1]) == (case, size)]
        before = statistics.median(float(r[3]) for r in samples)
        after = statistics.median(float(r[4]) for r in samples)
        print(f'{case:15} {size:>6}: {before:.1f} -> {after:.1f} CPU ns/call ({before / after:.2f}x)')

if __name__ == '__main__':
    main()
