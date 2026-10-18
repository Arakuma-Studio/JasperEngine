"""Exporting a document with a <video> (phase 6, p6-video, docs/html-ui/phase-6-video.md §3.12.2, §5.6).

- The source export (linux and web) ships the .webm and the poster as assets; with the native
  resource pack they are in resources.pak; the exported shader headers hold the YUV conversion
  variant (htmluipost Yuv), so the shipped game converts frames without the runtime compiler.
- A missing video, poster or <source> fails the export with project-relative file:line:column
  (HtmlUIDependencies' VIDEO references, the compat layer's checkDocument).
- JASPER_SLOW_BUILD=1 also builds the Desktop player and runs it: the test card plays on screen
  (its colour bars are found in the player's screenshot, and the frame it shows moves on).

Runs on a scratch copy of the fixture project.
"""

import asyncio
import os
import shutil
import time
from pathlib import Path

from harness import Editor, REPO, skip
from htmlui_pixels import count_near

TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))
JOBS = 2

S: dict = {}

CLIPS = REPO / "engine" / "tests" / "htmlui" / "fixtures" / "video"

HTML = """<!DOCTYPE html>
<html>
<head><style>
    body { margin: 0; background-color: #000000; }
    #v { position: absolute; left: 0; top: 0; width: 100%; height: 100%; object-fit: fill; }
</style></head>
<body><video id="v" src="video/testcard.webm" poster="video/poster.png" autoplay muted loop></video></body>
</html>
"""

BROKEN = """<!DOCTYPE html>
<html>
<body>
<video src="video/nothere.webm"></video>
<video src="video/testcard.webm" poster="video/noposter.png"></video>
<video><source src="video/nosource.webm" type="video/webm"></video>
</body>
</html>
"""

SHIPPED = ["ui/video.html", "ui/video/testcard.webm", "ui/video/poster.png"]

# the test card's colour bars (75 % SMPTE-like: yellow, cyan, magenta), BT.709 limited range
BARS = [(191, 191, 0), (0, 191, 191), (191, 0, 191)]


async def wait_job(ed: Editor, job_id: str, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)


async def export_source(ed: Editor, out: Path, platform: str = "linux", **args):
    raw = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(out), "platform": platform,
                                              "overwrite": True, "timeout_seconds": 900, **args})
    r = raw.value
    if isinstance(r, dict) and r.get("status") == "running":
        r = await wait_job(ed, r["job_id"], 900)
    return raw, r


async def test_project_with_video(ed: Editor):
    video = ed.project / "assets" / "ui" / "video"
    video.mkdir(parents=True, exist_ok=True)
    shutil.copy(CLIPS / "testcard.webm", video / "testcard.webm")
    shutil.copy(CLIPS / "poster.png", video / "poster.png")
    await ed.call("write_file", path="assets/ui/video.html", content=HTML)
    made = await ed.call("create_scene", name="HtmlUiVideoExport", type="UI")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Player")
    S["entity"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["entity"], values={"uidocument": {"filename": "assets/ui/video.html"}})
    saved = await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiVideoExport")
    await ed.call("edit_build_scenes", action="add", scene=saved["scene"]["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scene"]["path"])
    tree = await ed.call("inspect_ui", entity="Player", selector="#v", depth=0)
    m = tree["elements"][0]["media"]
    ed.check_eq((m["videoWidth"], m["videoHeight"], m["readyState"]), (320, 180, 4), f"the video is loaded in the editor: {m}")
    entries = await ed.console_since(None, types=["error", "warning"], limit=1000)
    problems = [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "")]
    ed.check(not problems, f"[HTML UI] problems: {problems}")


async def test_source_export_ships_the_video(ed: Editor):
    for platform in ("linux", "web"):
        out = ed.scratch.root / f"export-video-{platform}"
        raw, r = await export_source(ed, out, platform)
        ed.check_eq(r["status"], "succeeded", f"{platform}: {raw.text[:2000]}")
        assets = out / "project" / "assets"
        for rel in SHIPPED:
            ed.check((assets / rel).is_file(), f"{platform}: {rel} exported to {assets}")
        ed.check_eq((assets / "ui/video/testcard.webm").read_bytes(), (CLIPS / "testcard.webm").read_bytes(), "the clip, byte for byte")
        # the YUV conversion pass is among the exported shaders of every backend the platform has
        headers = [p for p in out.rglob("*.h") if p.name in ("glsl410.h", "glsl300es.h", "spirv10.h", "hlsl5.h")]
        ed.check(headers, f"{platform}: no shader headers in {out}")
        for header in headers:
            text = header.read_text(errors="ignore")
            ed.check("htmluipost_Yuv" in text or "htmluipost:Yuv" in text, f"{platform}: {header.name} has no Yuv post variant")
        S["out_" + platform] = out


async def test_resource_pack_ships_the_video(ed: Editor):
    await ed.call("set_project_settings", values={"player.pack_native_resources": True})
    try:
        out = ed.scratch.root / "export-video-pack"
        raw, r = await export_source(ed, out)
        ed.check_eq(r["status"], "succeeded", f"{raw.text[:2000]}")
        pack = out / "project" / "assets" / "resources.pak"
        data = pack.read_bytes()
        for rel in SHIPPED:
            ed.check(rel.encode() in data, f"{rel} not in the resource pack")
        ed.check((CLIPS / "testcard.webm").read_bytes() in data, "the clip's bytes are in the pack, unchanged")
    finally:
        await ed.call("set_project_settings", values={"player.pack_native_resources": False})


async def test_missing_media_fail_the_export(ed: Editor):
    await ed.call("write_file", path="assets/ui/broken-video.html", content=BROKEN)
    await ed.call("set_properties", scene=S["scene"], entity=S["entity"], values={"uidocument": {"filename": "assets/ui/broken-video.html"}})
    await ed.call("save_scene", scene=S["scene"])
    try:
        raw, r = await export_source(ed, ed.scratch.root / "export-video-broken")
        ed.check_eq(r["status"], "failed", f"the export should fail: {raw.text[:2000]}")
        message = str(r.get("message", "")) + str(r.get("result", "")) + raw.text
        for needle in ("assets/ui/broken-video.html:4:", "video/nothere.webm", "assets/ui/broken-video.html:5:", "video/noposter.png",
                       "assets/ui/broken-video.html:6:", "video/nosource.webm", "HTMLUI-GEN-0001"):
            ed.check(needle in message, f"'{needle}' not in the failure: {message[:3000]}")
    finally:
        await ed.call("set_properties", scene=S["scene"], entity=S["entity"], values={"uidocument": {"filename": "assets/ui/video.html"}})
        await ed.call("save_scene", scene=S["scene"])


async def test_desktop_build_plays_the_video(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (compiles the engine)")
    out = ed.scratch.root / "export-video-desktop"
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), build_jobs=JOBS, timeout_seconds=30)
    job = r if r["status"] != "running" else await wait_job(ed, r["job_id"], TIMEOUT - 300, poll=15.0)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    shots = []
    for after in (4, 5):
        raw = await ed.call_raw("run_build", {"seconds": after + 2, "screenshot_after_seconds": after})
        ed.check(not raw.is_error, raw.text)
        ed.check(not raw.value["crashed"], f"the player crashed: {raw.value}")
        png = raw.images[0][0]
        (ed.scratch.root / f"html-ui-video-player-{after}.png").write_bytes(png)
        shots.append(png)
        for colour in BARS:
            ed.check(count_near(png, colour, tol=24) > 500, f"the player shows the test card's bar {colour} (screenshot {after} s)")
    ed.check(shots[0] != shots[1], "the video plays in the player (two screenshots a second apart differ)")
