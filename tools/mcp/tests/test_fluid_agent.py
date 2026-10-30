"""The native Fluid Simulation component as an AI agent uses it, end to end through MCP (the workflow
of jasper://docs/fluid):

- create_entity type fluid_simulation with a preset in its values, set_properties `preset` (expanded
  into the preset's fields, one undo step, other values override it, unknown ids refused) and
  get_entity's read-only `fluid_stats` (the matching preset, support, sizes, steps);
- eval_lua with allow_edit_mode steps the edit-mode fluid (transient);
- orientation: eval_lua splat(0.25, 0.75, ...) on a SCREEN fluid lands at canvas (0.25 W, 0.25 H);
- pointer: a send_input drag over an interactive SCREEN fluid paints along the drag, nowhere else;
- export: a source export of a scene with a fluid ships licenses/webgl-fluid-simulation-LICENSE.txt
  (Pavel Dobryakov's MIT notice, the repository's file); a CLI export without the fluid's shader
  variants has no notice, one with only them has it.

Runs in a new empty project (EDITOR create).
"""

from __future__ import annotations

import asyncio
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import harness
from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from htmlui_pixels import decode_png  # noqa: E402

EDITOR = {"create": True, "name": "Fluid Agent"}
TIMEOUT = 1200

KEY = "fluidsimulation"
NAME = "Fluid"
LICENSE = harness.REPO / "engine" / "shaderlib" / "effects" / "LICENSE.webgl-fluid-simulation.txt"
NOTICE = Path("licenses") / "webgl-fluid-simulation-LICENSE.txt"

S: dict = {}

# the run configuration: a small, seeded, paused SCREEN fluid (scripted steps only)
RUN = {"output": "screen", "interactive": False, "paused": True, "seed": 4242, "splatsOnStart": True,
       "randomSplatCount": 10, "dyeResolution": 256, "simResolution": 64}


async def wait_job(ed: Editor, job_id: str, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)


def copy_project(source: Path) -> Path:
    tmp = Path(tempfile.mkdtemp(prefix="jasper-fluid-export-"))

    def ignore(directory: str, names: list[str]) -> set[str]:
        out = {".git"} & set(names)
        if Path(directory).name == ".jasperengine":
            out |= {"build", "export"} & set(names)
        return out

    shutil.copytree(source, tmp / "project", ignore=ignore, symlinks=True)
    return tmp / "project"


def cli_export(ed: Editor, project: Path, out: Path, *args: str) -> None:
    cmd = harness.editor_command() + ["export", "--project", str(project), "--out", str(out), "--backend", "opengl", *args]
    r = subprocess.run(cmd, cwd=str(project), capture_output=True, text=True, timeout=1800)
    ed.check(r.returncode == 0, f"export failed ({r.returncode}):\n{r.stdout[-3000:]}\n{r.stderr[-3000:]}")


def close(a, b, tol=1e-4) -> bool:
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol * max(1.0, abs(b))
    return a == b


async def fluid(ed: Editor) -> tuple[dict, dict]:
    """(component values, fluid_stats) of the edit-mode fluid."""
    got = await ed.call("get_entity", scene=S["scene"], entity=S["fluid"], components=[KEY])
    return got["components"][KEY], got.get("fluid_stats")


class Shot:
    """A Game view screenshot addressed in canvas pixels."""

    def __init__(self, png: bytes, info: dict):
        self.png = png
        self.w, self.h, self.ch, self.rows = decode_png(png)
        rect, canvas = info["canvas_rect_in_image"], info["canvas"]
        self.cw, self.chh = canvas["width"], canvas["height"]
        self.rx, self.ry = rect["x"], rect["y"]
        self.sx, self.sy = rect["width"] / canvas["width"], rect["height"] / canvas["height"]

    def px(self, fx: float, fy: float) -> tuple:
        """The pixel at a fraction of the canvas (from its top-left)."""
        x = min(self.w - 1, max(0, int(self.rx + fx * self.cw * self.sx)))
        y = min(self.h - 1, max(0, int(self.ry + fy * self.chh * self.sy)))
        return tuple(self.rows[y][x * self.ch:x * self.ch + 3])


async def game_shot(ed: Editor, name: str) -> Shot:
    r = await ed.call_raw("screenshot", {"view": "game", "max_width": 1024})
    ed.check(not r.is_error and r.images, f"game screenshot: {r.text[:300]}")
    png = r.images[0][0]
    (ed.scratch.root / f"{name}.png").write_bytes(png)
    return Shot(png, r.value)


async def lua(ed: Editor, code: str, **args):
    r = await ed.call("eval_lua", code=code, **args)
    return r.get("values")


async def until_lua(ed: Editor, condition: str, what: str):
    r = await ed.call_raw("wait", {"until": condition, "timeout_seconds": 120})
    ed.check(not r.is_error, f"{what}: {r.text[:800]}")


def handle() -> str:
    return f'object("{NAME}", "FluidSimulation")'


# ---------------------------------------------------------------------------- creating and presets


async def test_create_with_preset(ed: Editor):
    made = await ed.call("create_scene", name="FluidAgent", type="2D")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="fluid_simulation", name=NAME,
                        values={"fluid": {"preset": "ink", "curl": 12, "seed": 4242}})
    S["fluid"] = ent["entity_id"]
    values, stats = await fluid(ed)
    for name, want in {"densityDissipation": 0.15, "velocityDissipation": 0.8, "pressureIterations": 30,
                       "bloom": False, "sunrays": False, "curl": 12.0, "seed": 4242, "output": "material"}.items():
        ed.check(close(values[name], want), f"created with the preset: {name} = {values[name]!r}, expected {want!r}")
    ed.check(isinstance(stats, dict), f"get_entity has fluid_stats: {stats}")
    ed.check(stats["preset"] is None, f"curl 12 is no preset: {stats['preset']}")
    ed.check(stats["supported"] is True, f"the editor's device runs fluids: {stats}")

    # the preset alone matches; one undo step back
    await ed.call("set_properties", scene=S["scene"], entity=S["fluid"], values={"fluid": {"preset": "ink"}})
    values, stats = await fluid(ed)
    ed.check(close(values["curl"], 8.0) and stats["preset"] == "ink", f"ink applied: curl {values['curl']}, {stats['preset']}")
    await ed.call("undo", scene=S["scene"])
    values, stats = await fluid(ed)
    ed.check(close(values["curl"], 12.0) and close(values["densityDissipation"], 0.15), f"one undo step: {values['curl']}")

    # the changes form, in order; the clock and the compositing are left alone
    await ed.call("set_properties", scene=S["scene"], entity=S["fluid"],
                  changes=[{"component": "fluid", "property": "preset", "value": "mobile"}])
    values, stats = await fluid(ed)
    ed.check(values["dyeResolution"] == 512 and values["shading"] is False and stats["preset"] == "mobile",
             f"mobile applied: {values['dyeResolution']}, {values['shading']}, {stats['preset']}")
    ed.check(values["seed"] == 4242 and values["output"] == "material", "a preset keeps the seed and the output")
    await ed.check_error("set_properties", scene=S["scene"], entity=S["fluid"], values={"fluid": {"preset": "nope"}},
                         contains="calm-water")
    await ed.check_error("set_properties", scene=S["scene"], entity=S["fluid"],
                         changes=[{"component": "fluid", "property": "preset", "insert": "ink"}], contains="preset")

    # the run configuration
    await ed.call("set_properties", scene=S["scene"], entity=S["fluid"], values={"fluid": {"preset": "default", **RUN}})
    values, stats = await fluid(ed)
    for name, want in RUN.items():
        ed.check(close(values[name], want), f"run config {name} = {values[name]!r}")
    for key in ("running", "steps", "seed", "sim_size", "dye_size", "gpu_bytes", "passes_last_frame",
                "manual_filtering", "compact_formats"):
        ed.check(key in stats, f"fluid_stats has {key}: {stats}")
    filtered = await ed.call("get_entity", scene=S["scene"], entity=S["fluid"], components=["sprite"])
    ed.check("fluid_stats" not in filtered, "a component filter without the fluid leaves its stats out")

    saved = await ed.call("save_scene", scene=S["scene"], path="scenes/FluidAgent")
    S["path"] = saved["scene"]["path"]
    await ed.call("edit_build_scenes", action="add", scene=S["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=S["path"])
    await ed.call("save_project")


async def test_edit_mode_step(ed: Editor):
    # the editor draws the fluid still; eval_lua allow_edit_mode can step it (transient)
    await ed.call("select_scene", scene=S["scene"])
    await game_shot(ed, "fluid-edit")
    _, before = await fluid(ed)
    await lua(ed, f"local f = {handle()}; f:step(5); return true", allow_edit_mode=True, scene=S["scene"])
    after = before
    for _ in range(20):
        await game_shot(ed, "fluid-edit-stepped")
        _, after = await fluid(ed)
        if after["steps"] >= before["steps"] + 5:
            break
        await asyncio.sleep(0.25)
    ed.check(after["steps"] >= before["steps"] + 5, f"edit-mode steps: {before['steps']} -> {after['steps']}")
    ed.check(after["running"] and min(after["dye_size"]) == 256, f"the edit-mode fluid runs at its dye size: {after}")


# ---------------------------------------------------------------------------- determinism


# ---------------------------------------------------------------------------- orientation and pointer


async def test_splat_orientation_and_drag(ed: Editor):
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True, timeout_seconds=300)
    try:
        # one scripted splat, no look passes in the way: the dye colour itself
        await lua(ed, f"local f = {handle()}; f.splatsOnStart = false; f.shading = false; f.bloom = false; "
                      "f.sunrays = false; f:reset(); f:splat(0.25, 0.75, 0, 0, 1.0, 0.3, 0.1); return true")
        # (== 1: the play's own start burst counted 10 before the reset)
        await until_lua(ed, f"{handle()}:getStats().splats == 1", "the splat")
        shot = await game_shot(ed, "fluid-splat")
        at = shot.px(0.25, 0.25)
        ed.check(at[0] > 150 and at[0] > at[1] + 40 and at[0] > at[2] + 60,
                 f"splat(0.25, 0.75) is at canvas (0.25 W, 0.25 H): {at}")
        for fx, fy in ((0.25, 0.75), (0.75, 0.25), (0.75, 0.75)):
            ed.check(max(shot.px(fx, fy)) < 30, f"nothing at canvas ({fx} W, {fy} H): {shot.px(fx, fy)}")

        # a drag over the interactive SCREEN fluid (paused: strokes paint without moving)
        await lua(ed, f"local f = {handle()}; f.interactive = true; f:reset(); return true")
        await ed.call("wait", frames=2)
        clean = await game_shot(ed, "fluid-before-drag")
        ed.check(max(clean.px(0.5, 0.5)) < 10, f"reset clears the dye: {clean.px(0.5, 0.5)}")
        w, h = clean.cw, clean.chh
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "drag", "x": 0.15 * w, "y": 0.5 * h,
                                                         "to": [0.85 * w, 0.5 * h], "steps": 14}], "frames_after": 2})
        ed.check(not r.is_error, r.text[:600])
        shot = await game_shot(ed, "fluid-drag")
        for fx in (0.3, 0.5, 0.7, 0.85):
            ed.check(max(shot.px(fx, 0.5)) >= 15, f"the drag painted at canvas ({fx} W, 0.5 H): {shot.px(fx, 0.5)}")
        for fy in (0.15, 0.85):
            ed.check(max(shot.px(0.5, fy)) < 10, f"nothing off the drag at ({0.5} W, {fy} H): {shot.px(0.5, fy)}")
        live = await ed.call("get_runtime_entity", entity=NAME, components=["fluid"], include_script_state=False)
        ed.check(live["fluid_stats"]["splats"] >= 5, f"the pointer splatted: {live['fluid_stats']}")
        problems = [e["message"] for e in await ed.console_since(None, types=["error"], limit=200)]
        ed.check(not [p for p in problems if "luid" in p], f"no fluid errors: {problems}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


# ---------------------------------------------------------------------------- export


async def source_export(ed: Editor, name: str) -> Path:
    out = ed.scratch.root / name
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", overwrite=True,
                      timeout_seconds=900)
    if r.get("status") == "running":
        r = await wait_job(ed, r["job_id"], 1200)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    return out


async def test_export_ships_the_notice(ed: Editor):
    await ed.call("save_scene", scene=S["scene"])
    out = await source_export(ed, "export-fluid")
    notice = out / NOTICE
    ed.check(notice.is_file(), f"the fluid notice is exported: {sorted(p.name for p in out.iterdir())}")
    ed.check_eq(notice.read_text(), LICENSE.read_text(), "the notice is the engine's")
    ed.check("Copyright (c) 2017 Pavel Dobryakov" in notice.read_text(), "Pavel Dobryakov's MIT notice")
    ed.check(not (out / "licenses" / "shaders.com-LICENSE.txt").exists(), "no UI effect: no shaders.com notice")

    # the condition is the shipped variants (a scene keeps the keys it ever needed, so an explicit
    # list): none of the fluid's, no notice; only the fluid's, the notice
    await ed.call("save_project")
    project = copy_project(ed.project)
    plain = project.parent / "export-plain"
    try:
        cli_export(ed, project, plain, "--shader", "mesh")
        # (licenses/ itself holds S18's OpenEngine notice in every export)
        ed.check(not (plain / NOTICE).exists(), "no fluid variant: no notice")
        only = project.parent / "export-fluid-only"
        cli_export(ed, project, only, "--shader", "fluid")
        ed.check((only / NOTICE).is_file(), "the fluid's variants alone: the notice")
    finally:
        shutil.rmtree(project.parent, ignore_errors=True)
    errors = [e.get("message", "") for e in await ed.console_since(None, types=["error"], limit=500)]
    ed.check(not [e for e in errors if re.search("fluid", e, re.I)], f"Console fluid errors: {errors}")
