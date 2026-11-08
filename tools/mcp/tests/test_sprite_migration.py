"""Legacy inline sprite frames converted to texture sheets (docs/specs/S45-sprite-atlas.md R21, R22,
AC10), on a new project:

- set_sprite_frames {convert: true, all_users: true} on one of the sprites sharing a texture (one
  in the open scene with a clip keying frame 2, two in a closed scene, one in a prefab) writes the
  texture's Sprite Mode, converts all four, and the clip keyed `frame 2` still shows that frame;
  one undo restores the scenes, the prefab and the meta;
- a texture with another Sprite Mode gives sheet_conflict, with the fix to keep the sprite inline;
- `check --sprites` lists the legacy sprites before and none after, and exits 1 on a missing atlas
  member.
"""

from __future__ import annotations

import json
import subprocess

import harness
import pngdiff
from harness import Editor, TestFailure

CI = True
EDITOR = {"create": True, "name": "SpriteMigration"}

WALK = "assets/legacy/walk.png"
OTHER = "assets/legacy/other.png"
FRAMES = [{"name": "walk_0", "rect": [0, 0, 16, 16]}, {"name": "walk_1", "rect": [16, 0, 16, 16]},
          {"name": "walk_2", "rect": [32, 0, 16, 16]}]
S: dict = {}


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def png(width: int, height: int) -> bytes:
    rgba = bytearray()
    for y in range(height):
        for x in range(width):
            rgba += bytes(((x * 5) & 255, (y * 13) & 255, (x // 16) * 90 & 255, 255))
    return pngdiff.encode(width, height, bytes(rgba))


def check_sprites(ed: Editor) -> tuple[int, list]:
    proc = subprocess.run(harness.editor_command() + ["check", "--project", str(ed.project), "--sprites", "--format", "json"],
                          capture_output=True, text=True, timeout=300)
    try:
        out = json.loads(proc.stdout)
    except json.JSONDecodeError:
        raise TestFailure(f"check --sprites: {proc.stdout[-2000:]} {proc.stderr[-2000:]}")
    return proc.returncode, out.get("findings", [])


async def legacy(ed: Editor, scene, name: str) -> dict:
    return (await ed.call("set_sprite_frames", scene=scene, create={"name": name}, texture=WALK, frames=FRAMES, frame=0, inline=True))


async def test_setup(ed: Editor):
    for path, width in ((WALK, 48), (OTHER, 32)):
        (ed.project / path).parent.mkdir(parents=True, exist_ok=True)
        (ed.project / path).write_bytes(png(width, 16))
    await ed.call("sync_files", paths=[WALK, OTHER])
    S["main"] = (await ed.call("list_scenes"))["selected_scene_id"]
    # two in a closed scene
    closed = (await ed.call("create_scene", name="Closed", type="2d", path="scenes/Closed.scene"))["scene"]["id"]
    await legacy(ed, closed, "A")
    await legacy(ed, closed, "B")
    await ed.call("save_scene", scene=closed)
    await ed.call("close_scene", scene=closed)
    S["closed"] = ed.project / "scenes" / "Closed.scene"
    # one in a prefab (its instance removed: the prefab file holds it)
    await ed.call("select_scene", scene=S["main"])
    await legacy(ed, S["main"], "Prefabbed")
    # (drawn once first: a Sprite's mesh has its texture slot once it was built)
    await ed.call("wait", frames=5)
    made = await ed.call("create_prefab", scene=S["main"], entity="Prefabbed", path="prefabs/Walker.prefab")
    S["prefab"] = ed.project / "prefabs" / "Walker.prefab"
    check(S["prefab"].is_file(), f"the prefab: {made}")
    await ed.call("delete_entities", scene=S["main"], entities=["Prefabbed"])
    # the one converted, in the open scene, with a clip keying frame 2
    await legacy(ed, S["main"], "Walker")
    clip = "assets/anim/walk2.anim"
    await ed.call("edit_animation", clip=clip, create={}, wrap="once", length=1.0, tracks=[
        {"path": "", "channel": "property", "component": "sprite", "property": "frame", "keys": [{"time": 0.0, "value": 2}]}])
    await ed.call("add_component", scene=S["main"], entity="Walker", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": clip}], "playAutomatically": True}})
    await ed.call("save_scene", scene=S["main"], path="scenes/Main.scene")


async def test_check_lists_legacy_sprites(ed: Editor):
    code, findings = check_sprites(ed)
    legacy_found = [f for f in findings if f.get("code") == "sprite-inline-frames"]
    check(code == 0, f"legacy sprites are not errors: exit {code}")
    check(len(legacy_found) >= 4, f"the four legacy sprites (open scene, closed scene x2, prefab): {legacy_found}")
    check(any("Closed.scene" in f["path"] for f in legacy_found) and any("Walker.prefab" in f["path"] for f in legacy_found),
          f"closed scenes and prefabs are read: {legacy_found}")
    check(all("3 inline frame(s)" in f["message"] and WALK in f["message"] for f in legacy_found), f"the texture and frame count: {legacy_found}")


async def play_frame(ed: Editor) -> list:
    await ed.call("select_scene", scene=S["main"])
    await ed.call("play", action="start", fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", game_seconds=0.1)
        return (await ed.call("eval_lua", code="local s = object('Walker', 'Sprite') local r = s.textureRect "
                                               "return s.frame, s.frameName, s.sheet, r.x, r.width"))["values"]
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_convert_all_users_and_undo(ed: Editor):
    meta = ed.project / (WALK + ".meta")
    meta_before = meta.read_bytes() if meta.exists() else None
    closed_before = S["closed"].read_bytes()
    prefab_before = S["prefab"].read_bytes()
    before = await play_frame(ed)
    check(abs(before[3] - 32.0 / 48.0) < 1e-4, f"legacy: the clip's frame 2 shows the third rect: {before}")

    r = await ed.call("set_sprite_frames", scene=S["main"], entity="Walker", convert=True, all_users=True)
    check(len(r["converted"]) == 4 and r["frames"] == 3, f"all four converted: {r}\nprefab: {prefab_before.decode()[:3000]}")
    check("undo" in r.get("next", ""), f"the result says how to undo: {r}")
    check("spriteSheet" in meta.read_text() and "walk_2" in meta.read_text(), "the texture's Sprite Mode holds the frames")
    closed = S["closed"].read_text()
    check(closed.count("sheet: " + WALK) == 2 and "framesRect" not in closed, f"the closed scene's two sprites are sheet sprites: {closed[:400]}")
    sprite = (await ed.call("get_entity", scene=S["main"], entity="Walker", components=["sprite"]))["components"]["sprite"]
    check(WALK in json.dumps(sprite.get("sheet")) and not sprite.get("numFramesRect"), f"Walker is a sheet sprite: {sprite}")
    prefab_now = S["prefab"].read_text()
    check("sheet: " + WALK in prefab_now and "framesRect" not in prefab_now, f"the prefab file's sprite converted: {prefab_now[-800:]}")
    await ed.call("instantiate_prefab", scene=S["main"], prefab="prefabs/Walker.prefab", name="FromPrefab")
    got = (await ed.call("get_entity", scene=S["main"], entity="FromPrefab", components=["sprite"]))["components"]["sprite"]
    check(WALK in json.dumps(got.get("sheet")), f"the prefab's sprite converted: {got}")
    await ed.call("undo", scene=S["main"])
    after = await play_frame(ed)
    check(after[0] == 2 and after[1] == "walk_2" and WALK in str(after[2]), f"the clip keyed frame 2 shows walk_2: {after}")
    check(abs(after[3] - before[3]) < 1e-4 and abs(after[4] - before[4]) < 1e-4, f"the same rect: {before} {after}")

    # one undo restores the meta, the closed scene, the prefab and the open sprite
    await ed.call("undo", scope="project")
    now = meta.read_bytes() if meta.exists() else None
    check(now == meta_before, f"the meta as it was: {now!r} vs {meta_before!r}")
    check(S["closed"].read_bytes() == closed_before, "the closed scene as it was")
    sprite = (await ed.call("get_entity", scene=S["main"], entity="Walker", components=["sprite"]))["components"]["sprite"]
    check(not sprite.get("sheet") and sprite.get("numFramesRect") == 3, f"Walker is inline again: {sprite}")
    check(S["prefab"].read_bytes() == prefab_before, f"the prefab as it was: {S['prefab'].read_text()[-600:]}")


KEPT = "assets/legacy/kept.png"
KEPT_FRAMES = [{"name": "k_0", "rect": [0, 0, 16, 16]}, {"name": "k_0", "rect": [16, 0, 16, 16]}]


async def test_undo_keeps_saves_made_since(ed: Editor):
    """Undoing a conversion puts the sprites back without writing the old bytes over a save made
    since (another program's), and through the open document when the scene was opened meanwhile;
    a name used twice becomes unique, a shown rect that is none of the frames is kept as one, and a
    CRLF file keeps its line endings"""
    (ed.project / KEPT).write_bytes(png(32, 16))
    await ed.call("sync_files", paths=[KEPT])
    kept = (await ed.call("create_scene", name="Kept", type="2d", path="scenes/Kept.scene"))["scene"]["id"]
    await ed.call("set_sprite_frames", scene=kept, create={"name": "K1"}, texture=KEPT, frames=KEPT_FRAMES, frame=0, inline=True)
    # K2 shows a rect that is none of its frames (its texture rect set by hand)
    await ed.call("set_sprite_frames", scene=kept, create={"name": "K2"}, texture=KEPT, frames=KEPT_FRAMES, frame=0, inline=True)
    await ed.call("set_properties", scene=kept, entity="K2", values={"mesh": {"submeshes[0].textureRect": [0.25, 0, 0.5, 1]}})
    await ed.call("wait", frames=3)
    await ed.call("save_scene", scene=kept)
    await ed.call("close_scene", scene=kept)
    file = ed.project / "scenes" / "Kept.scene"
    # a Windows checkout (CRLF) of a file from before sheet sprites (scene format 7: no sheet sprite in it)
    text = file.read_bytes().replace(b"\r\n", b"\n")
    check(b"formatVersion: 8" in text, f"saved at format 8: {text[:80]}")
    file.write_bytes(text.replace(b"formatVersion: 8", b"formatVersion: 7").replace(b"\n", b"\r\n"))
    await ed.call("sync_files", paths=["scenes/Kept.scene"])
    await ed.call("set_sprite_frames", scene=S["main"], create={"name": "K0"}, texture=KEPT, frames=KEPT_FRAMES, frame=1, inline=True)

    r = await ed.call("set_sprite_frames", scene=S["main"], entity="K0", convert=True, all_users=True)
    check(len(r["converted"]) == 3, f"K0, K1, K2 converted: {r}")
    meta = (ed.project / (KEPT + ".meta")).read_text()
    check("k_0 2" in meta, f"the second k_0 is unique in the Sprite Mode: {meta}")
    converted = file.read_bytes()
    check(b"\r\n" in converted and b"\n" not in converted.replace(b"\r\n", b""), "the converted file keeps CRLF")
    check(b"formatVersion: 8" in converted, f"the file holding sheet sprites now says format 8: {converted[:80]}")
    k2 = next(c for c in r["converted"] if c["game_object"] == "K2")
    check(k2["frame"] == 2, f"K2's rect is kept as a third frame: {r['converted']}")
    k0 = (await ed.call("get_entity", scene=S["main"], entity="K0", components=["sprite"]))["components"]["sprite"]
    check(k0.get("frameName") == "k_0 2", f"K0 shows the second frame by its unique name: {k0}")

    # another program saves the closed scene (a rename) before the undo
    text = file.read_bytes().replace(b"name: K1", b"name: K1-renamed")
    file.write_bytes(text)
    await ed.call("sync_files", paths=["scenes/Kept.scene"])
    await ed.call("undo", scope="project")
    after = file.read_text()
    check("K1-renamed" in after, "the save made since is kept")
    check(f"sheet: {KEPT}" not in after and after.count("framesRect") == 2, f"the sprites are inline again: {after[:600]}")

    # converted again, then the scene opened before the undo: the open document is restored
    await ed.call("set_sprite_frames", scene=S["main"], entity="K0", convert=True, all_users=True)
    check(f"sheet: {KEPT}" in file.read_text(), "converted in its file")
    await ed.call("open_scene", scene="scenes/Kept.scene")
    on_disk = file.read_bytes()
    await ed.call("undo", scope="project")
    k1 = (await ed.call("get_entity", scene="Kept", entity="K1-renamed", components=["sprite"]))["components"]["sprite"]
    check(not k1.get("sheet") and k1.get("numFramesRect") == 2, f"the open K1 is inline again: {k1}")
    scenes = {sc["name"]: sc for sc in (await ed.call("list_scenes"))["scenes"]}
    check(scenes["Kept"]["modified"], f"the restored document is modified (unsaved): {scenes['Kept']}")
    check(file.read_bytes() == on_disk, "the file under the open document is not written behind its back")
    await ed.call("close_scene", scene="Kept", unsaved="discard")
    await ed.call("select_scene", scene=S["main"])
    await ed.call("delete_entities", scene=S["main"], entities=["K0"])


async def test_convert_onto_a_matching_grid_names_the_frame(ed: Editor):
    """A texture whose Grid already holds the sprite's rects keeps its Grid; the sprite shows its frame by the grid's name"""
    gridded = "assets/legacy/gridded.png"
    (ed.project / gridded).write_bytes(png(32, 16))
    (ed.project / (gridded + ".meta")).write_text("import:\n  spriteSheet: {version: 1, mode: grid, grid: {cell: [16, 16], offset: [0, 0], "
                                                  "spacing: [0, 0], count: 0, skipEmpty: false, prefix: gr_}, pivot: center}\n")
    await ed.call("sync_files", paths=[gridded, gridded + ".meta"])
    await ed.call("set_sprite_frames", scene=S["main"], create={"name": "OnGrid"}, texture=gridded,
                  frames=[{"name": "a", "rect": [0, 0, 16, 16]}, {"name": "b", "rect": [16, 0, 16, 16]}], frame=1, inline=True)
    r = await ed.call("set_sprite_frames", scene=S["main"], entity="OnGrid", convert=True)
    check(r["frames"] == 2, f"the grid's two frames: {r}")
    sprite = (await ed.call("get_entity", scene=S["main"], entity="OnGrid", components=["sprite"]))["components"]["sprite"]
    check(sprite.get("frame") == 1 and sprite.get("frameName") == "gr_1", f"frame 1 by the grid's name: {sprite}")
    check("mode: grid" in (ed.project / (gridded + ".meta")).read_text(), "the Grid is kept")
    await ed.call("delete_entities", scene=S["main"], entities=["OnGrid"])


async def test_three_hundred_inline_frames_play(ed: Editor):
    """AC8: a legacy sprite with 300 inline frames saves, loads again, draws and plays them all"""
    strip = "assets/legacy/strip.png"
    (ed.project / strip).write_bytes(png(160, 120))
    await ed.call("sync_files", paths=[strip])
    frames = [{"name": f"f{i}", "rect": [(i % 20) * 8, (i // 20) * 8, 8, 8]} for i in range(300)]
    sid = (await ed.call("create_scene", name="Many", type="2d", path="scenes/Many.scene"))["scene"]["id"]
    await ed.call("set_sprite_frames", scene=sid, create={"name": "Many", "position": [100, 100, 0]}, texture=strip, frames=frames, frame=0,
                  size=[64, 64], inline=True)
    await ed.call("set_sprite_animation", scene=sid, sprite="Many", fps=60, loop=True)
    await ed.call("wait", frames=3)
    await ed.call("save_scene", scene=sid)
    await ed.call("close_scene", scene=sid)
    await ed.call("open_scene", scene="scenes/Many.scene")
    got = (await ed.call("get_entity", scene="Many", entity="Many", components=["sprite"]))["components"]["sprite"]
    check(got.get("numFramesRect") == 300, f"300 frames saved and loaded: {got.get('numFramesRect')}")
    shot = await ed.call_raw("screenshot", {"view": "game", "scene": "Many", "max_width": 0})
    check(not shot.is_error and shot.images and not pngdiff.flat(pngdiff.decode(shot.images[0][0])), "it draws")
    await ed.call("play", action="start", fixed_fps=60, auto_pause=True)
    try:
        seen = set()
        for _ in range(4):
            await ed.call("wait", game_seconds=1.25)
            r = (await ed.call("eval_lua", code="local r = object('Many', 'Sprite').textureRect return r.x, r.y"))["values"]
            seen.add((round(r[0] * 20), round(r[1] * 15)))
        check(len(seen) >= 3 and any(y >= 10 for _, y in seen), f"it plays far into the 300 frames: {sorted(seen)}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    await ed.call("close_scene", scene="Many", unsaved="discard")
    await ed.call("select_scene", scene=S["main"])


async def test_sheet_conflict(ed: Editor):
    # OTHER already has a grid of 8 x 8 cells; a sprite with other rects cannot move into it
    await ed.call("set_sprite_frames", scene=S["main"], create={"name": "Gridded"}, texture=OTHER, slice={"tile_size": [8, 8]})
    await ed.call("set_sprite_frames", scene=S["main"], create={"name": "Clash"}, texture=OTHER,
                  frames=[{"name": "big", "rect": [0, 0, 16, 16]}], inline=True)
    raw = await ed.call_raw("set_sprite_frames", {"scene": S["main"], "entity": "Clash", "convert": True})
    check(raw.is_error and "sheet_conflict" in raw.text and "inline" in raw.text, f"sheet_conflict with the fix: {raw.text}")


async def test_check_after_conversion_and_atlas_errors(ed: Editor):
    await ed.call("set_sprite_frames", scene=S["main"], entity="Walker", convert=True, all_users=True)
    await ed.call("delete_entities", scene=S["main"], entities=["Clash"])
    await ed.call("save_scene", scene=S["main"])
    await ed.call("save_project")
    code, findings = check_sprites(ed)
    remaining = [f for f in findings if f.get("code") == "sprite-inline-frames" and WALK in f.get("message", "")]
    check(code == 0 and not remaining, f"no legacy sprite of {WALK} left: {remaining}")
    # an atlas with a missing member: exit 1
    atlas = ed.project / "assets" / "broken.spriteatlas"
    atlas.write_text(json.dumps({"format": "jasperengine.spriteatlas", "version": 1, "guid": "0" * 31 + "1", "include_in_build": True,
                                 "members": [{"path": "assets/legacy/gone.png"}], "exclude": [], "settings": {}}))
    code, findings = check_sprites(ed)
    check(code == 1 and any(f.get("code") == "missing_member" and f["severity"] == "error" for f in findings),
          f"a missing member is an error: exit {code}, {findings}")
    # a member that is there (the default project's assets folder is the project folder): exit 0
    atlas.write_text(json.dumps({"format": "jasperengine.spriteatlas", "version": 1, "guid": "0" * 31 + "2", "include_in_build": True,
                                 "members": [{"path": WALK}], "exclude": [], "settings": {}}))
    code, findings = check_sprites(ed)
    check(code == 0 and not any(f.get("code") == "missing_member" for f in findings), f"a present member: exit {code}, {findings}")
    atlas.unlink()
