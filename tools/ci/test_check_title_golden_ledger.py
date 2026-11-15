"""Unit tests of tools/titleart/check_golden_ledger.py (python3 tools/ci/test_check_title_golden_ledger.py;
no build): a new or updated title golden needs its ledger row, a Pachislop reference never
changes, the committed ledger parses, and a CI lane's shallow checkout without main (scratch git
repositories) is compared with its merge commit's first parent."""

import importlib.util
import subprocess
import sys
import tempfile
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


def run_git(cwd, *args):
    return subprocess.run(["git", "-C", str(cwd), "-c", "user.name=t", "-c", "user.email=t@t", "-c", "init.defaultBranch=main",
                           "-c", "protocol.file.allow=always"] + list(args), check=True, capture_output=True, text=True).stdout.strip()


def test_a_shallow_checkout_without_main_compares_with_the_first_parent():
    """CI's mcp lane checks out a pull request's merge commit (fetch-depth 2): no main, the base is
    HEAD^1; at depth 1 there is nothing to compare with, and the error names --base"""
    with tempfile.TemporaryDirectory() as tmp:
        origin = Path(tmp) / "origin"
        origin.mkdir()
        run_git(origin, "init", "-q")
        (origin / "a.txt").write_text("a")
        run_git(origin, "add", "a.txt")
        run_git(origin, "commit", "-q", "-m", "base")
        run_git(origin, "checkout", "-q", "-b", "pr")
        (origin / "b.txt").write_text("b")
        run_git(origin, "add", "b.txt")
        run_git(origin, "commit", "-q", "-m", "pr")
        run_git(origin, "checkout", "-q", "main")
        (origin / "c.txt").write_text("c")
        run_git(origin, "add", "c.txt")
        run_git(origin, "commit", "-q", "-m", "main moved")
        base = run_git(origin, "rev-parse", "HEAD")
        run_git(origin, "merge", "-q", "--no-ff", "-m", "merge pr", "pr")
        run_git(origin, "update-ref", "refs/pull/1/merge", "HEAD")
        run_git(origin, "reset", "-q", "--hard", base)
        assert ledger.default_base(str(origin)) == base, "with main: its merge base"
        for depth, expected in (("2", base), ("1", None)):
            clone = Path(tmp) / ("clone" + depth)
            clone.mkdir()
            run_git(clone, "init", "-q")
            run_git(clone, "fetch", "-q", "--depth", depth, "file://" + str(origin), "refs/pull/1/merge")
            run_git(clone, "checkout", "-q", "FETCH_HEAD")
            assert not ledger.has_commit("main", str(clone)), "the checkout has no main"
            if expected:
                assert ledger.default_base(str(clone)) == expected, "depth 2: the merge commit's first parent"
            else:
                try:
                    ledger.default_base(str(clone))
                except SystemExit as e:
                    assert "--base" in str(e), e
                else:
                    raise AssertionError("depth 1: no base, and the error says so")


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
