"""S80 diff_images (docs/specs/S80-title-art-core.md AC38; jasper://docs/title-art): identical
images match; premultiplied mode ignores colour under alpha 0 that straight mode reports; a size
mismatch is an error naming both sizes; tools/mcp/pngdiff.py --premultiplied agrees with the tool.
"""

import subprocess
import sys
from pathlib import Path

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Diff Images"}

HERE = Path(__file__).resolve().parent.parent


def write(ed: Editor, name: str, w: int, h: int, pixel) -> str:
    rgba = bytearray()
    for y in range(h):
        for x in range(w):
            rgba += bytes(pixel(x, y))
    (ed.project / "diff").mkdir(exist_ok=True)
    (ed.project / "diff" / name).write_bytes(pngdiff.encode(w, h, rgba))
    return "diff/" + name


async def test_diff_images(ed: Editor):
    await ed.call("tool_groups", enable=["editor_tools"])
    base = write(ed, "a.png", 16, 8, lambda x, y: (200, 40, 40, 255) if x < 8 else (0, 0, 0, 0))
    same = write(ed, "same.png", 16, 8, lambda x, y: (200, 40, 40, 255) if x < 8 else (0, 0, 0, 0))
    # the transparent half holds another colour: only straight comparison sees it
    under = write(ed, "under.png", 16, 8, lambda x, y: (200, 40, 40, 255) if x < 8 else (0, 255, 0, 0))
    other = write(ed, "other.png", 16, 8, lambda x, y: (40, 200, 40, 255) if x < 8 else (0, 0, 0, 0))
    small = write(ed, "small.png", 8, 8, lambda x, y: (200, 40, 40, 255))

    r = await ed.call("diff_images", a=base, b=same)
    ed.check(r["ok"], f"identical images match: {r}")
    ed.check_eq(r["changed_pct"], 0, "nothing changed")
    pre = await ed.call("diff_images", a=base, b=under)
    ed.check(pre["ok"] and pre["max"] == 0, f"premultiplied ignores colour under alpha 0: {pre}")
    straight = await ed.call("diff_images", a=base, b=under, mode="straight")
    ed.check(not straight["ok"] and straight["max"] == 255, f"straight reports it: {straight}")
    raw = await ed.call_raw("diff_images", {"a": base, "b": other})
    ed.check(not raw.is_error and not raw.structured["ok"], "a real difference fails the thresholds")
    ed.check(raw.images, "the three-panel image on a failure")
    ed.check_eq(raw.structured["bbox"], [0, 0, 8, 8], "the box of the changed pixels")
    msg = await ed.check_error("diff_images", a=base, b=small)
    ed.check_in("16 x 8", msg, "the size mismatch names the first size")
    ed.check_in("8 x 8", msg, "and the second")
    saved = await ed.call("diff_images", a=base, b=other, save_diff="diff/out.png", image="none")
    ed.check((ed.project / saved["saved_diff"]).exists(), "save_diff writes the difference image")

    # pngdiff.py --premultiplied agrees with the tool on both pairs
    for b, ok in ((under, True), (other, False)):
        proc = subprocess.run([sys.executable, str(HERE / "pngdiff.py"), str(ed.project / base), str(ed.project / b), "--premultiplied",
                               "--tolerance", "8", "--max-fraction", "0.001"], capture_output=True, text=True)
        ed.check_eq(proc.returncode == 0, ok, f"pngdiff --premultiplied on {b}: {proc.stdout}")
        stats = pngdiff.stats(pngdiff.load(str(ed.project / base)), pngdiff.load(str(ed.project / b)), premultiplied=True)
        tool = await ed.call("diff_images", a=base, b=b, image="none")
        ed.check_eq(stats["max"], tool["max"], f"the same max for {b}")
        ed.check(abs(stats["mean"] - tool["mean"]) < 1e-3, f"the same mean for {b}: {stats['mean']} vs {tool['mean']}")
