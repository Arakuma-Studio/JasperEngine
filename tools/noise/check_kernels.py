#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# The lint of T2 R5, R7 and R38, which tools/check-changed does not run: run it for this area.
#
#   python3 tools/noise/check_kernels.py [--self-test]
#
# - Kernel and plan code uses no libm beyond floor and sqrt, no <random>, long double or std::hash (R5),
#   and owns no thread or lock (R7).
# - Every source of engine/core/noise is in NoiseSources.cmake, so it builds with the flags (R38).
# - Kernel headers are included only from the sources NoiseSources.cmake lists, and the shared cases.

import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

BANNED = re.compile(
    r"\bstd::(sin|cos|tan|asin|acos|atan\w*|sinh|cosh|tanh|pow|exp\w*|log\w*|fmod|round\w*|lround|hash|thread|mutex|lock_guard|unique_lock|"
    r"mt19937\w*|\w+_distribution)\b|#include\s*<(random|thread|mutex)>|\blong\s+double\b")
# registerNoiseKind's once_flag runs once at registration, not while evaluating
ALLOWED_LINES = {("NoiseCompiler.cpp", "#include <mutex>")}
KERNEL_HEADERS = ("noise/NoiseKernelImpl.h", "noise/NoiseBackend.h", "noise/NoiseDetMath.h")
INCLUDE_ALLOWED_DIRS = ("engine/tests/core/noise", "engine/core/noise", "tools/noise")


def strip_comment(line):
    return line.split("//", 1)[0]


def listed_sources(root):
    with open(os.path.join(root, "engine", "core", "noise", "NoiseSources.cmake"), encoding="utf-8") as f:
        text = f.read()
    return {line.strip() for block in re.findall(r"set\(JASPERENGINE_NOISE(?:_COMPAT)?_SRCS(.*?)\)", text, re.S)
            for line in block.split("\n") if line.strip().startswith("core/")} | {"core/terrain/HeightfieldErosion.cpp"}


def check(root):
    problems = []
    listed = listed_sources(root)
    noise_dir = os.path.join(root, "engine", "core", "noise")
    scanned = [os.path.join(noise_dir, n) for n in sorted(os.listdir(noise_dir)) if n.endswith((".h", ".cpp"))]
    scanned += [os.path.join(root, "engine", "core", "terrain", n) for n in ("HeightfieldErosion.cpp", "HeightfieldErosion.h")
                if os.path.exists(os.path.join(root, "engine", "core", "terrain", n))]
    for path in scanned:
        name = os.path.basename(path)
        rel = os.path.relpath(path, os.path.join(root, "engine"))
        if name.endswith(".cpp") and rel.replace(os.sep, "/") not in listed:
            problems.append(f"{rel}: not in NoiseSources.cmake, so it builds without the noise flags")
        with open(path, encoding="utf-8") as f:
            for number, line in enumerate(f, 1):
                code = strip_comment(line)
                match = BANNED.search(code)
                if match and (name, code.strip()) not in ALLOWED_LINES:
                    problems.append(f"{rel}:{number}: banned in kernel and plan code: {match.group(0)}")
    for directory, _, names in os.walk(os.path.join(root, "engine", "core")):
        for name in names:
            if not name.endswith((".h", ".cpp")):
                continue
            path = os.path.join(directory, name)
            rel = os.path.relpath(path, os.path.join(root, "engine")).replace(os.sep, "/")
            if rel in listed or rel.startswith("core/noise/"):
                continue
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
            for header in KERNEL_HEADERS:
                if f'"{header}"' in text:
                    problems.append(f"{rel}: includes {header} outside the sources with the noise flags")
    return problems


def self_test():
    failures = []
    with tempfile.TemporaryDirectory() as temp:
        def fresh():
            target = os.path.join(temp, "tree")
            shutil.rmtree(target, ignore_errors=True)
            os.makedirs(os.path.join(target, "engine"))
            shutil.copytree(os.path.join(ROOT, "engine", "core", "noise"), os.path.join(target, "engine", "core", "noise"))
            terrain = os.path.join(target, "engine", "core", "terrain")
            os.makedirs(terrain)
            for name in ("HeightfieldErosion.cpp", "HeightfieldErosion.h"):
                shutil.copy(os.path.join(ROOT, "engine", "core", "terrain", name), terrain)
            return target
        tree = fresh()
        if check(tree):
            failures.append("the copied tree should pass: " + "; ".join(check(tree)))
        for planted, what in (("float x = std::pow(2.0f, 3.0f);", "std::pow"), ("std::thread worker;", "std::thread")):
            tree = fresh()
            with open(os.path.join(tree, "engine", "core", "noise", "NoiseEval.cpp"), "a", encoding="utf-8") as f:
                f.write(planted + "\n")
            if not any(what in problem for problem in check(tree)):
                failures.append(f"a planted {what} was not caught")
        tree = fresh()
        with open(os.path.join(tree, "engine", "core", "noise", "NoiseExtra.cpp"), "w", encoding="utf-8") as f:
            f.write("// a source nobody listed\n")
        if not any("NoiseExtra.cpp" in problem for problem in check(tree)):
            failures.append("a source missing from NoiseSources.cmake was not caught")
    return failures


def main():
    if "--self-test" in sys.argv:
        failures = self_test()
        for failure in failures:
            print("self-test: " + failure)
        print("check_kernels self-test: " + ("FAIL" if failures else "pass"))
        return 1 if failures else 0
    problems = check(ROOT)
    for problem in problems:
        print(problem)
    print(f"check_kernels: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
