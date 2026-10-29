#!/usr/bin/env python3
"""Benchmark real Matrix4 vector/AABB transforms, with baseline equivalence checks.

No Scene, graphics context or LTO is used. Separate translation units match the
engine's normal build. CPU nanoseconds measure isolated kernels, not FPS.
"""
import argparse
import csv
import os
from pathlib import Path
import statistics
import subprocess
from run_core import ROOT, source, definition


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', default='HEAD')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/performance-matrix-vectors')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    if args.sanitize:
        out = out / 'sanitize'
    out.mkdir(parents=True, exist_ok=True)
    baseline = source('engine/core/math/Matrix4.cpp', args.baseline_ref)
    methods = '\n'.join(definition(baseline, marker).replace('Matrix4::operator*', 'BaselineMatrix4::operator*')
        for marker in ('Vector3 Matrix4::operator*(', 'Vector4 Matrix4::operator*('))
    cpp = out / 'matrix_vectors.cpp'
    cpp.write_text((ROOT / 'tools/performance/matrix_vectors.cpp').read_text().replace('// BASELINE_METHODS', methods))
    flags = ['-std=c++17', '-O3', '-DNDEBUG', '-ffunction-sections', '-fdata-sections']
    if args.sanitize:
        flags = ['-std=c++17', '-O1', '-g', '-DCHECK_ONLY', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections']
    includes = ['engine/core', 'engine/core/math', 'engine/core/script', 'engine/core/util', 'engine/libs/lua', 'engine/libs/luabridge3', 'engine/renders', 'engine/libs/sokol']
    math_sources = ['Vector3', 'Vector4', 'AABB']
    binaries = {}
    for name, matrix in [('before',baseline), ('after', source('engine/core/math/Matrix4.cpp'))]:
        variant = out / name
        variant.mkdir(exist_ok=True)
        matrix_cpp = variant / 'Matrix4.cpp'
        matrix_cpp.write_text(matrix)
        binary = variant / 'matrix-vectors'
        subprocess.run([os.environ.get('CXX','c++'), *flags, *(f'-I{ROOT/path}' for path in includes), str(cpp), str(matrix_cpp),
            *(str(ROOT/f'engine/core/math/{name}.cpp') for name in math_sources), '-Wl,--gc-sections', '-o', str(binary)], check=True)
        binaries[name] = binary
    rows = {name: [] for name in binaries}
    # Alternate process order to reduce changes in CPU scheduling/frequency.
    for pair in range(3 if not args.sanitize else 1):
        for name in (('before','after') if pair % 2 == 0 else ('after','before')):
            output = subprocess.check_output([str(binaries[name]), '3'], text=True)
            rows[name].extend(csv.reader(output.splitlines()))
    for name, data in rows.items():
        with (out/f'{name}.csv').open('w') as f:
            csv.writer(f).writerows(data)
    for case in ('vec3','vec4','aabb'):
        samples = {name: [float(r[2]) for r in data if r[0]==case] for name,data in rows.items()}
        if not samples['before']:
            continue
        before, after = (statistics.median(samples[name]) for name in ('before','after'))
        print(f'{case}: {before:.3f} -> {after:.3f} CPU ns/transform ({before/after:.2f}x), 9 samples/revision')


if __name__ == '__main__':
    main()
