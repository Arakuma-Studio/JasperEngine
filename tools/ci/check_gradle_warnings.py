#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Fails on a Gradle deprecation in our Android Gradle files (platform-lifecycle spec D12).

    python3 tools/ci/check_gradle_warnings.py build/android/gradle.log

The log is a Gradle run of engine/workspaces/androidstudio with --warning-mode all (its
gradle.properties sets it). Gradle names the build file of a deprecation it found while
configuring ("Build file '.../app/build.gradle': line 48" before or after the message), and the
Android plugin warns about gradle.properties options it deprecated or removed ("The option setting
'android.nonFinalResIds=false' is deprecated"). Either one in our files is a failure: the template
and the Gradle files the export writes (app, jasperbridge, nativeplugins, the root project).
A deprecation that names a file of another project (a third-party plugin) is printed as a note:
the next Gradle may break it, but it is not ours to fix.

Exit 1 listing each failure as "<file>:<line>: <message>", 0 otherwise. Standard library only.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# the files this repository owns, as Gradle prints their paths (any checkout location)
OURS = re.compile(r"(engine/workspaces/androidstudio[^/]*|/nativeplugins)/(?:[\w.-]+/)*[\w.-]+\.gradle(?:\.kts)?|"
                  r"engine/workspaces/androidstudio[^/]*/gradle\.properties")
BUILD_FILE = re.compile(r"(?:Build|Settings|Script) file '([^']+)'(?::\s*line\s*(\d+))?")
DEPRECATION = re.compile(r"deprecat|obsolete|scheduled to be removed|will be removed", re.IGNORECASE)
OPTION = re.compile(r"The option (?:setting )?'(android\.[\w.]+)(?:=[^']*)?' is (deprecated|obsolete|experimental|removed)",
                    re.IGNORECASE)


def problems(text: str) -> tuple[list[str], list[str]]:
    """(failures, notes) of a Gradle log."""
    failures: list[str] = []
    notes: list[str] = []
    lines = text.splitlines()
    for index, line in enumerate(lines):
        option = OPTION.search(line)
        if option:
            failures.append(f"gradle.properties: {option.group(1)} is {option.group(2).lower()}: {line.strip()}")
            continue
        found = BUILD_FILE.search(line)
        if not found:
            continue
        # the message is on the line itself or right next to it
        nearby = [line] + lines[index + 1:index + 3] + lines[max(0, index - 2):index]
        message = next((candidate.strip() for candidate in nearby if DEPRECATION.search(candidate)), None)
        if message is None:
            continue
        where = f"{found.group(1)}:{found.group(2) or '?'}"
        if OURS.search(found.group(1).replace("\\", "/")):
            failures.append(f"{where}: {message}")
        else:
            notes.append(f"{where}: {message}")
    return sorted(set(failures)), sorted(set(notes))


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    failures, notes = problems(Path(argv[1]).read_text(errors="replace"))
    for note in notes:
        print(f"check_gradle_warnings: note (not ours): {note}")
    for failure in failures:
        print(f"check_gradle_warnings: FAIL: {failure}")
    if not failures:
        print("check_gradle_warnings: no deprecation in our Gradle files")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
