"""User post-process passes (shader library phase 4, tier T3): a pass is a project fork or a library
post-process shader ("library:<name>", @shader postprocess), with textures of its own (by uniform)
and a resolution scale. set_scene_settings post_process takes and get_scene_settings gives
{shader, enabled, uniforms, textures, scale} (textures and scale only when set); the scene file
saves them only when set, so a chain saved before them re-saves unchanged. list_shaders
type="postprocess" lists the library's post-process shaders."""

from __future__ import annotations

import struct

import yaml
import zlib

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Post Process Passes"}
TIMEOUT = 600

SCENE = "scenes/PostPasses.scene"
LIBRARY_POST = {"library:blur", "library:crt", "library:edge_neon", "library:gameboy", "library:pixelate", "library:posterize"}


def write_png(path, size=4):
    """A small grey RGBA PNG (stdlib only)."""
    raw = b"".join(b"\x00" + bytes([128, 128, 128, 255]) * size for _ in range(size))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


async def test_post_process_passes(ed: Editor):
    ed.require_tools("create_scene", "get_scene_settings", "set_scene_settings", "save_scene", "list_shaders", "undo")

    listed = await ed.call("list_shaders", type="postprocess")
    names = {s["name"] if "name" in s else s.get("shader") for s in listed["shaders"]}
    refs = {s.get("shader", s.get("reference", "")) for s in listed["shaders"]}
    found = {r for r in refs | names if isinstance(r, str) and r.startswith("library:")}
    ed.check(LIBRARY_POST <= found, f"list_shaders type=postprocess has the library post shaders: {sorted(found)}")
    ed.check(all(s.get("type") == "postprocess" for s in listed["shaders"]), "only post-process shaders")

    made = await ed.call("create_scene", name="PostPasses", type="3d")
    sid = made["scene"]["id"]
    await ed.call("save_scene", scene=sid, path=SCENE)
    before = (ed.project / SCENE).read_text()

    write_png(ed.project / "assets" / "textures" / "grain.png")
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": [
        {"shader": "library:pixelate", "uniforms": {"pixelSize": 6}, "scale": 0.5},
        {"shader": "library:crt", "enabled": False},
    ]})
    pp = (await ed.call("get_scene_settings", scene=sid, names=["post_process"]))["settings"]["post_process"]
    ed.check_eq(len(pp), 2)
    ed.check_eq(pp[0]["shader"], "library:pixelate")
    ed.check_eq(pp[0]["scale"], 0.5)
    ed.check_eq(pp[0]["uniforms"], {"pixelSize": [6.0, 0.0, 0.0, 0.0]})
    ed.check("scale" not in pp[1] and "textures" not in pp[1], "unset scale and textures are not listed")
    ed.check_eq(pp[1]["enabled"], False)

    await ed.call("save_scene", scene=sid)
    text = (ed.project / SCENE).read_text()
    saved = yaml.safe_load(text)["scene"].get("postProcess", [])
    ed.check_eq(saved[0].get("shader"), "library:pixelate")
    ed.check_eq(saved[0].get("scale"), 0.5, "the pass's scale is saved")
    ed.check("scale" not in saved[1], "a pass at full scale saves no scale")

    # a library shader of another type, a scale out of range, an undeclared library texture: refused
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_process": [{"shader": "library:toon"}]},
                         contains="postprocess")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_process": [{"shader": "library:blur", "scale": 2}]},
                         contains="0.1")
    await ed.check_error("set_scene_settings", scene=sid, settings={
        "post_process": [{"shader": "library:blur", "textures": {"u_nope": "assets/textures/grain.png"}}]}, contains="no texture")
    pp = (await ed.call("get_scene_settings", scene=sid, names=["post_process"]))["settings"]["post_process"]
    ed.check_eq(pp[0]["shader"], "library:pixelate", "a refused call changes nothing")

    # a fork's textures by uniform (a fork without a manifest takes any name)
    fork = await ed.call("fork_shader", scene=sid, target="post_process", name="PassWithTexture")
    base = fork["custom_shader"]
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": [
        {"shader": base, "textures": {"u_overlayTexture": "assets/textures/grain.png"}}]})
    pp = (await ed.call("get_scene_settings", scene=sid, names=["post_process"]))["settings"]["post_process"]
    ed.check_eq(pp[0].get("textures"), {"u_overlayTexture": "assets/textures/grain.png"}, "project-relative, as given")
    await ed.call("save_scene", scene=sid)
    text = (ed.project / SCENE).read_text()
    ed.check("u_overlayTexture" in text and "grain.png" in text, "the pass texture is saved")

    # no chain: no postProcess key, as before the passes (shaderKeys and maxValues record what was
    # loaded meanwhile)
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": []})
    await ed.call("save_scene", scene=sid)
    after = yaml.safe_load((ed.project / SCENE).read_text())
    ed.check("postProcess" not in after["scene"], "an empty chain saves no postProcess key")
    ed.check_eq(after["scene"], yaml.safe_load(before)["scene"], "the scene settings re-save as before the passes")
