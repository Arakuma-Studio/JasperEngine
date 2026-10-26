"""Exporting UI effects (phase 5, p5-tooling, spec 3.8, 3.16, 4.3).

A scene whose document uses the reference effects every way the collector must find them:
a paint background (`stripes`), gradient text (`stripes` in the filter form), a filter
(`pixelate`), a backdrop filter only inside a @media block the conversion leaves out on a desktop
(`frosted-glass` in `(hover: none)`), a paint effect only inside a width @media block
(`cursor-trail`), an effect only a Lua script sets through `style` (`spotlight`, both forms), and
a project effect (`tint-wave`, with a project include), and a stateful effect with a scatter pass
(`boids`, phase 6: its paint, step and scatter variants). A declared but unused project effect
(`spare-glow`), a .frag no document declares and the forms nothing uses are not exported.

- `jasperengine-editor export` (CLI, on a copy of the project) ships project .frag/include
  assets and licenses/shaders.com-LICENSE.txt; an export without UI effects has no notice.
- A Build Settings addition `uieffect/cursor-trail:Flt` (build_project shaders.add, an in-editor
  export) is written to project.yaml using the compatible shader-addition representation.
- JASPER_SLOW_BUILD=1 also builds the Desktop player without the runtime shader compiler and runs
  it: the effects draw from the exported variants (the shipped game compiles nothing).
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

import yaml

import harness
from harness import Editor, skip, REPO
from htmlui_pixels import decode_png, near

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}

TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))
JOBS = 2

S: dict = {}
_copies: list[tempfile.TemporaryDirectory] = []

HTML = """<html>
<head>
    <title>shipped effects</title>
    <link rel="stylesheet" href="shipped.css"/>
</head>
<body>
    <div id="hero" class="hero"></div>
    <h1 id="title" class="title">JASPER</h1>
    <div id="pix" class="pix"><div class="b1"></div><div class="b2"></div></div>
    <div id="glass" class="glass"></div>
    <div id="trail" class="trail"></div>
    <div id="lua" class="lua"></div>
    <div id="tint" class="tint"></div>
    <div id="flock" class="flock"></div>
</body>
</html>
"""

CSS = """@effect tint-wave { src: url(fx/tint-wave.frag); }
@effect spare-glow { src: url(fx/spare-glow.frag); }

/* spotlight is set from Lua (ShippedUI.lua); a comment uses nothing: .x { filter: effect(cursor-trail); } */
body { margin: 0; width: 1280px; height: 720px; background-color: #101218; }
.hero, .title, .pix, .glass, .trail, .lua, .tint { position: absolute; display: block; margin: 0; }
.hero { left: 40px; top: 40px; width: 360px; height: 220px;
        background-image: effect(stripes, color-a #ff3355, color-b #2255ff, density 6); }
.title { left: 40px; top: 300px; width: 360px; height: 100px; font-size: 80px; font-weight: bold; color: transparent;
         background-image: effect(stripes, color-a #ffcc00, color-b #00ccff); background-clip: text; }
.pix { left: 440px; top: 40px; width: 360px; height: 220px; filter: effect(pixelate, size 24px, gap 0.25); }
.pix div { position: absolute; top: 0; width: 50%; height: 100%; }
.b1 { left: 0; background-color: #e74c3c; }
.b2 { left: 50%; background-color: #2ecc71; }
.glass { left: 840px; top: 40px; width: 360px; height: 220px; background-color: #ffffff22; }
@media (hover: none) { .glass { backdrop-filter: effect(frosted-glass, blur 10px); } }
.trail { left: 840px; top: 300px; width: 10px; height: 10px; }
@media (max-width: 600px) { .trail { background-image: effect(cursor-trail); } }
.lua { left: 440px; top: 300px; width: 360px; height: 220px; background-color: #202840; }
.tint { left: 840px; top: 300px; width: 360px; height: 220px; background-color: #000000;
        background-image: effect(tint-wave, color #33ff88, speed 0.5); }
.flock { position: absolute; left: 440px; top: 540px; width: 360px; height: 160px;
         background-image: effect(boids, count 600, color-a #8ec5ff, color-b #8ec5ff); }
"""

LUA = """---@class ShippedUI : ScriptInstance
local ShippedUI = {}

function ShippedUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- the only use of spotlight: a Lua style (compat rewrites it to the typed form)
function ShippedUI:onDocumentLoad()
    local element = self.doc:getElementById("lua")
    if element then
        element.style.backgroundImage = "effect(spotlight, color #ffee88, radius 120px, idle-intensity 1)"
    end
end

return ShippedUI
"""

FRAG = """#version 450
// @effect     tint-wave
// @kind       paint
// @category   gradient
// @summary    A tinted wave (a project effect of the export tests)
// @source     original
// @cost       low
// @time       speed
// @param      color color #3388ff -- Tint of the wave
// @param      speed number 1 -5 5 -- Waves per second

#include "effects/lib/jfx_common.glsl"
#include "local-wave.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float speed;
} params;

vec4 jfx_paint(JfxContext c){
    return vec4(params.color.rgb * local_wave(c.uv.x, c.anim.x), params.color.a);
}

#include "effects/lib/main_paint.glsl"
"""

GLSL = """float local_wave(float x, float t){
    return 0.55 + 0.45 * sin((x * 2.0 + t) * 6.2831853);
}
"""

LICENSE = harness.REPO / "engine" / "shaderlib" / "effects" / "LICENSE.shaders-com.txt"


def write(project: Path, rel: str, text: str) -> None:
    path = project / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def copy_project(source: Path) -> Path:
    directory = tempfile.TemporaryDirectory(prefix="jasper-fx-export-")
    _copies.append(directory)
    tmp = Path(directory.name)

    def ignore(directory: str, names: list[str]) -> set[str]:
        out = {".git"} & set(names)
        if Path(directory).name == ".jasperengine":
            out |= {"build", "export"} & set(names)
        return out

    shutil.copytree(source, tmp / "project", ignore=ignore, symlinks=True)
    return tmp / "project"


def cli_export(ed: Editor, project: Path, out: Path, *args: str) -> subprocess.CompletedProcess:
    cmd = harness.editor_command() + ["export", "--project", str(project), "--out", str(out),
                                      "--backend", "opengl", "--backend", "opengles", *args]
    r = subprocess.run(cmd, cwd=str(project), capture_output=True, text=True, timeout=1800)
    ed.check(r.returncode == 0, f"export failed ({r.returncode}):\n{r.stdout[-3000:]}\n{r.stderr[-3000:]}")
    return r


async def wait_job(ed: Editor, job_id: str, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)


async def _setup_effects(ed: Editor):
    if "scene" in S:
        return
    write(ed.project, "assets/ui/fx/tint-wave.frag", FRAG)
    write(ed.project, "assets/ui/fx/local-wave.glsl", GLSL)
    write(ed.project, "assets/ui/fx/spare-glow.frag", FRAG.replace("tint-wave", "spare-glow").replace('#include "local-wave.glsl"\n', "")
          .replace("local_wave(c.uv.x, c.anim.x)", "1.0"))
    write(ed.project, "assets/ui/fx/unused.frag", FRAG.replace("tint-wave", "unused-wave"))
    write(ed.project, "assets/ui/shipped.html", HTML)
    write(ed.project, "assets/ui/shipped.css", CSS)
    made = await ed.call("create_scene", name="EffectsExport", type="UI")
    S["scene"] = made["scene"]["id"]
    r = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/shipped.html", name="Shipped")
    S["entity"] = r["entity"]["id"]
    ed.check_eq([d for d in r["diagnostics"] if d["severity"] != "info"], [], "the document loads clean")
    await ed.call("create_script", kind="lua", name="ShippedUI", content=LUA, scene=S["scene"], entity=S["entity"])
    saved = await ed.call("save_scene", scene=S["scene"], path="scenes/EffectsExport")
    await ed.call("edit_build_scenes", action="add", scene=saved["scene"]["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scene"]["path"])
    await ed.call("set_project_settings", values={"graphics.runtime_shaders": False, "player.window_width": 1280,
                                                  "player.window_height": 720})
    await ed.call("save_project")


async def test_cli_export_ships_effect_assets_and_license(ed: Editor):
    await _setup_effects(ed)
    project = copy_project(ed.project)
    out = project.parent / "export"
    cli_export(ed, project, out)
    notice = out / "licenses" / "shaders.com-LICENSE.txt"
    ed.check(notice.is_file(), f"the shaders.com notice is exported: {sorted(p.name for p in out.iterdir())}")
    ed.check_eq(notice.read_text(), LICENSE.read_text(), "the notice is the engine's")
    assets = out / "project" / "assets" / "ui" / "fx"
    for name in ("tint-wave.frag", "local-wave.glsl"):
        ed.check((assets / name).is_file(), f"{name} ships as an asset (the game reads the manifest)")


async def test_export_without_effects_has_no_notice(ed: Editor):
    await _setup_effects(ed)
    project = copy_project(ed.project)
    out = project.parent / "export-plain"
    cli_export(ed, project, out, "--shader", "mesh", "--shader", "htmlui")
    ed.check(not (out / "licenses").exists(), "no UI effect: no notice")
    out = project.parent / "export-one"
    cli_export(ed, project, out, "--shader", "uieffect/pixelate:Flt")
    ed.check((out / "licenses" / "shaders.com-LICENSE.txt").is_file(), "the notice with the effect")


async def test_build_settings_addition_persists(ed: Editor):
    await _setup_effects(ed)
    out = ed.scratch.root / "export-in-editor"
    raw = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(out), "platform": "linux", "overwrite": True,
                                              "backends": ["opengl"], "timeout_seconds": 1800,
                                              "shaders": {"add": ["uieffect/cursor-trail:Flt"]}})
    r = raw.value
    if isinstance(r, dict) and r.get("status") == "running":
        r = await wait_job(ed, r["job_id"], 1800)
    ed.check_eq(r["status"], "succeeded", raw.text[:2000])
    ed.check((out / "licenses" / "shaders.com-LICENSE.txt").is_file(), "the in-editor export has the notice")
    data = yaml.safe_load((ed.project / "project.yaml").read_text())
    additions = data["export"]["shaderAdditions"]
    ed.check({"type": "uieffect", "properties": 1, "customShader": "@effects/cursor-trail"} in additions,
             f"project.yaml names the effect: {additions}")


def stripes_seen(png: bytes) -> tuple[int, int, int]:
    w, h, ch, rows = decode_png(png)
    counts = [0, 0, 0]
    for y in range(0, h, 3):
        row = rows[y]
        for x in range(0, w, 3):
            px = tuple(row[x * ch:x * ch + 3])
            for i, color in enumerate(((0xff, 0x33, 0x55), (0x22, 0x55, 0xff), (0x33, 0xff, 0x88))):
                if near(px, color, 40):
                    counts[i] += 1
    return counts[0], counts[1], counts[2]


async def test_desktop_player_draws_the_effects(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (compiles the engine)")
    await _setup_effects(ed)
    out = ed.scratch.root / "export-effects-desktop"
    # This build also clears additions left by the persistence test; no reset-only export is needed.
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), build_jobs=JOBS, timeout_seconds=30,
                      shaders={"reset": True})
    job = r if r["status"] != "running" else await wait_job(ed, r["job_id"], TIMEOUT - 300, poll=15.0)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check((out / "licenses" / "shaders.com-LICENSE.txt").is_file(), "the player ships the notice")
    raw = await ed.call_raw("run_build", {"seconds": 10, "screenshot_after_seconds": 8})
    ed.check(not raw.is_error, raw.text)
    ed.check(not raw.value["crashed"], f"the player crashed: {raw.value}")
    png = raw.images[0][0]
    (ed.scratch.root / "html-ui-effects-player.png").write_bytes(png)
    # pixelate's cells: the gaps between them show the page (a plain red/green box has none)
    w, h, ch, rows = decode_png(png)
    sx, sy = w / 1280.0, h / 720.0
    gaps = sum(1 for y in range(int(50 * sy), int(250 * sy), 2) for x in range(int(450 * sx), int(790 * sx), 2)
               if near(tuple(rows[y][x * ch:x * ch + 3]), (0x10, 0x12, 0x18), 12))
    ed.check(gaps > 300, f"the player draws pixelate's cells ({gaps} gap samples)")
    red, blue, green = stripes_seen(png)
    ed.check(red > 200 and blue > 200, f"the player draws the stripes (red {red}, blue {blue} samples)")
    ed.check(green > 100, f"the player draws the project effect (green {green} samples)")
    # the flock (a stateful effect: its step and scatter variants run in the player)
    boids = sum(1 for y in range(int(545 * sy), int(695 * sy), 2) for x in range(int(445 * sx), int(795 * sx), 2)
                if near(tuple(rows[y][x * ch:x * ch + 3]), (0x8e, 0xc5, 0xff), 40))
    ed.check(boids > 40, f"the player draws the boids ({boids} samples)")
    log = str(raw.value)
    ed.check("Missing shader" not in log and "missing shader" not in log, f"no variant is missing: {log[:3000]}")
