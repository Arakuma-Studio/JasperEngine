#!/usr/bin/env python3
# The Title Art golden policy (docs/specs/S80-title-art-core.md §7): every title golden of the
# program (S80-S87, engine/tests/core/golden/titleart/**) is added or updated only with a row in
# docs/titleart/goldens.md, and the Pachislop references (golden/titleart/pachislop/**, copied
# unchanged from Pachislop) never change.
#
#   python3 tools/titleart/check_golden_ledger.py [--base <git ref>]
#
# --base: what the branch is compared with (default: the merge base with main). A new golden needs
# a row in the ledger's "New goldens" table (golden, reason); an updated one in "Updated goldens"
# (golden, metric before, metric after, reason). A deleted golden needs an "Updated goldens" row
# whose after column says "deleted". Exit 0 when the ledger covers every change, 1 otherwise.

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GOLDEN = "engine/tests/core/golden/titleart/"
LEDGER = os.path.join(ROOT, "docs", "titleart", "goldens.md")


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), check=True, capture_output=True, text=True).stdout


def changed_goldens(base):
    """(status, path relative to golden/titleart/ without .png) of every title golden changed since base"""
    out = git("diff", "--name-status", "--no-renames", base + "...HEAD", "--", GOLDEN)
    changes = []
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) != 2 or not parts[1].endswith(".png"):
            continue
        changes.append((parts[0][0], parts[1][len(GOLDEN):-len(".png")]))
    return changes


def parse_ledger(text):
    """the rows of the Updated and New goldens tables, {name: cells}"""
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
        (updated if section == "updated" else added)[cells[0].strip("`")] = cells
    return updated, added


def check(changes, ledger_text):
    problems = []
    updated, added = parse_ledger(ledger_text)
    for status, name in changes:
        if name.startswith("pachislop/"):
            if status != "A":
                problems.append("golden/titleart/%s.png is a Pachislop reference: it is never changed" % name)
            elif name not in added:
                problems.append("golden/titleart/%s.png is new: it needs a row in the ledger's New goldens table" % name)
            continue
        if status == "A":
            row = added.get(name)
            if row is None:
                problems.append("golden/titleart/%s.png is new: it needs a row in the ledger's New goldens table" % name)
            elif len(row) < 2 or not row[1]:
                problems.append("the New goldens row of %s gives no reason" % name)
            continue
        row = updated.get(name)
        if row is None:
            problems.append("golden/titleart/%s.png changed without a row in docs/titleart/goldens.md" % name)
            continue
        if len(row) < 4 or not all(row[1:4]):
            problems.append("the Updated goldens row of %s needs: golden, before, after, reason" % name)
        elif status == "D" and "deleted" not in row[2].lower():
            problems.append("golden/titleart/%s.png was deleted: its row's after column says so" % name)
    return problems


def main():
    base = sys.argv[sys.argv.index("--base") + 1] if "--base" in sys.argv else git("merge-base", "HEAD", "main").strip()
    ledger_text = open(LEDGER, encoding="utf-8").read() if os.path.exists(LEDGER) else ""
    changes = changed_goldens(base)
    problems = check(changes, ledger_text)
    for problem in problems:
        print("error: " + problem)
    print("%d title golden change(s) since %s, %d problem(s)" % (len(changes), base[:12], len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
