#!/usr/bin/env python3
"""Measure the exact pose transition settlement method against a git revision.

This CPU kernel runs when goToPose starts a live transition, not every frame.
The fixture uses the production settings, bone table, easing and loosen functions.
Only the runtime owner is reduced to its data/transition fields to avoid scene setup.
"""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def definition(source, marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', default='HEAD')
    parser.add_argument('--check', action='store_true', help='ASan/UBSan differential checks only')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/performance-pose-settle')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    path = 'engine/core/pose/PoseAnimatorRuntime.cpp'
    sources = [subprocess.check_output(['git', 'show', f'{args.baseline_ref}:{path}'], cwd=ROOT, text=True),
               (ROOT / path).read_text()]
    objects = []
    flags = ['-std=c++17', '-I' + str(ROOT / 'engine/core')]
    flags += ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.check else ['-O3']
    cxx = os.environ.get('CXX', 'g++')
    for name, source in zip(('before', 'after'), sources):
        code = '''#include <algorithm>
#include <array>
#include <cmath>
#include "pose/PoseTypes.h"
#include "pose/PoseTween.h"
using namespace jasperengine::pose;
namespace VARIANT {
class PoseAnimatorRuntime {
public:
    PoseAnimationData data;
    Transition transition;
    float transitionSettleTime() const;
};
HELPERS
METHOD
float run(const PoseAnimationSettings& settings, const Transition& transition) {
    PoseAnimatorRuntime runtime;
    runtime.data.settings = settings;
    runtime.transition = transition;
    return runtime.transitionSettleTime();
}
}
'''.replace('VARIANT', name).replace('HELPERS', '\n'.join(
            definition(source, marker) for marker in ('float settleX(', 'void boneTransition(')))
        code = code.replace('METHOD', definition(source, 'float PoseAnimatorRuntime::transitionSettleTime()'))
        generated = args.output / f'{name}.cpp'
        generated.write_text(code)
        obj = args.output / f'{name}.o'
        subprocess.run([cxx, *flags, '-c', str(generated), '-o', str(obj)], check=True)
        objects.append(str(obj))
    binary = args.output / ('check' if args.check else 'benchmark')
    subprocess.run([cxx, *flags, str(ROOT / 'tools/performance/pose_settle.cpp'), *objects,
                    str(ROOT / 'engine/core/pose/HumanBones.cpp'),
                    str(ROOT / 'engine/core/pose/PoseTween.cpp'), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), *(['--check'] if args.check else [])], check=True, text=True, capture_output=True)
    (args.output / ('check.txt' if args.check else 'measurements.csv')).write_text(result.stdout)
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
