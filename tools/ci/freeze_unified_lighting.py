#!/usr/bin/env python3
"""Make a copy of tools/fixtures/unified-lighting still, so that its screen can be compared with a golden.

    python3 tools/ci/freeze_unified_lighting.py <project copy>

The fixture's lights orbit with time and its light cap switches at 2 s and 4 s, so what a device shows
depends on when the screenshot is taken. A device run cannot pass it an environment on every platform,
so the packaging scripts edit their own copy: lights at their first position, the cap never switched.
"""

import sys
from pathlib import Path

REPLACEMENTS = {
    "local a = self.time * 0.6 + entry.phase": "local a = entry.phase",
    "self.capAt = { 2.0, 4.0 }": "self.capAt = { 1e9, 1e9 }",
}


def main() -> int:
    script = Path(sys.argv[1]) / "scripts" / "UnifiedLighting.lua"
    text = script.read_text()
    for old, new in REPLACEMENTS.items():
        if old not in text:
            print(f"{script} has no '{old}': the fixture changed, update this script", file=sys.stderr)
            return 1
        text = text.replace(old, new)
    script.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
