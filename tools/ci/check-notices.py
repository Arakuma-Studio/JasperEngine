#!/usr/bin/env python3
"""Checks the third-party notices (NOTICE.md) against the vendored libraries.

    python3 tools/ci/check-notices.py                      # the repository (CI's unit job)
    python3 tools/ci/check-notices.py --installed DIR      # an editor install (bin/ of it)
    python3 tools/ci/check-notices.py --install-manifest   # licence files to install, for CMake

Every directory under libs/, engine/libs/ and engine/libs/shadercompiler/libs/ is a vendored
library and needs:
  - a row in a NOTICE.md table that names its location in backticks (`engine/libs/sokol/`),
  - a licence file in the directory (LICENSE*, LICENCE*, COPYING*, UNLICENSE*).
FIRST_PARTY lists the directories there that are our own code.

The editor install carries NOTICE.md, LICENSE and licenses/<library>-LICENSE for every shipped
library (CMakeLists.txt installs what --install-manifest prints): its first licence file under
that name, any further licence or NOTICE file as <library>-<file name>. --installed checks an
install tree for exactly that, and that the repository-only engine/CMakePresets.json is absent.
Libraries in TESTS_ONLY are never shipped, so they have no licences/ entry.

Exit 1 with one line per problem. S30 reuses this for exported games. Standard library only.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
VENDOR_ROOTS = ("libs", "engine/libs", "engine/libs/shadercompiler/libs")
# our own code inside a vendor root
FIRST_PARTY = {
    "engine/libs/shadercompiler": "the engine's shader compiler (src/, MIT like the engine); its libs/ are checked",
}
# vendored for the tests only (not linked into anything shipped, not installed)
TESTS_ONLY = {"libs/googletest"}

LICENCE_FILE = re.compile(r"^(licen[cs]e|copying|unlicense)(-[\w-]+)?(\.(txt|md|rst))?$", re.IGNORECASE)
PRIMARY_LICENCE = re.compile(r"^(licen[cs]e|copying)(\.(txt|md))?$", re.IGNORECASE)
NOTICE_FILE = re.compile(r"^notice(\.(txt|md))?$", re.IGNORECASE)


def vendored_libraries(repo: Path) -> list[str]:
    """Repository-relative paths ('engine/libs/sokol') of every vendored library directory."""
    found = []
    for root in VENDOR_ROOTS:
        base = repo / root
        if not base.is_dir():
            continue
        for child in sorted(base.iterdir()):
            relative = f"{root}/{child.name}"
            if child.is_dir() and relative not in FIRST_PARTY:
                found.append(relative)
    return found


def licence_files(directory: Path) -> list[Path]:
    """The licence files of a library directory, the primary one first; then its NOTICE files."""
    files = [f for f in sorted(directory.iterdir()) if f.is_file()]
    licences = [f for f in files if LICENCE_FILE.match(f.name)]
    licences.sort(key=lambda f: (not PRIMARY_LICENCE.match(f.name), f.name.lower()))
    return licences + [f for f in files if NOTICE_FILE.match(f.name)]


def notice_locations(notice: str) -> set[str]:
    """The backticked directory locations ('engine/libs/sokol') named in NOTICE.md table rows."""
    locations = set()
    for line in notice.splitlines():
        if line.lstrip().startswith("|"):
            for location in re.findall(r"`([^`\s]+)/`", line):
                locations.add(location.rstrip("/"))
    return locations


def install_manifest(repo: Path) -> list[tuple[Path, str]]:
    """(licence file, name under licenses/) for every shipped library."""
    manifest = []
    for library in vendored_libraries(repo):
        if library in TESTS_ONLY:
            continue
        name = library.rsplit("/", 1)[1]
        files = licence_files(repo / library)
        for index, path in enumerate(files):
            manifest.append((path, f"{name}-LICENSE" if index == 0 else f"{name}-{path.name}"))
    return manifest


def check_repository(repo: Path) -> list[str]:
    problems = []
    notice_path = repo / "NOTICE.md"
    if not notice_path.is_file():
        return [f"{notice_path}: missing"]
    rows = notice_locations(notice_path.read_text(encoding="utf-8"))
    for library in vendored_libraries(repo):
        if library not in rows:
            problems.append(f"{library}: no NOTICE.md row names `{library}/`")
        if not any(LICENCE_FILE.match(f.name) for f in licence_files(repo / library)):
            problems.append(f"{library}: no licence file (LICENSE, LICENCE, COPYING or UNLICENSE)")
    return problems


def check_installed(install: Path, repo: Path) -> list[str]:
    """install: the directory the editor binary is in (bin/ of the install prefix)."""
    problems = []
    for name in ("NOTICE.md", "LICENSE"):
        if not (install / name).is_file():
            problems.append(f"{install}: no {name}")
    for _, name in install_manifest(repo):
        if not (install / "licenses" / name).is_file():
            problems.append(f"{install}: no licenses/{name}")
    for library in TESTS_ONLY:
        name = library.rsplit("/", 1)[1]
        if (install / "licenses" / f"{name}-LICENSE").exists():
            problems.append(f"{install}: licenses/{name}-LICENSE, but {library} is tests only")
    for presets in install.rglob("CMakePresets.json"):
        problems.append(f"{presets}: the repository's CMake presets do not belong in an install")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=REPO, help=argparse.SUPPRESS)
    parser.add_argument("--installed", type=Path, help="check this editor install directory (where the editor binary is)")
    parser.add_argument("--install-manifest", action="store_true", help="print '<licence file>|<installed name>' lines")
    args = parser.parse_args()

    if args.install_manifest:
        for path, name in install_manifest(args.repo):
            print(f"{path.as_posix()}|{name}")
        return 0
    problems = check_installed(args.installed, args.repo) if args.installed else check_repository(args.repo)
    for problem in problems:
        print(f"check-notices: {problem}")
    if not problems:
        what = args.installed or "NOTICE.md"
        print(f"check-notices: {what}: {len(vendored_libraries(args.repo))} vendored libraries, all covered")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
