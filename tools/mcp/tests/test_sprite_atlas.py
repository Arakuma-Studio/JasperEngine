"""Sprite Atlases and texture sheets over MCP (docs/specs/S45-sprite-atlas.md R2, R10-R12, R16-R19,
R33-R36; AC6, AC7, AC11-AC13, AC16, AC17), on a new project holding three item textures, a
`coin@8x8.png` grid and S44's hero_idle.aseprite:

- the `sprites` group (edit_sprite_atlas, get_sprite_atlas) is hidden until tool_groups enables it;
- AC16: an edit_sprite_atlas preview writes nothing (file hashes) and returns the page images;
  output: true creates the file and builds it (job polled with get_job), one undo deletes it;
  get_sprite_atlas search / limit / page and the member lookup; set_sprite_frames {sheet: atlas,
  frame: "sword"}; typed errors unknown_frame, frame_too_large, invalid_settings;
- get_asset_info: a texture's sprite_mode (the name@WxH grid) and packed_in;
- AC11: set_sprite_frames {texture, slice} writes the texture's import.spriteSheet and makes the
  Sprite a sheet sprite, one undo puts both back; inline: true keeps the inline frames;
- AC7: a scene showing hero_idle.aseprite and items/sword.png draws the same picture with the mode
  Disabled and Enabled, and Enabled draws both from one page (the sprites' textures, fewer draws);
  Enabled for Builds draws the sources; two atlases packing one source warn on both;
- AC12: a clip keying sprite.frame on a sprite whose source is packed shows the same frames as
  with the mode Disabled (game time, fixed 60 fps);
- AC6: one member's new pixels rewrite only its page (page file hashes) and the others keep their
  files; a member of a new size relayouts; the build reports progress and cancels; a changed
  member reaches its page within 300 ms (20 runs, harness.over_budget);
- AC17: Lua loads the atlas, finds sword, sets a Sprite's sheet and frameName, logs frameCount and
  getFrame("sword").rect;
- AC13: the Linux and web source exports ship the atlas under assets/je-imported/ (the web max
  size override: 2 pages instead of 1), no member PNG only sprites read, and the remaps in the index.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import shutil
import struct
import time
import zlib
from pathlib import Path

import harness
import pngdiff
from harness import REPO, Editor, TestFailure

CI = True
EDITOR = {"create": True, "name": "SpriteAtlas"}

IDLE_SRC = REPO / "editor" / "tests" / "fixtures" / "aseprite" / "s44-mcp" / "hero_idle.aseprite"
ITEMS = ["assets/items/sword.png", "assets/items/shield.png", "assets/items/gem.png"]
COIN = "assets/items/coin@8x8.png"
IDLE = "assets/sprites/hero_idle.aseprite"
ATLAS = "assets/ui/items.spriteatlas"
S: dict = {}


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def png(width: int, height: int, pixel) -> bytes:
    rgba = bytearray()
    for y in range(height):
        for x in range(width):
            rgba += bytes(pixel(x, y))
    return pngdiff.encode(width, height, bytes(rgba))


def item_png(seed: int, size: int = 24, inset: int = 3) -> bytes:
    """An opaque box with a transparent margin (the atlas trims it), coloured by seed"""
    def pixel(x, y):
        if inset <= x < size - inset - seed % 2 and inset + seed % 3 <= y < size - inset:
            return ((x * 9 + seed * 50) & 255, (y * 11 + seed * 30) & 255, (seed * 70 + 40) & 255, 255)
        return (seed * 20 & 255, 7, 9, 0)
    return png(size, size, pixel)


def hashes(root: Path) -> dict:
    out = {}
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        if rel.startswith(".jasperengine") or not path.is_file() or rel.startswith("editor") or rel.endswith(".log"):
            continue
        out[rel] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


async def wait_job(ed: Editor, job_id: str, timeout: float = 300.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(0.3)


async def atlas_call(ed: Editor, tool: str, expect_error: bool = False, **args):
    """The tool's result (a finished job's result when it ran as one) and its images"""
    raw = await ed.call_raw(tool, args)
    value = raw.structured if raw.structured is not None else raw.value
    images = [img for img, _ in raw.images]
    if isinstance(value, dict) and value.get("status") == "running" and value.get("job_id"):
        job = await wait_job(ed, value["job_id"])
        result = job.get("result", {})
        images = [base64.b64decode(i) for i in result.pop("images", [])]
        if job["status"] != "succeeded":
            if expect_error:
                return result, images
            ed.fail(f"{tool} failed: {job}")
        return result, images
    if raw.is_error and not expect_error:
        ed.fail(f"{tool}: {raw.text}")
    if raw.is_error:
        return value if isinstance(value, dict) else {"text": raw.text}, images
    return value, images


async def built(ed: Editor, path: str = ATLAS, timeout: float = 120.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        got = await ed.call("get_sprite_atlas", path=path, limit=200)
        if got.get("build_state") in ("built", "failed"):
            return got
        if time.monotonic() > deadline:
            ed.fail(f"{path} never built: {got}")
        await ed.call("wait", seconds=0.2)


def atlas_cache(ed: Editor) -> list[Path]:
    root = ed.project / ".jasperengine" / "cache" / "atlas" / "v1"
    return sorted(root.rglob("page*.png")) if root.is_dir() else []


async def test_setup_and_group_hidden(ed: Editor):
    for i, item in enumerate(ITEMS):
        (ed.project / item).parent.mkdir(parents=True, exist_ok=True)
        (ed.project / item).write_bytes(item_png(i))
    # four 8 x 8 coins and an empty cell, 3 columns over 2 rows (the last cell transparent)
    (ed.project / COIN).write_bytes(png(24, 16, lambda x, y: (200, 180 - y * 5, x * 7, 255) if not (x >= 16 and y >= 8) and (x % 8) < 7 else (0, 0, 0, 0)))
    (ed.project / IDLE).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(IDLE_SRC, ed.project / IDLE)
    (ed.project / "assets" / "ui").mkdir(parents=True, exist_ok=True)
    await ed.call("sync_files", paths=ITEMS + [COIN, IDLE])
    S["main"] = (await ed.call("list_scenes"))["selected_scene_id"]

    names = {t.name for t in await ed.tools(groups=None)}
    check("edit_sprite_atlas" not in names and "get_sprite_atlas" not in names, "the atlas tools are hidden in a new session")
    await ed.call("tool_groups", enable=["sprites"])
    names = {t.name for t in await ed.tools(groups=None)}
    check({"edit_sprite_atlas", "get_sprite_atlas"} <= names, "tool_groups enables them")


async def test_preview_writes_nothing(ed: Editor):
    before = hashes(ed.project)
    r, images = await atlas_call(ed, "edit_sprite_atlas", path=ATLAS, create=True, add=["assets/items", IDLE])
    check(r.get("written") is False and "output: true" in r.get("next", ""), f"a preview: {r}")
    check(r["frame_count"] >= 3 + 4 + 6, f"every member's frames: {r}")
    check(images and pngdiff.decode(images[0])[0] > 0, "the page images come back")
    check(r["usage_percent"] > 0 and r["compression"] == "lossless", f"usage, lossless (an .aseprite member): {r}")
    check(hashes(ed.project) == before, "the preview wrote no project file")
    check(not (ed.project / ATLAS).exists(), "no atlas file")
    r, images = await atlas_call(ed, "edit_sprite_atlas", path=ATLAS, create=True, add=["assets/items"], include_pages=False)
    check(not images, f"include_pages: false: no images: {r}")


async def test_typed_errors(ed: Editor):
    r, _ = await atlas_call(ed, "edit_sprite_atlas", expect_error=True, path=ATLAS, create=True, add=ITEMS, settings={"max_size": 16})
    err = r.get("error", {})
    check(err.get("kind") == "frame_too_large" and err.get("field") and err.get("fix"), f"frame_too_large names the frame: {r}")
    r, _ = await atlas_call(ed, "edit_sprite_atlas", expect_error=True, path=ATLAS, create=True, add=ITEMS, settings={"padding": 99})
    check(r.get("error", {}).get("kind") == "invalid_settings" and r["error"].get("field") == "padding", f"invalid_settings: {r}")
    r, _ = await atlas_call(ed, "edit_sprite_atlas", expect_error=True, path=ATLAS, add=ITEMS)
    check(r.get("error", {}).get("kind") == "missing_member" and "create" in r["error"].get("fix", ""), f"no atlas: {r}")


async def test_output_builds_and_undo(ed: Editor):
    r, images = await atlas_call(ed, "edit_sprite_atlas", path=ATLAS, create=True, add=["assets/items", IDLE], output=True)
    check(r.get("written") is True and (ed.project / ATLAS).is_file(), f"written: {r}")
    check(r["frame_count"] >= 13 and len(r["pages"]) >= 1, f"built: {r}")
    text = json.loads((ed.project / ATLAS).read_text())
    check(text["format"] == "jasperengine.spriteatlas" and text["version"] == 1 and len(text["guid"]) == 32, f"the file: {text}")
    got = await built(ed)
    check(got["build_state"] == "built" and got["mode"] == "enabled", f"get_sprite_atlas: {got}")
    # search, limit, page filter
    found = await ed.call("get_sprite_atlas", path=ATLAS, search="sword")
    check(found["frames_matched"] == 1 and found["frames"][0]["name"] == "sword", f"search sword: {found['frames']}")
    limited = await ed.call("get_sprite_atlas", path=ATLAS, limit=2)
    check(len(limited["frames"]) == 2 and limited["frames_matched"] >= 13, f"limit 2: {limited['frames_matched']}")
    paged = await ed.call("get_sprite_atlas", path=ATLAS, page=0, search="member:hero_idle")
    check(paged["frames"] and all(f["page"] == 0 and "hero_idle" in f["member"] for f in paged["frames"]), f"page + member: {paged['frames']}")
    member = await ed.call("get_sprite_atlas", path=ITEMS[0])
    check(member["packed_in"] == ATLAS and member["placements"] and member["placements"][0]["name"] == "sword", f"member lookup: {member}")
    info = await ed.call("get_asset_info", path=ITEMS[0])
    check(info.get("packed_in", {}).get("atlas") == ATLAS, f"get_asset_info packed_in: {info.get('packed_in')}")
    # one undo deletes the file
    await ed.call("undo", scope="project")
    check(not (ed.project / ATLAS).exists(), "one undo deletes the atlas")
    await ed.call("redo", scope="project")
    check((ed.project / ATLAS).is_file(), "redo writes it again")
    await built(ed)


async def test_texture_sheets(ed: Editor):
    info = await ed.call("get_asset_info", path=COIN)
    mode = info.get("sprite_mode", {})
    check(mode.get("mode") == "grid" and mode.get("stored") == "file_name" and mode.get("frame_count") == 5,
          f"coin@8x8 is a grid by its name, the empty cell skipped: {mode}")
    check(not (ed.project / (COIN + ".meta")).exists() or "spriteSheet" not in (ed.project / (COIN + ".meta")).read_text(),
          "nothing is written to the meta")
    r = await ed.call("set_sprite_frames", scene=S["main"], create={"name": "Coin"}, sheet=COIN, frame="coin_3")
    check(r["frame"] == 3 and r["frame_count"] == 5, f"a coin frame by name: {r}")


async def test_slicer(ed: Editor):
    """AC11: slice writes the texture's Sprite Mode and makes the Sprite a sheet sprite, one undo step"""
    texture = ITEMS[2]
    meta = ed.project / (texture + ".meta")
    old_meta = meta.read_bytes() if meta.exists() else None
    made = await ed.call("set_sprite_frames", scene=S["main"], create={"name": "Sliced"}, texture=texture,
                         slice={"tile_size": [12, 12]}, frame=1)
    check(made.get("sheet") == texture and made["frame"] == 1 and len(made["frames"]) == 4, f"sliced: {made}")
    check("spriteSheet" in meta.read_text() and "multiple" in meta.read_text(), "the meta has the Sprite Mode")
    sprite = (await ed.call("get_entity", scene=S["main"], entity="Sliced", components=["sprite"]))["components"]["sprite"]
    check(texture in json.dumps(sprite.get("sheet")) and sprite.get("frame") == 1, f"a sheet sprite: {sprite}")
    await ed.call("undo", scene=S["main"])
    now = meta.read_bytes() if meta.exists() else None
    check(now == old_meta or (now is not None and b"spriteSheet" not in now), "one undo restores the meta")
    check(not (await ed.call("find_entities", scene=S["main"], name="Sliced")).get("entities"), "and the sprite")
    # inline: true keeps the legacy per-sprite frames
    r = await ed.call("set_sprite_frames", scene=S["main"], create={"name": "Inline"}, texture=texture, slice={"tile_size": [12, 12]}, inline=True)
    check(len(r["frames"]) == 4 and "sheet" not in r, f"inline frames: {r}")
    check(old_meta is None or meta.read_bytes() == old_meta or b"spriteSheet" not in meta.read_bytes(), "inline writes no Sprite Mode")


async def test_set_sprite_frames_atlas_frame(ed: Editor):
    r = await ed.call("set_sprite_frames", scene=S["main"], create={"name": "FromAtlas"}, sheet=ATLAS, frame="sword")
    check(r["frame_name"] == "sword", f"an atlas frame by name: {r}")
    bad = await ed.call("set_sprite_frames", expect_error=True, scene=S["main"], entity="FromAtlas", sheet=ATLAS, frame="nope")
    check("unknown_frame" in bad, f"unknown_frame: {bad}")


async def screenshot(ed: Editor, scene) -> bytes:
    raw = await ed.call_raw("screenshot", {"view": "game", "scene": scene, "max_width": 0})
    check(not raw.is_error and raw.images, f"screenshot: {raw.text[:300]}")
    return raw.images[0][0]


async def textures_of(ed: Editor, scene, names) -> list[str]:
    out = []
    for name in names:
        got = await ed.call("get_entity", scene=scene, entity=name, components=["mesh"], properties=["submeshes"])
        out.append(json.dumps(got["components"]["mesh"]))
    return out


async def set_mode(ed: Editor, mode: str):
    await ed.call("set_project_settings", values={"sprite_atlas.mode": mode})
    await ed.call("wait", frames=10)


async def test_transparent_atlas(ed: Editor):
    """AC7: the atlas changes no scene and no picture; Enabled draws both from one page"""
    sid = (await ed.call("create_scene", name="AtlasView", type="2d"))["scene"]["id"]
    S["view"] = sid
    await ed.call("set_project_settings", values={"graphics.canvas_width": 160, "graphics.canvas_height": 90})
    await ed.call("set_sprite_frames", scene=sid, create={"name": "Hero", "position": [50, 45, 0]}, sheet=IDLE, frame=2, clips=False)
    await ed.call("set_sprite_frames", scene=sid, create={"name": "Sword", "position": [110, 45, 0]}, sheet=ITEMS[0])
    for name in ("Hero", "Sword"):
        await ed.call("set_transform", scene=sid, entity=name, scale=[2, 2, 1])
        await ed.call("set_texture_settings", scene=sid, entity=name, component="mesh", property="submeshes[0].material.baseColorTexture",
                      min_filter="nearest", mag_filter="nearest")
    await set_mode(ed, "disabled")
    disabled = await screenshot(ed, sid)
    tex_disabled = await textures_of(ed, sid, ["Hero", "Sword"])
    await set_mode(ed, "enabled")
    await built(ed)
    await ed.call("wait", frames=10)
    enabled = await screenshot(ed, sid)
    tex_enabled = await textures_of(ed, sid, ["Hero", "Sword"])
    check(pngdiff.decode(disabled)[2] == pngdiff.decode(enabled)[2], "the same picture with the atlas Disabled and Enabled")
    pages = [p for p in atlas_cache(ed)]
    check(any(p.name in tex_enabled[0] for p in pages) and any(p.name in tex_enabled[1] for p in pages),
          f"Enabled: both draw from atlas pages: {tex_enabled}")
    check(tex_disabled[0] != tex_enabled[0] and tex_disabled[1] != tex_enabled[1], "Disabled: their own textures")
    stats = await ed.call("get_render_stats", view="game")
    S["enabled_draws"] = stats.get("draw_calls")
    # Enabled for Builds: the editor draws the sources
    await set_mode(ed, "enabled_for_builds")
    check(await textures_of(ed, sid, ["Hero", "Sword"]) == tex_disabled, "Enabled for Builds: the editor draws the sources")
    await set_mode(ed, "enabled")
    # the scene file names the sources only
    await ed.call("save_scene", scene=sid, path="scenes/AtlasView")
    saved = (ed.project / "scenes" / "AtlasView.scene").read_text()
    check(".spriteatlas" not in saved and ".jasperengine" not in saved and ITEMS[0] in saved, "the saved scene references the sources, no atlas")
    # double packing warns on both atlases; the one whose path sorts first draws it
    second = "assets/ui/zz_more.spriteatlas"
    await atlas_call(ed, "edit_sprite_atlas", path=second, create=True, add=[ITEMS[0]], output=True)
    a = await built(ed, ATLAS)
    b = await built(ed, second)
    for got, which in ((a, ATLAS), (b, second)):
        check(any(w["kind"] == "double_packed" and ITEMS[0] in w["field"] for w in got["warnings"]), f"{which} warns: {got['warnings']}")
    member = await ed.call("get_sprite_atlas", path=ITEMS[0])
    check(member["packed_in"] == ATLAS and member["atlases"] == [ATLAS, second], f"the first path draws it: {member}")
    await ed.call("undo", scope="project")


async def test_clip_on_atlas(ed: Editor):
    """AC12: a clip keying sprite.frame on a packed sheet shows the frames it shows unpacked"""
    sid = S["view"]
    clip = "assets/anim/hero_frames.anim"
    keys = [{"time": i * 0.25, "value": i} for i in range(4)]
    await ed.call("edit_animation", clip=clip, create={}, wrap="once", length=1.0, tracks=[
        {"path": "", "channel": "property", "component": "sprite", "property": "frame", "keys": keys}])
    await ed.call("add_component", scene=sid, entity="Hero", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": clip}], "playAutomatically": True}})
    samples = {}
    for mode in ("disabled", "enabled"):
        await set_mode(ed, mode)
        if mode == "enabled":
            await built(ed)
        await ed.call("select_scene", scene=sid)
        await ed.call("play", action="start", fixed_fps=60, auto_pause=True)
        try:
            seen, t = [], 0.0
            for at in (0.125, 0.375, 0.625, 0.875):
                await ed.call("wait", game_seconds=round(at - t, 6))
                t = at
                seen.append((await ed.call("eval_lua", code="local s = object('Hero', 'Sprite') return s.frame, s.frameName"))["values"])
            samples[mode] = seen
        finally:
            await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    check(samples["disabled"] == samples["enabled"], f"the same frames: {samples}")
    check(len({json.dumps(s) for s in samples["enabled"]}) >= 3, f"the clip steps frames: {samples}")


async def test_incremental(ed: Editor):
    """AC6: a member's new pixels rewrite only its page; a new size relayouts; progress, cancel, 300 ms"""
    big = ["assets/big/a.png", "assets/big/b.png", "assets/big/c.png"]
    for i, path in enumerate(big):
        (ed.project / path).parent.mkdir(parents=True, exist_ok=True)
        (ed.project / path).write_bytes(item_png(10 + i, size=60, inset=0))
    await ed.call("sync_files", paths=big)
    path = "assets/ui/big.spriteatlas"
    await atlas_call(ed, "edit_sprite_atlas", path=path, create=True, add=big, settings={"max_size": 64}, output=True)
    first = await built(ed, path)
    check(len(first["pages"]) == 3, f"three pages of one member each: {first['pages']}")

    def page_files() -> dict:
        guid = json.loads((ed.project / path).read_text())["guid"]
        folder = ed.project / ".jasperengine" / "cache" / "atlas" / "v1" / guid / "desktop"
        return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.glob("page*.png")}
    before = page_files()
    timings = []
    for run in range(20):
        (ed.project / big[1]).write_bytes(item_png(40 + run, size=60, inset=0))
        count = (await ed.call("get_sprite_atlas", path=path)).get("frame_count")
        start = time.monotonic()
        await ed.call("sync_files", paths=[big[1]])
        deadline = time.monotonic() + 30
        while True:
            got = await ed.call("get_sprite_atlas", path=path, include_pages=False)
            files = page_files()
            new = set(files) - set(before)
            if got.get("build_state") == "built" and new:
                break
            if time.monotonic() > deadline:
                ed.fail(f"run {run}: the change never reached a page: {got}")
            await asyncio.sleep(0.01)
        timings.append((time.monotonic() - start) * 1000.0)
        check(count == got["frame_count"], "the same frames")
        if run == 0:
            changed = [n for n in files if n not in before]
            check(len(changed) == 1, f"only the changed member's page is new: {changed}")
            check(len([n for n in before if n in files]) == 2, "the other two pages keep their files")
        before = files
    timings.sort()
    over = harness.over_budget("a changed member visible (median of 20)", {"median": timings[len(timings) // 2]}, 300.0, unit="ms")
    check(not over, f"over budget: {over}")
    # a member of a new size: the layout changes
    (ed.project / big[2]).write_bytes(item_png(3, size=40, inset=0))
    await ed.call("sync_files", paths=[big[2]])
    await ed.call("wait", seconds=1.0)
    after = await built(ed, path)
    sizes = sorted(tuple(p["size"]) for p in after["pages"])
    check((40 + 3, 40 + 3) in sizes or any(s[0] < 60 for s in sizes), f"a smaller page for the new size: {sizes}")
    # progress and cancel: a build cancelled through its job
    raw = await ed.call_raw("edit_sprite_atlas", {"path": path, "settings": {"padding": 2}, "output": True})
    value = raw.structured or {}
    if value.get("status") == "running":
        await ed.call("cancel_job", job_id=value["job_id"])
        job = await wait_job(ed, value["job_id"])
        check(job["status"] in ("cancelled", "failed", "succeeded"), f"the job ends: {job}")
    await built(ed, path)


async def test_lua(ed: Editor):
    """AC17: an atlas is just a sheet in Lua"""
    sid = S["view"]
    await ed.call("select_scene", scene=sid)
    await ed.call("play", action="start", fixed_fps=60, auto_pause=True)
    try:
        code = ("local atlas = SpriteSheet.load('" + ATLAS + "') "
                "local sword = atlas:getFrame('sword') "
                "local s = object('Sword', 'Sprite') s.sheet = '" + ATLAS + "' s.frameName = 'sword' "
                "print('atlas frames', atlas.frameCount, 'sword', sword.rect.width, sword.rect.height, 'sprite', s.frameCount, s.frameName) "
                "return atlas.frameCount, atlas:findFrame('sword') >= 0, s.frameName, s.frameCount, sword.rect.width")
        values = (await ed.call("eval_lua", code=code))["values"]
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    got = await ed.call("get_sprite_atlas", path=ATLAS, search="sword")
    check(values[0] == got["frame_count"] and values[1] is True and values[2] == "sword" and values[3] == got["frame_count"], f"Lua: {values}")
    check(values[4] == got["frames"][0]["rect"][2], f"getFrame('sword').rect: {values} vs {got['frames'][0]}")
    console = await ed.call("read_console", limit=50)
    check("atlas frames" in json.dumps(console), "the Console has the line")


async def test_export(ed: Editor):
    """AC13: the atlas ships per family (web override: 2 pages instead of 1), atlas-only textures stay out"""
    ed.require_tools("build_project", "edit_build_scenes")
    path = "assets/ui/export.spriteatlas"
    only = ["assets/export/a.png", "assets/export/b.png"]
    for i, p in enumerate(only):
        (ed.project / p).parent.mkdir(parents=True, exist_ok=True)
        (ed.project / p).write_bytes(item_png(60 + i, size=40, inset=0))
    await ed.call("sync_files", paths=only)
    await atlas_call(ed, "edit_sprite_atlas", path=path, create=True, add=only, output=True)
    await atlas_call(ed, "edit_sprite_atlas", path=path, platform="web", settings={"max_size": 64}, output=True)
    desktop = await built(ed, path)
    check(len(desktop["pages"]) == 1, f"desktop: one page: {desktop['pages']}")
    web = await ed.call("get_sprite_atlas", path=path, platform="web")
    if web.get("status") == "running":
        web = (await wait_job(ed, web["job_id"]))["result"]
    check(len(web["pages"]) == 2, f"web (max 64): two pages: {web['pages']}")
    sid = (await ed.call("create_scene", name="ExportAtlas", type="2d"))["scene"]["id"]
    for i, p in enumerate(only):
        await ed.call("set_sprite_frames", scene=sid, create={"name": f"S{i}", "position": [40 + 60 * i, 45, 0]}, sheet=p)
    scene_path = (await ed.call("save_scene", scene=sid, path="scenes/ExportAtlas"))["scene"]["path"]
    await ed.call("edit_build_scenes", action="add", scene=scene_path)
    await ed.call("edit_build_scenes", action="set_start", scene=scene_path)
    for platform, pages in (("linux", 1), ("web", 2)):
        out = ed.scratch.root / f"export-atlas-{platform}"
        r = await ed.call("build_project", mode="source", output_dir=str(out), platform=platform, timeout_seconds=900)
        if r.get("status") == "running":
            r = await wait_job(ed, r["job_id"], 900)
        check(r["status"] == "succeeded", f"{platform}: {r!r}")
        assets = out / "project" / "assets"
        if not assets.is_dir():
            assets = next(p for p in out.rglob("assets") if p.is_dir() and (p / "je-imported").is_dir())
        index = json.loads((assets / "je-imported" / "index.json").read_text())["files"]
        family = "web" if platform == "web" else "desktop"
        atlas_pages = [k for k in index if k.startswith(path + "#image#")]
        check(len(atlas_pages) == pages, f"{platform}: {pages} page(s): {atlas_pages}")
        check(all(f"/atlas/" in index[k] and f"/{family}/" in index[k] for k in atlas_pages), f"{platform}: the {family} family: {atlas_pages}")
        for p in only:
            check(f"{p}#sheet" in index and f"{p}#atlas" in index, f"{platform}: {p}'s remap ships")
            check(not (assets / p).exists(), f"{platform}: {p}, read only by sprites, is left out")
        for k in atlas_pages:
            check((assets / index[k]).is_file() or (assets / "je-cooked").is_dir(), f"{platform}: {k} ships")
