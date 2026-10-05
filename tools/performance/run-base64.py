#!/usr/bin/env python3
"""Compile real Base64.cpp at a baseline revision and in the working tree.

Usage: python3 tools/performance/run-base64.py --baseline 6479bcb0
Artifacts are ignored under build/performance/base64. CXX defaults to c++.
"""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', default='6479bcb0')
parser.add_argument('--check', action='store_true', help='Correctness only, with ASan/UBSan')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
out = root / 'build/performance/base64'
out.mkdir(parents=True, exist_ok=True)
before = out / 'before.cpp'
before.write_bytes(subprocess.check_output(['git', 'show', f'{args.baseline}:engine/core/util/Base64.cpp'], cwd=root))
for label, source in [('before', before), ('after', root / 'engine/core/util/Base64.cpp')]:
    binary = out / (label + ('-sanitized' if args.check else ''))
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.check else ['-O3', '-DNDEBUG']
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', *flags,
                    '-I' + str(root / 'engine/core'), '-I' + str(root / 'engine/core/util'),
                    str(root / 'tools/performance/base64.cpp'), str(source), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), *(['--check'] if args.check else [])], check=True, text=True, capture_output=True)
    (out / (label + ('-check' if args.check else '') + '.txt')).write_text(result.stdout)
    print(label + ':\n' + result.stdout, flush=True)
