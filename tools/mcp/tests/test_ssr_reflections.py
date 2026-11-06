"""SSR under reflection probes (docs/superpowers/specs/2026-11-04-ssr-probes-design.md, 10.5,
docs/rendering/reflections.md): the SSR debug views an agent checks the swap with (Reflection
Source, Occlusion; the range set_scene_settings takes), and get_render_stats' ssr object, which
names the reflection probes with and without a composite slot.

Screenshots are kept in the scratch directory (shots/)."""

from __future__ import annotations

import sys
from pathlib import Path

from harness import Editor

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import pngdiff  # noqa: E402

EDITOR = {"create": True, "name": "SSRReflections"}
CI = True
TIMEOUT = 900

S: dict = {}
M = "submeshes[0].material."

# Reflection Source colours (composite.frag sourceColor), as written to the target
SKY = (51, 115, 255)
PROBE_SLOT_0 = (255, 0, 0)


def shots(ed: Editor) -> Path:
    return (ed.scratch.root if ed.scratch else Path("/tmp/ssr-reflections")) / "shots"


def mean_color(png: bytes, x0: float, y0: float, x1: float, y1: float) -> tuple:
    """the mean RGB of a region given in fractions of the picture"""
    width, height, rgba = pngdiff.decode(png)
    total, count = [0, 0, 0], 0
    for y in range(int(height * y0), int(height * y1)):
        row = y * width * 4
        for x in range(int(width * x0), int(width * x1)):
            for c in range(3):
                total[c] += rgba[row + x * 4 + c]
            count += 1
    return tuple(round(t / max(count, 1)) for t in total)


def share(png: bytes, color: tuple, x0: float, y0: float, x1: float, y1: float, tolerance: int = 12) -> float:
    """the share of a region's pixels (fractions of the picture) within `tolerance` of `color`"""
    width, height, rgba = pngdiff.decode(png)
    near, total = 0, 0
    for y in range(int(height * y0), int(height * y1)):
        row = y * width * 4
        for x in range(int(width * x0), int(width * x1)):
            total += 1
            if all(abs(rgba[row + x * 4 + c] - color[c]) <= tolerance for c in range(3)):
                near += 1
    return near / max(total, 1)


async def errors(ed: Editor):
    return [e for e in await ed.console_since(None) if e.get("type") == "error"]


async def metal_tile(ed: Editor, sid: int, name: str, position: list, size: float, depth: float | None = None):
    await ed.call("create_entity", scene=sid, type="plane", name=name, position=position, scale=[size, 1, depth or size],
                  values={"mesh": {M + "metallicFactor": 1.0, M + "roughnessFactor": 0.05, "receiveIBL": True}})


async def prepare_scene(ed: Editor) -> int:
    if S.get("editor") is ed:
        return S["scene"]
    S.clear()
    sid = (await ed.call("create_scene", name="SSRReflections", type="3D", path="scenes/SSRReflections"))["scene"]["id"]
    S["scene"] = sid
    # a metal floor of two halves (a mesh takes the probe its centre is in), the right one (x > 0)
    # inside a reflection probe, an unlit cube on it
    await metal_tile(ed, sid, "Ground Left", [-7.5, 0, 0], 15, 30)
    await metal_tile(ed, sid, "Ground Right", [7.5, 0, 0], 15, 30)
    await ed.call("create_entity", scene=sid, type="cube", name="Panel", position=[0, 1, -1], scale=[2, 2, 2],
                  values={"mesh": {M + "lighting": "none", M + "baseColorFactor": [1, 1, 1, 1]}})
    await ed.call("create_entity", scene=sid, type="reflection_probe", name="Probe Right", position=[7.5, 1, 0],
                  values={"reflectionprobe": {"boxSize": [15, 10, 30]}})
    await ed.call("create_entity", scene=sid, type="camera", name="Main Camera", position=[0, 3, 8],
                  rotation={"euler": [-15, 0, 0]})
    await ed.call("set_main_camera", scene=sid, entity="Main Camera")
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_enabled": True})
    await ed.call("configure_game_view", aspect="16:9")
    await ed.call("wait", frames=60)
    S["editor"] = ed
    return sid


async def test_debug_view_names_and_range(ed: Editor):
    sid = await prepare_scene(ed)
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_debug_mode": 8})
    got = await ed.call("get_scene_settings", scene=sid, names=["ssr_debug_mode"], detail=True)
    ed.check_eq(got["settings"]["ssr_debug_mode"], 8, f"ssr_debug_mode reads back: {got!r}")
    # the help names every view, so an agent finds the two that check the swap
    help_text = got["schema"]["ssr_debug_mode"]["help"]
    ed.check("7 reflection source" in help_text and "8 occlusion" in help_text, f"the views are named: {help_text!r}")
    await ed.check_error("set_scene_settings", scene=sid, settings={"ssr_debug_mode": 9}, contains="0 to 8")
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_debug_mode": 0})


async def test_reflection_source_view(ed: Editor):
    sid = await prepare_scene(ed)
    plain = await ed.screenshot(view="game", path=shots(ed) / "plain.png", max_width=640)
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_debug_mode": 7})
    await ed.call("wait", frames=10)
    source = await ed.screenshot(view="game", path=shots(ed) / "reflection-source.png", max_width=640)
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_debug_mode": 0})

    # the floor's left half takes the sky, its right half the probe in slot 0
    left = share(source, SKY, 0.05, 0.8, 0.3, 0.97)
    right = share(source, PROBE_SLOT_0, 0.7, 0.8, 0.95, 0.97)
    ed.check(left > 0.6, f"the floor outside the probe is Sky in Reflection Source ({left:.2f} of the region, "
             f"mean {mean_color(source, 0.05, 0.8, 0.3, 0.97)})")
    ed.check(right > 0.6, f"the floor in the probe is probe slot 0 in Reflection Source ({right:.2f} of the region, "
             f"mean {mean_color(source, 0.7, 0.8, 0.95, 0.97)})")
    # the unlit panel takes no SSR: its (white) pixels are black (Skip) in Reflection Source
    _, _, plain_rgba = pngdiff.decode(plain)
    _, _, source_rgba = pngdiff.decode(source)
    panel = [i for i in range(0, len(plain_rgba), 4) if min(plain_rgba[i:i + 3]) >= 250]
    ed.check(len(panel) > 200, f"the unlit panel is drawn white ({len(panel)} pixels)")
    skipped = sum(1 for i in panel if max(source_rgba[i:i + 3]) <= 4)
    ed.check(skipped >= 0.9 * len(panel), f"the unlit panel is Skip (black): {skipped} of {len(panel)} pixels")
    ed.check(not await errors(ed), f"console errors: {(await errors(ed))[:5]!r}")


async def test_render_stats_report_probe_slots(ed: Editor):
    sid = await prepare_scene(ed)
    # five more probes over their own tiles: six in view, four slots
    for i in range(5):
        x = -9 + 4.5 * i
        await metal_tile(ed, sid, f"Tile {i}", [x, 0.01, -4], 3)
        await ed.call("create_entity", scene=sid, type="reflection_probe", name=f"Probe {i}", position=[x, 1, -4],
                      values={"reflectionprobe": {"boxSize": [3.2, 4, 3.2], "resolution": 32}})
    await ed.call("wait", frames=120)
    r = await ed.call_raw("get_render_stats", {})
    ssr = r.value.get("ssr")
    ed.check(ssr is not None, f"get_render_stats has an ssr object with SSR on: {sorted(r.value)}")
    ed.check_eq(ssr["probe_slots"], 4, f"four probe slots: {ssr!r}")
    ed.check_eq(ssr["probes_in_view"], 6, f"six probes light drawn meshes: {ssr!r}")
    ed.check_eq(ssr["probes_without_slot"], 2, f"two probes have no slot: {ssr!r}")
    without = [p["name"] for p in ssr["probes"] if p["slot"] is None]
    ed.check_eq(len(without), 2, f"the probes without a slot are named: {ssr['probes']!r}")
    ed.check("Probe Right" not in without, f"the floor's probe, the largest on screen, has a slot: {without!r}")
    ed.check("SSR: 4/6 probes with slots" in r.text, f"the summary says it: {r.text!r}")
    console = await ed.console_since(None)
    ed.check(any("reflection probes in view" in e.get("message", "") for e in console),
             "the overflow is logged")
