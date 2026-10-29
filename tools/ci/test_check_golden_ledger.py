#!/usr/bin/env python3
"""Unit tests of tools/htmlui/check_golden_ledger.py (python3 tools/ci/test_check_golden_ledger.py;
CI's unit job): the S16 golden policy (docs/specs/S16-ui-render-text.md AC2, AC3) on given changes
and ledger texts."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_golden_ledger", HERE.parent / "htmlui" / "check_golden_ledger.py")
ledger = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = ledger
spec.loader.exec_module(ledger)

LEDGER = """# Goldens changed by S16

## Updated goldens

| Golden | Before | After | Category | Reason | Chromium |
|---|---|---|---|---|---|
| `p1/clip_rounded` | max 3 | max 40 | analytic-clip | the corner is antialiased | - |
| `effects/glow` | max 0 | max 30 | nonsense | why | - |
| `p6/empty` | max 0 | max 30 | sdf-aa |  | - |

## New goldens

| Golden | Test | What it shows |
|---|---|---|
| `p1/rounded_clip_1x` | RenderClip | one sample |
"""


class GoldenLedgerTest(unittest.TestCase):
    def test_a_row_covers_an_update(self):
        self.assertEqual(ledger.check([("M", "p1/clip_rounded")], LEDGER), [])

    def test_an_update_without_a_row_fails(self):
        problems = ledger.check([("M", "p1/other")], LEDGER)
        self.assertEqual(len(problems), 1)
        self.assertIn("without a row", problems[0])

    def test_chromium_references_are_never_changed(self):
        problems = ledger.check([("M", "browser/paint/box-shadow")], LEDGER)
        self.assertEqual(len(problems), 1)
        self.assertIn("Chromium reference", problems[0])

    def test_categories_and_reasons_are_checked(self):
        self.assertIn("category", ledger.check([("M", "effects/glow")], LEDGER)[0])
        self.assertIn("no reason", ledger.check([("M", "p6/empty")], LEDGER)[0])

    def test_new_goldens_need_their_own_row(self):
        self.assertEqual(ledger.check([("A", "p1/rounded_clip_1x")], LEDGER), [])
        self.assertIn("is new", ledger.check([("A", "p1/rounded_clip_9x")], LEDGER)[0])

    def test_more_than_60_updates_need_a_review(self):
        rows = "".join("| `p1/g%d` | a | b | sdf-aa | r | - |\n" % i for i in range(61))
        text = "## Updated goldens\n\n| Golden | Before | After | Category | Reason | Chromium |\n|---|---|---|---|---|---|\n" + rows
        problems = ledger.check([], text)
        self.assertEqual(len(problems), 1)
        self.assertIn("separate review", problems[0])
        self.assertEqual(ledger.check([], text + "\nReviewed: someone 2026-10-25\n"), [])


if __name__ == "__main__":
    unittest.main()
