"""Rendering performance fixes of the upstream catch-up, in a new project's 3D scene:

- the directional light's cascades leave out the casters that cannot shadow what the camera sees,
  but keep a caster outside the view whose shadow falls into it (a slab above the view);
- a reflection probe's runtime capture is prefiltered into roughness mips (the Probe Filter
  variant is in the build's shaders, the picture with the probe, SSR and shadows has no black or
  NaN-coloured pixels and the console no errors);
- the variants these change (probefilter, ssr, Lit with shadows and IBL) compile for every
  graphics API.

Screenshots are kept in the scratch directory (shots/).

The editor's cost with a large scene (S98 REQ-30, REQ-31, REQ-41, AC24; ledger E11, E12/E42).

The fixture is generated: 1,500 GameObjects (100 point lights, 100 rigidbodies, 50 cameras, 50
particle systems, the rest cubes) under 12 groups, the Hierarchy fully expanded, nothing selected.
- Scene-view gizmos share their renderers per kind, so the scene takes no "No free system entity
  slot" error (the system range is 1..1000) when it is made or opened.
- The editor-side CPU scopes of a frame (the panels, the Scene view's gizmo update) stay within
  8 ms; in Play with the Game view maximised, the Hierarchy and the Scene view within 1 ms. Both
  are budgets: reported through harness.over_budget, enforced in the benchmark run.
- A light's icon is still picked where it is drawn.
"""

from __future__ import annotations

import sys
from pathlib import Path

from harness import Editor, over_budget

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import pngdiff  # noqa: E402

EDITOR = {"create": True}
CI = True
# the editor-cost tests build a 1,500 GameObject scene
TIMEOUT = 1500

S: dict = {}
M = "submeshes[0].material."


def shots(ed: Editor) -> Path:
    return (ed.scratch.root if ed.scratch else Path("/tmp/render-perf")) / "shots"


def stats(png: bytes):
    """the share of black and of saturated-magenta (NaN-like) pixels"""
    width, height, rgba = pngdiff.decode(png)
    total, black, magenta = 0, 0, 0
    for y in range(0, height, 2):
        row = y * width * 4
        for x in range(0, width, 2):
            r, g, b = rgba[row + x * 4], rgba[row + x * 4 + 1], rgba[row + x * 4 + 2]
            total += 1
            if r < 4 and g < 4 and b < 4:
                black += 1
            if r > 250 and g < 5 and b > 250:
                magenta += 1
    return black / total, magenta / total


def mean_luma(png: bytes, x0: float, y0: float, x1: float, y1: float) -> float:
    """mean luma of a region given in fractions of the picture"""
    width, height, rgba = pngdiff.decode(png)
    total, count = 0.0, 0
    for y in range(int(height * y0), int(height * y1)):
        row = y * width * 4
        for x in range(int(width * x0), int(width * x1)):
            r, g, b = rgba[row + x * 4], rgba[row + x * 4 + 1], rgba[row + x * 4 + 2]
            total += 0.299 * r + 0.587 * g + 0.114 * b
            count += 1
    return total / max(count, 1)


async def errors(ed: Editor):
    return [e for e in await ed.console_since(None) if e.get("type") == "error"]


async def prepare_scene(ed: Editor):
    if S.get("editor") is ed:
        return
    S.clear()
    sid = (await ed.call("create_scene", name="RenderPerf", type="3D", path="scenes/RenderPerf"))["scene"]["id"]
    S["scene"] = sid
    await ed.call("create_entity", scene=sid, type="plane", name="Ground", scale=[30, 1, 30],
                  values={"mesh": {M + "baseColorFactor": [0.7, 0.7, 0.7, 1], M + "roughnessFactor": 0.3,
                                   "receiveIBL": True}})
    # a smooth and a rough metal sphere reflecting the probe, a red box between them
    await ed.call("create_entity", scene=sid, type="sphere", name="Smooth", position=[-1.6, 1, 0],
                  values={"mesh": {M + "metallicFactor": 1.0, M + "roughnessFactor": 0.05, "receiveIBL": True}})
    await ed.call("create_entity", scene=sid, type="sphere", name="Rough", position=[1.6, 1, 0],
                  values={"mesh": {M + "metallicFactor": 1.0, M + "roughnessFactor": 0.7, "receiveIBL": True}})
    await ed.call("create_entity", scene=sid, type="cube", name="Box", position=[0, 0.5, -2.5],
                  values={"mesh": {M + "baseColorFactor": [0.9, 0.15, 0.1, 1]}})
    # the camera looks down at the ground around the origin; the slab is high above its view
    await ed.call("create_entity", scene=sid, type="camera", name="Main Camera", position=[0, 3, 8],
                  rotation={"euler": [-15, 0, 0]})
    await ed.call("set_main_camera", scene=sid, entity="Main Camera")
    await ed.call("create_entity", scene=sid, type="cube", name="Slab", position=[0, 14, 1.5], scale=[3, 0.4, 3])
    # the new scene's sun (shadows on) travels down, slightly sideways: the slab's shadow falls on
    # the ground in view
    await ed.call("set_properties", scene=sid, entity="Directional Light",
                  values={"light": {"direction": [0.1, -1.0, 0.05], "shadows": True}})
    await ed.call("create_entity", scene=sid, type="reflection_probe", name="Probe", position=[0, 1, 0],
                  values={"reflectionprobe": {"boxSize": [30, 10, 30]}})
    await ed.call("set_scene_settings", scene=sid, settings={"shadows_quality": "high"})
    await ed.call("configure_game_view", aspect="16:9")
    await ed.call("wait", frames=30)
    S["base"] = await ed.screenshot(view="game", path=shots(ed) / "base.png", max_width=960)
    ed.check(not await errors(ed), "no console errors building the scene")
    S["editor"] = ed


async def test_offview_caster_still_shadows_the_view(ed: Editor):
    """The slab is outside the view; its shadow is in it, so the cascades keep it"""
    await prepare_scene(ed)
    sid = S["scene"]
    lit = S["base"]
    await ed.call("set_properties", scene=sid, entity="Slab", values={"mesh": {"castShadows": False}})
    await ed.call("wait", frames=10)
    unshadowed = await ed.screenshot(view="game", path=shots(ed) / "slab-no-shadow.png", max_width=960)
    # the slab itself is not in the picture: moved away (casting nothing) it looks the same
    await ed.call("set_transform", scene=sid, entity="Slab", position=[0, 60, 1.5])
    await ed.call("wait", frames=10)
    away = await ed.screenshot(view="game", path=shots(ed) / "slab-away.png", max_width=960)
    ok, fraction, _ = pngdiff.compare(pngdiff.decode(unshadowed), pngdiff.decode(away), channel_tolerance=8, max_fraction=0.002)
    ed.check(ok, f"the slab is out of the camera's view ({fraction:.4f} of the pixels change when it moves)")
    await ed.call("set_transform", scene=sid, entity="Slab", position=[0, 14, 1.5])
    await ed.call("set_properties", scene=sid, entity="Slab", values={"mesh": {"castShadows": True}})
    await ed.call("wait", frames=10)

    # its shadow: the ground under it is darker with it casting
    ok, fraction, _ = pngdiff.compare(pngdiff.decode(lit), pngdiff.decode(unshadowed), channel_tolerance=24, max_fraction=0.01)
    ed.check(not ok and fraction > 0.004, f"the slab's shadow is in the picture ({fraction:.4f} of the pixels)")
    # where it falls: the lower right of the picture (the probe and sky keep it a soft one)
    with_shadow = mean_luma(lit, 0.52, 0.7, 0.86, 0.94)
    without = mean_luma(unshadowed, 0.52, 0.7, 0.86, 0.94)
    ed.check(with_shadow < without - 3, f"the ground in front is darker in the slab's shadow ({with_shadow:.1f} vs {without:.1f})")


async def test_probe_ssr_and_shadows_render(ed: Editor):
    await prepare_scene(ed)
    sid = S["scene"]
    names = {s["name"] for s in (await ed.call("get_build_info", include_shaders=True))["shaders"]}
    ed.check("Probe Filter" in names, f"the probe filter ships with a probe: {sorted(names)}")

    await ed.call("set_scene_settings", scene=sid, settings={"ssr_enabled": True})
    await ed.call("wait", frames=30)
    ssr = await ed.screenshot(view="game", path=shots(ed) / "ssr.png", max_width=960)
    for tag, png in (("probe and shadows", S["base"]), ("with SSR", ssr)):
        black, magenta = stats(png)
        ed.check(black < 0.02, f"{tag}: no black areas ({black:.4f})")
        ed.check(magenta == 0, f"{tag}: no NaN-coloured pixels")
    # the probe reflects the scene: the smooth sphere is not the empty black cube
    ed.check(mean_luma(S["base"], 0.25, 0.35, 0.42, 0.7) > 20, "the smooth sphere reflects the probe")
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_enabled": False})
    ed.check(not await errors(ed), f"console errors: {(await errors(ed))[:5]!r}")


async def test_variants_compile_for_every_api(ed: Editor):
    out = ed.scratch.root / "render-perf-shaders"
    specs = ["probefilter", "ssr", "mesh:Uv1,Puc,Shw,Nor", "mesh:Uv1,Puc,Shw,Nor,Ibl"]
    r = await ed.call("generate_shaders", output_dir=str(out), backends="all", format="binary",
                      shaders=specs, timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"generate_shaders: {r}")
    files = {p.name for p in out.rglob("*") if p.is_file()}
    for stem in ("probefilter", "ssr", "mesh_Uv1PucShwNor", "mesh_Uv1PucShwNorIbl"):
        for language in ("glsl410", "glsl300es", "hlsl5", "msl21macos", "msl21ios", "spirv10"):
            ed.check(f"{stem}_{language}.sdat" in files, f"{stem}_{language}.sdat not generated: {sorted(files)}")


# ---- the editor's cost with a large scene (S98 REQ-30, REQ-31, REQ-41) ----

COST_GROUPS = 12
COST_KINDS = [("point_light", 100), ("rigidbody", 100), ("camera", 50), ("particle_system", 50)]
COST_TOTAL = 1500
COST_SLOT_ERROR = "No free system entity slot"

COST: dict = {}


def fixture_calls(scene: int, groups: list[int]) -> list[dict]:
    """create_entity calls for every GameObject but the groups, spread over the groups: away from
    the origin (x >= 20), so the light the pick test aims at has the view's centre to itself."""
    kinds: list[str] = []
    for kind, count in COST_KINDS:
        kinds += [kind] * count
    kinds += ["cube"] * (COST_TOTAL - COST_GROUPS - len(kinds) - 1)   # (one more light is the picked one)
    calls = []
    for i, kind in enumerate(kinds):
        position = [20.0 + (i % 40) * 3.0, 0.0, -60.0 + (i // 40) * 3.0]
        calls.append({"tool": "create_entity", "arguments": {
            "scene": scene, "type": kind, "name": f"{kind}_{i}", "parent": groups[i % len(groups)],
            "position": position, "select": False}})
    return calls


async def cost_scene(ed: Editor) -> int:
    if "scene" in COST:
        return COST["scene"]
    made = await ed.call("create_scene", name="EditorCost", type="3D", path="scenes/EditorCost")
    scene = made["scene"]["id"]
    groups = []
    for g in range(COST_GROUPS):
        groups.append((await ed.call("create_entity", scene=scene, type="cube", name=f"Group {g}", select=False))["entity_id"])
    calls = fixture_calls(scene, groups)
    for at in range(0, len(calls), 200):
        batch = await ed.call("batch", calls=calls[at:at + 200], timeout=600)
        ed.check(batch["failed"] == 0, f"the fixture's GameObjects were made: {batch.get('results', [])[:2]}")
    COST["light"] = (await ed.call("create_entity", scene=scene, type="point_light", name="PickLight",
                                position=[0, 0, 0], select=False))["entity_id"]
    found = await ed.call("find_entities", scene=scene, limit=100000)
    ed.check(len(found["entities"]) >= COST_TOTAL, f"the scene holds {COST_TOTAL} GameObjects: {len(found['entities'])}")
    await ed.call("save_scene", scene=scene)
    await ed.call("select_entities", scene=scene, entities=[])
    await ed.call("focus_window", window="hierarchy")
    await ed.call("set_scene_camera", scene=scene, position=[0, 0, 12], target=[0, 0, 0])
    await ed.call("wait", frames=10)
    COST["scene"] = scene
    return scene


async def slot_errors(ed: Editor) -> list[str]:
    console = await ed.call("read_console", contains=COST_SLOT_ERROR, limit=1000)
    return [e.get("message", "") for e in console["entries"]]


async def scope_ms(ed: Editor, capture: int, name: str) -> float:
    """total ms of the scopes named `name` in the capture's median frame (0 when it did not run)"""
    flat = await ed.call("get_cpu_profile", capture_id=capture, view="flat", frame="median", filter=name, max_rows=50)
    return sum(row["total_ms"] for row in flat.get("rows", []) if row["name"] == name)


async def test_large_scene_takes_no_system_slots(ed: Editor):
    """AC24 (a): making and reopening the fixture logs no system slot error."""
    scene = await cost_scene(ed)
    errors = await slot_errors(ed)
    ed.check(not errors, f"the gizmos of {COST_TOTAL} GameObjects took no system entity slot each: {len(errors)} errors, {errors[:1]}")
    await ed.call("close_scene", scene=scene, unsaved="discard")
    S.pop("scene")
    reopened = (await ed.call("open_scene", scene="scenes/EditorCost.scene"))["scene"]["id"]
    await ed.call("focus_window", window="hierarchy")
    await ed.call("set_scene_camera", scene=reopened, position=[0, 0, 12], target=[0, 0, 0])
    await ed.call("wait", frames=10)
    COST["scene"] = reopened
    COST["light"] = (await ed.call("find_entities", scene=reopened, name="PickLight"))["entities"][0]["id"]
    errors = await slot_errors(ed)
    ed.check(not errors, f"opening the {COST_TOTAL}-GameObject scene took no system entity slot per gizmo: {errors[:1]}")


async def test_light_icon_is_picked(ed: Editor):
    """AC24 (c): the light at the view's centre is what a click there selects."""
    scene = await cost_scene(ed)
    picked = await ed.call("pick_at", scene=scene, u=0.5, v=0.5)
    ed.check_eq((picked.get("would_select") or {}).get("entity"), COST["light"], f"a click on the light's icon selects the light: {picked}")


async def test_editor_frame_budget(ed: Editor):
    """AC24 (b), REQ-41: the editor-side scopes of a frame with the fixture open, nothing selected."""
    await cost_scene(ed)
    await ed.call("focus_window", window="hierarchy")
    await ed.call("wait", frames=5)
    capture = (await ed.call("capture_profile", frames=60, gpu=False))["capture_id"]
    measured = {name: await scope_ms(ed, capture, name)
                for name in ("Editor UI", "Scene View Gizmos", "Hierarchy", "Hierarchy Rows", "Hierarchy Scene Sync",
                             "Hierarchy Search Order", "Hierarchy Popups", "Hierarchy Tree Rebuild", "Inspector", "Scene Window")}
    ed.check(measured["Hierarchy"] > 0 and measured["Editor UI"] > 0, f"the panels' scopes were recorded: {measured}")
    for name, ms in measured.items():
        print(f"     [bench] {name}: {ms:.3f} ms (median frame of 60, {COST_TOTAL} GameObjects)")
    over = over_budget("editor-side frame scopes (Editor UI + Scene View Gizmos)",
                       {"editor": measured["Editor UI"] + measured["Scene View Gizmos"]}, 8.0, "ms")
    ed.check(not over, f"REQ-41: the editor's own scopes took {over} ms a frame (8 ms)")


async def test_hierarchy_rebuild_budget(ed: Editor):
    """The Hierarchy's timed tree rebuild (at most every 0.5 s and 15 frames; it refreshes the rows
    in place while the structure stays) with the fixture: the median of those in 60 frames, at most
    1 ms (a budget). No full rebuild runs while nothing changes the structure."""
    await cost_scene(ed)
    await ed.call("focus_window", window="hierarchy")
    await ed.call("wait", frames=5)
    # 60 frames: capture_profile allows 250 ms a frame (30 s at least), and this scene draws at about
    # 4 fps on a software renderer, where 120 frames ran out of time; 60 hold four timed rebuilds or more
    capture = await ed.call("capture_profile", frames=60, gpu=False)
    rebuilds: list[float] = []
    full = 0
    first = capture["first_frame"]
    for index in range(first, first + capture["frames"]):
        flat = await ed.call("get_cpu_profile", capture_id=capture["capture_id"], view="flat", frame=index,
                             filter="Hierarchy Tree Rebuild")
        ms = sum(row["total_ms"] for row in flat.get("rows", []) if row["name"] == "Hierarchy Tree Rebuild")
        if ms > 0.0:
            rebuilds.append(ms)
            deep = await ed.call("get_cpu_profile", capture_id=capture["capture_id"], view="flat", frame=index,
                                 filter="Hierarchy Tree Full Rebuild")
            full += sum(1 for row in deep.get("rows", []) if row["name"] == "Hierarchy Tree Full Rebuild")
    ed.check(rebuilds, "the Hierarchy rebuilt its tree within 60 frames (the timed rebuild)")
    ed.check_eq(full, 0, "the timed rebuilds refreshed the rows in place (no full rebuild without a structural change)")
    rebuilds.sort()
    median = rebuilds[len(rebuilds) // 2]
    print(f"     [bench] Hierarchy tree rebuild: median {median:.3f} ms of {len(rebuilds)} rebuilds, {COST_TOTAL} GameObjects")
    over = over_budget("Hierarchy tree rebuild", {"rebuild": median}, 1.0, "ms")
    ed.check(not over, f"the Hierarchy's tree rebuild took {over} ms with {COST_TOTAL} GameObjects (1 ms)")


async def test_play_maximized_budget(ed: Editor):
    """REQ-41: in Play with the Game view maximised, the Hierarchy and the Scene view stay out of
    the frame (at most 1 ms together): the Scene view updates no gizmo while it is not drawn."""
    scene = await cost_scene(ed)
    await ed.call("configure_game_view", maximize_on_play=True)
    await ed.call("play", action="start", scene=scene, fixed_fps=30, auto_pause=False)
    try:
        await ed.call("wait", frames=10)
        capture = (await ed.call("capture_profile", frames=60, gpu=False))["capture_id"]
        measured = {name: await scope_ms(ed, capture, name) for name in ("Hierarchy", "Scene View Gizmos", "Scene Window")}
        for name, ms in measured.items():
            print(f"     [bench] Play maximised, {name}: {ms:.3f} ms (median frame of 60)")
        ed.check(measured["Scene View Gizmos"] == 0.0,
                 f"the hidden Scene view updated its gizmos in Play with the Game view maximised: {measured}")
        over = over_budget("Play maximised: Hierarchy + Scene view", {"hierarchy_scene": sum(measured.values())}, 1.0, "ms")
        ed.check(not over, f"REQ-41: the Hierarchy and the Scene view took {over} ms a frame in maximised Play (1 ms)")
    finally:
        await ed.call("play", action="stop")
        await ed.call("configure_game_view", maximize_on_play=False)
