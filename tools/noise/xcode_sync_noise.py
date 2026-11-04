#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# Lists the T2 noise sources in the Xcode workspace (CMake lists them in
# engine/core/noise/NoiseSources.cmake; the Xcode project lists files explicitly):
#
#   python3 tools/noise/xcode_sync_noise.py [--check]
#
# The same phases and object ids as tools/particles/xcode_sync_particles.py, whose helpers this uses.
# Each listed .cpp gets -ffp-contract=off, as CMake gives it (R38). The removed terrain/TerrainNoise.h
# leaves the project here too. --check exits 1 when something is missing or stale.

import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "particles"))
import xcode_sync_particles as base  # noqa: E402

SOURCES_CMAKE = os.path.join(base.ROOT, "engine", "core", "noise", "NoiseSources.cmake")
EXTRA_FILES = ["terrain/HeightfieldErosion.cpp", "terrain/HeightfieldErosion.h"]
STALE = "TerrainNoise.h"


def noise_files():
    names = sorted(n for n in os.listdir(os.path.join(base.ROOT, "engine", "core", "noise")) if n.endswith((".cpp", ".h")))
    return ["noise/" + n for n in names] + EXTRA_FILES


def flagged_sources():
    with open(SOURCES_CMAKE, encoding="utf-8") as f:
        text = f.read()
    out = set()
    for block in re.findall(r"set\(JASPERENGINE_NOISE(?:_COMPAT)?_SRCS(.*?)\)", text, re.S):
        out |= {line.strip()[len("core/"):] for line in block.split("\n") if line.strip().startswith("core/")}
    # in TerrainSources.cmake, with the same flag
    out.add("terrain/HeightfieldErosion.cpp")
    return out


def main():
    check = "--check" in sys.argv
    with open(base.PROJECT, encoding="utf-8") as f:
        text = f.read()
    files = noise_files()
    missing = [path for path in files if base.object_id("ref", path) not in text]
    stale = STALE in text
    if not missing and not stale:
        print("Xcode project lists every noise file")
        return 0
    if check:
        print("missing from the Xcode project: " + ", ".join(missing) + (" (TerrainNoise.h still listed)" if stale else ""))
        return 1
    lines = [line for line in text.split("\n") if STALE not in line]
    flagged = flagged_sources()
    for path in missing:
        base.add_file(lines, path, flagged)
    with open(base.PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d noise file(s) to the Xcode project" % len(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
