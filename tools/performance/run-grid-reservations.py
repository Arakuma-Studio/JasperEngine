#!/usr/bin/env python3
"""Compare exact production grid-reservation classes and methods against a revision.

The fixture isolates the reservation book from scene/graphics startup. Checks compare
all observable holder/expiry queries through random mutations. Default timing uses
the normal allocator, alternates baseline/current order and reports process CPU
time. --allocations builds a separate instrumented fixture and counts one warmed
batch per variant without timing it.
"""
import argparse
import csv
import math
import os
from pathlib import Path
import statistics
import subprocess

from run_core import ROOT, definition, source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', default='HEAD')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/performance-grid-reservations')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--sanitize', action='store_true', help='ASan/UBSan differential checks only')
    modes.add_argument('--allocations', action='store_true', help='untimed allocation counts only')
    modes.add_argument('--check-only', action='store_true', help='differential checks with the normal allocator')
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
    elif args.allocations:
        flags.append('-DGRID_RESERVATION_ALLOCATIONS')
    binary = args.output / 'grid_reservations'
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    *flags, str(generated), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), *(['--check'] if args.check_only else [])],
                            check=True, text=True, capture_output=True)
    (args.output / 'samples.csv').write_text(result.stdout)
    (args.output / 'checks.txt').write_text(result.stderr)
    print(result.stderr, end='')
    rows = list(csv.reader(result.stdout.splitlines()))
    if args.sanitize or args.check_only:
        if rows:
            raise ValueError('check-only mode unexpectedly emitted measurement rows')
        return
    counts = {'1', '8', '128', '1024', '8192'}
    if args.allocations:
        if len(rows) != len(counts) or any(len(row) != 3 for row in rows) or {row[0] for row in rows} != counts:
            raise ValueError('expected one allocation row per agent count')
        for count, before, after in rows:
            if any(not math.isfinite(float(value)) or float(value) < 0 for value in (before, after)):
                raise ValueError('allocation count must be finite and non-negative')
            print(f'{count:>5} agents: {float(before):g} -> {float(after):g} allocations/update (untimed)')
        return
    expected = {(count, str(sample)) for count in counts for sample in range(11)}
    if len(rows) != len(expected) or any(len(row) != 4 for row in rows) or {(row[0], row[1]) for row in rows} != expected:
        raise ValueError('expected eleven paired timing rows per agent count')
    if any(not math.isfinite(float(value)) or float(value) <= 0 for row in rows for value in row[2:]):
        raise ValueError('timing samples must be finite and positive')
    for count in sorted({r[0] for r in rows}, key=int):
        samples = [r for r in rows if r[0] == count]
        before, after = [statistics.median(float(r[c]) for r in samples) for c in (2, 3)]
        ratio = statistics.median(float(r[2]) / float(r[3]) for r in samples)
        print(f'{count:>5} agents: {before:.2f} -> {after:.2f} CPU ns/update ({ratio:.2f}x)')


if __name__ == '__main__':
    main()
