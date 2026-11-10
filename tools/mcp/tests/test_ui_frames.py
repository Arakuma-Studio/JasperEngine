"""Exact-time HTML UI previews, without starting Play, and dependency-driven regeneration."""

from __future__ import annotations

import asyncio
import json
import subprocess

import harness
import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "UI frame preview"}
TIMEOUT = 300

HTML = """<!doctype html><html><head><link rel="stylesheet" href="frames.css"></head><body>
<div id="css" class="box"></div><div id="anime" class="box"></div><div id="game" class="box"></div>
<script type="text/lua">
anime.animate("#anime", { left = {from = 0, to = 80}, duration = 1000, ease = "linear" })
anime.animate("#game", { left = {from = 0, to = 80}, duration = 1000, ease = "linear", clock = "game", autoplay = false })
</script></body></html>"""

CSS = """body { margin:0; width:128px; height:96px; background:#000; }
.box { position:absolute; left:0; width:16px; height:16px; }
#css { top:0; background:CSS_COLOR; animation: slide 1s linear forwards; }
#anime { top:32px; background:#00ff00; }
#game { top:64px; background:#0000ff; }
@keyframes slide { from {left:0;} to {left:80px;} }
"""


def pixel(rgba: bytes, width: int, x: int, y: int) -> tuple[int, ...]:
    offset = (y * width + x) * 4
    return tuple(rgba[offset:offset + 4])


async def setup(ed: Editor) -> None:
    folder = ed.project / "assets" / "ui"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "frames.html").write_text(HTML, encoding="utf-8")
    (folder / "frames.css").write_text(CSS.replace("CSS_COLOR", "#ff0000"), encoding="utf-8")
    await ed.call("sync_files")


async def render(ed: Editor, out: str, **args):
    result = await ed.call_raw("render_ui_frames", {
        "document": "assets/ui/frames.html", "times": [1, 0.25, 0, 0.5, 1],
        "width": 128, "height": 96, "layout": "frames", "out": out, **args,
    }, timeout=150)
    ed.check(not result.is_error, f"rendered without Play: {result.text}")
    return result


async def test_group_and_cli_schema(ed: Editor):
    ed.check("render_ui_frames" not in {t.name for t in await ed.tools(groups=None)}, "preview hidden with html_ui")
    await ed.call("tool_groups", enable=["html_ui"])
    ed.check("render_ui_frames" in {t.name for t in await ed.tools(groups=None)}, "preview listed after enabling html_ui")
    proc = await asyncio.to_thread(subprocess.run, harness.editor_command() + [
        "call", "--dry-run", "render_ui_frames", "--document", "assets/ui/frames.html",
        "--times", "0.3,1,2.5", "--out", "sheet.png", "--watch",
    ], capture_output=True, text=True, timeout=30)
    ed.check_eq(proc.returncode, 0, f"requested CLI syntax: {proc.stderr}")
    arguments = json.loads(proc.stdout)["arguments"]
    ed.check_eq(arguments["times"], [0.3, 1, 2.5], "comma-separated exact times")
    ed.check(arguments["watch"] is True and arguments["out"] == "sheet.png", "watch and output parsed")


async def test_exact_times_css_and_both_anime_clocks(ed: Editor):
    await setup(ed)
    result = await render(ed, "frames-a")
    ed.check_eq(len(result.images), 5, "each requested PNG returned")
    ed.check_eq(result.value["times"], [1, 0.25, 0, 0.5, 1], "request order and duplicate times kept")
    for i, seconds in enumerate(result.value["times"]):
        frame = ed.project / "frames-a" / f"frame_{i}.png"
        width, height, rgba = pngdiff.decode(frame.read_bytes())
        ed.check_eq((width, height), (128, 96), "exact preview resolution")
        x = round(seconds * 80) + 8
        for y, expected in ((8, (255, 0, 0, 255)), (40, (0, 255, 0, 255)), (72, (0, 0, 255, 255))):
            ed.check_eq(pixel(rgba, width, x, y), expected, f"CSS/UI/game clock at {seconds}s row {y}")
        if seconds > 0.25:
            ed.check_eq(pixel(rgba, width, 8, 8), (0, 0, 0, 255), "CSS origin is not the sampled time")
    state = await ed.call("editor_status")
    ed.check(not state.get("playing", False), "preview leaves the editor stopped")


async def test_repeat_and_sheet(ed: Editor):
    await setup(ed)
    await render(ed, "repeat-a")
    await render(ed, "repeat-b")
    for i in range(5):
        ed.check_eq((ed.project / "repeat-a" / f"frame_{i}.png").read_bytes(),
                    (ed.project / "repeat-b" / f"frame_{i}.png").read_bytes(), "fresh previews are byte-identical")
    result = await render(ed, "sheet.png", times=[0, 0.5, 1], layout="sheet", columns=2)
    width, height, _ = pngdiff.decode((ed.project / "sheet.png").read_bytes())
    ed.check(width > 256 and height > 192, "sheet has a two-column grid with time label strips")
    ed.check_eq(len(result.images), 1, "one sheet returned")


async def test_source_scene(ed: Editor):
    await setup(ed)
    scene = (await ed.call("create_scene", name="Preview source", type="UI"))["scene"]["id"]
    await ed.call("create_ui_document", scene=scene, html="assets/ui/frames.html", name="Preview")
    result = await ed.call("render_ui_frames", scene=scene, times=[0.5], width=128, height=96, out="scene.png")
    ed.check_eq(result["times"], [0.5], "scene UI documents can be sampled")
    ed.check((ed.project / "scene.png").is_file(), "scene sheet written")


async def test_attached_ui_lua_scripts_and_start_are_previewed(ed: Editor):
    await setup(ed)
    plain = HTML[:HTML.index('<script type="text/lua">')] + "</body></html>"
    (ed.project / "assets/ui/attached.html").write_text(plain, encoding="utf-8")
    scripts = ed.project / "scripts"
    scripts.mkdir(exist_ok=True)
    (scripts / "PreviewUI.lua").write_text('''local ui = {}
function ui:init()
    local doc = UIDocument(self.scene, self.entity)
    self.timeline = anime.createTimeline({autoplay=false})
    self.timeline:add(doc:getElementById("anime"), {left={from=0,to=80},duration=1000,ease="linear"})
end
function ui:start()
    local doc = UIDocument(self.scene, self.entity)
    anime.animate(doc:getElementById("game"), {left={from=0,to=80},duration=1000,ease="linear",clock="game"})
end
return ui
''', encoding="utf-8")
    await ed.call("sync_files")
    scene = (await ed.call("create_scene", name="Attached UI preview", type="UI"))["scene"]["id"]
    made = await ed.call("create_entity", scene=scene, type="ui_document", name="Attached UI")
    entity = made["entity_id"]
    await ed.call("set_properties", scene=scene, entity=entity, values={"uidocument": {"filename": "ui/attached.html"}})
    await ed.call("attach_script", scene=scene, entity=entity, path="scripts/PreviewUI.lua")
    await ed.call("render_ui_frames", scene=scene, times=[0.5], width=128, height=96, layout="frames", out="attached-frames")
    width, _, rgba = pngdiff.decode((ed.project / "attached-frames/frame_0.png").read_bytes())
    ed.check_eq(pixel(rgba, width, 48, 40), (0, 255, 0, 255), "attached init creates the UI-clock animation")
    ed.check_eq(pixel(rgba, width, 48, 72), (0, 0, 255, 255), "attached start creates the game-clock animation")
    state = await ed.call("editor_status")
    ed.check(not state.get("playing", False), "attached UI scripts run in the isolated preview")


async def test_invalid_times_and_output(ed: Editor):
    await setup(ed)
    for times in ([], [-1], [3601], [0] * 65):
        result = await ed.call_raw("render_ui_frames", {"document": "assets/ui/frames.html", "times": times})
        ed.check(result.is_error, f"invalid time set rejected: {times[:3]}")
    for out in ("../outside.png", ".jasperengine/sheet.png", "project.yaml"):
        result = await ed.call_raw("render_ui_frames", {"document": "assets/ui/frames.html", "times": [0], "out": out})
        ed.check(result.is_error, f"unsafe/protected output rejected: {out}")


async def test_watch_reloads_stylesheet_and_cancels(ed: Editor):
    await setup(ed)
    first = await render(ed, "watched", times=[0.5], watch=True)
    ed.check(first.value["watching"], "first sheet is ready while watch job stays running")
    job_id = first.value["job_id"]
    frame = ed.project / "watched" / "frame_0.png"
    old = frame.read_bytes()
    (ed.project / "assets/ui/frames.css").write_text(CSS.replace("CSS_COLOR", "#ffff00"), encoding="utf-8")
    await ed.call("sync_files")
    changed = False
    for _ in range(200):
        job = await ed.call("get_job", job_id=job_id)
        ed.check_eq(job["status"], "running", "watch remains active")
        if frame.read_bytes() != old:
            changed = True
            break
    ed.check(changed, "saving a linked stylesheet regenerates output")
    width, _, rgba = pngdiff.decode(frame.read_bytes())
    ed.check_eq(pixel(rgba, width, 48, 8), (255, 255, 0, 255), "regeneration uses fresh CSS")
    await ed.call("cancel_job", job_id=job_id)
    for _ in range(40):
        job = await ed.call("get_job", job_id=job_id)
        if job["status"] != "running":
            break
    ed.check_eq(job["status"], "cancelled", "cancel_job releases the watch and capture")
