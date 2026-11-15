#!/usr/bin/env python3
"""No-silent-green gate over googletest XML results (docs/testing.md).

    python3 tools/ci/gtest-gate.py --xml build/test-results/core.xml --min-tests 300 --max-skipped 0

Fails (exit 1) when, over all the given files together:
  - a file is missing or is not googletest XML (the binary crashed or never ran),
  - no test ran (a --gtest_filter that matches nothing is not a pass),
  - fewer than --min-tests tests ran,
  - any test failed or errored,
  - more than --max-skipped tests were skipped (GTEST_SKIP). A skip whose message matches an
    --allow-skip-reason regex (e.g. a platform whose test binary cannot count allocations) is listed
    and not counted; any other skip still is.
It also lists the disabled tests (DISABLED_ prefix), so a quarantine is visible, never silent.
Standard library only.
"""

from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Totals:
    ran: int = 0
    skipped: int = 0
    failed: list[str] = field(default_factory=list)
    disabled: list[str] = field(default_factory=list)
    skipped_names: list[str] = field(default_factory=list)
    allowed_skips: list[tuple[str, str]] = field(default_factory=list)


def read_results(path: Path, totals: Totals, allow_reasons: list[str] = ()) -> str | None:
    """Adds one file's test cases to totals; returns an error message instead when unreadable."""
    if not path.is_file():
        return f"{path}: missing (the test binary crashed, or never ran)"
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as error:
        return f"{path}: not valid XML ({error})"
    if root.tag != "testsuites":
        return f"{path}: not googletest XML (root element <{root.tag}>)"
    for suite in root.iter("testsuite"):
        for case in suite.iter("testcase"):
            name = f"{suite.get('name', '?')}.{case.get('name', '?')}"
            if case.get("status") == "notrun" or case.get("result") == "suppressed":
                totals.disabled.append(name)
                continue
            if case.get("result") == "skipped" or case.find("skipped") is not None:
                skip = case.find("skipped")
                message = (skip.get("message", "") if skip is not None else "") or ""
                reason = next((r for r in allow_reasons if re.search(r, message)), None)
                if reason is not None:
                    totals.allowed_skips.append((name, reason))
                    continue
                totals.skipped += 1
                totals.skipped_names.append(name)
                continue
            totals.ran += 1
            if case.find("failure") is not None or case.find("error") is not None:
                totals.failed.append(name)
    return None


def gate(files: list[Path], min_tests: int, max_skipped: int, out=sys.stdout,
         allow_reasons: list[str] = ()) -> int:
    totals = Totals()
    errors = []
    for path in files:
        error = read_results(path, totals, allow_reasons)
        if error:
            errors.append(error)

    names = ", ".join(str(f) for f in files)
    print(f"gtest-gate: {names}: {totals.ran} ran, {len(totals.failed)} failed, "
          f"{totals.skipped} skipped, {len(totals.disabled)} disabled", file=out)
    for name in totals.disabled:
        print(f"  disabled: {name}", file=out)
    for name in totals.skipped_names:
        print(f"  skipped:  {name}", file=out)
    for name, reason in totals.allowed_skips:
        print(f"  skipped (allowed: {reason}): {name}", file=out)
    for name in totals.failed:
        print(f"  FAILED:   {name}", file=out)

    if not errors:
        if totals.ran == 0:
            errors.append("no test ran: a filter that matches nothing is not a pass")
        elif totals.ran < min_tests:
            errors.append(f"only {totals.ran} tests ran, at least {min_tests} expected")
        if totals.failed:
            errors.append(f"{len(totals.failed)} tests failed")
        if totals.skipped > max_skipped:
            errors.append(f"{totals.skipped} tests skipped, at most {max_skipped} allowed")
    for error in errors:
        print(f"gtest-gate: FAIL: {error}", file=out)
    return 1 if errors else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--xml", action="append", required=True, type=Path, help="googletest XML file (repeatable)")
    parser.add_argument("--min-tests", type=int, default=1, help="tests that must have run (default 1)")
    parser.add_argument("--max-skipped", type=int, default=0, help="skipped tests allowed (default 0)")
    parser.add_argument("--allow-skip-reason", action="append", default=[], metavar="REGEX",
                        help="a skip whose message matches is listed, not counted (repeatable)")
    args = parser.parse_args()
    return gate(args.xml, max(1, args.min_tests), args.max_skipped, allow_reasons=args.allow_skip_reason)


if __name__ == "__main__":
    sys.exit(main())
