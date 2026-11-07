"""S75 navigation diagnostics in the editor UI (docs/specs/S75-navigation-diagnostics.md R25-R29,
AC13-AC15), driven like a person in the harness's Xvfb and checked through MCP.

Scene NavOverlays (3D): two 10 x 10 m floors 12 m apart under one NavMesh Surface, a 2 m cube
standing on the first (its sides are steeper than the agent's Max Slope; its top is a third
island), and a walker
with a NavMesh Agent far off both floors (finding agent_off_mesh).

- test_path_tester_flow: navigation.pathTester (run_command) opens the Path Tester; Shift+click and
  click in the Scene view set the ends; dragging the end's handle queries again; an end on the
  other floor is Partial (disconnected); Esc ends it; it never changes the selection.
- test_overlay_goldens: Scene view > Gizmos > AI Navigation > Color By Islands / Tiles and Show Steep
  Slopes each change the image, and match golden/navigation_islands.png, navigation_tiles.png and
  navigation_slope.png (JASPER_UPDATE_GOLDEN=1 rewrites them; a missing golden fails).
- test_window_last_bake_and_problems: Window > AI > Navigation, Surfaces: the Last Bake, Walkable and
  Islands cells, and the Problems row of the walker, whose click selects it.
- test_inspector_summary_and_estimate: the NavMesh Surface's Inspector rows (get_inspector_layout):
  last bake, walkable area and islands, the "Voxels ... tiles" estimate, and its fewer-than-2-voxels
  warning once Voxel Size is 0.4 m for the 0.5 m agent.
- test_inspector_estimate_volume_matches_bake: a scaled Volume surface's estimate counts the tiles its
  bake builds (the box is not stretched by the scale).

Screenshots of the UI go to $JE_EVIDENCE_DIR (else the scratch directory).
"""

from __future__ import annotations

import os
import re
from pathlib import Path

import pngdiff
from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Navigation Overlays", "ui_probe": True}
TIMEOUT = 1200

GOLDEN_DIR = Path(__file__).resolve().parent / "golden"
UPDATE = os.environ.get("JASPER_UPDATE_GOLDEN") == "1"

S: dict = {}


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root)
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        S["ui"].xdo("mouseup", "1")
        await S["ui"].release_all()
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    S["ui"] = Ui(ed)
    made = await ed.call("create_scene", name="NavOverlays", type="3D", path="scenes/NavOverlays")
    sid = made["scene"]["id"]
    S["scene"] = sid
    S["floor"] = (await ed.call("create_entity", scene=sid, type="plane", name="Floor", position=[-6, 0, 0]))["entity_id"]
    await ed.call("create_entity", scene=sid, type="plane", name="Island", position=[6, 0, 0])
    S["cube"] = (await ed.call("create_entity", scene=sid, type="cube", name="Block", position=[-6, 1, 0], scale=[2, 2, 2]))["entity_id"]
    await ed.call("add_component", scene=sid, entity=S["floor"], component="navmeshsurface")
    walker = await ed.call("create_entity", scene=sid, type="capsule", name="Walker", position=[0, 1, 20])
    S["walker"] = walker["entity_id"]
    await ed.call("add_component", scene=sid, entity=S["walker"], component="navmeshagent", values={"navmeshagent": {"baseOffset": 1.0}})
    await ed.call("save_scene", scene=sid)
    baked = await ed.call("bake_navigation", scene=sid, mode="full")
    ed.check(all(item.get("ok") for item in baked["surfaces"]), f"bake: {baked}")
    await ed.call("select_entities", scene=sid, entities=[])
    await ed.call("set_scene_camera", scene=sid, position=[0.0, 26.0, 0.01], target=[0.0, 0.0, 0.0],
                  display={"show_grid_3d": False, "hide_light_icons": True})
    S["ui"].focus()
    await S["ui"].frames(4)
    return S


async def view_rect(ed: Editor, sid: int) -> dict:
    shot = await ed.call_raw("screenshot", {"view": "scene", "scene": sid, "max_width": 0})
    return shot.structured["view_rect_in_window"]


def view_point(ui: Ui, rect: dict, u: float, v: float) -> tuple[int, int]:
    return ui.at(rect, (rect["width"] * u, rect["height"] * v))


async def probe_names(ed: Editor, ui: Ui, prefix: str, frames: int = 30) -> list[str]:
    """The UIProbe names drawn with this prefix (waits for the editor to draw one)."""
    for _ in range(frames):
        names = [name for name in ed.ui_items() if name.startswith(prefix)]
        if names:
            return names
        await ui.frames(2)
    return []


async def tester_status(ed: Editor, ui: Ui) -> str | None:
    names = await probe_names(ed, ui, "pathTester.status.")
    return names[0].rsplit(".", 1)[1] if names else None


async def tester_length(ed: Editor, ui: Ui) -> int | None:
    names = await probe_names(ed, ui, "pathTester.length.")
    return int(names[0].rsplit(".", 1)[1]) if names else None


async def selection(ed: Editor, sid: int) -> list[int]:
    return [e["id"] for e in (await ed.call("get_selection", scene=sid)).get("entities", [])]


async def test_path_tester_flow(ed: Editor):
    s = await setup(ed)
    ui: Ui = s["ui"]
    sid = s["scene"]
    rect = await view_rect(ed, sid)
    ed.check((await ui.find("Copy Query", frames=4)) is None, "the Path Tester starts closed")

    await ed.call("run_command", id="navigation.pathTester")
    copy = await ui.find("Copy Query")
    ed.check(copy is not None, "navigation.pathTester opens the Path Tester's panel")

    # the camera looks straight down at the origin: the first floor is left of the view's centre
    start = view_point(ui, rect, 0.36, 0.5)
    end = view_point(ui, rect, 0.40, 0.42)
    await ui.click(start, modifier="shift")
    await ui.click(end)
    ed.check_eq(await tester_status(ed, ui), "complete", "Shift+click the start, click the end: a Complete path")
    length = await tester_length(ed, ui)
    ed.check(length is not None and length > 0, f"its length shows ({length})")
    await ed.screenshot("editor", path=evidence(ed) / "path_tester_complete.png")
    ed.check_eq(await selection(ed, sid), [], "the tester's clicks select nothing")

    # the end's handle drags: the length follows (toward the start)
    await ui.drag(end, view_point(ui, rect, 0.38, 0.47), steps=8)
    moved = await tester_length(ed, ui)
    ed.check(moved is not None and moved != length, f"dragging the end queries again ({length} -> {moved})")

    # an end on the other floor: Partial, the islands do not connect
    await ui.click(view_point(ui, rect, 0.64, 0.5))
    ed.check_eq(await tester_status(ed, ui), "partial", "an end on the other island")
    reasons = await probe_names(ed, ui, "pathTester.reason.")
    ed.check_eq(reasons, ["pathTester.reason.disconnected"], "the reason line")
    await ed.screenshot("editor", path=evidence(ed) / "path_tester_partial.png")

    # Locate: off the floors, why
    await ui.click_item("##testerMode")
    await ui.click_item("Locate")
    await ui.click(view_point(ui, rect, 0.5, 0.15))
    located = await probe_names(ed, ui, "pathTester.locate.")
    ed.check(located and located[0] in ("pathTester.locate.near_mesh", "pathTester.locate.not_found"), f"off the mesh: {located}")
    why = await probe_names(ed, ui, "pathTester.why.")
    ed.check(why, "the probe's reasons show beside the point")
    await ed.screenshot("editor", path=evidence(ed) / "path_tester_locate.png")
    await ui.click_item("##testerMode")
    await ui.click_item("Path")

    # Esc ends it
    await ui.move(view_point(ui, rect, 0.5, 0.5))
    await ui.key("Escape")
    ed.check((await ui.find("Copy Query", frames=4)) is None, "Esc closes the Path Tester")
    ed.check_eq(await selection(ed, sid), [], "nothing was selected on the way")


async def gizmos_menu(ed: Editor, ui: Ui, sid: int) -> None:
    await ui.click_item(f"##Gizmos{sid}")
    ed.check((await ui.find("##navcolorby")) is not None, "the Gizmos menu shows the AI Navigation rows")


async def set_color_by(ed: Editor, ui: Ui, sid: int, name: str) -> None:
    await gizmos_menu(ed, ui, sid)
    await ui.click_item("##navcolorby")
    await ui.click_item(name)
    await ui.key("Escape")


async def toggle_row(ed: Editor, ui: Ui, sid: int, label: str) -> None:
    await gizmos_menu(ed, ui, sid)
    await ui.click_item("##    " + label)
    await ui.key("Escape")


async def shot(ed: Editor, sid: int) -> bytes:
    await ui_frames(ed)
    return await ed.screenshot("scene", scene=sid, max_width=640)


async def ui_frames(ed: Editor) -> None:
    await S["ui"].frames(4)


def check_golden(ed: Editor, name: str, data: bytes) -> None:
    golden = GOLDEN_DIR / name
    if UPDATE:
        golden.parent.mkdir(parents=True, exist_ok=True)
        golden.write_bytes(data)
        print(f"     [golden] wrote {golden}")
        return
    ed.check(golden.exists(), f"{golden.name} is missing (JASPER_UPDATE_GOLDEN=1 records it)")
    ok, fraction, diff = pngdiff.compare(pngdiff.decode(golden.read_bytes()), pngdiff.decode(data), max_fraction=0.01)
    if not ok and ed.scratch:
        (ed.scratch.root / (golden.stem + "_diff.png")).write_bytes(diff)
        (ed.scratch.root / (golden.stem + "_actual.png")).write_bytes(data)
    ed.check(ok, f"the Scene view differs from {golden.name} in {fraction:.4f} of the pixels")


async def test_overlay_goldens(ed: Editor):
    s = await setup(ed)
    ui: Ui = s["ui"]
    sid = s["scene"]
    await ed.call("set_scene_camera", scene=sid, position=[0.0, 15.0, 13.0], target=[0.0, 0.0, 0.0])
    await ed.call("visualize_navigation", scene=sid, show={"navmesh": True, "tiles": False, "links": True})
    areas = pngdiff.decode(await shot(ed, sid))

    images = {}
    for name, golden in (("Islands", "navigation_islands.png"), ("Tiles", "navigation_tiles.png")):
        await set_color_by(ed, ui, sid, name)
        data = await shot(ed, sid)
        same, fraction, _ = pngdiff.compare(areas, pngdiff.decode(data), max_fraction=0.002)
        ed.check(not same, f"Color By {name} changes the image ({fraction:.4f} of the pixels differ)")
        images[golden] = data
    await set_color_by(ed, ui, sid, "Areas")
    back = pngdiff.decode(await shot(ed, sid))
    same, fraction, _ = pngdiff.compare(areas, back, max_fraction=0.002)
    ed.check(same, f"Color By Areas draws the area colours again ({fraction:.4f} differ)")

    await toggle_row(ed, ui, sid, "Show Steep Slopes")
    data = await shot(ed, sid)
    same, fraction, _ = pngdiff.compare(areas, pngdiff.decode(data), max_fraction=0.002)
    ed.check(not same, f"Show Steep Slopes tints the cube's sides ({fraction:.4f} of the pixels differ)")
    images["navigation_slope.png"] = data
    await toggle_row(ed, ui, sid, "Show Steep Slopes")

    # the tile labels are ImGui text over the view (not in the Scene view image): the evidence
    await toggle_row(ed, ui, sid, "Show Tile Labels")
    await ed.screenshot("editor", path=evidence(ed) / "navigation_tile_labels.png")
    await toggle_row(ed, ui, sid, "Show Tile Labels")
    for golden, data in images.items():
        check_golden(ed, golden, data)


async def test_window_last_bake_and_problems(ed: Editor):
    s = await setup(ed)
    ui: Ui = s["ui"]
    sid = s["scene"]
    await ed.call("run_command", id="window.navigation")
    await ui.click_item("Surfaces", window="Navigation")
    key = f"{sid}.{s['floor']}"
    last = await ui.find(f"##lastbake{key}", window="Navigation")
    ed.check(last is not None, "the Last Bake cell")
    ed.check(re.match(r"\d+ ms, \d+/\d+ tiles", last["label"]) is not None, f"Last Bake reads time and tiles: {last['label']!r}")
    walkable = await ui.find(f"##walkable{key}", window="Navigation")
    ed.check(walkable is not None, "the Walkable cell")
    area = float(walkable["label"].split(" ")[0])
    # two 10 x 10 m floors less the agent's margin and the cube, and the cube's top
    ed.check(120.0 < area < 170.0, f"Walkable {area} m^2")
    islands = await ui.find(f"##islands{key}", window="Navigation")
    # the floors and the cube's top
    ed.check(islands is not None and islands["label"].split("##")[0] == "3", f"three islands: {islands}")

    problem = await ui.find(f"##navproblem.agent_off_mesh.{s['walker']}", window="Navigation")
    ed.check(problem is not None, "the walker's agent_off_mesh row in Problems")
    header = await ui.find("###NavProblems", window="Navigation")
    ed.check(header is not None and header["label"].startswith("Problems ("), f"the Problems (N) header: {header}")
    await ed.screenshot("editor", path=evidence(ed) / "navigation_window.png")
    await ui.click(ui.centre(problem["rect"]))
    ed.check_eq(await selection(ed, sid), [s["walker"]], "a click on a problem selects its GameObject")
    await ed.call("run_command", id="window.navigation")


async def layout_labels(ed: Editor, sid: int, entity: int) -> dict[str, str]:
    layout = await ed.call("get_inspector_layout", scene=sid, entities=[entity])
    return {row["path"]: row["label"] for row in layout["rows"] if row.get("kind") == "Label"}


async def test_inspector_estimate_volume_matches_bake(ed: Editor):
    """A Volume surface's estimate is the box the bake collects: Volume Size in nav space, which the
    surface's scale does not stretch (the estimate used to multiply by it)."""
    ui = Ui(ed)
    made = await ed.call("create_scene", name="NavVolumeEstimate", type="3D", path="scenes/NavVolumeEstimate")
    sid = made["scene"]["id"]
    floor = (await ed.call("create_entity", scene=sid, type="plane", name="Floor", scale=[4, 1, 4]))["entity_id"]
    await ed.call("add_component", scene=sid, entity=floor, component="navmeshsurface",
                  values={"navmeshsurface": {"collectObjects": "volume", "volumeSize": [8, 4, 8]}})
    await ed.call("save_scene", scene=sid)
    baked = await ed.call("bake_navigation", scene=sid, mode="full")
    ed.check(len(baked["surfaces"]) == 1 and baked["surfaces"][0].get("ok"), f"bake: {baked}")
    tiles = baked["surfaces"][0]["tiles_total"]
    await ed.call("focus_window", window="inspector")
    await ed.call("select_entities", scene=sid, entities=[floor])
    await ui.frames(6)
    rows = await layout_labels(ed, sid, floor)
    found = re.match(r"Voxels \d+ × \d+, (\d+) tiles", rows.get("estimate", ""))
    ed.check(found is not None, f"the estimate: {rows}")
    estimated = int(found.group(1))
    # the same box on the same whole-tile grid (the scaled box counted 9 tiles, the unaligned one 1)
    ed.check_eq(estimated, tiles, "the estimate's tiles are the bake's")


async def test_inspector_summary_and_estimate(ed: Editor):
    s = await setup(ed)
    ui: Ui = s["ui"]
    sid = s["scene"]
    floor = s["floor"]
    await ed.call("focus_window", window="inspector")
    await ed.call("select_entities", scene=sid, entities=[floor])
    await ui.frames(6)
    rows = await layout_labels(ed, sid, floor)
    ed.check(rows.get("lastBake", "").startswith("Last bake: "), f"the last bake row: {rows}")
    ed.check(re.search(r"Walkable: [\d.]+ m², 3 islands", rows.get("walkable", "")) is not None, f"walkable and islands: {rows}")
    ed.check(re.match(r"Voxels \d+ × \d+, \d+ tiles \([\d.]+ voxels per agent radius\)", rows.get("estimate", "")) is not None,
             f"the estimate: {rows}")
    ed.check("estimateVoxels" not in rows, "the default voxel size has 3 voxels per radius")
    await ed.screenshot("editor", path=evidence(ed) / "inspector_surface.png")

    await ed.call("set_properties", scene=sid, entity=floor,
                  values={"navmeshsurface": {"overrideCellSize": True, "cellSize": 0.4}})
    await ui.frames(6)
    rows = {}
    for _ in range(10):
        rows = await layout_labels(ed, sid, floor)
        if "estimateVoxels" in rows:
            break
        await ui.frames(10)
    ed.check(rows.get("estimateVoxels", "").startswith("Fewer than 2 voxels per agent radius"), f"the coarse-voxel warning: {rows}")
    ed.check("1.2 voxels per agent radius" in rows.get("estimate", ""), f"0.5 / 0.4: {rows.get('estimate')}")
    await ed.screenshot("editor", path=evidence(ed) / "inspector_surface_coarse.png")
    await ed.call("undo", scene=sid)
