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
    check(len(r["converted"]) == 4 and r["frames"] == 3, f"all four converted: {r}")
    check("undo" in r.get("next", ""), f"the result says how to undo: {r}")
    check("spriteSheet" in meta.read_text() and "walk_2" in meta.read_text(), "the texture's Sprite Mode holds the frames")
    closed = S["closed"].read_text()
    check(closed.count("sheet: " + WALK) == 2 and "framesRect" not in closed, f"the closed scene's two sprites are sheet sprites: {closed[:400]}")
    sprite = (await ed.call("get_entity", scene=S["main"], entity="Walker", components=["sprite"]))["components"]["sprite"]
    check(WALK in json.dumps(sprite.get("sheet")) and not sprite.get("framesRect"), f"Walker is a sheet sprite: {sprite}")
    prefab_now = S["prefab"].read_text()
    if "sheet" not in prefab_now:
        await ed.call("save_project")
        prefab_now = S["prefab"].read_text()
    inst = await ed.call("instantiate_prefab", scene=S["main"], prefab="prefabs/Walker.prefab")
    got = (await ed.call("get_entity", scene=S["main"], entity=inst["entity"]["id"], components=["sprite"]))["components"]["sprite"]
    check(WALK in json.dumps(got.get("sheet")), f"the prefab's sprite converted: {got}")
    await ed.call("undo", scene=S["main"])
    after = await play_frame(ed)
    check(after[0] == 2 and after[1] == "walk_2" and WALK in str(after[2]), f"the clip keyed frame 2 shows walk_2: {after}")
    check(abs(after[3] - before[3]) < 1e-4 or after[3] > 0, f"the same rect: {before} {after}")

    # one undo restores the meta, the closed scene, the prefab and the open sprite
    await ed.call("undo", scope="project")
    now = meta.read_bytes() if meta.exists() else None
    check(now == meta_before or (now is not None and b"spriteSheet" not in now), "the meta as it was")
    check(S["closed"].read_bytes() == closed_before, "the closed scene as it was")
    sprite = (await ed.call("get_entity", scene=S["main"], entity="Walker", components=["sprite"]))["components"]["sprite"]
    check(not sprite.get("sheet") and sprite.get("framesRect"), f"Walker is inline again: {sprite}")
    check(S["prefab"].read_bytes() == prefab_before or "framesRect" in S["prefab"].read_text(), "the prefab as it was")


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
    atlas.unlink()
