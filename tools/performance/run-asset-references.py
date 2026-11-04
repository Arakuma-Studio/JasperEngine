#!/usr/bin/env python3
"""Compare exact production Find References queries with the default allocator.

The fixture extracts collectReferences/rebuildReverse from both revisions and uses
real Guid/DepEdge types. A small database stand-in supplies only getInfo (container
sub-assets and former GUIDs); file loading, overlays and editor startup are outside
this query kernel. --allocations builds separate instrumentation for untimed
allocation counts; add --check-cuts to enforce its allocation regression control.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess

from run_core import ROOT, source, definition


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', default='HEAD')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/performance-asset-references')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--sanitize', action='store_true')
    modes.add_argument('--allocations', action='store_true', help='count allocations without timing')
    parser.add_argument('--check-cuts', action='store_true')
    args = parser.parse_args()
    if args.check_cuts and not args.allocations:
        parser.error('--check-cuts requires --allocations')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    methods = []
    for revision, name in [(args.baseline_ref, 'BeforeIndex'), (None, 'AfterIndex')]:
        text = source('editor/asset/AssetIndex.cpp', revision)
        for marker in ('void AssetIndex::collectReferences(', 'void AssetIndex::rebuildReverse()'):
            methods.append(definition(text, marker).replace('AssetIndex::', f'{name}::'))
    cpp = output / 'asset_references.cpp'
    cpp.write_text((ROOT / 'tools/performance/asset_references.cpp').read_text().replace('// PRODUCTION_METHODS', '\n\n'.join(methods)))
    compiler = os.environ.get('CXX', 'g++')
    metadata = {'baseline_ref': args.baseline_ref,
                'baseline_commit': subprocess.check_output(['git', 'rev-parse', args.baseline_ref], cwd=ROOT, text=True).strip(),
                'working_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                'platform': platform.platform(),
                'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
                'affinity': sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
                'sanitize': args.sanitize,
                'mode': 'allocations' if args.allocations else 'correctness' if args.sanitize else 'timing',
                'allocator': 'instrumented' if args.allocations else 'default'}
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    flags = ['-O3', '-DNDEBUG']
    if args.sanitize:
        flags = ['-O1', '-g', '-DCHECK_ONLY', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    elif args.allocations:
        flags.append('-DCOUNT_ALLOCATIONS')
    binary = output / 'asset_references'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                    f'-I{ROOT / "engine/core"}', f'-I{ROOT / "editor"}',
                    str(cpp), '-o', str(binary)], check=True)
    run = subprocess.run([str(binary), *(['--check-cuts'] if args.check_cuts else [])], text=True, capture_output=True)
    (output / 'samples.csv').write_text(run.stdout)
    (output / 'checks.log').write_text(run.stderr)
    print(run.stderr, end='')
    if run.returncode:
        raise SystemExit(run.returncode)
    rows = list(csv.reader(run.stdout.splitlines()))
    expected = {'one-referrer', 'fan-in-32', 'fan-in-1024', 'fan-in-10000', 'absent-path-1024', 'absent-path-10000'}
    if args.sanitize:
        if rows:
            raise RuntimeError('correctness mode unexpectedly emitted measurements')
    elif {r[0] for r in rows if r} != expected or any(len(r) != 4 for r in rows) or \
            any(sum(r[0] == name for r in rows) != 9 for name in expected):
        raise RuntimeError('expected nine samples for each of the six benchmark cases')
    for name in dict.fromkeys(r[0] for r in rows):
        samples = [r for r in rows if r[0] == name]
        before, after = [statistics.median(float(r[c]) for r in samples) for c in (2, 3)]
        if args.allocations:
            print(f'{name:28}: {before:g} -> {after:g} allocations (untimed)')
        else:
            print(f'{name:28}: {before:.1f} -> {after:.1f} CPU ns ({before / after:.2f}x); allocation counts unavailable')


if __name__ == '__main__':
    main()
