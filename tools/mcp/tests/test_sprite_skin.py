"""2D skeletal sprites and layered PSD/PSB import over MCP (docs/specs/S94-rendering-anim-gaps.md
F48, F49; AC45, AC46), on a new project:

- edit_sprite_skin on a Sprite of a plain texture: bones.create (lower, upper), auto_geometry and
  auto_weights in one call give a weighted skin, the skin in the texture's meta (import.spriteSkin),
  two bone GameObjects under the Sprite and its Sprite Skin bound to them; a call with no edit reads
  the skin; a bad bone names the bones; one undo (scope timeline) takes the meta and the GameObjects
  back, redo restores them;
- set_weights at weight 1 binds the whole mesh to one bone (a rigid part);
- Play pose: in Play, Lua turns the upper bone through SpriteSkin(scene, e).bones and the Game view
  changes where the upper half was, while the Sprite Skin reports itself skinned;
- set_texture_import_settings import_mode: layers on a PSD and on a PSB written here (two layers in a
  group, one below): three sprites named after the layers with layer keys, <file>.prefab with the
  group as a parent; instantiate_prefab places them; a refusal on a PNG names the PSD/PSB rule; one
  undo (scope project) takes the import mode back.
"""

from __future__ import annotations

import struct

import harness
import pngdiff
from harness import Editor, TestFailure

CI = True
EDITOR = {"create": True, "name": "SpriteSkin"}

TEXTURE = "assets/skin/column.png"
S: dict = {}


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def column_png() -> bytes:
    """A 16 x 64 opaque column, red down it, green across it"""
    rgba = bytearray()
    for y in range(64):
        for x in range(16):
            rgba += bytes((60 + y * 3, 40 + x * 12, 200, 255))
    return pngdiff.encode(16, 64, bytes(rgba))


async def screenshot(ed: Editor, scene, view: str = "game") -> tuple[int, int, bytes]:
    raw = await ed.call_raw("screenshot", {"view": view, "scene": scene, "max_width": 0})
    check(not raw.is_error and raw.images, f"screenshot: {raw.text[:300]}")
    return pngdiff.decode(raw.images[0][0])


def changed_pixels(a: tuple[int, int, bytes], b: tuple[int, int, bytes]) -> int:
    check(a[0] == b[0] and a[1] == b[1], "the screenshots have one size")
    return sum(1 for i in range(0, len(a[2]), 4) if a[2][i:i + 3] != b[2][i:i + 3])


async def test_setup(ed: Editor):
    (ed.project / TEXTURE).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / TEXTURE).write_bytes(column_png())
    await ed.call("sync_files", paths=[TEXTURE])
    await ed.call("tool_groups", enable=["sprites"])
    S["scene"] = (await ed.call("create_scene", name="Skin", type="2d"))["scene"]["id"]
    made = await ed.call("set_sprite_frames", scene=S["scene"], create={"name": "Arm"}, sheet=TEXTURE, frame=0)
    check(made.get("sheet") == TEXTURE, f"a sheet Sprite of the texture: {made}")
    await ed.call("set_properties", scene=S["scene"], entity="Arm",
                  values={"sprite": {"width": 48, "height": 192, "pivotPreset": "bottom_center"}})


async def test_bones_geometry_weights_one_undo(ed: Editor):
    meta = ed.project / (TEXTURE + ".meta")
    before = meta.read_bytes() if meta.exists() else None
    r = await ed.call("edit_sprite_skin", scene=S["scene"], entity="Arm",
                      bones={"create": [{"name": "lower", "head": [8, 64], "tail": [8, 32]},
                                        {"name": "upper", "parent": "lower", "head": [8, 32], "tail": [8, 0]}]},
                      auto_geometry={"cell_size": 8}, auto_weights=True)
    check([b["name"] for b in r["bones"]] == ["lower", "upper"], f"two bones: {r}")
    check(r["vertices"] > 8 and r["triangles"] > 4 and r["weighted"], f"a weighted mesh: {r}")
    check(all("gameobject" in b for b in r["bones"]), f"each bone has its GameObject: {r}")
    check(r["bones"][0]["vertices"] > 0 and r["bones"][1]["vertices"] > 0, f"both bones move vertices: {r}")
    check("next" in r, "the result says what is next")
    check("spriteSkin" in meta.read_text(), "the skin is in the texture's meta")
    skin = (await ed.call("get_entity", scene=S["scene"], entity="Arm", components=["spriteskin"]))["components"]["spriteskin"]
    check(len(skin.get("bones", [])) == 2, f"the Sprite Skin binds two bones: {skin}")
    S["upper"] = r["bones"][1]["gameobject"]["id"]
    upper = (await ed.call("get_entity", scene=S["scene"], entity=S["upper"], components=["transform"]))["components"]
    check("transform" in upper, f"a bone is a GameObject with a Transform: {upper}")
    found = await ed.call("find_entities", scene=S["scene"], name="upper")
    check(found.get("entities"), "the bone GameObject is in the scene")

    read = await ed.call("edit_sprite_skin", scene=S["scene"], entity="Arm")
    check(read["vertices"] == r["vertices"] and not read["changed"], f"no edit reads the skin: {read}")
    bad = await ed.call("edit_sprite_skin", expect_error=True, scene=S["scene"], entity="Arm", set_weights=[{"bone": "elbow"}])
    check("lower" in bad and "upper" in bad, f"an unknown bone names the bones: {bad}")

    await ed.call("undo", scope="timeline", scene=S["scene"])
    now = meta.read_bytes() if meta.exists() else None
    check(now == before or (now is not None and b"spriteSkin" not in now), "one undo takes the skin out of the meta")
    check(not (await ed.call("find_entities", scene=S["scene"], name="upper")).get("entities"), "and the bone GameObjects")
    await ed.call("redo", scope="timeline", scene=S["scene"])
    check("spriteSkin" in meta.read_text(), "redo writes it again")
    check((await ed.call("find_entities", scene=S["scene"], name="upper")).get("entities"), "and makes the bones again")


async def test_bind_to_one_bone(ed: Editor):
    r = await ed.call("edit_sprite_skin", scene=S["scene"], entity="Arm", set_weights=[{"bone": "upper", "weight": 1}])
    check(r["bones"][1]["vertices"] == r["vertices"] and r["bones"][0]["vertices"] == 0, f"every vertex on upper: {r}")
    await ed.call("edit_sprite_skin", scene=S["scene"], entity="Arm", auto_weights=True)


async def test_play_pose(ed: Editor):
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=30, auto_pause=True)
    try:
        await ed.call("wait", frames=5)
        rest = await screenshot(ed, S["scene"])
        seen = await ed.call("eval_lua", scene=S["scene"], code="""
            local skin = SpriteSkin(scene, entity("Arm"))
            return {skinned = skin.skinned, bones = skin.bones, count = skin.skinBoneCount, upper = entity("upper")}
        """)
        value = (seen.get("values") or [{}])[0]
        check(value.get("skinned") is True and value.get("count") == 2, f"Lua sees the skin: {seen}")
        check(len(value.get("bones", [])) == 2 and value["bones"][1] == value.get("upper"), f"bones lists the bone GameObjects: {seen}")
        await ed.call("eval_lua", scene=S["scene"], code="""
            local skin = SpriteSkin(scene, entity("Arm"))
            object(skin.bones[2]):setRotation(0, 0, 60)
            return true
        """)
        await ed.call("wait", frames=3)
        bent = await screenshot(ed, S["scene"])
        check(changed_pixels(rest, bent) > 500, "turning the upper bone bends the Sprite in the Game view")
    finally:
        await ed.call("play", action="stop", scene=S["scene"])


# ---- layered PSD / PSB (a minimal raw-channel writer: Adobe's layer records, lsct groups, lyid)

def _pascal(name: str) -> bytes:
    raw = name.encode("ascii")
    data = bytes([len(raw)]) + raw
    return data + b"\0" * ((4 - len(data) % 4) % 4)


def psd_bytes(width: int, height: int, layers: list, psb: bool) -> bytes:
    """layers bottom first: (name, (x, y, w, h), rgb, kind) with kind "pixels", "start" (a group's
    end marker, below its children) or "group" (the group record, above them)"""
    long_len = ">Q" if psb else ">I"
    records = b""
    channels = b""
    for index, (name, rect, rgb, kind) in enumerate(layers):
        x, y, w, h = rect if kind == "pixels" else (0, 0, 0, 0)
        planes = []
        for c in (-1, 0, 1, 2):
            value = 255 if c == -1 else rgb[c]
            planes.append((c, b"\0\0" + bytes([value]) * (w * h)))
        record = struct.pack(">iiiiH", y, x, y + h, x + w, 4)
        for c, data in planes:
            record += struct.pack(">h", c) + struct.pack(long_len, len(data))
        record += b"8BIMnorm" + bytes([255, 0, 0, 0])
        extra = struct.pack(">I", 0) + struct.pack(">I", 0) + _pascal(name)
        if kind in ("start", "group"):
            body = struct.pack(">I", 3 if kind == "start" else 1) + (b"8BIMpass" if kind == "group" else b"")
            extra += b"8BIMlsct" + struct.pack(">I", len(body)) + body
        extra += b"8BIMlyid" + struct.pack(">II", 4, index + 1)
        record += struct.pack(">I", len(extra)) + extra
        records += record
        for _, data in planes:
            channels += data
    info = struct.pack(">h", len(layers)) + records + channels
    if len(info) % 2:
        info += b"\0"
    layer_info = struct.pack(long_len, len(info)) + info
    layer_and_mask = layer_info + struct.pack(">I", 0)
    merged = b"\0\0" + bytes(width * height * 3) + bytes([255]) * (width * height)
    header = b"8BPS" + struct.pack(">H", 2 if psb else 1) + b"\0" * 6 + struct.pack(">HIIHH", 4, height, width, 8, 3)
    return header + struct.pack(">I", 0) + struct.pack(">I", 0) + struct.pack(long_len, len(layer_and_mask)) + layer_and_mask + merged


CHARACTER = [
    ("Body", (20, 20, 24, 24), (200, 40, 40), "pixels"),
    ("</Layer group>", None, (0, 0, 0), "start"),
    ("Face", (24, 4, 16, 16), (240, 200, 150), "pixels"),
    ("Hat", (22, 0, 20, 6), (30, 30, 200), "pixels"),
    ("Head", None, (0, 0, 0), "group"),
]


async def import_layers(ed: Editor, path: str, psb: bool):
    (ed.project / path).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / path).write_bytes(psd_bytes(64, 48, CHARACTER, psb))
    await ed.call("sync_files", paths=[path])
    r = await ed.call("set_texture_import_settings", paths=[path], settings={"import_mode": "layers"})
    layers = r.get("layers", [])
    check(len(layers) == 1 and "error" not in layers[0], f"{path} imported its layers: {r}")
    names = sorted(s["name"] for s in layers[0]["sprites"])
    check(names == ["Body", "Face", "Hat"], f"three sprites named after the layers: {layers}")
    check(all(s["key"].startswith("layer:") for s in layers[0]["sprites"]), f"keyed by layer id: {layers}")
    prefab = layers[0]["prefab"]
    check((ed.project / prefab).exists(), f"{prefab} is written")
    check("next" in r, "the result says how to place them")
    return prefab


async def test_psd_layers(ed: Editor):
    prefab = await import_layers(ed, "assets/art/hero.psd", psb=False)
    text = (ed.project / prefab).read_text()
    check("Head" in text and "Hat" in text and "frameName" in text, f"the prefab has the group and the sprites: {text[:400]}")
    await ed.call("sync_files", paths=[prefab])
    placed = await ed.call("instantiate_prefab", scene=S["scene"], prefab=prefab)
    check(placed, f"instantiated: {placed}")
    found = await ed.call("find_entities", scene=S["scene"], name="Hat")
    check(found.get("entities"), "the Hat layer is a GameObject")
    face = (await ed.call("find_entities", scene=S["scene"], name="Face"))["entities"][0]
    check("Head" in face.get("path", ""), f"Face is under the Head group: {face}")
    sprite = (await ed.call("get_entity", scene=S["scene"], entity=face["id"], components=["sprite"]))["components"]["sprite"]
    check("hero.psd" in str(sprite.get("sheet")) and sprite.get("frameName") == "Face", f"a Sprite of the PSD's Face layer: {sprite}")

    meta = ed.project / "assets/art/hero.psd.meta"
    await ed.call("undo", scope="project")
    check("layers" not in meta.read_text(), "one undo takes the import mode back")


async def test_psb_layers(ed: Editor):
    await import_layers(ed, "assets/art/truck.psb", psb=True)


async def test_import_mode_refused_on_png(ed: Editor):
    msg = await ed.call("set_texture_import_settings", expect_error=True, paths=[TEXTURE], settings={"import_mode": "layers"})
    check(".psd" in msg and ".psb" in msg, f"the refusal names the PSD/PSB rule: {msg}")
