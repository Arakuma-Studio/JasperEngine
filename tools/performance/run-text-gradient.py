#!/usr/bin/env python3
"""Compare production text gradient sampling with baseline differential checks.

Uses separate Release translation units without LTO. Results are CPU kernel
nanoseconds per glyph sample, not total frame-time or FPS improvements.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess

from run_core import ROOT, definition, source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', default='HEAD')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/performance-text-gradient')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--native', action='store_true', help='exercise the host CPU including FMA')
    parser.add_argument('--allocations', action='store_true', help='count allocations separately from timings')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--cpu', type=int, help='pin measurements to this CPU')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    baseline = source('engine/core/util/TextEffects.cpp', args.baseline_ref)
    reference = '\n'.join(definition(baseline, marker) for marker in (
        'float clamp01(', 'float lerpFloat(', 'Vector4 TextEffects::evaluateGradient('))
    reference = reference.replace('TextEffects::evaluateGradient', 'baselineGradient')
    fixture = out / 'text_gradient.cpp'
    fixture.write_text((ROOT / 'tools/performance/text_gradient.cpp').read_text()
                       .replace('// BASELINE_EVALUATOR', reference))
    flags = ['-std=c++20', '-O3', '-DNDEBUG', '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra', '-Werror']
    if args.sanitize:
        flags = ['-std=c++20', '-O1', '-g', '-DCHECK_ONLY', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections']
    if args.native:
        flags.append('-march=native')
    includes = ['engine/core', 'engine/core/util', 'engine/core/math', 'engine/tests/core']
    extra_sources, link_flags = [], []
    if args.allocations:
        flags += ['-DTRACK_ALLOCATIONS', '-DJE_TEST_COUNTS_ALLOCATIONS']
        extra_sources.append(ROOT / 'engine/tests/core/support/AllocationCounter.cpp')
        link_flags += ['-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=realloc']
        if args.sanitize:
            flags.append('-DJASPERENGINE_SANITIZE_ADDRESS')
    compiler = os.environ.get('CXX', 'c++')
    binaries = {}
    for name, text in [('before', baseline), ('after', source('engine/core/util/TextEffects.cpp'))]:
        variant = out / name
        variant.mkdir(exist_ok=True)
        cpp = variant / 'TextEffects.cpp'
        cpp.write_text(text)
        binary = variant / 'text-gradient'
        subprocess.run([compiler, *flags, *(f'-I{ROOT / p}' for p in includes), str(fixture), str(cpp),
                        str(ROOT / 'engine/core/math/Vector4.cpp'), *(str(p) for p in extra_sources),
                        '-Wl,--gc-sections', *link_flags, '-o', str(binary)], check=True)
        binaries[name] = binary
    (out / 'environment.json').write_text(json.dumps({
        'platform': platform.platform(), 'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
        'flags': flags, 'cpu': args.cpu, 'baseline': args.baseline_ref,
    }, indent=2) + '\n')
    if args.build_only:
        return
    rows = {name: [] for name in binaries}
    pairs = []
    for pair in range(1 if args.sanitize or args.allocations else 9):
        paired = {}
        for name in (('before', 'after') if pair % 2 == 0 else ('after', 'before')):
            affinity = ['taskset', '-c', str(args.cpu)] if args.cpu is not None else []
            output = subprocess.check_output([*affinity, str(binaries[name])], text=True)
            values = list(csv.reader(output.splitlines()))
            rows[name].extend(values)
            paired[name] = {(row[0], row[1]): float(row[2]) for row in values}
        pairs.append(paired)
    for name, values in rows.items():
        with (out / f'{name}.csv').open('w') as stream:
            csv.writer(stream).writerows(values)
    for case, count in sorted({(row[0], row[1]) for row in rows['before']}):
        before, after = (statistics.median(float(row[2]) for row in rows[name] if row[:2] == [case, count])
                         for name in ('before', 'after'))
        if args.allocations:
            print(f'{case} {count} stops: {before:.0f} -> {after:.0f} allocations/sample')
        else:
            ratio = statistics.median(pair['before'][case, count] / pair['after'][case, count] for pair in pairs)
            print(f'{case} {count} stops: {before:.3f} -> {after:.3f} CPU ns/sample; paired ratio {ratio:.3f}x')


if __name__ == '__main__':
    main()
