"""S99 AC2: the screenshot's frame checks (R6-R8). Each finding kind on a scratch scene made to
trigger it (an over-bright light, a black scene, a camera facing away, a selected object off-screen,
a tiny object, a material with a missing texture, two coplanar quads) and a clean scene with none;
where the fix is a call, it is made and the finding goes. The checks' cost is reported against the
10 ms budget (harness.over_budget). The z-fighting plane buckets themselves are a gtest
(FrameChecksZFighting.*). The Scene view's Frame Checks overlay (R9, the display flag frame_checks) is
turned on over two of the scenes and the editor shot to build/test-results/screenshot-findings/.
"""

from __future__ import annotations

import re
import shutil

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Screenshot Findings"}
TIMEOUT = 900

DISPLAY = {"show_grid_3d": False, "hide_light_icons": True, "hide_selection_outline": True,
           "hide_camera_view": True, "hide_sound_icons": True}
COSTS: dict[str, float] = {}


async def fresh(ed: Editor, name: str, eye=(0.0, 1.0, 6.0), target=(0.0, 0.0, 0.0)) -> dict:
    """A 3D scene (sun and sky) with a 1 m cube at the origin, a main camera and the Scene view both
    at eye looking at target, nothing selected"""
    sid = (await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}"))["scene"]["id"]
    cube = (await ed.call("create_entity", scene=sid, type="cube", name="Cube"))["entity_id"]
    camera = (await ed.call("create_entity", scene=sid, type="camera", name="Camera"))["entity_id"]
    await ed.call("set_main_camera", scene=sid, entity=camera)
    await ed.call("set_transform", scene=sid, entity=camera, position=list(eye))
    await ed.call("set_properties", scene=sid, entity=camera, values={"camera": {"useTarget": True, "target": list(target)}})
    await ed.call("select_entities", scene=sid, entities=[])
    await ed.call("set_scene_camera", scene=sid, position=list(eye), target=list(target), display=DISPLAY)
    return {"scene": sid, "cube": cube, "camera": camera}


async def shoot(ed: Editor, s: dict, view: str, label: str) -> dict:
    r = await ed.call("screenshot", view=view, scene=s["scene"], max_width=256)
    ed.check(isinstance(r.get("findings"), list), f"{label}: a {view} screenshot carries findings: {r}")
    ed.check(len(r["findings"]) <= 5, f"{label}: at most 5 findings: {r['findings']}")
    for finding in r["findings"]:
        ed.check({"kind", "severity", "message", "fix"} <= set(finding), f"{label}: a finding's fields: {finding}")
    COSTS[f"{label} {view}"] = r["findings_ms"]
    return r


async def overlay_shot(ed: Editor, s: dict, name: str) -> None:
    """R9: Frame Checks on in the scene's view (a view setting, saved per user), drawn over it; the
    whole editor shot to build/test-results/screenshot-findings/ as evidence"""
    await ed.call("set_scene_camera", scene=s["scene"], display={"frame_checks": True})
    workspace = (ed.project / ".jasperengine" / "user" / "workspace.yaml").read_text()
    ed.check("frameChecks: true" in workspace, "Frame Checks is saved with the user's workspace")
    await ed.call("focus_window", window="scene", scene=s["scene"])
    # its list refreshes twice a second
    await ed.call("wait", frames=90)
    relative = f"screenshots/{name}.png"
    r = await ed.call_raw("screenshot", {"view": "editor", "max_width": 1600, "save_path": relative})
    ed.check(not r.is_error and r.images, f"editor screenshot {name}: {r.text[:300]}")
    out = harness.REPO / "build" / "test-results" / "screenshot-findings"
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ed.project / relative, out / f"{name}.png")


def numbers(fix: str, key: str) -> list[float]:
    """The [x, y, z] after `key: ` in a finding's fix"""
    return [float(v) for v in re.search(key + r": \[([^\]]*)\]", fix).group(1).split(",")]


def kinds(r: dict) -> list[str]:
    return [f["kind"] for f in r["findings"]]


def finding(ed: Editor, r: dict, kind: str) -> dict:
    found = [f for f in r["findings"] if f["kind"] == kind]
    ed.check(found, f"a {kind} finding: {r['findings']}")
    # the first finding's fix is the next step
    ed.check(r["findings"][0]["fix"] in r.get("next", ""), f"next names the first fix: {r.get('next')}")
    return found[0]


async def test_clean_scene_has_no_findings(ed: Editor):
    s = await fresh(ed, "Clean")
    for view in ("scene", "game"):
        r = await shoot(ed, s, view, "clean")
        ed.check_eq(r["findings"], [], f"a lit cube in view: no {view} findings")
        ed.check("next" not in r, f"no next without findings: {r.get('next')}")


async def test_over_bright_light(ed: Editor):
    s = await fresh(ed, "Bright")
    # the cube fills the view, a strong point light just in front of it
    await ed.call("set_transform", scene=s["scene"], entity=s["cube"], scale=[8, 8, 1])
    lamp = (await ed.call("create_entity", scene=s["scene"], type="point_light", name="Lamp", position=[0, 0, 2.5]))["entity_id"]
    await ed.call("set_properties", scene=s["scene"], entity=lamp, values={"light": {"intensity": 200, "range": 30}})
    r = await shoot(ed, s, "game", "bright")
    clipped = finding(ed, r, "exposure_clipped")
    ed.check_eq(clipped["entities"], [lamp], f"it names the brightest light: {clipped}")
    ed.check("Lamp" in clipped["message"] and f"entity: {lamp}" in clipped["fix"] and "intensity" in clipped["fix"],
             f"the message names the light, the fix lowers it: {clipped}")
    ed.check("subject_out_of_frame" not in kinds(r), f"a cube filling the view is not out of frame: {r['findings']}")


async def test_black_scene(ed: Editor):
    s = await fresh(ed, "Black")
    for component in ("light", "skybox"):
        found = await ed.call("find_entities", scene=s["scene"], component=component)
        if found["entities"]:
            await ed.call("delete_entities", scene=s["scene"], entities=[e["id"] for e in found["entities"]])
    await ed.call("set_scene_settings", scene=s["scene"], settings={"background_color": [0, 0, 0, 1], "light_state": "on",
                                                                     "global_illumination_intensity": 0})
    r = await shoot(ed, s, "game", "black")
    dark = finding(ed, r, "exposure_dark")
    ed.check("directional_light" in dark["fix"], f"no light: the fix adds one: {dark}")
    ed.check("empty_frame" not in kinds(r), f"the cube is in view: {r['findings']}")


async def test_camera_facing_away(ed: Editor):
    s = await fresh(ed, "Away", target=(0.0, 1.0, 20.0))
    r = await shoot(ed, s, "game", "away")
    empty = finding(ed, r, "empty_frame")
    ed.check(f"set_transform {{entity: {s['camera']}" in empty["fix"] and "target" in empty["fix"],
             f"the fix moves and aims the game camera: {empty}")
    # the fix's own values frame the cube again
    await ed.call("set_transform", scene=s["scene"], entity=s["camera"], position=numbers(empty["fix"], "position"), space="world")
    await ed.call("set_properties", scene=s["scene"], entity=s["camera"], values={"camera": {"target": numbers(empty["fix"], "target")}})
    r = await shoot(ed, s, "game", "away fixed")
    ed.check("empty_frame" not in kinds(r), f"aimed at the cube by the fix: {r['findings']}")


async def test_selected_object_off_screen(ed: Editor):
    s = await fresh(ed, "OffScreen")
    far = (await ed.call("create_entity", scene=s["scene"], type="cube", name="Far", position=[30, 0, 0]))["entity_id"]
    await ed.call("select_entities", scene=s["scene"], entities=[far])
    r = await shoot(ed, s, "scene", "off-screen")
    out = finding(ed, r, "subject_out_of_frame")
    ed.check_eq(out["entities"], [far])
    ed.check_eq(out["fix"], f"frame_entity {{entity: {far}}}")
    game = await shoot(ed, s, "game", "off-screen")
    ed.check(f"entity: {s['camera']}" in finding(ed, game, "subject_out_of_frame")["fix"], f"the Game view's fix moves its camera: {game}")
    await overlay_shot(ed, s, "frame-checks-off-screen")
    await ed.call("frame_entity", scene=s["scene"], entity=far)
    await ed.call("wait", frames=30)
    r = await shoot(ed, s, "scene", "off-screen framed")
    ed.check("subject_out_of_frame" not in kinds(r), f"framed: {r['findings']}")


async def test_tiny_object(ed: Editor):
    s = await fresh(ed, "Tiny")
    await ed.call("set_transform", scene=s["scene"], entity=s["cube"], scale=[0.1, 0.1, 0.1])
    await ed.call("select_entities", scene=s["scene"], entities=[s["cube"]])
    r = await shoot(ed, s, "scene", "tiny")
    small = finding(ed, r, "subject_too_small")
    ed.check_eq(small["entities"], [s["cube"]])
    # frame_entity frames a metre at least: the fix brings the Scene view camera close by hand
    ed.check(small["fix"].startswith("set_scene_camera {position: "), f"{small}")
    await ed.call("set_scene_camera", scene=s["scene"], position=numbers(small["fix"], "position"), target=numbers(small["fix"], "target"))
    r = await shoot(ed, s, "scene", "tiny framed")
    ed.check("subject_too_small" not in kinds(r), f"framed: {r['findings']}")


async def test_material_with_a_missing_texture(ed: Editor):
    s = await fresh(ed, "Missing")
    texture = "assets/textures/brick.png"
    await ed.call("generate_placeholder_art", path=texture, width=16, height=16, shape="rect", color=[0.8, 0.3, 0.2, 1])
    material = "assets/materials/Brick.material"
    await ed.call("create_material", path=material, values={"baseColorTexture": texture})
    await ed.call("link_material", scene=s["scene"], entity=s["cube"], material=material)
    await ed.call("save_scene", scene=s["scene"])
    r = await shoot(ed, s, "scene", "texture present")
    ed.check("texture_missing" not in kinds(r), f"the texture is there: {r['findings']}")
    await ed.call("delete_asset", paths=[texture])
    r = await shoot(ed, s, "scene", "texture missing")
    missing = finding(ed, r, "texture_missing")
    ed.check_eq(missing["entities"], [s["cube"]])
    ed.check("Brick.material" in missing["message"] and "brick.png" in missing["message"], f"{missing}")
    ed.check("retarget_asset_references" in missing["fix"], f"{missing}")


async def test_coplanar_quads(ed: Editor):
    s = await fresh(ed, "Coplanar", eye=(0.0, 8.0, 4.0))
    floor = (await ed.call("create_entity", scene=s["scene"], type="plane", name="Floor", position=[0, 0, 0]))["entity_id"]
    decal = (await ed.call("create_entity", scene=s["scene"], type="plane", name="Decal", position=[0.3, 0, 0.2], scale=[0.3, 1, 0.3]))["entity_id"]
    await ed.call("delete_entities", scene=s["scene"], entities=[s["cube"]])
    r = await shoot(ed, s, "scene", "coplanar")
    fight = finding(ed, r, "z_fighting")
    ed.check_eq(sorted(fight["entities"]), sorted([floor, decal]))
    ed.check(fight["fix"].startswith(f"set_transform {{entity: {decal},"), f"the smaller one moves: {fight}")
    await overlay_shot(ed, s, "frame-checks-z-fighting")
    await ed.call("set_transform", scene=s["scene"], entity=decal, position=numbers(fight["fix"], "position"), space="world")
    r = await shoot(ed, s, "scene", "coplanar fixed")
    ed.check("z_fighting" not in kinds(r), f"5 mm apart: {r['findings']}")


async def test_2d_scene_draws_in_order(ed: Editor):
    """A 2D scene draws in its sorting order (no depth test): two quads on one plane never z-fight there,
    and objects larger than the view are no out-of-frame subject (the fixture project's 2D main scene)"""
    sid = (await ed.call("create_scene", name="Flat", type="2D", path="scenes/Flat"))["scene"]["id"]
    # two backgrounds on one plane, each wider than the view (none fits the frame)
    for name in ("Back A", "Back B"):
        await ed.call("create_entity", scene=sid, type="plane", name=name, position=[0, 0, -8], scale=[5000, 1, 5000],
                      rotation={"euler": [90, 0, 0]})
    await ed.call("select_entities", scene=sid, entities=[])
    await ed.call("set_scene_camera", scene=sid, center=[0, 0], zoom=4)
    r = await shoot(ed, {"scene": sid}, "scene", "2d")
    ed.check("z_fighting" not in kinds(r) and "subject_out_of_frame" not in kinds(r), f"{r['findings']}")


async def test_frame_checks_cost(ed: Editor):
    """R8: the checks of every screenshot above against the 10 ms budget"""
    ed.check(COSTS, "the screenshots above ran")
    over = harness.over_budget("screenshot frame checks", COSTS, 10.0, unit="ms")
    ed.check(not over, f"frame checks over 10 ms: {over}")
