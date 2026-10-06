#!/usr/bin/env python3
"""Compare actual baseline/current StringUtils.cpp, including differential checks."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', default='0da9f6ab81b25880ef6b51341f80c00eabead4b0')
parser.add_argument('--check', action='store_true', help='ASan/UBSan correctness checks only')
parser.add_argument('--allocations', action='store_true', help='Measure allocations separately from timings')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
out = root / 'build/performance/string-utils'
out.mkdir(parents=True, exist_ok=True)
before = out / 'before.cpp'
before.write_bytes(subprocess.check_output(['git', 'show', f'{args.baseline}:engine/core/util/StringUtils.cpp'], cwd=root))
flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.check else ['-O3', '-DNDEBUG']
common = [os.environ.get('CXX', 'c++'), '-std=c++17', *flags, '-I' + str(root / 'engine/core'), '-I' + str(root / 'engine/core/util')]
if args.allocations:
    common.append('-DTRACK_ALLOCATIONS')
objects = []
for label, source in [('before', before), ('after', root / 'engine/core/util/StringUtils.cpp')]:
    obj = out / (label + '.o')
    subprocess.run([*common, *(['-Djasperengine=baseline'] if label == 'before' else []), '-c', str(source), '-o', str(obj)], check=True)
    objects.append(str(obj))
name = 'allocations' if args.allocations else 'check' if args.check else 'benchmark'
binary = out / name
subprocess.run([*common, str(root / 'tools/performance/string_utils.cpp'), *objects, '-o', str(binary)], check=True)
result = subprocess.run([str(binary), *(['--check'] if args.check else [])], check=True, text=True, capture_output=True)
(out / (name + '.txt')).write_text(result.stdout)
print(result.stdout)
