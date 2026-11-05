#!/usr/bin/env python3
"""Compare production quaternion rotation-matrix and TRS CPU costs against a revision.

Uses separate translation units without LTO, matching the normal editor build.
Differential checks compare every output float, including signed zero and NaNs.
Timings are isolated CPU kernels, not frame times.
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
    parser.add_argument('--output', type=Path, default=ROOT / 'build/performance-quaternion-matrix')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--native', action='store_true', help='also allow host SIMD/FMA instructions')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--run-only', action='store_true')
    args = parser.parse_args()
    if args.compile_only and args.run_only:
        parser.error('--compile-only and --run-only are mutually exclusive')
    out = args.output.resolve()
    if args.native:
        out = out / 'native'
    if args.sanitize:
        out = out / 'sanitize'
    out.mkdir(parents=True, exist_ok=True)
    baseline = source('engine/core/math/Quaternion.cpp', args.baseline_ref)
    method = definition(baseline, 'Matrix4 Quaternion::getRotationMatrix()').replace('Quaternion::', 'BaselineQuaternion::')
    cpp = out / 'quaternion_matrix.cpp'
    cpp.write_text((ROOT / 'tools/performance/quaternion_matrix.cpp').read_text().replace('// BASELINE_METHOD', method))
    flags = ['-std=c++20', '-O3', '-DNDEBUG', '-ffunction-sections', '-fdata-sections']
    if args.sanitize:
        flags = ['-std=c++20', '-O1', '-g', '-DCHECK_ONLY', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections']
    if args.native:
        flags.append('-march=native')
    includes = ['engine/core', 'engine/core/math', 'engine/core/script', 'engine/core/util',
                'engine/libs/lua', 'engine/libs/luabridge3', 'engine/renders', 'engine/libs/sokol']
    binaries = {}
    for name, quaternion in [('before', baseline), ('after', source('engine/core/math/Quaternion.cpp'))]:
        variant = out / name
        variant.mkdir(exist_ok=True)
        quaternion_cpp = variant / 'Quaternion.cpp'
        quaternion_cpp.write_text(quaternion)
        binary = variant / 'quaternion-matrix'
        if not args.run_only:
            subprocess.run([os.environ.get('CXX', 'c++'), *flags, *(f'-I{ROOT / path}' for path in includes),
                            str(cpp), str(quaternion_cpp),
                            *(str(ROOT / f'engine/core/math/{name}.cpp') for name in ('Matrix4', 'Vector3', 'Vector4')),
                            '-Wl,--gc-sections', '-o', str(binary)], check=True)
        binaries[name] = binary
    if args.compile_only:
        return
    rows = {name: [] for name in binaries}
    for pair in range(1 if args.sanitize else 3):
        for name in (('before', 'after') if pair % 2 == 0 else ('after', 'before')):
            output = subprocess.check_output([str(binaries[name]), '3'], text=True)
            rows[name].extend(csv.reader(output.splitlines()))
    for name, data in rows.items():
        with (out / f'{name}.csv').open('w') as file:
            csv.writer(file).writerows(data)
    for case in ('rotation', 'trs'):
        samples = {name: [float(row[2]) for row in data if row[0] == case] for name, data in rows.items()}
        if not samples['before']:
            continue
        before, after = (statistics.median(samples[name]) for name in ('before', 'after'))
        print(f'{case}: {before:.3f} -> {after:.3f} CPU ns/call ({before / after:.2f}x), 9 samples/revision')


if __name__ == '__main__':
    main()
