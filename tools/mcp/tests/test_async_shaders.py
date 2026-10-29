"""Asynchronous shader and pipeline creation (docs/specs/S32-async-shaders.md, docs/mcp/guide/
shaders.md "Asynchronous creation and the warm-up list"): get_render_stats.shader_creation (AC17),
the Shader Creation settings rows (AC16), a lobe toggle that never leaves an empty frame and ends
on the exact variant (AC6), the scene ready state waiting for creations (AC8), recording the
warm-up list in Play and exporting it (AC9), and the Lua API (AC17)."""

from __future__ import annotations

import json

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "AsyncShaders"}
TIMEOUT = 1500

MODES = {"sync", "worker", "parallel", "deferred", "thread", "metal"}
SETTINGS = {
    "shader_creation.async_creation": (True, False),
    "shader_creation.main_thread_budget_ms": (2.0, 6.5),
    "shader_creation.gl_warm_draws": (True, False),
    "shader_creation.record_during_play": (False, True),
    "shader_creation.include_in_builds": (True, False),
    "shader_creation.hold_first_scene": (True, False),
}

S: dict = {}


async def idle(ed: Editor, frames: int = 3000) -> dict:
    """get_render_stats.shader_creation once nothing is pending."""
    stats: dict = {}
    for _ in range(frames // 10):
        stats = (await ed.call("get_render_stats"))["shader_creation"]
        if stats["pending_shaders"] == 0 and stats["pending_pipelines"] == 0 and stats["pending_warm_draws"] == 0:
            return stats
        await ed.call("wait", frames=10, timeout_seconds=900)
    raise AssertionError(f"shader creation never went idle: {stats!r}")


async def ball_scene(ed: Editor) -> int:
    if "scene" in S:
        return S["scene"]
    made = await ed.call("create_scene", name="AsyncBalls", type="3D", path="async_balls")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    await ed.call("set_scene_settings", scene=scene, settings={"lit_quality": "standard"})
    await ed.call("create_entity", scene=scene, type="sphere", name="Ball", select=False)
    await ed.call("set_scene_camera", scene=scene, position=[0, 0.5, 3.0], target=[0, 0, 0],
                  display={"show_grid_3d": False, "hide_light_icons": True, "hide_selection_outline": True})
    await ed.call("save_scene", scene=scene)
    await idle(ed)
    S["scene"] = scene
    return scene


async def scene_shot(ed: Editor):
    """The Scene view, decoded (width, height, rgba)."""
    shot = await ed.call_raw("screenshot", {"view": "scene", "max_width": 256}, timeout=300)
    ed.check(not shot.is_error and shot.images, f"no scene screenshot: {shot.text[:300]}")
    return pngdiff.decode(shot.images[0][0])


async def test_render_stats_shader_creation(ed: Editor):
    await ball_scene(ed)
    stats = await idle(ed)
    ed.check_in(stats["mode"], MODES, "a known mode")
    for key in ("pending_shaders", "pending_pipelines", "pending_warm_draws", "created_shaders", "created_pipelines",
                "warm_draws", "interned_pipelines"):
        ed.check(isinstance(stats[key], int) and stats[key] >= 0, f"{key} is a count: {stats[key]!r}")
    for key in ("last_frame_main_ms", "max_main_ms", "max_create_ms"):
        ed.check(isinstance(stats[key], (int, float)) and stats[key] >= 0, f"{key} is a time: {stats[key]!r}")
    ed.check(stats["created_shaders"] > 0 and stats["created_pipelines"] > 0, "the scene's shaders and pipelines were created")
    ed.check(stats["interned_pipelines"] > 0, "its pipelines are interned")
    warmup = stats["warmup"]
    ed.check_eq(sorted(warmup.keys()), ["active", "pipelines", "progress", "recording", "shaders"], "warmup fields")
    ed.check_eq(warmup["recording"], False, "not recording in edit mode")


async def test_settings_rows(ed: Editor):
    listed = await ed.call("get_project_settings")
    text = json.dumps(listed)
    for key, (default, other) in SETTINGS.items():
        ed.check(key in text, f"{key} is listed")
        await ed.call("set_project_settings", settings={key: other})
        read = await ed.call("get_project_settings", keys=[key])
        ed.check(json.dumps(other) in json.dumps(read), f"{key} reads back {other!r}: {read!r}")
        await ed.call("set_project_settings", settings={key: default})
    await ed.check_error("set_project_settings", settings={"shader_creation.main_thread_budget_ms": 100}, contains="main_thread_budget_ms")
    ed.check("shader_creation.warmup_entries" in text, "the list's entry count is listed")
    yaml = (ed.project / "project.yaml").read_text()
    ed.check("shaderCreation" not in yaml, "defaults are not written to project.yaml")


async def test_lobe_toggle_never_empty(ed: Editor):
    """Clear coat on: no frame shows the background alone (the stand-in draws, then the exact
    variant), and the last one differs from before (the coat's highlight)."""
    scene = await ball_scene(ed)
    await ed.call("set_entity_state", scene=scene, entity="Ball", active=False)
    await ed.call("wait", frames=5)
    background = await scene_shot(ed)
    await ed.call("set_entity_state", scene=scene, entity="Ball", active=True)
    await idle(ed)
    before = await scene_shot(ed)
    _, ball_fraction, _ = pngdiff.compare(before, background, 8, 1.0)
    ed.check(ball_fraction > 0.02, f"the ball covers part of the view ({ball_fraction:.3f})")
    await ed.call("set_properties", scene=scene, entity="Ball",
                  values={"mesh": {"submeshes[0].material": {"pbr": {"clearcoat": {"enabled": True, "factor": 1.0, "roughness": 0.02}}}}})
    empty_frames = 0
    for _ in range(40):
        _, fraction, _ = pngdiff.compare(await scene_shot(ed), background, 8, 1.0)
        if fraction < ball_fraction * 0.5:
            empty_frames += 1
        stats = (await ed.call("get_render_stats"))["shader_creation"]
        if stats["pending_shaders"] == 0 and stats["pending_pipelines"] == 0 and stats["pending_warm_draws"] == 0:
            break
        await ed.call("wait", frames=5, timeout_seconds=900)
    ed.check_eq(empty_frames, 0, "no frame lost the ball while its variant was created")
    await idle(ed)
    after = await scene_shot(ed)
    ok, _, _ = pngdiff.compare(after, before, 2, 0.001)
    ed.check(not ok, "the clear coat variant draws in the end")
    variant = await ed.call("get_material_variant", scene=scene, entity="Ball")
    ed.check("PBR_CLEARCOAT" in json.dumps(variant), f"the exact variant: {variant!r}")


async def test_record_and_export(ed: Editor):
    scene = await ball_scene(ed)
    await ed.call("set_project_settings", settings={"shader_creation.record_during_play": True})
    try:
        await ed.call("set_properties", scene=scene, entity="Ball",
                      values={"mesh": {"submeshes[0].material": {"pbr": {"sheen": {"enabled": True}}}}})
        await ed.call("save_scene", scene=scene)
        await ed.call("play", action="start", scene=scene)
        try:
            await ed.call("wait", frames=30, timeout_seconds=900)
            stats = (await ed.call("get_render_stats"))["shader_creation"]
            ed.check_eq(stats["warmup"]["recording"], True, "Play records")
            await idle(ed)
        finally:
            await ed.call("play", action="stop")
    finally:
        await ed.call("set_project_settings", settings={"shader_creation.record_during_play": False})

    path = ed.project / "ShaderWarmup.json"
    ed.check(path.exists(), "Stop wrote the list next to project.yaml")
    listed = json.loads(path.read_text())
    ed.check_eq(listed["version"], 1, "version 1")
    ed.check(len(listed["shaders"]) > 0 and len(listed["pipelines"]) > 0, f"shaders and pipelines recorded: {len(listed['shaders'])}, {len(listed['pipelines'])}")
    ed.check(all("key" in s and "custom" in s for s in listed["shaders"]), "every shader has its key and custom name")
    ed.check(all(0 <= p["shader"] < len(listed["shaders"]) and isinstance(p["desc"], dict) for p in listed["pipelines"]), "pipelines name their shader")
    rows = json.dumps(await ed.call("get_project_settings", keys=["shader_creation.warmup_entries"]))
    ed.check(f'"shaders": {len(listed["shaders"])}' in rows, f"the settings row counts the file: {rows}")

    # the source export ships the list (embedded in the shader headers of a source export)
    out = ed.scratch.root / "async-export"
    built = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"],
                          overwrite=True, timeout_seconds=1400)
    ed.check(not built.get("errors"), f"the export succeeded: {built.get('errors')!r}")
    headers = list(out.rglob("glsl410.h"))
    shipped = (out / "project" / "shaders" / "warmup.json")
    found = shipped.exists() or any('"warmup"' in h.read_text(errors="replace") for h in headers)
    ed.check(found, "the export carries the warm-up list (shaders/warmup.json or the shader header's \"warmup\" entry)")

    # Clear empties it
    await ed.call("run_command", id="assets.clearShaderWarmup")
    cleared = json.loads(path.read_text())
    ed.check_eq((len(cleared["shaders"]), len(cleared["pipelines"])), (0, 0), "Clear empties the list")


async def test_lua_api(ed: Editor):
    scene = await ball_scene(ed)
    await ed.call("play", action="start", scene=scene)
    try:
        await ed.call("wait", frames=10, timeout_seconds=900)
        values = (await ed.call("eval_lua", code="""
            return ShaderPool.getPendingShaderCount() >= 0, ShaderPool.getPendingPipelineCount() >= 0,
                   type(ShaderPool.isGpuCreationIdle()), ShaderWarmup.isDone(), ShaderWarmup.getProgress(),
                   ShaderWarmup.getEntryCount()
        """))["values"]
        ed.check_eq(values[:4], [True, True, "boolean", True], "the counts and states")
        ed.check_eq(values[4], 1, "no list replays in the editor: progress 1")
        ed.check_eq(values[5], 0, "and no entries")
    finally:
        await ed.call("play", action="stop")
