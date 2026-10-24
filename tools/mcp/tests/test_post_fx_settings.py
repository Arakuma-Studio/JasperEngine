"""S13 post FX through MCP (docs/specs/S13-post-fx.md AC16, AC18): every new post_processing field
round-trips through set_scene_settings / get_scene_settings and the scene file (enums by their
lower-case names), the film / vhs / mobile_adaptive presets apply as one named undo step,
inspect_color_lut reports the OpenEngine .cube fixtures and their errors, a .cube is a colour LUT
asset (lut_texture takes it, get_post_processing_status reads its stage), and every scene of
tools/fixtures re-saves byte-identically (no S13 key is written while its fields are default)."""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Post FX Settings"}
TIMEOUT = 1800

SCENE = "scenes/PostFx.scene"
CUBE_FIXTURES = harness.REPO / "engine" / "tests" / "core" / "fixtures" / "cubelut"

# every S13 field with a value away from its default (exact in binary floating point)
S13_VALUES = {
    "film_grain_type": "lens", "film_grain_size": 2.5, "film_grain_colored": True, "film_frame_rate": 12.0,
    "vhs_enabled": True, "vhs_intensity": 0.5, "vhs_wobble": 1.5, "vhs_tracking": 0.25, "vhs_color_bleed": 2.0,
    "vhs_tape_noise": 0.125, "vhs_chroma_streaks": 0.75, "vhs_dropouts": 1.0, "vhs_scanlines": 0.25,
    "vhs_interference": 0.5, "vhs_glitches": 0.25, "vhs_speed": 2.0,
    "film_damage_enabled": True, "film_damage_intensity": 2.0, "film_hair_amount": 3.0, "film_scratch_amount": 4.0,
    "film_dust_amount": 5.0, "film_dust_size": 6.0, "film_gate_weave": 0.5, "film_gate_mask": "anamorphic_239",
    "film_gate_mask_feather": 10.0,
    "halation_enabled": True, "halation_intensity": 1.25, "halation_threshold": 0.5, "halation_radius": 0.75,
    "halation_tint": [1.0, 0.5, 0.25],
    "lut_input_encoding": "acescct",
    "dynamic_resolution_enabled": True, "dynamic_resolution_min_scale": 0.75, "dynamic_resolution_target_fps": 30,
}
# the YAML keys the scene file gets for some of them
SAVED_KEYS = ("vhsEnabled: true", "filmGrainType: lens", "filmGateMask: anamorphic_239", "lutInputEncoding: acescct",
              "halationTint: [1, 0.5, 0.25]", "dynamicResolutionTargetFps: 30")


def cube(title: str, size: int, look=lambda r, g, b: (r, g, b)) -> str:
    rows = [f'TITLE "{title}"', f"LUT_3D_SIZE {size}"]
    for b in range(size):
        for g in range(size):
            for r in range(size):
                rows.append("%.6f %.6f %.6f" % look(r / (size - 1), g / (size - 1), b / (size - 1)))
    return "\n".join(rows) + "\n"


async def post(ed: Editor, sid: int) -> dict:
    return (await ed.call("get_scene_settings", scene=sid, names=["post_processing"]))["settings"]["post_processing"]


async def test_fields_round_trip(ed: Editor):
    ed.require_tools("create_scene", "get_scene_settings", "set_scene_settings", "save_scene", "undo")
    sid = (await ed.call("create_scene", name="PostFx", type="3d"))["scene"]["id"]
    detail = await ed.call("get_scene_settings", scene=sid, names=["post_processing.film_grain_type",
                                                                    "post_processing.film_gate_mask",
                                                                    "post_processing.lut_input_encoding"], detail=True)
    ed.check_eq(detail["schema"]["post_processing.film_grain_type"]["options"], ["classic", "lens", "filmic"])
    ed.check_eq(detail["schema"]["post_processing.film_gate_mask"]["options"],
                ["none", "rounded", "academy_137", "widescreen_185", "anamorphic_239"])
    ed.check_eq(detail["schema"]["post_processing.lut_input_encoding"]["options"],
                ["auto", "srgb", "linear", "logc3", "acescct", "cineon", "davinci_intermediate"])

    # one field at a time (each its own undo step), read back exactly
    for name, value in S13_VALUES.items():
        out = await ed.call("set_scene_settings", scene=sid, settings={f"post_processing.{name}": value})
        ed.check_eq(out["settings"][f"post_processing.{name}"], value, name)
    ed.check_eq(await post(ed, sid), S13_VALUES, "every S13 field away from its default")

    await ed.call("save_scene", scene=sid, path=SCENE)
    text = (ed.project / SCENE).read_text()
    for key in SAVED_KEYS:
        ed.check(key in text, f"{key} saved: {text[text.find('postProcessing'):][:600]}")

    # undo walks back one field per step
    await ed.call("undo")
    ed.check("dynamic_resolution_target_fps" not in await post(ed, sid), "undo: the last field only")

    # out of range and unknown names change nothing
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing.vhs_wobble": 3.0}, contains="2")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing.film_grain_type": "fidelity"}, contains="filmic")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing.dynamic_resolution_min_scale": 0.25},
                         contains="0.5")

    # reset: no S13 key in the file
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": None})
    await ed.call("save_scene", scene=sid)
    ed.check("postProcessing" not in (ed.project / SCENE).read_text(), "defaults: nothing saved")


async def test_presets_one_named_undo_step(ed: Editor):
    ed.require_tools("list_post_processing_presets", "apply_post_processing_preset", "get_undo_history")
    sid = (await ed.call("create_scene", name="PostFxPresets", type="3d"))["scene"]["id"]
    listed = await ed.call("list_post_processing_presets", scene=sid)
    by_id = {p["id"]: p for p in listed["presets"]}
    for preset in ("film", "vhs", "mobile_adaptive"):
        ed.check(preset in by_id, f"preset {preset} listed: {sorted(by_id)}")
    ed.check_eq(by_id["film"]["settings"].get("film_grain_type"), "lens")
    ed.check(by_id["film"]["settings"].get("halation_enabled") is True, "film: halation")
    ed.check(by_id["vhs"]["settings"].get("vhs_enabled") is True, "vhs: the tape")
    adaptive = dict(by_id["mobile"]["settings"], dynamic_resolution_enabled=True)
    ed.check_eq(by_id["mobile_adaptive"]["settings"], adaptive, "mobile_adaptive = mobile + dynamic resolution (min 0.6 is the default)")
    ed.check_eq(by_id["film"]["cost"]["passes"], 3, "film: halation's two small passes and the uber pass")
    ed.check_eq(by_id["vhs"]["cost"]["passes"], 1, "vhs: the uber pass alone")

    for preset, name in (("film", "Film"), ("vhs", "VHS Tape"), ("mobile_adaptive", "Mobile Adaptive")):
        await ed.call("apply_post_processing_preset", scene=sid, preset=preset)
        ed.check_eq((await ed.call("list_post_processing_presets", scene=sid))["current"]["preset"], preset)
        names = [entry.get("name", "") for entry in (await ed.call("get_undo_history"))["entries"]]
        ed.check(names and f"Apply {name} Preset" in names[0], f"the undo step is named for {name}: {names[:3]}")
    await ed.call("undo")
    ed.check_eq((await ed.call("list_post_processing_presets", scene=sid))["current"]["preset"], "vhs", "one undo: the previous preset")
    await ed.call("undo")
    ed.check_eq((await ed.call("list_post_processing_presets", scene=sid))["current"]["preset"], "film")


async def test_inspect_color_lut(ed: Editor):
    ed.require_tools("inspect_color_lut", "set_project_settings", "get_post_processing_status")
    (ed.project / "assets" / "luts").mkdir(parents=True, exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets"})
    for path in CUBE_FIXTURES.glob("*.cube"):
        shutil.copy(path, ed.project / "assets" / "luts" / path.name)

    # the OpenEngine fixtures: title, sizes, range, the proposal and its stage
    expected = {
        "acescct_look.cube": ("acescct", "scene"),
        "arri_logc_look.cube": ("logc3", "scene"),
        "cineon_print_to_rec709.cube": ("cineon", "scene"),
        "davinci_intermediate_grade.cube": ("davinci_intermediate", "scene"),
        "display_linear_bake.cube": ("linear", "display"),
        "hdr_range.cube": ("linear", "scene"),
        "unnamed_display.cube": ("srgb", "display"),
    }
    for name, (encoding, stage) in expected.items():
        out = await ed.call("inspect_color_lut", path=f"assets/luts/{name}")
        ed.check(out["ok"] is True and out["error"] is None, f"{name}: {out}")
        ed.check(out["size_3d"] > 0 or out["size_1d"] > 0, f"{name}: a table")
        ed.check_eq((out["proposed_encoding"], out["stage"]), (encoding, stage), f"{name}: {out.get('reason')}")
        ed.check(isinstance(out["input_range"], list) and len(out["input_range"]) == 2, f"{name}: input range")

    # malformed: one line with the line number
    bad = ed.project / "assets" / "luts" / "bad.cube"
    bad.write_text("LUT_3D_SIZE 130\n")
    out = await ed.call("inspect_color_lut", path="assets/luts/bad.cube")
    ed.check(out["ok"] is False and "line 1" in out["error"] and "129" in out["error"], f"3D size 130 refused: {out}")
    bad.write_text("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n")
    out = await ed.call("inspect_color_lut", path="assets/luts/bad.cube")
    ed.check(out["ok"] is False and "Incomplete" in out["error"], f"incomplete data: {out}")
    await ed.check_error("inspect_color_lut", path="assets/luts/missing.cube", contains="No file")

    # a .cube as the LUT: the status reads the scene stage of a log LUT (the curve), and the display
    # stage of an sRGB one (the grade)
    sid = (await ed.call("create_scene", name="Cube", type="3d"))["scene"]["id"]
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": {
        "color_grading_enabled": True, "lut_texture": "assets/luts/acescct_look.cube"}})
    status = await ed.call("get_post_processing_status", scene=sid)
    ed.check(status["tail_color"] is True, f"a scene-stage LUT is part of the in-shader curve: {status}")
    got = (await ed.call("get_scene_settings", scene=sid, names=["post_processing.lut_texture"]))["settings"]
    ed.check_eq(got["post_processing.lut_texture"], "assets/luts/acescct_look.cube")
    await ed.call("save_scene", scene=sid, path="assets/scenes/Cube.scene")
    text = (ed.project / "assets/scenes/Cube.scene").read_text()
    ed.check("lutTexture: luts/acescct_look.cube" in text, f"stored assets-relative: {text[text.find('postProcessing'):][:300]}")
    meta = ed.project / "assets" / "luts" / "acescct_look.cube.meta"
    ed.check(meta.exists(), "a .cube is an asset with an identity (S04 meta)")

    # an identity LUT written as a .cube: inspect agrees with the file
    (ed.project / "assets" / "luts" / "identity.cube").write_text(cube("Identity", 5))
    out = await ed.call("inspect_color_lut", path="assets/luts/identity.cube")
    ed.check_eq((out["title"], out["size_3d"], out["proposed_encoding"], out["stage"]), ("Identity", 5, "srgb", "display"))


async def test_resave_identical(ed: Editor):
    """AC18: every scene of every tools/fixtures project opens and saves to the same bytes."""
    ed.require_tools("list_scenes", "open_scene", "save_scene")
    projects = sorted(p.parent for p in (harness.REPO / "tools" / "fixtures").glob("*/project.yaml"))
    ed.check(len(projects) >= 5, f"fixture projects: {projects}")
    checked = 0
    for source in projects:
        async with Editor(source, label=source.name) as fixture:
            scenes = [s for s in (await fixture.call("list_scenes"))["scenes"] if s.get("path")]
            for scene in scenes:
                path = fixture.project / scene["path"]
                before = path.read_bytes()
                opened = await fixture.call("open_scene", scene=scene["path"])
                await fixture.call("save_scene", scene=opened["scene"]["id"])
                after = path.read_bytes()
                if after != before:
                    fixture.keep = True
                    (Path(tempfile.gettempdir()) / f"resave-{source.name}-{path.name}").write_bytes(after)
                ed.check(after == before, f"{source.name}/{scene['path']} re-saves byte-identically "
                                          f"({len(before)} -> {len(after)} bytes)")
                checked += 1
    print(f"{checked} fixture scenes re-saved byte-identically")
