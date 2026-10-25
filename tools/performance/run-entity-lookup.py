#!/usr/bin/env python3
"""Paired exact-production registry lookup CPU/allocation benchmark.

Compiles real EntityManager and extracted EntityRegistry lookup methods, with
minimal TagManager and parent predicate fixtures to avoid graphics startup.
Both versions share identical metadata and fixture predicates. Differential
checks exercise ordered duplicates, recreation, deletion, renaming, retagging,
empty names, invalid tags, parent filters, and long strings.
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
parser.add_argument('--output', type=Path, default=ROOT / 'build/performance-entity-lookup')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
markers = ['Entity EntityRegistry::findEntity(const std::string& name) const',
           'Entity EntityRegistry::findEntity(const std::string& name, Entity parent) const',
           'Entity EntityRegistry::findEntityByTag', 'std::vector<Entity> EntityRegistry::findEntitiesByTag']
methods = []
for revision, name in [(args.baseline_ref, 'BaselineRegistry'), (None, 'WorkingRegistry')]:
    text = source('engine/core/registry/EntityRegistry.cpp', revision)
    methods += [definition(text, marker).replace('EntityRegistry::', f'{name}::') for marker in markers]
cpp = args.output / 'entity_lookup.cpp'
cpp.write_text((ROOT / 'tools/performance/entity_lookup.cpp').read_text().replace('// PRODUCTION_METHODS', '\n'.join(methods)))
flags = ['-O3']
if args.sanitize:
    flags = ['-O1', '-g', '-DCORE_CHECK_ONLY', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
binary = args.output / 'entity_lookup'
subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', *flags,
                f'-I{ROOT / "engine/core"}', f'-I{ROOT / "engine/core/ecs"}',
                str(cpp), '-o', str(binary)], check=True)
output = subprocess.check_output([str(binary)], text=True)
(args.output / 'samples.csv').write_text(output)
print('Differential behavior checks passed.')
rows = list(csv.reader(output.splitlines()))
for length, count, mode in sorted({(r[0], r[1], r[2]) for r in rows}, key=lambda r: tuple(map(int, r))):
    samples = [r for r in rows if (r[0], r[1], r[2]) == (length, count, mode)]
    before, after, alloc_before, alloc_after = [statistics.median(float(r[c]) for r in samples) for c in (4, 5, 6, 7)]
    print(f'{length}-byte prefix, {count:>5} entities, {("name_last", "name_parent", "tag_first", "tag_all", "name_first", "name_absent")[int(mode)]:11}: {before:.2f} -> {after:.2f} CPU ns ({before / after:.2f}x); {alloc_before:g} -> {alloc_after:g} allocations')
