#!/usr/bin/env python3
"""Measure exact EntityRegistry::isParentOf before/after in the same process.

Uses real Transform/EntityManager/ComponentManager/ComponentArray. The owner only
avoids initializing a graphics/audio Scene. Measures CPU kernel time, not FPS.
Differential assertions cover valid trees, reordered transforms, missing parents,
missing transforms, self-parenting, cycles and direct-parent order exceptions.
Nine paired samples alternate run order, independently calibrating each method
to at least 10ms process CPU per recorded timing block.
"""
import argparse
import csv
import os
from pathlib import Path
import statistics
import subprocess
from run_core import ROOT, source, definition

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline-ref', default='HEAD')
parser.add_argument('--output', type=Path, default=ROOT / 'build/performance-ancestry')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
methods = []
for revision, variant in [(args.baseline_ref, 'before'), (None, 'after')]:
    method = definition(source('engine/core/registry/EntityRegistry.cpp', revision), 'bool EntityRegistry::isParentOf')
    methods.append(method.replace('EntityRegistry::isParentOf', f'Registry::{variant}'))
cpp = args.output / 'ancestry.cpp'
cpp.write_text((ROOT / 'tools/performance/ancestry.cpp').read_text().replace('// PRODUCTION_METHODS', '\n\n'.join(methods)))
binary = args.output / 'ancestry'
flags = ['-O3']
if args.sanitize:
    flags = ['-O1', '-g', '-DCORE_CHECK_ONLY', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
includes = [ROOT / 'engine/core', ROOT / 'engine/core/ecs', ROOT / 'engine/core/math']
math = [ROOT / f'engine/core/math/{name}.cpp' for name in ('Vector2', 'Vector3', 'Vector4', 'Matrix3', 'Matrix4', 'Quaternion')]
subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', *flags, *(f'-I{path}' for path in includes), str(cpp), str(ROOT / 'engine/core/ecs/ComponentManager.cpp'), *(str(path) for path in math), '-o', str(binary)], check=True)
output = subprocess.check_output([str(binary)], text=True)
(args.output / 'samples.csv').write_text(output)
print('Differential behavior checks passed.', flush=True)
rows = list(csv.reader(output.splitlines()))
if rows:
    minimum_cpu_ms = min(float(row[col]) for row in rows for col in (10, 11)) / 1e6
    assert minimum_cpu_ms >= 10, f'Insufficient timing calibration: {minimum_cpu_ms} ms'
    print(f'All {len(rows) * 2} timing blocks used >=10 ms process CPU (minimum {minimum_cpu_ms:.3f} ms).', flush=True)
for shape, size, query in sorted({tuple(row[:3]) for row in rows}, key=lambda row: (row[0], int(row[1]), row[2])):
    samples = [row for row in rows if tuple(row[:3]) == (shape, size, query)]
    before, after = [statistics.median(float(row[col]) for row in samples) for col in (4, 5)]
    speedup = statistics.median(float(row[4]) / float(row[5]) for row in samples)
    print(f'{shape:5} {size:>5} {query:9}: {before:.2f} -> {after:.2f} CPU ns/call; median paired speedup {speedup:.2f}x', flush=True)
