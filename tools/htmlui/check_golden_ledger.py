#!/usr/bin/env python3
# The golden policy of S16 (docs/specs/S16-ui-render-text.md AC2, AC3): every self-golden PNG under
# engine/tests/htmlui/golden that a branch changed has a row in docs/html-ui/goldens-S16.md (its
# name, the metric before and after, the category, the reason), the Chromium references
# (golden/browser/**, written by tools/htmlui/render-reference.mjs) are never changed, and at
# most 60 rows exist without the ledger's "Reviewed:" line.
#
#   python3 tools/htmlui/check_golden_ledger.py [--base <git ref>]
#
# --base: what the branch is compared with (default: the merge base with oe-port). New goldens
# (added files) need a row in the ledger's "New goldens" table. Exit 0 when the ledger covers
# every change, 1 otherwise (each problem printed).

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GOLDEN = "engine/tests/htmlui/golden/"
LEDGER = os.path.join(ROOT, "docs", "html-ui", "goldens-S16.md")
CATEGORIES = {"sdf-aa", "analytic-clip", "analytic-shadow", "exact-scale-text", "gpu-text-effect"}
MAX_UNREVIEWED = 60


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), check=True, capture_output=True, text=True).stdout


def changed_goldens(base):
    """(status, path relative to golden/ without .png) of every golden PNG changed since base"""
    out = git("diff", "--name-status", "--no-renames", base + "...HEAD", "--", GOLDEN)
    changes = []
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) != 2 or not parts[1].endswith(".png"):
            continue
        changes.append((parts[0][0], parts[1][len(GOLDEN):-len(".png")]))
    return changes


def parse_ledger(text):
    """the updated and the new goldens' rows ({name: cells}), and whether the ledger is reviewed"""
    updated, added = {}, {}
    section = None
    for line in text.splitlines():
        if line.startswith("## "):
            title = line[3:].strip().lower()
            section = "updated" if title.startswith("updated") else ("new" if title.startswith("new") else None)
            continue
        if section is None or not line.startswith("|") or re.match(r"^\|\s*-", line):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if not cells or cells[0].lower() in ("golden", ""):
            continue
        name = cells[0].strip("`")
        (updated if section == "updated" else added)[name] = cells
    reviewed = re.search(r"^Reviewed:\s*\S+", text, re.MULTILINE) is not None
    return updated, added, reviewed


def check(changes, ledger_text):
    problems = []
    updated, added, reviewed = parse_ledger(ledger_text)
    for status, name in changes:
        if name.startswith("browser/"):
            problems.append("golden/%s.png is a Chromium reference: it is never regenerated (AC2)" % name)
            continue
        if status == "A":
            if name not in added:
                problems.append("golden/%s.png is new: it needs a row in the ledger's New goldens table" % name)
            continue
        if status == "D":
            problems.append("golden/%s.png was deleted" % name)
            continue
        row = updated.get(name)
        if row is None:
            problems.append("golden/%s.png changed without a row in docs/html-ui/goldens-S16.md (AC3)" % name)
            continue
        if len(row) < 5:
            problems.append("the row of %s needs: golden, before, after, category, reason" % name)
            continue
        if row[3].strip("`") not in CATEGORIES:
            problems.append("the row of %s has category '%s', not one of %s" % (name, row[3], ", ".join(sorted(CATEGORIES))))
        if not row[4]:
            problems.append("the row of %s gives no reason" % name)
    if len(updated) > MAX_UNREVIEWED and not reviewed:
        problems.append("%d updated goldens: more than %d need a separate review (a 'Reviewed:' line)" % (len(updated), MAX_UNREVIEWED))
    return problems


def main():
    base = None
    if "--base" in sys.argv:
        base = sys.argv[sys.argv.index("--base") + 1]
    else:
        base = git("merge-base", "HEAD", "oe-port").strip()
    ledger_text = open(LEDGER, encoding="utf-8").read() if os.path.exists(LEDGER) else ""
    changes = changed_goldens(base)
    problems = check(changes, ledger_text)
    for problem in problems:
        print("error: " + problem)
    print("%d golden change(s) since %s, %d problem(s)" % (len(changes), base[:12], len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
