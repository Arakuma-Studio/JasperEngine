#!/usr/bin/env python3
"""Compare the production scheduler's serial singleton waves and heap allocations.

Compiles complete SystemScheduler.cpp revisions against their production scheduler
header. Only Scene ownership, empty command playback and serial jobs are adapters;
the measured kernels include the actual planning, wave execution, timing and tracing.
Nine process pairs alternate order, each calibrated to >=50 ms process CPU per case.
Timing uses the default allocator, without allocation overrides or counters. Use
--allocations for a separate, untimed instrumentation build that reports counts.
The full engine SchedulerTest suite covers real scenes and parallel worker behavior.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess

from run_core import ROOT, source

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline-ref', default='HEAD')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--allocations', action='store_true', help='count allocations separately, without timing')
modes = parser.add_mutually_exclusive_group()
modes.add_argument('--compile-only', action='store_true')
modes.add_argument('--run-only', action='store_true', help='reuse previously compiled binaries for quiet paired timing')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get('CXX', 'g++')
fixture = ROOT / 'tools/performance/scheduler_fixture'
build_mode = {'sanitize': args.sanitize, 'allocations': args.allocations}
if args.run_only:
    metadata = json.loads((args.output / 'metadata.json').read_text())
    if metadata.get('build_mode') != build_mode:
        parser.error('cached build mode differs; run with --compile-only first')
binaries = {}
for revision, name in [(args.baseline_ref, 'before'), (None, 'after')]:
    directory = args.output / name
    directory.mkdir(exist_ok=True)
    binary = directory / 'scheduler'
    binaries[name] = binary
    if args.run_only:
        if not binary.is_file():
            parser.error(f'{binary} is missing; run with --compile-only first')
        for filename in ['SystemScheduler.cpp', 'SystemScheduler.h']:
            expected = source('engine/core/ecs/' + filename, revision)
            if (directory / filename).read_text() != expected:
                parser.error(f'{directory / filename} is stale; run with --compile-only first')
        continue
    for filename in ['SystemScheduler.cpp', 'SystemScheduler.h']:
        (directory / filename).write_text(source('engine/core/ecs/' + filename, revision))
    flags = ['-O3']
    if args.sanitize:
        flags = ['-O1', '-g', '-DCORE_CHECK_ONLY', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    if args.allocations:
        flags += ['-DSCHEDULER_COUNT_ALLOCATIONS']
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                    '-I' + str(directory), '-I' + str(fixture), '-I' + str(ROOT / 'engine/core'),
                    '-I' + str(ROOT / 'engine/core/ecs'), '-I' + str(ROOT / 'engine/core/log'),
                    str(directory / 'SystemScheduler.cpp'), str(ROOT / 'tools/performance/scheduler.cpp'),
                    str(ROOT / 'engine/core/ecs/ChangeTracking.cpp'), '-o', str(binary)], check=True)
if not args.run_only:
    (args.output / 'metadata.json').write_text(json.dumps({
        'baseline': args.baseline_ref, 'platform': platform.platform(),
        'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
        'scope': 'serial scheduler kernel with Scene/jobs adapters; not frame-time or parallel measurement',
        'build_mode': build_mode,
    }, indent=2) + '\n')
if args.compile_only:
    raise SystemExit(0)
samples = {'before': [], 'after': []}
for pair in range(1 if args.sanitize or args.allocations else 9):
    for name in (['before', 'after'] if pair % 2 == 0 else ['after', 'before']):
        output = subprocess.check_output([str(binaries[name])], text=True)
        (args.output / f'{name}-{pair}.csv').write_text(output)
        samples[name].extend(csv.reader(output.splitlines()))
print('Behavior checks passed for both revisions; 0 skipped, 0 disabled.')
if args.allocations:
    for name in ['before', 'after']:
        rows = samples[name]
        assert len(rows) == 1 and rows[0][0] == 'allocations'
        print(f'{name}: {int(rows[0][1])} allocations/frame for 1 singleton wave; '
              f'{int(rows[0][2])} for 20 singleton waves (untimed)')
    raise SystemExit(0)
if args.sanitize:
    assert all(not rows for rows in samples.values())
    raise SystemExit(0)
for case in ['declared', 'exclusive']:
    before = [float(row[1]) for row in samples['before'] if row[0] == case]
    after = [float(row[1]) for row in samples['after'] if row[0] == case]
    assert len(before) == len(after) == 9
    print(f'{case}: {statistics.median(before):.2f} -> {statistics.median(after):.2f} CPU ns/20 systems; '
          f'{statistics.median(b / a for b, a in zip(before, after)):.3f}x paired speedup')
    for name in ['before', 'after']:
        rows = [row for row in samples[name] if row[0] == case]
        assert min(float(row[4]) for row in rows) >= 50000000
