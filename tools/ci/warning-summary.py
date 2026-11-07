#!/usr/bin/env python3
"""Counts compiler warnings per CMake target in a Ninja build log (docs/testing.md, "Warnings").

    WERROR=1 tools/build.sh unit-tests 2>&1 | tee build/test-results/unit-build.log
    python3 tools/ci/warning-summary.py build/test-results/unit-build.log

Ninja prints each step's description ("[12/900] Building CXX object CMakeFiles/jasperengine.dir/
core/Engine.cpp.o") before the output of its command, so every "warning:" line belongs to the step
above it, and the step's object path names its target (<target>.dir). Prints a Markdown table:
warnings per target and per warning option. --max N exits 1 when there are more than N.
Standard library only.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

STEP = re.compile(r"^\[\d+/\d+\] (?:Building \w+ object |Linking \w+ \S+ )(\S+)")
TARGET = re.compile(r"(?:^|/)CMakeFiles/([^/]+)\.dir/")
WARNING = re.compile(r": warning: .*?(?:\[(-W[^\]]+)\])?$|: warning C(\d{4}):")


def summarize(lines) -> tuple[Counter, Counter]:
    per_target: Counter = Counter()
    per_option: Counter = Counter()
    target = "(no step)"
    for line in lines:
        line = line.rstrip("\n")
        step = STEP.match(line)
        if step:
            found = TARGET.search(step.group(1))
            target = found.group(1) if found else step.group(1)
            continue
        warning = WARNING.search(line)
        if warning:
            per_target[target] += 1
            option = warning.group(1) or (f"C{warning.group(2)}" if warning.group(2) else "(no option)")
            per_option[option] += 1
    return per_target, per_option


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=Path, nargs="+", help="Ninja build logs")
    parser.add_argument("--max", type=int, help="fail when the logs hold more warnings than this")
    args = parser.parse_args()

    per_target: Counter = Counter()
    per_option: Counter = Counter()
    for log in args.log:
        targets, options = summarize(log.read_text(errors="replace").splitlines())
        per_target.update(targets)
        per_option.update(options)

    total = sum(per_target.values())
    print(f"### Compiler warnings: {total}\n")
    if total:
        print("| Target | Warnings |\n|---|---|")
        for target, count in per_target.most_common():
            print(f"| `{target}` | {count} |")
        print("\n| Warning | Count |\n|---|---|")
        for option, count in per_option.most_common():
            print(f"| `{option}` | {count} |")
    return 1 if args.max is not None and total > args.max else 0


if __name__ == "__main__":
    sys.exit(main())
