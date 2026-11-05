"""Aseprite import over MCP (docs/specs/S44-aseprite-import.md R16-R18, R21, R24, R25, R33, R35,
R36, R38, R47, R50, R51; AC10-AC13, AC17, AC19, AC24), on a new project holding asefile's
layers_and_tags (hero.aseprite: 16x16, 4 frames, 6 layers, tags T1 0-1, T3, T2) and s44-mcp's
hero_idle (32x32, 6 frames, tags walk 0-3 and Idle 4-5 ping-pong x3, slice hurt moving on frame 1
and hidden from frame 4, an events cel onStep("left", 1) on frame 1, sprite properties
light2d.intensity = 2.5 and note):

- a project without .aseprite files has no import cache, no [Aseprite] Console line, no import job;
- the `sprites` group is hidden until tool_groups enables it;
- the import: get_asset_info has the summary, the resolved settings, the outputs (clip references)
  and import_state imported; view_asset is a contact sheet;
- set_sprite_import_settings: a preview changes no file (hashes); typed errors for an unknown layer
  and an unknown key; output: true writes the meta and reimports, one project undo puts the old
  meta back;
- tag clips: get_animation reads `<file>#T1`; edit_animation on it fails with read_only_clip;
- set_sprite_frames {create, sheet}: a sheet Sprite, frame by name, the Animation Player with the
  tag clips (Default Clip: the first tag, there is no idle), unknown_frame; one undo removes it;
- Lua in play: playTag, tag, getTag().repeat (hero_idle's Idle: 3), SpriteSheet.load, frameName;
- AC10: an Animation Player playing hero_idle#walk shows each frame at the game times the file's
  durations give (fixed 60 fps), and frame 1's events cel is the clip's onStep("left", 1) event;
- AC24: a .anim keying sprite.frame 0..3 shows those sheet frames by index; get_animation's
  bindings warn "clip made for hero_idle.aseprite, sprite shows hero.aseprite"; a Lua script logs
  SpriteSheet.load(...).frameCount, findFrame("hero_idle 3") and getFrame(3).rect;
- AC11: a Rigidbody 2D collider with slice hurt: the Box2D shape (read back with rays against the
  body) is the slice's box per frame, changes on frame 1, is gone on the hidden frames, and the
  saved collider is unchanged after play;
- AC12: a real drag of hero_idle.aseprite from the Project window into the Scene view (xdotool on
  the harness' Xvfb) makes hero_idle (Sprite on frame 0 with the frame's pivot, Animation Player
  with walk and Idle, Default Clip Idle, Play Automatically, Light 2D intensity 2.5) in one undo
  step; dropping hero.aseprite onto it swaps its sheet in one step; set_sprite_frames {create}
  (the same code) gives the same GameObject;
- R36: an external save of a file whose settings name a missing layer reports the import's
  unknown_layer warning in external_changes.problems;
- AC13: the source exports (Linux and web backends) ship assets/je-imported/ with its index and no
  .aseprite; the Game view and (JASPER_SLOW_BUILD=1) the exported Desktop player draw hero_idle's
  frame 3 as `jasperengine-editor aseprite render` does.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

import yaml

import build_cache
import harness
import pngdiff
import ui_input
from harness import REPO, Editor, TestFailure, skip

# the Desktop player test builds with the run's prebuilt glcore runtime (tools/mcp/build_cache.py);
# the drop test drives the editor with xdotool on the harness' display
PREBUILT_PLAYERS = ["glcore-release"]
CI = True
EDITOR = {"create": True, "name": "AsepriteImport", "xvfb": "harness", "ui_probe": True,
          "env": build_cache.player_env("glcore", "release")}
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200")) if os.environ.get("JASPER_SLOW_BUILD", "") not in ("", "0") else 900

FIXTURES = REPO / "editor" / "tests" / "fixtures" / "aseprite"
S44_MCP = FIXTURES / "s44-mcp"
HERO = "assets/sprites/hero.aseprite"
# the project's asset paths are project-relative (its assets root is the project folder)
HERO_ASSET = HERO
IDLE = "assets/sprites/hero_idle.aseprite"
WARNED = "assets/sprites/warned.aseprite"
FRAMES_CLIP = "assets/anim/f.anim"
S: dict = {}


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def near(a, b, eps: float = 1e-2) -> bool:
    return a is not None and b is not None and abs(a - b) <= eps


def hashes(root: Path) -> dict:
    out = {}
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        # the import cache and the editor's own state may change; the project's files may not
        if rel.startswith(".jasperengine") or not path.is_file():
            continue
        out[rel] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


async def imported(ed: Editor, path: str) -> dict:
    info = None
    for _ in range(200):
        info = await ed.call("get_asset_info", path=path)
        state = info.get("aseprite", info).get("import_state") if isinstance(info, dict) else None
        if state in ("imported", "failed"):
            return info.get("aseprite", info)
        await ed.call("wait", seconds=0.1)
    raise TestFailure(f"{path} never imported: {info}")


async def lua(ed: Editor, code: str, **args) -> list:
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def component(ed: Editor, entity, name: str, properties: list[str], scene=None) -> dict:
    args = {"entity": entity, "components": [name], "properties": properties}
    if scene is not None:
        args["scene"] = scene
    got = await ed.call("get_entity", **args)
    return got["components"].get(name, {})


def render_cli(path: Path, out: Path, *args: str) -> tuple:
    """`jasperengine-editor aseprite render`'s PNG of `path`, decoded (S44 R37: no project)."""
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    r = subprocess.run(harness.editor_command() + ["aseprite", "render", str(path), str(out), *args],
                       env=env, cwd=str(out.parent), capture_output=True, text=True, timeout=300)
    check(r.returncode == 0, f"aseprite render {' '.join(args)}: exit {r.returncode}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
    return pngdiff.decode(out.read_bytes())


def frame_mismatch(shot: bytes, frame: tuple) -> str | None:
    """None when the image draws `frame` (decoded RGBA) at an integer-ish scale: its opaque
    pixels, each sampled at the centre of the block it covers, equal the frame's exactly. The
    block is found as the bounding box of the screen pixels with one of the frame's opaque
    colours, so a wrong frame (other colours) or a filtered one finds nothing."""
    sw, sh, s = pngdiff.decode(shot)
    fw, fh, f = frame
    opaque = {(x, y): tuple(f[(y * fw + x) * 4:(y * fw + x) * 4 + 3]) for y in range(fh) for x in range(fw)
              if f[(y * fw + x) * 4 + 3] == 255}
    if not opaque:
        return "the rendered frame has no opaque pixel"
    colours = set(opaque.values())
    xs = [x for x, _ in opaque]
    ys = [y for _, y in opaque]
    fx0, fx1, fy0, fy1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    # within a few levels: the view's colour pipeline (sRGB round trips) may move a channel by one or two
    def near_colour(c, want, tolerance=6):
        return all(abs(a - b) <= tolerance for a, b in zip(c, want))
    found = [(x, y) for y in range(sh) for x in range(sw)
             if any(near_colour(tuple(s[(y * sw + x) * 4:(y * sw + x) * 4 + 3]), c) for c in colours)]
    if not found:
        return f"no pixel of the {sw}x{sh} image has the frame's colours {sorted(colours)}"
    sx0, sx1 = min(x for x, _ in found), max(x for x, _ in found) + 1
    sy0, sy1 = min(y for _, y in found), max(y for _, y in found) + 1
    kx, ky = (sx1 - sx0) / (fx1 - fx0), (sy1 - sy0) / (fy1 - fy0)
    if kx < 1 or abs(kx - ky) > 0.1 * kx:
        return f"the frame's opaque {fx1 - fx0}x{fy1 - fy0} pixels cover {sx1 - sx0}x{sy1 - sy0} on screen (scale {kx:.2f} x {ky:.2f})"
    bad = []
    for (x, y), want in sorted(opaque.items()):
        px, py = int(sx0 + (x - fx0 + 0.5) * kx), int(sy0 + (y - fy0 + 0.5) * ky)
        got = tuple(s[(py * sw + px) * 4:(py * sw + px) * 4 + 3])
        if not near_colour(got, want):
            bad.append(f"frame ({x}, {y}) {want} is {got} at ({px}, {py})")
    return f"{len(bad)} of {len(opaque)} opaque pixels differ: {bad[:5]}" if bad else None


# ---------------------------------------------------------------------------- R25


async def test_no_aseprite_no_cache(ed: Editor):
    """AC13's last part, while the new project holds no .aseprite file (this runs first)."""
    await ed.call("sync_files")
    check(not list(ed.project.rglob("*.aseprite")), "the new project holds no .aseprite file")
    check(not (ed.project / ".jasperengine" / "cache" / "imported").exists(), "no import cache dir without .aseprite files")
    lines = [e["message"] for e in await ed.console_since(None, limit=200) if "[Aseprite]" in e.get("message", "")]
    check(not lines, f"no [Aseprite] Console line without .aseprite files: {lines}")
    # the importer's jobsStarted() (AsepriteImporter.h), as get_job_system_stats reports it
    stats = await ed.call("get_job_system_stats")
    aseprite = stats.get("aseprite") if isinstance(stats, dict) else None
    check(isinstance(aseprite, dict) and aseprite.get("imports_started") == 0,
          f"get_job_system_stats.aseprite.imports_started is 0 without .aseprite files: {aseprite}")


async def test_group_hidden_until_enabled(ed: Editor):
    names = {t.name for t in await ed.tools(groups=None)}
    check("set_sprite_import_settings" not in names, "set_sprite_import_settings is hidden in a new session")
    check("set_sprite_frames" in names, "set_sprite_frames is core")
    await ed.call("tool_groups", enable=["sprites"])
    names = {t.name for t in await ed.tools(groups=None)}
    check("set_sprite_import_settings" in names, "tool_groups enables the sprites group")


async def test_import_and_asset_info(ed: Editor):
    S["main"] = (await ed.call("list_scenes"))["selected_scene_id"]
    (ed.project / "assets" / "sprites").mkdir(parents=True, exist_ok=True)
    shutil.copy(FIXTURES / "asefile" / "layers_and_tags.aseprite", ed.project / HERO)
    shutil.copy(S44_MCP / "hero_idle.aseprite", ed.project / IDLE)
    await ed.call("sync_files", paths=[HERO, IDLE])
    info = await imported(ed, HERO)
    check(info["import_state"] == "imported", f"imported: {info['import_state']}")
    sprite = info["sprite"]
    check(sprite["frame_count"] == 4 and len(sprite["layers"]) == 6, f"summary: {sprite.get('frame_count')} frames")
    check([t["name"] for t in sprite["tags"]] == ["T1", "T3", "T2"], f"tags: {sprite['tags']}")
    check("cels" not in json.dumps(sprite["layers"][0]), "the summary has no cels")
    check(info["import_settings"]["resolved"]["import_mode"] == "sprite_sheet", f"settings: {info['import_settings']}")
    clips = {c["name"]: c for c in info["import"]["clips"]}
    check(set(clips) == {"T1", "T3", "T2"}, f"clips: {list(clips)}")
    check(clips["T1"]["reference"] == f"{HERO_ASSET}#T1", f"reference: {clips['T1']}")
    check(all(c["guid"] for c in clips.values()), f"clip GUIDs: {clips}")
    check(info["import"]["frames"] == 4 and info["import"]["pages"], f"outputs: {info['import']}")
    r = await ed.call_raw("view_asset", {"path": HERO})
    check(not r.is_error and r.images and r.images[0][0].startswith(b"\x89PNG"), f"view_asset gives a PNG: {r.text}")
    check(r.structured["frames"] == 4, f"view_asset frames: {r.structured}")

    # the character the later tests use is what make_hero_idle.lua wrote
    idle = await imported(ed, IDLE)
    check(idle["import_state"] == "imported", f"hero_idle imported: {idle['import_state']}")
    sprite = idle["sprite"]
    check(sprite["frame_count"] == 6 and sprite["frame_durations_ms"] == [100, 200, 100, 200, 300, 300],
          f"hero_idle's frames: {sprite.get('frame_durations_ms')}")
    tags = {t["name"]: t for t in sprite["tags"]}
    check(set(tags) == {"walk", "Idle"} and (tags["walk"]["from"], tags["walk"]["to"], tags["walk"]["repeat"]) == (0, 3, 0)
          and (tags["Idle"]["from"], tags["Idle"]["to"], tags["Idle"]["repeat"]) == (4, 5, 3), f"hero_idle's tags: {sprite['tags']}")
    check([s["name"] for s in sprite["slices"]] == ["hurt"], f"hero_idle's slices: {sprite['slices']}")
    check("light2d.intensity" in json.dumps(sprite), "the summary has the sprite's user properties")
    check({c["name"] for c in idle["import"]["clips"]} == {"walk", "Idle"}, f"hero_idle's clips: {idle['import']['clips']}")


async def test_settings_preview_and_errors(ed: Editor):
    before = hashes(ed.project)
    r = await ed.call("set_sprite_import_settings", paths=[HERO], settings={"texture": {"layout": "grid"}})
    check(r["written"] is False and r["files"][0]["outputs"], f"preview: {r}")
    check(hashes(ed.project) == before, "a preview writes no project file")
    for settings, kind, field in [({"layers": {"include": ["Nope"]}}, "unknown_layer", "layers"),
                                  ({"bogus": 1}, "invalid_settings", "bogus")]:
        raw = await ed.call_raw("set_sprite_import_settings", {"paths": [HERO], "settings": settings})
        check(raw.is_error, f"{settings} fails")
        error = raw.structured["error"]
        check(error["kind"] == kind and field in error["field"] and error["fix"], f"{settings}: {error}")
    check(hashes(ed.project) == before, "errors write nothing")


async def test_settings_output_and_undo(ed: Editor):
    meta = ed.project / (HERO + ".meta")
    old = meta.read_bytes() if meta.exists() else None
    r = await ed.call("set_sprite_import_settings", paths=[HERO], settings={"texture": {"layout": "grid"}}, output=True)
    check(r["written"] is True, f"written: {r}")
    check("grid" in meta.read_text(), "the meta has the new layout")
    check(r["files"][0]["settings"]["texture"]["layout"] == "grid", f"settings: {r['files'][0]['settings']}")
    await ed.call("undo", scope="project")
    now = meta.read_bytes() if meta.exists() else None
    check(now == old or (now is not None and b"grid" not in now), "one undo restores the meta")
    info = await imported(ed, HERO)
    check(info["import_settings"]["resolved"]["texture"]["layout"] == "packed", f"after undo: {info['import_settings']}")


async def test_tag_clips_read_only(ed: Editor):
    clip = await ed.call("get_animation", clip=f"{HERO_ASSET}#T1")
    check(clip.get("read_only") is True, f"a tag clip is read-only: {clip.get('read_only')}")
    raw = await ed.call_raw("edit_animation", {"clip": f"{HERO_ASSET}#T1", "length": 1.0})
    check(raw.is_error and raw.structured and raw.structured["error"]["kind"] == "read_only_clip", f"edit_animation: {raw.text}")
    check("create" in raw.structured["error"]["fix"], f"the fix extracts: {raw.structured['error']}")


async def test_set_sprite_frames_sheet(ed: Editor):
    r = await ed.call("set_sprite_frames", create={"name": "Hero"}, sheet=HERO, frame="hero 2")
    check(r["frame"] == 2 and r["frame_count"] == 4, f"frame by name: {r}")
    check(r["added_player"] is True and r["default_clip"] == "T1", f"player: {r}")
    check(sorted(r["clips"]) == sorted(f"{HERO_ASSET}#{t}" for t in ("T1", "T2", "T3")), f"clips: {r['clips']}")
    S["hero"] = r["sprite"]["id"]
    raw = await ed.call_raw("set_sprite_frames", {"entity": "Hero", "frame": "nope"})
    check(raw.is_error and raw.structured["error"]["kind"] == "unknown_frame", f"unknown frame: {raw.text}")
    check("hero 0" in raw.structured["error"]["fix"], f"the fix lists frames: {raw.structured['error']}")
    entity = await ed.call("get_entity", entity="Hero")
    check("animationplayer" in json.dumps(entity).lower(), f"the Animation Player is there: {entity}")
    await ed.call("undo")
    found = await ed.call("find_entities", name="Hero")
    check(not found.get("entities"), "one undo removes the GameObject and its player")
    await ed.call("redo")


async def test_lua_tags_and_sheet(ed: Editor):
    await ed.call("play", action="start", fixed_fps=60, auto_pause=False)
    try:
        values = await lua(ed, (
            "local s = object('Hero', 'Sprite') s:playTag('T1') "
            "local sheet = SpriteSheet.load('assets/sprites/hero.aseprite') "
            "local idle = SpriteSheet.load('assets/sprites/hero_idle.aseprite'):getTag('Idle') "
            "return s.tag, s.isTagPlaying, s:getTag('T1').from, s:getTag('T1').to, s.frameCount, "
            "sheet.frameCount, sheet:findFrame('hero 3'), s.frameName, idle['repeat'], idle.from, idle.to"))
        check(values[0] == "T1" and values[1] is True, f"playTag: {values}")
        check(values[2:7] == [0, 1, 4, 4, 3], f"tags and sheet: {values}")
        check(values[7] == "hero 0", f"playTag shows the tag's first frame: {values}")
        # `repeat` is a Lua keyword: the tag table is indexed with a string
        check(values[8:11] == [3, 4, 5], f"hero_idle's Idle repeats 3 times over frames 4-5: {values}")
        await ed.call("wait", game_seconds=0.5)
        frame = (await lua(ed, "return object('Hero', 'Sprite').frame"))[0]
        check(frame in (0, 1), f"T1 keeps to frames 0-1: {frame}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0)


# ---------------------------------------------------------------------------- AC10


async def test_clip_plays(ed: Editor):
    info = await imported(ed, IDLE)
    durations = info["sprite"]["frame_durations_ms"]
    walk = next(t for t in info["sprite"]["tags"] if t["name"] == "walk")
    sequence = list(range(walk["from"], walk["to"] + 1))    # forward, repeat 0: once, looping
    starts, total = [], 0
    for f in sequence:
        starts.append(total / 1000.0)
        total += durations[f]
    length = total / 1000.0

    clip = await ed.call("get_animation", clip=f"{IDLE}#walk")
    check(clip.get("read_only") is True and clip["wrap"] == "loop" and near(clip["length"], length, 1e-6),
          f"walk: a read-only loop of {length} s: {clip.get('wrap')}, {clip.get('length')}")
    # §6.6: the events cel of frame 1 is an event at frame 1's start in the clip
    events = clip["events"]
    check(len(events) == 1 and events[0]["function"] == "onStep" and events[0]["args"] == ["left", 1]
          and near(events[0]["time"], starts[sequence.index(1)], 1e-6), f"walk's events: {events}")
    idle = await ed.call("get_animation", clip=f"{IDLE}#Idle")
    check(idle["wrap"] == "once" and not idle["events"], f"Idle repeats 3 times (once) and has no event: {idle.get('wrap')}, {idle.get('events')}")

    r = await ed.call("set_sprite_frames", scene=S["main"], create={"name": "Walker"}, sheet=IDLE)
    check(r["added_player"] is True and r["default_clip"] == "Idle", f"the player starts with Idle (any case): {r}")
    await ed.call("set_properties", scene=S["main"], entity="Walker", values={"animationplayer": {"defaultClip": "walk"}})
    await ed.call("play", action="start", fixed_fps=60, auto_pause=True)
    try:
        # the middle of each step for two loops: each frame shows for its own duration
        samples, t = [], 0.0
        for k in range(2 * len(sequence)):
            step = k % len(sequence)
            at = (k // len(sequence)) * length + starts[step] + durations[sequence[step]] / 2000.0
            await ed.call("wait", game_seconds=round(at - t, 6))
            t = at
            frame = (await lua(ed, "return object('Walker', 'Sprite').frame"))[0]
            samples.append((round(at, 3), sequence[step], frame))
        check(all(want == got for _, want, got in samples), f"walk's frames at (game time, expected, shown): {samples}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


# ---------------------------------------------------------------------------- AC24


async def test_clip_on_sheet(ed: Editor):
    keys = [{"time": i * 0.25, "value": i} for i in range(4)]
    made = await ed.call("edit_animation", clip=FRAMES_CLIP, create={}, wrap="once", length=1.0, tracks=[
        {"path": "", "channel": "property", "component": "sprite", "property": "frame", "keys": keys}])
    check(made["created"] and len(made["tracks"]) == 1, f"the clip: {made}")
    await ed.call("set_sprite_frames", scene=S["main"], create={"name": "Indexed"}, sheet=IDLE, frame=5, clips=False)
    await ed.call("add_component", scene=S["main"], entity="Indexed", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": FRAMES_CLIP}], "playAutomatically": True}})

    # R50: a tag clip of hero_idle on a sprite that shows hero.aseprite
    other = (await ed.call("get_animation", scene=S["main"], clip=f"{IDLE}#walk", root="Hero")).get("bindings") or {}
    warnings = other.get("warnings", [])
    check(any("clip made for hero_idle.aseprite" in w and "sprite shows hero.aseprite" in w for w in warnings),
          f"the binding report warns of the other sheet: {other}")
    same = (await ed.call("get_animation", scene=S["main"], clip=f"{IDLE}#walk", root="Indexed")).get("bindings") or {}
    check(not any("clip made for" in w for w in same.get("warnings", [])), f"no warning on its own sheet: {same}")

    await ed.call("play", action="start", fixed_fps=60, auto_pause=True)
    try:
        samples, t = [], 0.0
        for at, want in ((0.125, 0), (0.375, 1), (0.625, 2), (0.875, 3), (1.25, 3)):
            await ed.call("wait", game_seconds=round(at - t, 6))
            t = at
            frame = (await lua(ed, "local s = object('Indexed', 'Sprite') return s.frame, s.frameName"))
            samples.append((at, want, frame))
        check(all(want == got[0] and got[1] == f"hero_idle {want}" for _, want, got in samples),
              f"sprite.frame keys are sheet frame indices (game time, expected, [frame, frameName]): {samples}")

        r = await ed.call("eval_lua", code=(
            "local sheet = SpriteSheet.load('assets/sprites/hero_idle.aseprite') "
            "local r = sheet:getFrame(3).rect "
            "print(string.format('sheet %d frames, hero_idle 3 is %d, rect %g %g %g %g', sheet.frameCount, "
            "sheet:findFrame('hero_idle 3'), r.x, r.y, r.width, r.height)) "
            "return sheet.frameCount, sheet:findFrame('hero_idle 3'), r.x, r.y, r.width, r.height"))
        values = r["values"]
        check(values[0] == 6 and values[1] == 3, f"frameCount and findFrame: {values}")
        # trimmed to the union of Body (8,4 16x24) and Shadow (6,28 20x3): 20x27
        check((values[4], values[5]) == (20, 27), f"getFrame(3).rect is the trimmed frame: {values}")
        check(any(line.startswith("sheet 6 frames, hero_idle 3 is 3, rect") for line in r["printed"]), f"the script logs them: {r['printed']}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


# ---------------------------------------------------------------------------- AC11

# the Box2D shapes of Hurtbox's body (collider 0), found with rays against the body from the four
# sides through its local square [-R, R]: the extents of what the rays hit, or nil without a hit
BOX2D_PROBE = """
local go = object('Hurtbox')
local b = object('Hurtbox', 'Rigidbody2D')
local s = object('Hurtbox', 'Sprite')
local p = go.position
local R = 128
local x0, x1, y0, y1
for i = -R * 4, R * 4 do
  local c = i / 4
  local h = Ray(Vector3(p.x - R, p.y + c, p.z), Vector3(2 * R, 0, 0)):intersects(b)
  if h.hit then x0 = math.min(x0 or h.point.x, h.point.x) end
  h = Ray(Vector3(p.x + R, p.y + c, p.z), Vector3(-2 * R, 0, 0)):intersects(b)
  if h.hit then x1 = math.max(x1 or h.point.x, h.point.x) end
  h = Ray(Vector3(p.x + c, p.y - R, p.z), Vector3(0, 2 * R, 0)):intersects(b)
  if h.hit then y0 = math.min(y0 or h.point.y, h.point.y) end
  h = Ray(Vector3(p.x + c, p.y + R, p.z), Vector3(0, -2 * R, 0)):intersects(b)
  if h.hit then y1 = math.max(y1 or h.point.y, h.point.y) end
end
local out = {frame = s.frame, pieces = b:getColliderPieceCount(0)}
if x0 then out.box = {x0 - p.x, y0 - p.y, x1 - p.x, y1 - p.y} end
local sl = s:getSlice('hurt')
if sl then
  out.visible = sl.visible
  -- a slice hidden on this frame has no rect
  if sl.rect then out.rect = {sl.rect.x, sl.rect.y, sl.rect.width, sl.rect.height} end
end
return out
"""


async def test_slice_collider(ed: Editor):
    info = await imported(ed, IDLE)
    check([s["name"] for s in info["sprite"]["slices"]] == ["hurt"], f"hero_idle has the slice hurt: {info['sprite']['slices']}")
    await ed.call("set_sprite_frames", scene=S["main"], create={"name": "Hurtbox"}, sheet=IDLE, clips=False)
    await ed.call("add_component", scene=S["main"], entity="Hurtbox", component="rigidbody2d")
    await ed.call("set_properties", scene=S["main"], entity="Hurtbox", values={"rigidbody2d": {"colliders": [{"slice": "hurt"}]}})
    saved = await component(ed, "Hurtbox", "rigidbody2d", ["colliders"], scene=S["main"])
    check(saved.get("colliders"), f"the saved collider: {saved}")

    await ed.call("play", action="start", fixed_fps=60, auto_pause=True)
    try:
        shapes = {}
        for frame in (0, 1, 2, 4, 5):
            await lua(ed, f"object('Hurtbox', 'Sprite').frame = {frame}")
            await ed.call("wait", frames=2)
            shapes[frame] = (await lua(ed, BOX2D_PROBE, timeout_seconds=30))[0]
        for frame in (0, 1, 2):
            got = shapes[frame]
            box, rect = got.get("box"), got.get("rect")
            check(got.get("frame") == frame and got.get("pieces", 0) >= 1 and box and got.get("visible") is True and rect,
                  f"frame {frame}: the slice shows and the body has its shape: {got}")
            check(all(near(a, b) for a, b in zip(box, [rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]])),
                  f"frame {frame}: the Box2D shape {box} is the slice's box {rect}")
        # make_hero_idle: hurt is 16x16 on frame 0, 12x20 from frame 1 (frame 2 holds that key)
        size = {f: (shapes[f]["box"][2] - shapes[f]["box"][0], shapes[f]["box"][3] - shapes[f]["box"][1]) for f in (0, 1, 2)}
        check(near(size[0][0], 16) and near(size[0][1], 16), f"frame 0's shape is 16x16: {size}")
        check(near(size[1][0], 12) and near(size[1][1], 20) and shapes[1]["box"] != shapes[0]["box"], f"frame 1's shape changes to 12x20: {size}")
        check(all(near(a, b) for a, b in zip(shapes[2]["box"], shapes[1]["box"])), f"a key holds until the next: {shapes[1]} {shapes[2]}")
        for frame in (4, 5):
            got = shapes[frame]
            check(got.get("pieces") == 0 and not got.get("box") and not got.get("visible"),
                  f"frame {frame}: the slice is hidden and the body has no shape there: {got}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    after = await component(ed, "Hurtbox", "rigidbody2d", ["colliders"], scene=S["main"])
    check(after == saved, f"play leaves the saved collider (type, points) as it was: {saved} -> {after}")


# ---------------------------------------------------------------------------- AC12


async def drop_result(ed: Editor, scene, entity) -> dict:
    sprite = await component(ed, entity, "sprite", ["sheet", "frame", "frameName", "pivotPreset"], scene=scene)
    player = await component(ed, entity, "animationplayer", ["clips", "defaultClip", "playAutomatically"], scene=scene)
    light = await component(ed, entity, "light2d", ["intensity"], scene=scene)
    return {"sprite": sprite, "player": player, "light": light}


def check_dropped(got: dict, what: str) -> None:
    sprite, player, light = got["sprite"], got["player"], got["light"]
    check("hero_idle.aseprite" in json.dumps(sprite.get("sheet")) and sprite.get("frame") == 0 and sprite.get("frameName") == "hero_idle 0",
          f"{what}: a Sprite on hero_idle's frame 0: {sprite}")
    check(str(sprite.get("pivotPreset", "")).lower() in ("frame", "from frame"), f"{what}: the frame's pivot: {sprite}")
    clips = json.dumps(player.get("clips"))
    check("hero_idle.aseprite#walk" in clips and "hero_idle.aseprite#Idle" in clips, f"{what}: the player lists walk and Idle: {player}")
    check(player.get("defaultClip") == "Idle" and player.get("playAutomatically") is True, f"{what}: Idle plays automatically: {player}")
    check(near(light.get("intensity"), 2.5, 1e-6), f"{what}: light2d.intensity from the sprite's user properties: {light}")


async def test_drop_shares_set_sprite_frames(ed: Editor):
    """The drop's code through set_sprite_frames {create} (AsepriteSpriteSetup, S44 Deviations)."""
    r = await ed.call("set_sprite_frames", scene=S["main"], create={"name": "Made"}, sheet=IDLE)
    check(r["default_clip"] == "Idle" and "light2d.intensity" in r.get("applied_properties", []), f"set_sprite_frames {{create}}: {r}")
    check("note" not in r.get("applied_properties", []), f"note is data only: {r}")
    check_dropped(await drop_result(ed, S["main"], "Made"), "set_sprite_frames {create}")
    await ed.call("undo", scene=S["main"])
    check(not (await ed.call("find_entities", scene=S["main"], name="Made")).get("entities"), "one undo removes all of it")


async def project_file(ed: Editor, ui: ui_input.Ui, name: str, folders=("assets", "sprites")) -> tuple[int, int]:
    """The screen point of the Project window's entry `name` (probe project.file.<name>), the
    folders opened by double clicks on the way"""
    await ed.call("focus_window", window="project")
    for _ in range(12):
        await ui.frames(4)
        items = ed.ui_items()
        if "project.file." + name in items:
            r = items["project.file." + name]
            return (int(ui.origin[0] + (r[0] + r[2]) / 2), int(ui.origin[1] + (r[1] + r[3]) / 2))
        for folder in reversed(folders):
            if "project.file." + folder in items:
                r = items["project.file." + folder]
                await ui.double_click((int(ui.origin[0] + (r[0] + r[2]) / 2), int(ui.origin[1] + (r[1] + r[3]) / 2)))
                break
    ed.fail(f"the Project window never lists {name}: {sorted(ed.ui_items())}")


async def scene_view_centre(ed: Editor, ui: ui_input.Ui, scene) -> tuple[int, int]:
    shot = await ed.call_raw("screenshot", {"view": "scene", "scene": scene, "max_width": 0})
    check(not shot.is_error, f"the Scene view: {shot.text[:300]}")
    rect = shot.structured["view_rect_in_window"]
    return ui.at(rect, (rect["width"] / 2, rect["height"] / 2))


async def dropped(ed: Editor, ui: ui_input.Ui, scene, name: str) -> list[dict]:
    """The GameObjects named `name` once one is there (a drop is applied in a frame or two)"""
    for _ in range(30):
        found = (await ed.call("find_entities", scene=scene, name=name)).get("entities", [])
        if found:
            return found
        await ui.frames(2)
    return []


async def test_drop(ed: Editor):
    ui = ui_input.Ui(ed)
    await ui.release_all()
    sid = (await ed.call("create_scene", name="Drop", type="2d"))["scene"]["id"]
    S["drop"] = sid
    await ed.call("select_scene", scene=sid)
    try:
        start = await project_file(ed, ui, "hero_idle.aseprite")
        end = await scene_view_centre(ed, ui, sid)
        await ui.drag(start, end)
        found = await dropped(ed, ui, sid, "hero_idle")
        status = (await ed.call("editor_status")).get("status_message", "")
        check(len(found) == 1, f"the drop makes one GameObject hero_idle: {found}; dragged {start} -> {end}, status {status!r}")
        entity = found[0]["id"]
        check_dropped(await drop_result(ed, sid, entity), "the drop")

        await ed.call("undo", scene=sid)
        check(not (await ed.call("find_entities", scene=sid, name="hero_idle")).get("entities"),
              "one undo removes the GameObject, its Sprite, player and Light 2D")
        await ed.call("redo", scene=sid)
        check_dropped(await drop_result(ed, sid, entity), "redo")

        # onto the Sprite: its sheet is swapped in one step, no GameObject is made
        await ed.call("select_entities", scene=sid, entities=[])
        await ed.call("frame_entity", scene=sid, entity=entity)
        start = await project_file(ed, ui, "hero.aseprite")
        end = await scene_view_centre(ed, ui, sid)
        count = len((await ed.call("find_entities", scene=sid)).get("entities", []))
        await ui.drag(start, end)
        swapped = {}
        for _ in range(30):
            swapped = await component(ed, entity, "sprite", ["sheet", "frame", "frameName"], scene=sid)
            if "hero.aseprite" in json.dumps(swapped.get("sheet")):
                break
            await ui.frames(2)
        check("hero.aseprite" in json.dumps(swapped.get("sheet")) and swapped.get("frameName") == "hero 0",
              f"dropping hero.aseprite onto hero_idle swaps its sheet: {swapped}")
        check(len((await ed.call("find_entities", scene=sid)).get("entities", [])) == count, "a drop onto a Sprite makes no GameObject")
        await ed.call("undo", scene=sid)
        back = await component(ed, entity, "sprite", ["sheet", "frameName"], scene=sid)
        check("hero_idle.aseprite" in json.dumps(back.get("sheet")) and back.get("frameName") == "hero_idle 0",
              f"one undo puts hero_idle back: {back}")
    finally:
        await ui.release_all()
        await ed.call("select_scene", scene=S["main"])


# ---------------------------------------------------------------------------- R36


async def test_external_save_reports_import_problems(ed: Editor):
    target = ed.project / WARNED
    shutil.copy(S44_MCP / "hero_idle.aseprite", target)
    await ed.call("sync_files", paths=[WARNED])
    await imported(ed, WARNED)
    meta = target.with_name(target.name + ".meta")
    for _ in range(100):
        if meta.exists():
            break
        await ed.call("wait", seconds=0.1)
    check(meta.exists(), f"the asset database gives {WARNED} a meta")
    original = meta.read_text()
    # as a person would: the settings by hand, then a save from Aseprite (appended when the meta
    # has no import key yet, so its other lines stay as the editor wrote them)
    if not any(line.startswith("import:") for line in original.splitlines()):
        meta.write_text(original.rstrip("\n") + "\nimport:\n  asepriteImporter:\n    version: 1\n    layers:\n      include: [Nope]\n")
    else:
        data = yaml.safe_load(original)
        data["import"].setdefault("asepriteImporter", {})["layers"] = {"include": ["Nope"]}
        meta.write_text(yaml.safe_dump(data, sort_keys=False))
    time.sleep(0.05)
    target.write_bytes(target.read_bytes())
    found, seen = [], []
    deadline = time.monotonic() + 15
    while not found and time.monotonic() < deadline:
        r = await ed.call_raw("editor_status")
        ext = (r.structured or {}).get("external_changes") or {}
        seen.extend(ext.get("problems", []))
        found = [p for p in ext.get("problems", []) if str(p.get("path", "")).endswith("warned.aseprite")
                 and "unknown_layer" in p.get("message", "") and "Nope" in p.get("message", "")]
        await asyncio.sleep(0.1)
    check(found, f"external_changes.problems has the import's unknown_layer warning: {seen}")
    check(all(p.get("severity") == "warning" for p in found), f"as a warning: {found}")
    target.unlink()
    meta.unlink()
    await ed.call("sync_files", paths=[WARNED])


# ---------------------------------------------------------------------------- AC13


async def export_scene(ed: Editor) -> dict:
    """A 2D start scene showing hero_idle's frame 3 four times its size at the canvas centre, and
    the main scene (Hero plays hero.aseprite's clips) after it in the build"""
    if "export" in S:
        return S["export"]
    ed.require_tools("build_project", "edit_build_scenes")
    await ed.call("set_project_settings", values={"graphics.canvas_width": 320, "graphics.canvas_height": 180})
    main = (await ed.call("save_scene", scene=S["main"], path="scenes/Main"))["scene"]["path"]
    sid = (await ed.call("create_scene", name="SpriteExport", type="2d"))["scene"]["id"]
    await ed.call("set_sprite_frames", scene=sid, create={"name": "Shown", "position": [160, 90, 0]}, sheet=IDLE,
                  frame=3, clips=False, pivot="center")
    await ed.call("set_transform", scene=sid, entity="Shown", scale=[4, 4, 1])
    # hero_idle's sprite properties gave it a Light 2D (light2d.intensity): the frame unlit, as the CLI renders it
    await ed.call("remove_component", scene=sid, entity="Shown", component="light2d")
    path = (await ed.call("save_scene", scene=sid, path="scenes/SpriteExport"))["scene"]["path"]
    for scene in (path, main):
        await ed.call("edit_build_scenes", action="add", scene=scene)
    await ed.call("edit_build_scenes", action="set_start", scene=path)
    frame = render_cli(ed.project / IDLE, ed.scratch.root / "hero_idle_3.png", "--frame", "3")
    S["export"] = {"scene": sid, "frame": frame}
    return S["export"]


async def test_export(ed: Editor):
    s = await export_scene(ed)
    # what the export will draw, in the editor first: the Game view of the start scene
    await ed.call("select_scene", scene=s["scene"])
    try:
        shot = await ed.screenshot("game", path=ed.scratch.root / "aseprite-game-view.png", max_width=0)
    finally:
        await ed.call("select_scene", scene=S["main"])
    bad = frame_mismatch(shot, s["frame"])
    check(bad is None, f"the Game view draws hero_idle's frame 3 as the render CLI: {bad}")

    # the source export with the Linux and with the web (GLES3) backends
    for platform in ("linux", "web"):
        out = ed.scratch.root / f"export-aseprite-{platform}"
        r = await ed.call("build_project", mode="source", output_dir=str(out), platform=platform, timeout_seconds=900)
        if r.get("status") == "running":
            r = await wait_job(ed, r["job_id"], 900)
        check(r["status"] == "succeeded", f"{platform}: {r!r}")
        assets = out / "project" / "assets"
        if not assets.is_dir():
            assets = next(p for p in out.rglob("assets") if p.is_dir())
        check(not list(assets.rglob("*.aseprite")), f"{platform}: no .aseprite file ships")
        index = json.loads((assets / "je-imported" / "index.json").read_text())
        check(index["format"] == "jasperengine.imported" and index["version"] == 1, f"{platform}: index {index.get('format')}")
        files = index["files"]
        for key in (f"{HERO_ASSET}#sheet", f"{HERO_ASSET}#image#0", f"{HERO_ASSET}#animation:T1",
                    f"{IDLE}#sheet", f"{IDLE}#image#0", f"{IDLE}#animation:walk", f"{IDLE}#animation:Idle"):
            check(key in files and (assets / files[key]).is_file(), f"{platform}: {key} ships: {files.get(key)}")


async def wait_job(ed: Editor, job_id: str, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)


async def test_export_desktop_player(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (the run builds its prebuilt runtime once)")
    s = await export_scene(ed)
    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-aseprite-desktop"
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    job = r if r["status"] != "running" else await wait_job(ed, r["job_id"], TIMEOUT - 300, poll=5.0)
    check(job["status"] == "succeeded", f"{job}")
    check((job["result"]["method_used"], job["result"]["player_id"]) == ("prebuilt", runtime.name), f"{job}")
    check(not list(out.rglob("*.aseprite")), "no .aseprite file ships with the player")
    # the player's own frame, 60 frames of 1/60 s after it got ready (not a window grab)
    raw = await ed.call_raw("run_build", {"after_ready": True, "fixed_fps": 60, "quit_after": 60})
    check(not raw.is_error, raw.text)
    check(not raw.value["crashed"] and raw.value.get("exit_code") == 0, f"the player got ready and quit: {raw.value}")
    check(raw.images, f"the player's own screenshot: {raw.text[:2000]}")
    png = raw.images[0][0]
    (ed.scratch.root / "aseprite-player.png").write_bytes(png)
    bad = frame_mismatch(png, s["frame"])
    check(bad is None, f"the exported player draws hero_idle's frame 3 as the render CLI: {bad}")
