#!/usr/bin/env python3
"""Compare two behaviour transcripts of the upgrade fixture.

    python3 tools/ci/upgrade_from_main/compare.py <reference.txt> <actual.txt> [--tolerance 0.002]

A transcript is the game's `T|<scene>|<frame>|<key>|<values...>` lines (lib/transcript.lua).
The lines must match one to one, in order: the same scene, frame, key and the same number of
values; strings and integers exactly; decimals (the game prints 3 decimals) within the
tolerance, also inside vectors "(x,y,z)". Prints the differences (at most --max), exit 1 when
there are any.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

DECIMAL = re.compile(r"^-?\d+\.\d+$")


def values(field: str) -> list[str]:
    if field.startswith("(") and field.endswith(")"):
        return field[1:-1].split(",")
    return [field]


def same_field(a: str, b: str, tol: float) -> tuple[bool, float]:
    """(equal, the largest decimal difference seen)."""
    if a == b:
        return True, 0.0
    va, vb = values(a), values(b)
    if len(va) != len(vb):
        return False, 0.0
    worst = 0.0
    for x, y in zip(va, vb):
        if x == y:
            continue
        if DECIMAL.match(x) and DECIMAL.match(y):
            d = abs(float(x) - float(y))
            worst = max(worst, d)
            if d > tol + 1e-9:
                return False, worst
        else:
            return False, worst
    return True, worst


def compare(ref: list[str], act: list[str], tol: float) -> tuple[list[str], float]:
    diffs: list[str] = []
    worst = 0.0
    for i in range(max(len(ref), len(act))):
        a = ref[i] if i < len(ref) else "<missing>"
        b = act[i] if i < len(act) else "<missing>"
        fa, fb = a.split("|"), b.split("|")
        ok = len(fa) == len(fb)
        if ok:
            for x, y in zip(fa, fb):
                eq, w = same_field(x, y, tol)
                worst = max(worst, w)
                if not eq:
                    ok = False
                    break
        if not ok:
            diffs.append(f"line {i + 1}:\n  reference: {a}\n  actual:    {b}")
    return diffs, worst


def read(path: Path) -> list[str]:
    return [line.rstrip() for line in path.read_text().splitlines() if line.startswith("T|")]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference", type=Path)
    ap.add_argument("actual", type=Path)
    ap.add_argument("--tolerance", type=float, default=0.0)
    ap.add_argument("--max", type=int, default=30)
    args = ap.parse_args()
    ref, act = read(args.reference), read(args.actual)
    diffs, worst = compare(ref, act, args.tolerance)
    exact = sum(1 for a, b in zip(ref, act) if a == b)
    print(f"{args.actual}: {len(act)} lines against {len(ref)} ({exact} identical, largest decimal difference "
          f"{worst:g}, tolerance {args.tolerance:g})")
    for d in diffs[:args.max]:
        print(d)
    if len(diffs) > args.max:
        print(f"... {len(diffs) - args.max} more")
    print("MATCH" if not diffs else f"DIFFER: {len(diffs)} line(s)")
    return 0 if not diffs else 1


if __name__ == "__main__":
    sys.exit(main())
