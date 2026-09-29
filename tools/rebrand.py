#!/usr/bin/env python3
"""Rename Doriax to JasperEngine across the tree.

    tools/rebrand.py --dry-run         per-rule match counts and planned renames
    tools/rebrand.py --apply-paths     git mv every path containing the name
    tools/rebrand.py --apply-content   rewrite text files in place
    tools/rebrand.py --check           fail if the name survives outside the allowlist

Paths and contents are separate steps so that a commit of pure renames lets git
follow file history across the rebrand. Every step is idempotent. Operates on
files tracked by git; run from anywhere inside the repository.

Also usable on a patch cherry-picked from upstream Doriax: apply it, then run
--apply-paths and --apply-content again.
"""

import argparse
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO_URL = "https://github.com/JasperRaccoon/JasperEngine"

# Ordered: special cases first so the general case rules never see them.
RULES = [
    # URLs. The site and docs are not hosted yet, so they point at the repo.
    (r"github\.com/doriaxengine/doriax(?![-\w])", "github.com/JasperRaccoon/JasperEngine"),
    (r"https?://(?:www\.|docs\.)?doriax\.org[^\s\"'<>)\]]*", REPO_URL),
    (r"https?://doriaxengine\.org", "https://jaspergameengine.com"),
    # AppImage update info: gh-releases-zsync|<user>|<repo>|latest|<file glob>
    (r"gh-releases-zsync\|doriaxengine\|doriax\|", "gh-releases-zsync|JasperRaccoon|JasperEngine|"),
    # Bundle and package identifiers.
    (r"org\.doriax\.doriaxengine", "com.jaspergameengine.jasperengine"),
    (r"org\.doriax\.editor", "com.jaspergameengine.editor"),
    (r"org\.doriax(?![\w.])", "com.jaspergameengine"),
    (r"org\.doriaxengine\.doriax", "com.jaspergameengine.jasperengine"),
    (r"org/doriaxengine/doriax", "com/jaspergameengine/jasperengine"),
    (r'"org"(\s*/\s*)"doriaxengine"(\s*/\s*)"doriax"', r'"com"\1"jaspergameengine"\2"jasperengine"'),
    # Display strings, so the product name is not followed by a redundant word.
    (r"Doriax [Ee]ngine", "JasperEngine"),
    (r"Doriax [Ee]ditor", "JasperEngine Editor"),
    # Glued editor macro prefix.
    (r"DORIAXEDITOR_", "JASPERENGINE_EDITOR_"),
    # General case-preserving rules; substrings, so compound identifiers convert.
    (r"DORIAX", "JASPERENGINE"),
    (r"Doriax", "JasperEngine"),
    (r"doriax", "jasperengine"),
]
COMPILED = [(re.compile(p.encode()), r.encode()) for p, r in RULES]
NAME = re.compile(rb"doriax", re.IGNORECASE)

# Never rewritten; the only places the upstream name is expected to remain.
ALLOW_FILES = {"LICENSE", "NOTICE.md", "README.md", "tools/rebrand.py"}
ALLOW_PREFIXES = ("docs/superpowers/",)
# Attribution lines elsewhere (e.g. the About window) carry this marker.
ALLOW_LINE = re.compile(rb"[Bb]ased on Doriax")

THIRD_PARTY = ("libs/", "engine/libs/", "shadercompiler/libs/")
# Vendored files that Doriax patched to call into the engine.
THIRD_PARTY_PATCHED = {
    "engine/libs/sokol/sokol.cpp",
    "engine/libs/sokol/sokol_app.h",
    "engine/libs/harfbuzz/CMakeLists.txt",
}


def repo_root():
    out = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True)
    return Path(out.stdout.strip())


def tracked_files(root):
    out = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True, check=True)
    return [p for p in out.stdout.decode().split("\0") if p]


def allowlisted(path):
    return path in ALLOW_FILES or path.startswith(ALLOW_PREFIXES)


def in_scope(path):
    """Files whose contents the rename rules may rewrite."""
    if allowlisted(path):
        return False
    if path.startswith(THIRD_PARTY) and path not in THIRD_PARTY_PATCHED:
        return False
    return True


def is_binary(data):
    return b"\0" in data[:8192]


def rewrite(data, counts=None):
    for i, (pattern, repl) in enumerate(COMPILED):
        data, n = pattern.subn(repl, data)
        if counts is not None and n:
            counts[i] += n
    return data


def new_path(path):
    return rewrite(path.encode()).decode()


def planned_renames(files):
    return [(p, new_path(p)) for p in files if NAME.search(p.encode())]


def text_files(root, files):
    """Yield (path, bytes) for in-scope regular text files."""
    for p in files:
        full = root / p
        if not in_scope(p) or full.is_symlink() or not full.is_file():
            continue
        data = full.read_bytes()
        if not is_binary(data):
            yield p, data


def cmd_dry_run(root, files):
    counts = Counter()
    changed = 0
    for p, data in text_files(root, files):
        if rewrite(data, counts) != data:
            changed += 1
    print("Content rules (matches):")
    for i, (pattern, repl) in enumerate(RULES):
        print(f"  {counts[i]:6d}  {pattern}  ->  {repl}")
    print(f"  {changed} files would change")
    renames = planned_renames(files)
    print(f"\nPath renames ({len(renames)}):")
    for old, new in renames:
        print(f"  {old}\n    -> {new}")
    # A leftover "doriaxengine" would come out doubled; it needs its own rule.
    doubled = [p for p, d in text_files(root, files) if re.search(rb"jasperengine\s*engine", rewrite(d), re.I)]
    doubled += [new for _, new in renames if "jasperengineengine" in new.lower()]
    if doubled:
        print("\nERROR: a doubled 'jasperengine engine' would be produced in:", *doubled, sep="\n  ")
    return 1 if doubled else 0


def cmd_apply_paths(root, files):
    renames = planned_renames(files)
    for old, new in renames:
        (root / new).parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "mv", old, new], cwd=root, check=True)
    # git mv leaves the emptied source directories behind.
    for old, _ in renames:
        d = (root / old).parent
        while d != root and d.is_dir() and not any(d.iterdir()):
            d.rmdir()
            d = d.parent
    print(f"Renamed {len(renames)} paths")
    return 0


def cmd_apply_content(root, files):
    counts = Counter()
    changed = 0
    for p, data in text_files(root, files):
        new = rewrite(data, counts)
        if new != data:
            (root / p).write_bytes(new)
            changed += 1
    print(f"Rewrote {changed} files, {sum(counts.values())} replacements")
    return 0


def cmd_check(root, files):
    problems = []
    for p in files:
        if NAME.search(p.encode()):
            problems.append(f"path: {p}")
        full = root / p
        if allowlisted(p) or full.is_symlink() or not full.is_file():
            continue
        data = full.read_bytes()
        if not NAME.search(data):
            continue
        if is_binary(data):
            problems.append(f"binary contains name: {p}")
            continue
        for n, line in enumerate(data.split(b"\n"), 1):
            if NAME.search(line) and not ALLOW_LINE.search(line):
                problems.append(f"{p}:{n}: {line.strip().decode(errors='replace')[:160]}")
    for line in problems:
        print(line)
    print(f"{len(problems)} occurrence(s) outside the allowlist")
    return 1 if problems else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--apply-paths", action="store_true")
    mode.add_argument("--apply-content", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args()

    root = repo_root()
    files = tracked_files(root)
    if args.dry_run:
        return cmd_dry_run(root, files)
    if args.apply_paths:
        return cmd_apply_paths(root, files)
    if args.apply_content:
        return cmd_apply_content(root, files)
    return cmd_check(root, files)


if __name__ == "__main__":
    sys.exit(main())
