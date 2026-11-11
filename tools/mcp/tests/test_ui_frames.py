"""Exact-time HTML UI previews, without starting Play, and dependency-driven regeneration."""

from __future__ import annotations

import asyncio
import json
import subprocess
import signal

import harness
import pngdiff
from harness import Editor, free_port

CI = True
PORT = free_port()
TOKEN = "ui-frame-test-token"
EDITOR = {"create": True, "name": "UI frame preview", "env": {"JASPER_TEXTURE_LOAD_DELAY_MS": "250"},
          "extra_args": ["--http-port", str(PORT)], "settings": {"mcp": {"http_token": TOKEN}}}
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
    await ed.call("set_properties", scene=scene, entity=entity, values={"uidocument": {"filename": "assets/ui/attached.html"}})
    await ed.call("attach_script", scene=scene, entity=entity, path="scripts/PreviewUI.lua")
    await ed.call("render_ui_frames", scene=scene, times=[0.5], width=128, height=96, layout="frames", out="attached-frames")
    width, _, rgba = pngdiff.decode((ed.project / "attached-frames/frame_0.png").read_bytes())
    ed.check_eq(pixel(rgba, width, 48, 40), (0, 255, 0, 255), "attached init creates the UI-clock animation")
    ed.check_eq(pixel(rgba, width, 48, 72), (0, 0, 255, 255), "attached start creates the game-clock animation")
    state = await ed.call("editor_status")
    ed.check(not state.get("playing", False), "attached UI scripts run in the isolated preview")
    watched = await ed.call("render_ui_frames", scene=scene, times=[0.5], width=128, height=96,
                            layout="frames", out="attached-watch", watch=True)
    job_id = watched["job_id"]
    try:
        frame = ed.project / "attached-watch/frame_0.png"
        previous = frame.read_bytes()
        script_path = scripts / "PreviewUI.lua"
        script_path.write_text(script_path.read_text(encoding="utf-8").replace("to=80", "to=40"), encoding="utf-8")
        await ed.call("sync_files")
        for _ in range(200):
            await ed.call("get_job", job_id=job_id)
            if frame.read_bytes() != previous:
                break
        width, _, rgba = pngdiff.decode(frame.read_bytes())
        ed.check_eq(pixel(rgba, width, 28, 40), (0, 255, 0, 255), "watch recompiles the changed attached Lua chunk")
        ed.check_eq(pixel(rgba, width, 28, 72), (0, 0, 255, 255), "watch recompiles lifecycle start code")
    finally:
        await ed.call("cancel_job", job_id=job_id)
        for _ in range(40):
            if (await ed.call("get_job", job_id=job_id))["status"] != "running":
                break


async def test_attached_preview_globals_are_private(ed: Editor):
    await setup(ed)
    scene = (await ed.call("create_scene", name="Private preview globals", type="UI"))["scene"]["id"]
    host = (await ed.call("create_entity", scene=scene, type="ui_document", name="Private globals"))["entity_id"]
    page = '''<!doctype html><html><head><style>
body {margin:0; width:128px; height:96px; background:#000;}
#box {position:absolute; left:0; top:0; width:16px; height:16px; background:#ff0000;}
</style></head><body><div id="box"></div></body></html>'''
    (ed.project / "assets/ui/private-globals.html").write_text(page, encoding="utf-8")
    scripts = ed.project / "scripts"
    scripts.mkdir(exist_ok=True)
    (scripts / "preview_global_counter.lua").write_text(
        "previewFrameRequired = (previewFrameRequired or 0) + 1; return previewFrameRequired", encoding="utf-8")
    await ed.call("sync_files")
    await ed.call("set_properties", scene=scene, entity=host,
                  values={"uidocument": {"filename": "assets/ui/private-globals.html"}})
    await ed.call("create_script", scene=scene, entity=host, kind="lua", name="PrivatePreviewGlobals", content='''
previewFrameTop = (previewFrameTop or 0) + 1
local required = require("preview_global_counter")
assert(require("_G") == _G)
local ui = {}
function ui:init()
    previewFrameN = (previewFrameN or 0) + 1
    _G.previewFrameExplicit = (_G.previewFrameExplicit or 0) + 1
    local doc = UIDocument(self.scene, self.entity)
    doc:getElementById("box"):setStyle("left", tostring((previewFrameN % 3) * 20) .. "px")
    if required ~= 1 or previewFrameTop ~= 1 or _G.previewFrameExplicit ~= 1 then
        doc:getElementById("box"):setStyle("background-color", "#0000ff")
    end
end
return ui
''')
    await ed.call("eval_lua", scene=scene, allow_edit_mode=True, code='''
_G.previewFrameN = 40
_G.previewFrameExplicit = 50
_G.previewFrameRequired = 60
_G.previewFrameTop = 70
''')
    try:
        result = await ed.call_raw("render_ui_frames", {"scene": scene, "times": [0, 0],
            "width": 128, "height": 96, "layout": "frames", "out": "private-globals"})
        live = await ed.call("eval_lua", scene=scene, allow_edit_mode=True,
            code="return previewFrameN, previewFrameExplicit, previewFrameRequired, previewFrameTop")
        ed.check_eq(live["values"], [40, 50, 60, 70], "preview leaves the live Lua globals unchanged")
        ed.check(not result.is_error, f"attached script uses a fresh private environment: {result.text}")
        first = (ed.project / "private-globals/frame_0.png").read_bytes()
        second = (ed.project / "private-globals/frame_1.png").read_bytes()
        ed.check_eq(first, second, "duplicate preview times do not share global writes")
        width, _, rgba = pngdiff.decode(first)
        ed.check_eq(pixel(rgba, width, 28, 8), (255, 0, 0, 255), "private counters start at one")
    finally:
        await ed.call("eval_lua", scene=scene, allow_edit_mode=True, code='''
_G.previewFrameN = nil; _G.previewFrameExplicit = nil; _G.previewFrameRequired = nil; _G.previewFrameTop = nil
''')
        await ed.call("clear_console")


async def test_callback_started_css_transition_has_its_frame_history(ed: Editor):
    await setup(ed)
    (ed.project / "assets/ui/mixed.html").write_text('''<!doctype html><html><head><style>
body {margin:0; width:128px; height:96px; background:#000;}
#box {position:absolute; left:0; top:0; width:16px; height:16px; background:#ff0000; transition:left 1s linear;}
</style></head><body><div id="box"></div><script type="text/lua">
local completions = 0
anime.createTimer({duration=500,onComplete=function()
    completions = completions + 1
    if completions > 1 then document:getElementById("box"):setStyle("background-color", "#0000ff") end
    document:getElementById("box"):setStyle("left","80px")
end})
</script></body></html>''', encoding="utf-8")
    await ed.call("sync_files")
    await ed.call("render_ui_frames", document="assets/ui/mixed.html", times=[1,1.5], fps=60,
                  width=128, height=96, layout="frames", out="mixed")
    for i, x in enumerate((48, 88)):
        width, _, rgba = pngdiff.decode((ed.project / "mixed" / f"frame_{i}.png").read_bytes())
        ed.check_eq(pixel(rgba, width, x, 8), (255, 0, 0, 255), "callback starts the transition at0.5s")
        ed.check_eq(pixel(rgba, width, 8, 8), (0, 0, 0, 255), "capture includes transition history")


async def test_lua_callback_errors_fail_without_publishing(ed: Editor):
    await setup(ed)
    broken = HTML.replace('duration = 1000, ease = "linear" })',
                          'duration = 1000, ease = "linear", onUpdate=function() error("preview callback failure") end })', 1)
    (ed.project / "assets/ui/broken.html").write_text(broken, encoding="utf-8")
    await ed.call("sync_files")
    result = await ed.call_raw("render_ui_frames", {"document": "assets/ui/broken.html", "times": [0.5],
                                                    "width": 128, "height": 96, "out": "broken.png"})
    ed.check(result.is_error and "preview callback failure" in result.text, "callback errors explain why preview failed")
    ed.check(not (ed.project / "broken.png").exists(), "a failed preview publishes no PNG")
    await ed.call("clear_console")


async def test_individual_pngs_use_straight_alpha(ed: Editor):
    await setup(ed)
    (ed.project / "assets/ui/alpha.html").write_text('''<!doctype html><html><head><style>
body {margin:0; width:128px; height:96px; background:transparent;}
#box {position:absolute; left:0; top:0; width:16px; height:16px; background:rgba(255,0,0,0.5);}
</style></head><body><div id="box"></div></body></html>''', encoding="utf-8")
    await ed.call("sync_files")
    await ed.call("render_ui_frames", document="assets/ui/alpha.html", times=[0], width=128, height=96,
                  layout="frames", out="alpha")
    width, _, rgba = pngdiff.decode((ed.project / "alpha/frame_0.png").read_bytes())
    red, green, blue, alpha = pixel(rgba, width, 8, 8)
    ed.check(red >= 254 and green == 0 and blue == 0 and 126 <= alpha <= 129, "PNG retains straight red with half alpha")
    ed.check_eq(pixel(rgba, width, 64, 64), (0, 0, 0, 0), "outside document art remains transparent")


async def test_callback_created_roots_start_at_their_birth_time(ed: Editor):
    await setup(ed)
    page = HTML[:HTML.index('<script type="text/lua">')] + '''<script type="text/lua">
anime.createTimer({duration=500,onComplete=function()
    anime.animate("#anime", {left={from=0,to=80}, duration=1000, ease="linear"})
    anime.animate("#game", {left={from=0,to=80}, duration=1000, ease="linear", clock="game"})
end})
</script></body></html>'''
    (ed.project / "assets/ui/birth.html").write_text(page, encoding="utf-8")
    await ed.call("sync_files")
    await ed.call("render_ui_frames", document="assets/ui/birth.html", times=[0.5,1], fps=60,
                  width=128, height=96, layout="frames", out="birth")
    for i, x in enumerate((8,48)):
        width, _, rgba = pngdiff.decode((ed.project / "birth" / f"frame_{i}.png").read_bytes())
        ed.check_eq(pixel(rgba, width, x,40), (0,255,0,255), "UI root begins at callback birth")
        ed.check_eq(pixel(rgba, width, x,72), (0,0,255,255), "game root begins at callback birth")


async def test_waits_for_image_assets_before_returning(ed: Editor):
    await setup(ed)
    (ed.project / "assets/ui/pixel.png").write_bytes(pngdiff.encode(4,4,bytes((255,0,255,255))*16))
    (ed.project / "assets/ui/image.html").write_text('''<!doctype html><html><head><style>
body {margin:0; width:128px; height:96px; background:#000;}
img {position:absolute; left:0; top:0; width:32px; height:32px;}
</style></head><body><img src="pixel.png"></body></html>''', encoding="utf-8")
    await ed.call("sync_files")
    await ed.call("render_ui_frames", document="assets/ui/image.html", times=[0], width=128, height=96,
                  layout="frames", out="loaded-image")
    width, _, rgba = pngdiff.decode((ed.project / "loaded-image/frame_0.png").read_bytes())
    ed.check_eq(pixel(rgba,width,8,8), (255,0,255,255), "delayed image is loaded instead of a stand-in")


async def test_real_cli_one_shot_and_watch(ed: Editor):
    await setup(ed)
    command = harness.editor_command() + ["call", "--port", str(PORT), "--token", TOKEN, "--json", "render_ui_frames",
        "--document", "assets/ui/frames.html", "--times", "0.5", "--width", "128", "--height", "96"]
    one = await asyncio.to_thread(subprocess.run, command + ["--out", "cli.png"], cwd=str(ed.scratch.root),
                                  capture_output=True, text=True, timeout=120)
    ed.check_eq(one.returncode, 0, f"CLI one-shot: {one.stderr}")
    ed.check((ed.project / "cli.png").is_file(), "CLI writes the requested sheet")
    ed.check_eq(json.loads(one.stdout)["structured"]["times"], [0.5], "CLI passes the exact sample")
    process = await asyncio.create_subprocess_exec(*command, "--layout", "frames", "--out", "cli-watch", "--watch",
        cwd=str(ed.scratch.root), stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    job_id = None
    try:
        line = await asyncio.wait_for(process.stdout.readline(), timeout=120)
        ed.check(bool(line), "watch returns its first result before waiting")
        result = json.loads(line)["structured"]
        ed.check(result["watching"] and result["status"] == "running", "CLI watch stays attached")
        job_id = result["job_id"]
        frame = ed.project / "cli-watch/frame_0.png"
        old = frame.read_bytes()
        (ed.project / "assets/ui/frames.css").write_text(CSS.replace("CSS_COLOR", "#00ffff"), encoding="utf-8")
        await ed.call("sync_files")
        for _ in range(200):
            await ed.call("get_job", job_id=job_id)
            if frame.read_bytes() != old:
                break
        width, _, rgba = pngdiff.decode(frame.read_bytes())
        ed.check_eq(pixel(rgba,width,48,8), (0,255,255,255), "CLI watch regenerates on a stylesheet save")
        ed.check(process.returncode is None, "CLI continues watching after regeneration")
    finally:
        if process.returncode is None:
            process.send_signal(signal.SIGINT)
        await asyncio.wait_for(process.communicate(), timeout=15)
        if job_id:
            for _ in range(40):
                job = await ed.call("get_job", job_id=job_id)
                if job["status"] != "running": break
            ed.check_eq(job["status"], "cancelled", "CLI Ctrl-C cancels the editor watch")


async def test_preview_private_require_cache_and_dependency_watch(ed: Editor):
    await setup(ed)
    scripts = ed.project / "scripts"
    scripts.mkdir(exist_ok=True)
    module = scripts / "motion_counter.lua"
    module.write_text("return {n=0}", encoding="utf-8")
    page = '''<!doctype html><html><head><style>
body {margin:0; width:128px; height:96px; background:#000;}
#box {position:absolute; left:0; top:0; width:16px; height:16px; background:#ff0000;}
</style></head><body><div id="box"></div><script type="text/lua">
local m=require("motion_counter"); m.n=m.n+1
anime.animate("#box", {left=m.n*20,duration=0})
</script></body></html>'''
    (ed.project / "assets/ui/modules.html").write_text(page, encoding="utf-8")
    await ed.call("sync_files")
    await ed.call("render_ui_frames", document="assets/ui/modules.html", times=[0,0], width=128,height=96,
                  layout="frames",out="module-duplicates")
    ed.check_eq((ed.project / "module-duplicates/frame_0.png").read_bytes(),
                (ed.project / "module-duplicates/frame_1.png").read_bytes(), "duplicate times get fresh private project modules")
    result = await ed.call("render_ui_frames", document="assets/ui/modules.html",times=[0],width=128,height=96,
                           layout="frames",out="module-watch",watch=True)
    job_id=result["job_id"]
    try:
        frame=ed.project/"module-watch/frame_0.png"
        old=frame.read_bytes()
        module.write_text("return {n=2}",encoding="utf-8")
        await ed.call("sync_files")
        for _ in range(200):
            await ed.call("get_job",job_id=job_id)
            if frame.read_bytes()!=old: break
        width,_,rgba=pngdiff.decode(frame.read_bytes())
        ed.check_eq(pixel(rgba,width,68,8),(255,0,0,255),"saving a required module recompiles the watched preview")
    finally:
        await ed.call("cancel_job",job_id=job_id)
        for _ in range(40):
            if (await ed.call("get_job",job_id=job_id))["status"]!="running": break


async def test_external_exposed_scene_references_are_rejected(ed: Editor):
    await setup(ed)
    other=(await ed.call("create_scene",name="Protected scene",type="UI"))["scene"]["id"]
    target=(await ed.call("create_entity",scene=other,type="ui_document",name="Protected"))["entity_id"]
    source=(await ed.call("create_scene",name="Referencing scene",type="UI"))["scene"]["id"]
    host=(await ed.call("create_entity",scene=source,type="ui_document",name="Host"))["entity_id"]
    await ed.call("set_properties",scene=source,entity=host,values={"uidocument":{"filename":"assets/ui/frames.html"}})
    script='''local ui={properties={{name="other",type="entity"}}}
function ui:init() self.other.name="leaked" end
return ui
'''
    await ed.call("create_script",scene=source,entity=host,kind="lua",name="CrossScenePreview",content=script)
    await ed.call("set_script_properties",scene=source,entity=host,values={"other":{"scene":other,"entity":target}})
    result=await ed.call_raw("render_ui_frames",{"scene":source,"times":[0.5],"width":128,"height":96,"out":"external.png"})
    ed.check(result.is_error and "external scene" in result.text,"external scene references fail before script init")
    tree=json.dumps(await ed.call("get_scene_tree",scene=other))
    ed.check("Protected" in tree and "leaked" not in tree,"the other scene is unchanged")
    ed.check(not(ed.project/"external.png").exists(),"unsafe preview publishes no output")


async def test_ownerless_timer_and_coroutine_scheduling_is_rejected(ed: Editor):
    await setup(ed)
    for index,code in enumerate(('Timer.every(0.1,function() end,{clock="real"})',
                                  'Coroutine.start(function() while true do Timer.wait(0.1) end end)')):
        page=HTML[:HTML.index('<script type="text/lua">')]+ '<script type="text/lua">'+code+'</script></body></html>'
        path=f"assets/ui/no-scheduler-{index}.html"
        (ed.project/path).write_text(page,encoding="utf-8")
        await ed.call("sync_files")
        result=await ed.call_raw("render_ui_frames",{"document":path,"times":[0],"width":128,"height":96})
        ed.check(result.is_error and "anime.createTimer" in result.text,"game scheduling gives the supported UI alternative")
        await ed.call("clear_console")
    ed.check(not(await ed.call("editor_status")).get("playing",False),"rejected scheduling leaves the editor responsive and stopped")


async def test_runaway_inline_and_start_callbacks_are_bounded(ed: Editor):
    await setup(ed)
    page=HTML[:HTML.index('<script type="text/lua">')]+ '<script type="text/lua">while true do end</script></body></html>'
    (ed.project/"assets/ui/runaway.html").write_text(page,encoding="utf-8")
    await ed.call("sync_files")
    result=await ed.call_raw("render_ui_frames",{"document":"assets/ui/runaway.html","times":[0],"width":128,"height":96},timeout=30)
    ed.check(result.is_error and "exceeded 5 seconds" in result.text,"runaway inline Lua returns a bounded error")
    await ed.call("clear_console")
    scene=(await ed.call("create_scene",name="Runaway start",type="UI"))["scene"]["id"]
    host=(await ed.call("create_entity",scene=scene,type="ui_document",name="Runaway"))["entity_id"]
    plain=HTML[:HTML.index('<script type="text/lua">')]+"</body></html>"
    (ed.project/"assets/ui/start.html").write_text(plain,encoding="utf-8")
    await ed.call("set_properties",scene=scene,entity=host,values={"uidocument":{"filename":"assets/ui/start.html"}})
    await ed.call("create_script",scene=scene,entity=host,kind="lua",name="RunawayStart",
                  content="local ui={} function ui:start() while true do end end return ui")
    result=await ed.call_raw("render_ui_frames",{"scene":scene,"times":[0],"width":128,"height":96},timeout=30)
    ed.check(result.is_error and "exceeded 5 seconds" in result.text,"runaway lifecycle start returns a bounded error")
    await ed.call("clear_console")
    ed.check(not(await ed.call("editor_status")).get("playing",False),"the editor remains responsive after the runaway scripts")


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
