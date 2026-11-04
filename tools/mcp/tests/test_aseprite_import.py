"""Aseprite import over MCP (docs/specs/S44-aseprite-import.md R35, R36, R33, R38, R17, R47; AC11,
AC12, AC13, AC17, AC19), on a new project holding one real Aseprite file (asefile's
layers_and_tags: 16x16, 4 frames, 6 layers, tags T1 0-1, T3, T2) and Aseprite's slices-moving:

- the `sprites` group is hidden until tool_groups enables it;
- the import: get_asset_info has the summary, the resolved settings, the outputs (clip references)
  and import_state imported; view_asset is a contact sheet;
- set_sprite_import_settings: a preview changes no file (hashes); typed errors for an unknown layer
  and an unknown key; output: true writes the meta and reimports, one project undo puts the old
  meta back;
- tag clips: get_animation reads `<file>#T1`; edit_animation on it fails with read_only_clip;
- set_sprite_frames {create, sheet}: a sheet Sprite, frame by name, the Animation Player with the
  tag clips (Default Clip: the first tag, there is no idle), unknown_frame; one undo removes it;
- Lua in play: playTag, tag, getTag().repeat, SpriteSheet.load, frameName;
- a slice collider: a Rigidbody 2D collider with slice follows the slice's box per frame;
- the source export ships assets/je-imported/ with its index and no .aseprite.
"""

from __future__ import annotations

import hashlib
import json
import shutil
from pathlib import Path

from harness import REPO, Editor, TestFailure

CI = True
EDITOR = {"create": True, "name": "AsepriteImport"}
TIMEOUT = 900

FIXTURES = REPO / "editor" / "tests" / "fixtures" / "aseprite"
HERO = "assets/sprites/hero.aseprite"
HERO_ASSET = "sprites/hero.aseprite"
SLICES = "assets/sprites/slices.aseprite"
S: dict = {}


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


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


async def test_group_hidden_until_enabled(ed: Editor):
    names = {t.name for t in await ed.tools(groups=None)}
    check("set_sprite_import_settings" not in names, "set_sprite_import_settings is hidden in a new session")
    check("set_sprite_frames" in names, "set_sprite_frames is core")
    await ed.call("tool_groups", enable=["sprites"])
    names = {t.name for t in await ed.tools(groups=None)}
    check("set_sprite_import_settings" in names, "tool_groups enables the sprites group")


async def test_import_and_asset_info(ed: Editor):
    (ed.project / "assets" / "sprites").mkdir(parents=True, exist_ok=True)
    shutil.copy(FIXTURES / "asefile" / "layers_and_tags.aseprite", ed.project / HERO)
    shutil.copy(FIXTURES / "aseprite-tests" / "slices-moving.aseprite", ed.project / SLICES)
    await ed.call("sync_files", paths=[HERO, SLICES])
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
    raw = await ed.call_raw("edit_animation", {"clip": f"{HERO_ASSET}#T1", "ops": []})
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
        values = (await ed.call("eval_lua", code=(
            "local s = object('Hero', 'Sprite') s:playTag('T1') "
            "local sheet = SpriteSheet.load('sprites/hero.aseprite') "
            "return s.tag, s.isTagPlaying, s:getTag('T1').from, s:getTag('T1').to, s.frameCount, "
            "sheet.frameCount, sheet:findFrame('hero 3'), s.frameName")))["values"]
        check(values[0] == "T1" and values[1] is True, f"playTag: {values}")
        check(values[2:7] == [0, 1, 4, 4, 3], f"tags and sheet: {values}")
        check(values[7] == "hero 0", f"playTag shows the tag's first frame: {values}")
        await ed.call("wait", game_seconds=0.5)
        frame = (await ed.call("eval_lua", code="return object('Hero', 'Sprite').frame"))["values"][0]
        check(frame in (0, 1), f"T1 keeps to frames 0-1: {frame}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0)


async def test_slice_collider(ed: Editor):
    info = await imported(ed, SLICES)
    sprite = info["sprite"]
    check(sprite["slices"], f"slices-moving has slices: {sprite}")
    name = sprite["slices"][0]["name"]
    await ed.call("set_sprite_frames", create={"name": "Mover"}, sheet=SLICES, clips=False)
    await ed.call("set_properties", entity="Mover", values={"rigidbody2d": {"colliders": [{"slice": name}]}})
    await ed.call("play", action="start", fixed_fps=60, auto_pause=False)
    try:
        widths = []
        for frame in range(min(2, sprite["frame_count"])):
            values = (await ed.call("eval_lua", code=(
                f"local s = object('Mover', 'Sprite') s.frame = {frame} "
                f"local r = s:getSlice('{name}') return r and r.visible, r and r.rect.x, r and r.rect.width")))["values"]
            await ed.call("wait", frames=2)
            widths.append(values)
        check(all(v[0] for v in widths), f"the slice shows on the first frames: {widths}")
        check(widths[0][1:] != widths[1][1:], f"its box moves between frames: {widths}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0)


async def test_source_export_ships_the_import(ed: Editor):
    ed.require_tools("build_project", "edit_build_scenes")
    await ed.call("save_scene")
    scenes = await ed.call("list_scenes")
    path = scenes["scenes"][0]["path"]
    await ed.call("edit_build_scenes", action="add", scene=path)
    await ed.call("edit_build_scenes", action="set_start", scene=path)
    out = ed.scratch.root / "export-aseprite"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=900)
    check(r["status"] == "succeeded", f"{r!r}")
    assets = out / "project" / "assets"
    if not assets.is_dir():
        assets = next(p for p in out.rglob("assets") if p.is_dir())
    check(not list(assets.rglob("*.aseprite")), "no .aseprite file ships")
    index = json.loads((assets / "je-imported" / "index.json").read_text())
    check(index["format"] == "jasperengine.imported" and index["version"] == 1, f"index: {index.get('format')}")
    files = index["files"]
    for key in (f"{HERO_ASSET}#sheet", f"{HERO_ASSET}#image#0", f"{HERO_ASSET}#animation:T1"):
        check(key in files and (assets / files[key]).is_file(), f"{key} ships: {files.get(key)}")
