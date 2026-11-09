# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

"""S84 AC17 (R37, §6.14.3): check_title's thresholds against tools/titleart/calibration.json, the
measurements `jasperengine-editor title-library calibrate` took over the library's renders and the
red controls (tools/mcp/tests/fixtures/title-library-red/).

- The thresholds the editor reports equal the file's: a threshold changed in code without a new
  calibration fails here (the red control of that rule is this comparison itself).
- For every check whose metric separates the library from its red controls, the threshold sits in
  the middle half of the gap: R + 0.25 (L - R) <= T <= L - 0.25 (L - R), with L the library's worst
  value and R the red controls' best (mirrored for checks that pass below their threshold). A
  threshold set on the library's worst value fails (the second red control: `middle_half` below is
  exercised on such a value)."""

import json
from pathlib import Path

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Library Calibration"}

CALIBRATION = Path(__file__).resolve().parents[2] / "titleart" / "calibration.json"
RED = Path(__file__).resolve().parent / "fixtures" / "title-library-red"


def middle_half(library_worst: float, red_best: float, threshold: float) -> bool:
    """§6.14.3: T in the middle half of the gap between the red controls and the library."""
    lo, hi = sorted((library_worst, red_best))
    gap = hi - lo
    return gap > 0 and lo + 0.25 * gap <= threshold <= hi - 0.25 * gap


async def test_rule_itself(ed: Editor):
    """The rule's own red control: a threshold on the library's worst value never passes."""
    ed.check(middle_half(10.0, 2.0, 6.0), "a threshold mid-gap passes")
    ed.check(not middle_half(10.0, 2.0, 10.0), "a threshold on the library's worst value fails")
    ed.check(not middle_half(2.0, 2.0, 2.0), "a check that cannot separate fails")


async def test_thresholds_match_the_file(ed: Editor):
    ed.check(CALIBRATION.exists(), f"{CALIBRATION} exists (title-library calibrate)")
    data = json.loads(CALIBRATION.read_text())
    await ed.call("tool_groups", enable=["title_art"])
    index = json.loads((RED / "index.json").read_text())
    seen = {}
    (ed.project / "assets" / "red").mkdir(parents=True, exist_ok=True)
    for entry in index.values():
        path = f"assets/red/{entry['bad']}"
        (ed.project / path).write_text((RED / entry["bad"]).read_text())
        report = await ed.call("check_title", path=path, backgrounds=["black", "white", "checker"])
        for c in report["checks"]:
            if isinstance(c.get("threshold"), (int, float)):
                seen[c["id"]] = c["threshold"]
    for cid, threshold in seen.items():
        entry = data["checks"].get(cid)
        if entry is None:
            continue
        ed.check_eq(entry.get("threshold"), threshold, f"{cid}: the code's threshold is the calibrated one (re-run title-library calibrate)")
    for cid, entry in data["checks"].items():
        red = entry.get("red_controls")
        if not red or not isinstance(entry.get("threshold"), (int, float)):
            continue
        lib = entry["library"]
        values = [k for k in lib.values()]
        # the library's worst: the extreme on the red side
        red_mid = red["median"]
        lib_min = min(v["min"] for v in values)
        lib_max = max(v["max"] for v in values)
        lower_is_red = red_mid < lib_min
        L = lib_min if lower_is_red else lib_max
        R = red["max"] if lower_is_red else red["min"]
        ed.check(middle_half(L, R, entry["threshold"]), f"{cid}: T {entry['threshold']} in the middle half of L {L} (library) and R {R} (red)")
