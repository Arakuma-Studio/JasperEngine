"""Unit tests of tools/titleart/check_golden_ledger.py (python3 tools/ci/test_check_title_golden_ledger.py;
no git, no build): a new or updated title golden needs its ledger row, a Pachislop reference never
changes, and the committed ledger parses."""

import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_title_golden_ledger", HERE.parent / "titleart" / "check_golden_ledger.py")
ledger = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ledger)

LEDGER = """# Title Art goldens

## New goldens

| Golden | Reason |
|---|---|
| `effects/outer_glow` | the effect at defaults |
| `pachislop/effect/outerGlow` | copied from Pachislop |

## Updated goldens

| Golden | Before | After | Reason |
|---|---|---|---|
| `effects/drop_shadow` | 60 / 0.4 / 1.2 % | 0 / 0 / 0 | the blur's taps moved |
| `effects/old` | 0 / 0 / 0 | deleted | the case was merged into another |
"""


def test_rows_cover_the_changes():
    changes = [("A", "effects/outer_glow"), ("M", "effects/drop_shadow"), ("D", "effects/old"), ("A", "pachislop/effect/outerGlow")]
    assert ledger.check(changes, LEDGER) == []


def test_missing_rows_fail():
    problems = ledger.check([("A", "effects/inner_glow"), ("M", "effects/outline")], LEDGER)
    assert len(problems) == 2, problems
    assert "New goldens" in problems[0] and "without a row" in problems[1]


def test_pachislop_references_never_change():
    problems = ledger.check([("M", "pachislop/effect/outerGlow")], LEDGER)
    assert problems and "never changed" in problems[0]


def test_a_deleted_golden_says_so():
    text = LEDGER.replace("| deleted |", "| 0 / 0 / 0 |")
    problems = ledger.check([("D", "effects/old")], text)
    assert problems and "deleted" in problems[0]


def test_the_committed_ledger_parses():
    text = (HERE.parent.parent / "docs" / "titleart" / "goldens.md").read_text(encoding="utf-8")
    updated, added = ledger.parse_ledger(text)
    assert added, "the ledger lists the goldens it added"
    for name, cells in added.items():
        assert len(cells) >= 2 and cells[1], name


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print("ok   " + name)
            except AssertionError as e:
                failures += 1
                print("FAIL " + name + ": " + str(e))
    sys.exit(1 if failures else 0)
