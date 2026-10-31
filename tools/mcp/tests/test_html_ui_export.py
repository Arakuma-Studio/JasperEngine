"""Exporting HTML UI documents (phase 1, p1-editor, spec §3.8.8), through MCP.

- A document referring to a stylesheet, a PNG, a TTF (@font-face), an SVG and a Lottie file
  exports: the source export's assets/ and, with the native resource pack, resources.pak
  hold every one of them.
- A missing reference, a path outside the assets directory and a remote URL fail the export
  (Exporter::checkHtmlUIReferences; for .html documents the compat layer's checkDocument,
  phase-2 hook H14) with project-relative file:line:column in the message.
- JASPER_SLOW_BUILD=1 also builds the Desktop game and runs it (run_build) with the document
  on screen: built with the prebuilt glcore player runtime the run builds once (what runs is the
  same player; compiling the engine for it is test_build.py's check).

Runs on a scratch copy of the fixture project.
"""

import os
import shutil
import time
import asyncio
from pathlib import Path

import build_cache
from harness import Editor, REPO, skip
from htmlui_pixels import count_near

# the Desktop player test builds with the run's prebuilt glcore runtime (tools/mcp/build_cache.py)
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui", "env": build_cache.player_env("glcore", "release")}
CI = True

TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))

S: dict = {}

FIXTURES = REPO / "engine" / "tests" / "htmlui" / "fixtures"

HTML = """<html>
<head>
    <title>export</title>
    <link type="text/css" href="export.css"/>
</head>
<body>
    <div id="panel">
        <img src="img/checker.png"/>
        <img src="icons/icon.svg"/>
        <lottie src="anim/spin.json"/>
        <p>Exported</p>
    </div>
</body>
</html>
"""

CSS = """@font-face { font-family: Shipped; src: ui/fonts/Shipped.ttf; }
body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
       font-family: Shipped; font-size: 24px; color: #ffffff; }
div, p, img { display: block; }
#panel { position: absolute; left: 0; top: 0; width: 100%; height: 100%; background-color: #ff0000; }
"""

LOTTIE = '{"v":"5.7.4","fr":30,"ip":0,"op":30,"w":64,"h":64,"nm":"spin","ddd":0,"assets":[],"layers":[]}'

BROKEN = """<html>
<head>
    <link type="text/css" href="export.css"/>
    <link type="text/css" href="https://cdn.example.com/theme.css"/>
</head>
<body>
    <img src="img/nothere.png"/>
    <img src="../../outside.png"/>
    <img src="/tmp/absolute-only-here.png"/>
</body>
</html>
"""

SHIPPED = ["ui/export.html", "ui/export.css", "ui/img/checker.png", "ui/icons/icon.svg", "ui/anim/spin.json",
           "ui/fonts/Shipped.ttf"]


async def wait_job(ed: Editor, job_id: str, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)


async def export_source(ed: Editor, out: Path, **args):
    raw = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(out), "platform": "linux",
                                              "overwrite": True, "timeout_seconds": 900, **args})
    r = raw.value
    if isinstance(r, dict) and r.get("status") == "running":
        r = await wait_job(ed, r["job_id"], 900)
    return raw, r


async def _setup_document(ed: Editor):
    if "scene" in S:
        return
    ui = ed.project / "assets" / "ui"
    for sub in ("img", "icons", "anim", "fonts"):
        (ui / sub).mkdir(parents=True, exist_ok=True)
    shutil.copy(FIXTURES / "common" / "checker.png", ui / "img" / "checker.png")
    shutil.copy(FIXTURES / "common" / "icon.svg", ui / "icons" / "icon.svg")
    shutil.copy(FIXTURES / "fonts" / "Roboto-Bold.ttf", ui / "fonts" / "Shipped.ttf")
    (ui / "anim" / "spin.json").write_text(LOTTIE)
    await ed.call("write_file", path="assets/ui/export.html", content=HTML)
    await ed.call("write_file", path="assets/ui/export.css", content=CSS)

    made = await ed.call("create_scene", name="HtmlUiExport", type="UI")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Menu")
    S["menu"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": "assets/ui/export.html"}})
    saved = await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiExport")
    S["path"] = saved["scene"]["path"]
    await ed.call("edit_build_scenes", action="add", scene=S["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=S["path"])


async def test_source_export_ships_everything(ed: Editor):
    await _setup_document(ed)
    out = ed.scratch.root / "export-html-ui"
    raw, r = await export_source(ed, out)
    ed.check_eq(r["status"], "succeeded", f"{raw.text[:2000]}")
    assets = out / "project" / "assets"
    for rel in SHIPPED:
        ed.check((assets / rel).is_file(), f"{rel} exported to {assets}")


async def test_resource_pack_ships_everything(ed: Editor):
    await _setup_document(ed)
    await ed.call("set_project_settings", values={"player.pack_native_resources": True})
    try:
        out = ed.scratch.root / "export-html-ui-pack"
        raw, r = await export_source(ed, out)
        ed.check_eq(r["status"], "succeeded", f"{raw.text[:2000]}")
        pack = out / "project" / "assets" / "resources.pak"
        ed.check(pack.is_file(), f"no resource pack at {pack}")
        data = pack.read_bytes()
        for rel in SHIPPED:
            ed.check(rel.encode() in data, f"{rel} not in the resource pack")
            ed.check(not (out / "project" / "assets" / rel).exists(), f"{rel} also left loose next to the pack")
    finally:
        await ed.call("set_project_settings", values={"player.pack_native_resources": False})


async def test_broken_references_fail_the_export(ed: Editor):
    await _setup_document(ed)
    await ed.call("write_file", path="assets/ui/broken.html", content=BROKEN)
    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": "assets/ui/broken.html"}})
    await ed.call("save_scene", scene=S["scene"])
    await ed.console_since(None, limit=1)
    try:
        raw, r = await export_source(ed, ed.scratch.root / "export-html-ui-broken")
        ed.check_eq(r["status"], "failed", f"the export should fail: {raw.text[:2000]}")
        message = str(r.get("message", "")) + str(r.get("result", "")) + raw.text
        # .html documents are checked by the compat layer (phase-2 hook H14): its diagnostics, in
        # the common text format, with project-relative paths
        for needle in ("assets/ui/broken.html:4:", "https://cdn.example.com/theme.css", "HTMLUI-IMP-0001",
                       "assets/ui/broken.html:7:", "img/nothere.png", "HTMLUI-GEN-0001",
                       "assets/ui/broken.html:8:", "above the assets directory"):
            ed.check(needle in message, f"'{needle}' not in the failure: {message[:3000]}")
        # file:line:column exactly: the remote URL starts at column 33 of line 4
        ed.check("assets/ui/broken.html:4:33:" in message, f"remote URL position: {message[:3000]}")
        # "/tmp/..." is a root-relative URL in HTML (the assets directory), not a filesystem path
        ed.check("assets/ui/broken.html:9:" in message and "absolute-only-here.png" in message,
                 f"the root-relative image: {message[:3000]}")
    finally:
        await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": "assets/ui/export.html"}})
        await ed.call("save_scene", scene=S["scene"])


async def test_missing_document_fails_the_export(ed: Editor):
    await _setup_document(ed)
    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": "assets/ui/gone.html"}})
    await ed.call("save_scene", scene=S["scene"])
    try:
        raw, r = await export_source(ed, ed.scratch.root / "export-html-ui-gone")
        ed.check_eq(r["status"], "failed", f"{raw.text[:2000]}")
        message = str(r.get("message", "")) + raw.text
        ed.check("assets/ui/gone.html" in message and "missing" in message, f"{message[:2000]}")
    finally:
        await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": "assets/ui/export.html"}})
        await ed.call("save_scene", scene=S["scene"])


async def test_desktop_build_runs_the_document(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (the run builds its prebuilt runtime once)")
    await _setup_document(ed)
    # what runs is the game: the prebuilt player (the compile path is test_build.py's)
    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-html-ui-desktop"
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    job = r if r["status"] != "running" else await wait_job(ed, r["job_id"], TIMEOUT - 300, poll=5.0)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")
    raw = await ed.call_raw("run_build", {"seconds": 8, "screenshot_after_seconds": 6})
    ed.check(not raw.is_error, raw.text)
    ed.check(not raw.value["crashed"], f"the player crashed: {raw.value}")
    png = raw.images[0][0]
    (ed.scratch.root / "html-ui-player.png").write_bytes(png)
    ed.check(count_near(png, (255, 0, 0)) > 500, "the player shows the document")
