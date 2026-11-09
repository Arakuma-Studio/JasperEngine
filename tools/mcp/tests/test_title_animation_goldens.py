"""S85 §7's fixtures and goldens: the documents of tools/fixtures/title-art-anim rendered at fixed
times (render_title options.animation.time), against tools/mcp/tests/golden/title/anim (pngdiff).
JASPER_MCP_TITLE_ANIM_UPDATE_GOLDEN=1 builds the fixtures from their ops and writes both.
"""

import json
import os
import struct
import zlib
from pathlib import Path

import harness
import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Animation Goldens"}
TIMEOUT = 900

FIXTURES = harness.REPO / "tools" / "fixtures" / "title-art-anim"
GOLDEN = harness.REPO / "tools" / "mcp" / "tests" / "golden" / "title" / "anim"
UPDATE = os.environ.get("JASPER_MCP_TITLE_ANIM_UPDATE_GOLDEN") == "1"
TOLERANCE = 24
MAX_FRACTION = 0.01


def key(*a):
    return list(a)


# name -> create text, ops, the animation rendered, its times
CASES = {
    "two_word": ("BIG WIN", [
        {"op": "add_animation", "name": "Intro", "duration": 1.0},
        {"op": "set_track", "animation": "Intro", "field": "opacity", "keys": [key(0, 0), key(0.5, 1)]},
        {"op": "set_track", "animation": "Intro", "field": "transform.scale", "relative": True, "keys": [key(0, 2, "back_out"), key(0.6, 1)]},
    ], "Intro", [0.0, 0.25, 0.5, 1.0]),
    "vertical_kanji": ("勝利", [
        {"op": "set", "layer": "Title", "values": {"vertical": True}},
        {"op": "add_animation", "name": "Intro", "duration": 1.0},
        {"op": "set_track", "animation": "Intro", "layer": "Title", "field": "glyph.opacity", "keys": [key(0, 0), key(0.4, 1)],
         "stagger": {"each": 0.1, "order": "forward"}},
    ], "Intro", [0.0, 0.3, 1.0]),
    "adjustment": ("MEGA", [
        {"op": "add_layer", "type": "adjustment", "name": "Grade", "adjustment": "hue_saturation"},
        {"op": "add_animation", "name": "Move", "duration": 1.0},
        {"op": "set_track", "animation": "Move", "layer": "Title", "field": "transform.position", "keys": [key(0, [0, 0]), key(1, [200, 0])]},
    ], "Move", [0.0, 0.5, 1.0]),
    "loop_seam": ("LOOP", [
        {"op": "add_animation", "name": "Idle", "duration": 1.0, "wrap": "loop"},
        {"op": "set_track", "animation": "Idle", "field": "transform.rotation", "drive": {"from": [0], "to": [360], "cycles": 1}},
    ], "Idle", [0.0, 0.5]),
    "strobe": ("FLASH", [
        {"op": "set", "layer": "Title", "values": {"face.fill.color": "#303030"}},
        {"op": "add_animation", "name": "Strobe", "duration": 1.0},
        {"op": "set_track", "animation": "Strobe", "field": "flash.amount", "keys": [key(k / 20, 3 if k % 2 else 0, "step") for k in range(21)]},
    ], "Strobe", [0.05, 0.1]),
    "over_budget_flipbook": ("HUGE", [
        {"op": "add_rim", "layer": "Title"},
        {"op": "add_animation", "name": "Pulse", "duration": 4.0},
        {"op": "set_animation", "animation": "Pulse", "values": {"flipbook.fps": 60}},
        {"op": "set_track", "animation": "Pulse", "layer": "Title", "field": "rim#0.width", "keys": [key(0, 2), key(2, 18), key(4, 2)]},
    ], "Pulse", [0.0, 2.0]),
}


async def prepare(ed: Editor, name: str, text: str, ops: list) -> str:
    path = f"assets/{name}.titleart"
    fixture = FIXTURES / f"{name}.titleart"
    if UPDATE:
        await ed.call("edit_title", path=path, create={"text": text, "canvas": [480, 270], "overwrite": True}, bake="never", ops=ops)
        FIXTURES.mkdir(parents=True, exist_ok=True)
        fixture.write_text((Path(ed.project) / path).read_text())
    else:
        target = Path(ed.project) / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(fixture.read_text())
    return path


async def test_fixed_times_match_the_goldens(ed: Editor):
    await ed.call("tool_groups", enable=["title_art", "editor_tools"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    bad = []
    for name, (text, ops, animation, times) in CASES.items():
        path = await prepare(ed, name, text, ops)
        for t in times:
            stem = f"{name}_{int(round(t * 1000)):04d}"
            save = f"renders/{stem}.png"
            await ed.call("render_title", path=path, save_path=save, backgrounds=["black"], image="none",
                          options={"animation": {"name": animation, "time": t}})
            rendered = Path(ed.project) / save
            ed.check(rendered.exists(), f"{save} was written")
            golden = GOLDEN / f"{stem}.png"
            if UPDATE:
                GOLDEN.mkdir(parents=True, exist_ok=True)
                golden.write_bytes(rendered.read_bytes())
                continue
            ed.check(golden.exists(), f"no golden {golden.name}: run with JASPER_MCP_TITLE_ANIM_UPDATE_GOLDEN=1")
            ok, fraction, _ = pngdiff.compare(pngdiff.load(rendered), pngdiff.load(golden), channel_tolerance=TOLERANCE, max_fraction=MAX_FRACTION)
            if not ok:
                bad.append(f"{stem}: {fraction:.4f} of the pixels differ")
    ed.check(not bad, "\n".join(bad))
