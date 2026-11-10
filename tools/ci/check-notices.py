#!/usr/bin/env python3
"""Checks the third-party notices (NOTICE.md) against the vendored libraries.

    python3 tools/ci/check-notices.py                      # the repository (CI's unit job)
    python3 tools/ci/check-notices.py --installed DIR      # an editor install (bin/ of it)
    python3 tools/ci/check-notices.py --install-manifest   # licence files to install, for CMake

Every directory under libs/, engine/libs/ and engine/libs/shadercompiler/libs/ is a vendored
library, and so is each NESTED_LIBRARIES directory (code inside a library's directory that ships
under other conditions, such as HarfBuzz's editor-only subsetter); each needs:
  - a row in a NOTICE.md table that names its location in backticks (`engine/libs/sokol/`),
  - a licence file in the directory (LICENSE*, LICENCE*, COPYING*, UNLICENSE*).
FIRST_PARTY lists the directories there that are our own code.

Every directory under a CONTENT_ROOTS root (the Title Art library's fonts, S84 R31) is third-party
content the editor install carries: it needs a NOTICE.md row naming it the same way and its licence
file (OFL.txt), installed as licenses/<prefix>-<directory>-<licence file>.

The editor install carries NOTICE.md, LICENSE and licenses/<library>-LICENSE for every shipped
library (CMakeLists.txt installs what --install-manifest prints): its first licence file under
that name, any further licence or NOTICE file as <library>-<file name>. --installed checks an
install tree for exactly that, and that the repository-only engine/CMakePresets.json is absent.
Libraries in TESTS_ONLY are never shipped, so they have no licences/ entry.

engine/notices.json (S30 R27) says which shipped games link each engine/libs library (always, a player
feature such as physics3d or a list of features, or editor: never in a game); every engine/libs directory
needs an entry.
--player-notices FEATURE... prints the licence files a player with those features stages ('<file>|<name>'
like --install-manifest), and --third-party-notices FEATURE... the THIRD-PARTY-NOTICES.txt of such a
player (its libraries' NOTICE.md rows as text): tools/players/package_player.py writes both into a runtime.

Exit 1 with one line per problem. S30 reuses this for exported games. Standard library only.
"""

from __future__ import annotations

import argparse
import json
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
# libraries inside another library's directory, with their own engine/notices.json entry (their name)
NESTED_LIBRARIES = {
    "engine/libs/harfbuzz/subset": "harfbuzz-subset",
}
# third-party content staged with the editor: root -> (each directory's licence file, licenses/ prefix)
CONTENT_ROOTS = {
    "editor/resources/titleart/fonts": ("OFL.txt", "titleart-font"),
}

# (the extension may name the licence and its version, as FFmpeg's COPYING.LGPLv2.1 does)
LICENCE_FILE = re.compile(r"^(licen[cs]e|copying|unlicense)(-[\w-]+)?(\.(txt|md|rst|[al]?gplv\d(\.\d)?))?$", re.IGNORECASE)
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
    found += [nested for nested in sorted(NESTED_LIBRARIES) if (repo / nested).is_dir()]
    return found


def content_directories(repo: Path) -> list[str]:
    """Repository-relative paths of every CONTENT_ROOTS directory ('editor/resources/titleart/fonts/anton')."""
    found = []
    for root in sorted(CONTENT_ROOTS):
        base = repo / root
        if base.is_dir():
            found += [f"{root}/{child.name}" for child in sorted(base.iterdir()) if child.is_dir()]
    return found


def content_licence(content: str) -> tuple[str, str]:
    """A content directory's licence file name and its name under licenses/."""
    root, directory = content.rsplit("/", 1)
    licence, prefix = CONTENT_ROOTS[root]
    return licence, f"{prefix}-{directory}-{licence}"


def library_name(library: str) -> str:
    """A library's name (licenses/<name>-LICENSE, its engine/notices.json key): its directory's, or its nested name."""
    return NESTED_LIBRARIES.get(library, library.rsplit("/", 1)[-1])


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
        name = library_name(library)
        files = licence_files(repo / library)
        for index, path in enumerate(files):
            manifest.append((path, f"{name}-LICENSE" if index == 0 else f"{name}-{path.name}"))
    for content in content_directories(repo):
        licence, name = content_licence(content)
        if (repo / content / licence).is_file():
            manifest.append((repo / content / licence, name))
    return manifest


GAME_CONDITIONS = {"always", "htmlui", "video", "physics2d", "physics3d", "navigation", "runtimeshaders", "voxel", "tracy",
                   "titleart", "editor"}


def when_conditions(entry: dict) -> list:
    """An entry's `when`: one condition, or a list of them (ThorVG ships with the HTML UI or Title Art)."""
    when = entry.get("when")
    return when if isinstance(when, list) else [when]


def load_game_notices(repo: Path) -> dict:
    """engine/notices.json's libraries: {name: {dir, when}}."""
    return json.loads((repo / "engine/notices.json").read_text(encoding="utf-8"))["libraries"]


def check_game_notices(repo: Path) -> list[str]:
    problems = []
    if not (repo / "engine/notices.json").is_file():
        return ["engine/notices.json: missing (which shipped games link each engine/libs library)"]
    try:
        libraries = load_game_notices(repo)
    except (ValueError, KeyError) as error:
        return [f"engine/notices.json: {error}"]
    listed = {}
    for name, entry in libraries.items():
        directory = entry.get("dir", "")
        listed[directory] = name
        conditions = when_conditions(entry)
        if not conditions or any(not isinstance(c, str) or c not in GAME_CONDITIONS for c in conditions):
            problems.append(f"engine/notices.json: {name}: when must be one of {', '.join(sorted(GAME_CONDITIONS))}"
                            " or a list of player features")
        if not (repo / directory).is_dir():
            problems.append(f"engine/notices.json: {name}: {directory} does not exist")
        elif library_name(directory) != name:
            problems.append(f"engine/notices.json: {name}: the name must be its directory's ({directory})")
    for library in vendored_libraries(repo):
        if library.startswith("engine/libs/") and library not in listed:
            problems.append(f"{library}: no engine/notices.json entry (when: always, a player feature or editor)")
    return problems


def player_libraries(repo: Path, features: list[str]) -> list[str]:
    """The engine/notices.json libraries a player with these features ships, sorted."""
    return sorted(name for name, entry in load_game_notices(repo).items()
                  if any(c == "always" or c in features for c in when_conditions(entry)))


def player_manifest(repo: Path, features: list[str]) -> list[tuple[Path, str]]:
    """(licence file, name under licenses/) of the libraries a player with these features ships."""
    libraries = load_game_notices(repo)
    manifest = []
    for name in player_libraries(repo, features):
        for index, path in enumerate(licence_files(repo / libraries[name]["dir"])):
            manifest.append((path, f"{name}-LICENSE" if index == 0 else f"{name}-{path.name}"))
    return manifest


def third_party_notices(repo: Path, features: list[str]) -> str:
    """THIRD-PARTY-NOTICES.txt of a player: the NOTICE.md row of each library it ships, as text."""
    libraries = load_game_notices(repo)
    rows = [line for line in (repo / "NOTICE.md").read_text(encoding="utf-8").splitlines() if line.lstrip().startswith("|")]
    out = ["Third-party software in this game", "",
           "Each library below keeps its own licence; its text is in licenses/<library>-LICENSE.", ""]
    for name in player_libraries(repo, features):
        location = "`" + libraries[name]["dir"] + "/`"
        row = next((r for r in rows if location in r), None)
        if row is None:
            continue
        cells = [re.sub(r"\[([^]]*)\]\([^)]*\)", r"\1", cell.strip()).replace("`", "") for cell in row.strip().strip("|").split("|")]
        library, version, _, licence, used = (cells + [""] * 5)[:5]
        out.append(f"{name}: {library}, version {version}, {licence} (licenses/{name}-LICENSE). Used for: {used}")
    return "\n".join(out) + "\n"


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
    for content in content_directories(repo):
        if content not in rows:
            problems.append(f"{content}: no NOTICE.md row names `{content}/`")
        licence, _ = content_licence(content)
        if not (repo / content / licence).is_file():
            problems.append(f"{content}: no {licence}")
    problems += check_game_notices(repo)
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
    parser.add_argument("--player-notices", nargs="*", metavar="FEATURE",
                        help="print '<licence file>|<name>' lines of a player with these features")
    parser.add_argument("--third-party-notices", nargs="*", metavar="FEATURE",
                        help="print the THIRD-PARTY-NOTICES.txt of a player with these features")
    args = parser.parse_args()

    if args.player_notices is not None:
        for path, name in player_manifest(args.repo, args.player_notices):
            print(f"{path.as_posix()}|{name}")
        return 0
    if args.third_party_notices is not None:
        sys.stdout.write(third_party_notices(args.repo, args.third_party_notices))
        return 0
    if args.install_manifest:
        for path, name in install_manifest(args.repo):
            print(f"{path.as_posix()}|{name}")
        return 0
    problems = check_installed(args.installed, args.repo) if args.installed else check_repository(args.repo)
    for problem in problems:
        print(f"check-notices: {problem}")
    if not problems:
        what = args.installed or "NOTICE.md"
        print(f"check-notices: {what}: {len(vendored_libraries(args.repo))} vendored libraries and "
              f"{len(content_directories(args.repo))} content directories, all covered")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
