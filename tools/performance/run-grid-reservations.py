#!/usr/bin/env python3
"""Compare exact production grid-reservation classes and methods against a revision.

The fixture isolates the reservation book from scene/graphics startup. Checks compare
all observable holder/expiry queries through random mutations; timing alternates
baseline/current order and reports process CPU time and allocations per update.
"""
import argparse
import csv
import os
from pathlib import Path
import statistics
import subprocess

from run_core import ROOT, definition, source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', default='HEAD')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/performance-grid-reservations')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    variants = []
    for revision, name in [(args.baseline_ref, 'BeforeReservations'), (None, 'AfterReservations')]:
        header = source('engine/core/navigation/GridPathfinder.h', revision)
        impl = source('engine/core/navigation/GridPathfinder.cpp', revision)
        declaration = definition(header, 'class JASPERENGINE_API GridReservations') + ';'
        methods = '\n'.join(definition(impl, marker) for marker in (
            'void GridReservations::reset(', 'void GridReservations::release(',
            'void GridReservations::reserve(', 'bool GridReservations::isReservedByOther(',
            'uint32_t GridReservations::holder('))
        variants.append((declaration + '\n' + methods).replace('GridReservations', name)
                        .replace('JASPERENGINE_API ', ''))
    generated = args.output / 'grid_reservations.cpp'
    generated.write_text((ROOT / 'tools/performance/grid_reservations.cpp').read_text()
                         .replace('// PRODUCTION_CLASSES', '\n'.join(variants)))
    flags = ['-O3']
    if args.sanitize:
        flags = ['-O1', '-g', '-DCHECK_ONLY', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    binary = args.output / 'grid_reservations'
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    *flags, str(generated), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
    (args.output / 'samples.csv').write_text(result.stdout)
    (args.output / 'checks.txt').write_text(result.stderr)
    print(result.stderr, end='')
    rows = list(csv.reader(result.stdout.splitlines()))
    for count in sorted({r[0] for r in rows}, key=int):
        samples = [r for r in rows if r[0] == count]
        before, after, alloc_before, alloc_after = [statistics.median(float(r[c]) for r in samples)
                                                   for c in (2, 3, 4, 5)]
        ratio = statistics.median(float(r[2]) / float(r[3]) for r in samples)
        print(f'{count:>5} agents: {before:.2f} -> {after:.2f} CPU ns/update ({ratio:.2f}x); '
              f'{alloc_before:g} -> {alloc_after:g} allocations/update')


if __name__ == '__main__':
    main()
