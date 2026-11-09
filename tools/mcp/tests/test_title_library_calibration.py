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


# the checks of §6.14.3 whose metric cannot separate the library from its red controls; each is a
# recorded finding (spec Deviations), not a pass
CANNOT_SEPARATE = {"legibility_contrast", "not_clipped", "style_lint.hard_shadow", "style_lint.rims_thin"}


async def test_thresholds_match_the_file(ed: Editor):
    ed.check(CALIBRATION.exists(), f"{CALIBRATION} exists (title-library calibrate)")
    data = json.loads(CALIBRATION.read_text())
    ed.check(data["checks"], "the calibration measured something")
    await ed.call("tool_groups", enable=["title_art"])
    index = json.loads((RED / "index.json").read_text())
    (ed.project / "assets" / "red").mkdir(parents=True, exist_ok=True)
    for entry in index.values():
        path = f"assets/red/{entry['bad']}"
        (ed.project / path).write_text((RED / entry["bad"]).read_text())
        report = await ed.call("check_title", path=path, backgrounds=["black", "white", "checker"], render_scale=0.25)
        for c in report["checks"]:
            file_entry = data["checks"].get(c["id"])
            if file_entry is None or c.get("threshold") is None:
                continue
            ed.check_eq(file_entry.get("threshold"), c["threshold"], f"{c['id']}: the code's threshold is the calibrated one (re-run title-library calibrate)")
    for cid, entry in data["checks"].items():
        if "R" not in entry:
            continue
        if cid in CANNOT_SEPARATE:
            ed.check(not entry["separates"], f"{cid}: listed as inseparable, yet it separates: drop it from the list")
            continue
        ed.check(entry["separates"], f"{cid}: L {entry['L']} ({entry.get('L_item')}) and R {entry['R']} do not separate")
        ed.check(middle_half(entry["L"], entry["R"], entry["T"]), f"{cid}: T {entry['T']} in the middle half of L {entry['L']} and R {entry['R']}")
