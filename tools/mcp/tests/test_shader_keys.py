"""Shader variant keys with 64 property bits, end to end (the unit tests are editor/tests/ShaderKeyTest.cpp):

- a scene's shaderKeys: re-saved byte-identical, and the legacy packed form (a bare integer, 32
  property bits, a custom id that is dropped) still read and written back as a map; MESH bit 5 (the
  retired 'Pcf', Stochastic Tiling since October 2026) read from a key without `keyVersion: 2` is
  dropped, and kept with it;
- get_build_info lists keys in the short packed hex they always had; generate_shaders takes those, the
  wide 24-digit form ("0x" customId type properties) and specs, and writes .sdat files of format
  version 2 (the key: u64 properties, u32 type, u32 customId) under the names the variants always had.
"""

from __future__ import annotations

import re
import struct

import yaml

from harness import Editor

EDITOR = {"create": True, "name": "Shader Keys"}
TIMEOUT = 900

MESH, SKYBOX, DEPTH = 2, 3, 4   # ShaderType
STATE: dict = {}


def packed(shader_type: int, properties: int, custom_id: int = 0) -> int:
    """The legacy packed key: customId << 48 | type << 32 | 32 property bits."""
    return (custom_id << 48) | (shader_type << 32) | properties


def scene_text() -> str:
    return STATE["path"].read_text()


def shader_keys(text: str) -> list:
    return yaml.safe_load(text).get("shaderKeys", [])


def shader_keys_text(text: str) -> str:
    """The shaderKeys block as written (the rest of a scene holds runtime state, vertexCount...)."""
    match = re.search(r"^shaderKeys:\n(?:[ ]+.*\n)*", text, re.M)
    return match.group(0) if match else ""


async def test_scene_resaves_byte_identical(ed: Editor):
    made = await ed.call("create_scene", name="Keys", type="3D")
    sid = made["scene"]["id"]
    STATE["id"] = sid
    await ed.call("create_entity", scene=sid, type="box", name="Box")
    saved = await ed.call("save_scene", scene=sid, path="scenes/Keys")
    STATE["path"] = ed.project / saved["scene"]["path"]
    first = scene_text()
    keys = shader_keys(first)
    ed.check(keys and all(isinstance(k, dict) for k in keys), f"the box's variants are stored as maps: {keys}")
    ed.check(any(k["type"] == "mesh" for k in keys), f"a mesh variant: {keys}")
    await ed.call("save_scene", scene=sid, path="scenes/Keys")
    ed.check(shader_keys_text(first), "a shaderKeys block")
    ed.check_eq(shader_keys_text(scene_text()), shader_keys_text(first), "unchanged keys re-save byte-identical")


async def test_scene_reads_legacy_keys(ed: Editor):
    sid = STATE["id"]
    text = scene_text()
    match = re.search(r"^shaderKeys:\n(\s*)- ", text, re.M)
    ed.check(match, "a shaderKeys list in the scene file")
    indent = match.group(1)
    legacy = [packed(MESH, 0x41), packed(DEPTH, 0x3, 12), packed(MESH, 0x61 | 0x80000000, 3)]  # bit 5: retired
    extra = "".join(f"{indent}- {value}\n" for value in legacy)
    extra += f"{indent}- type: mesh\n{indent}  properties: 4294967263\n"   # every MESH bit but 5 (32-bit value)
    # bit 5 in a map: a stale 'Pcf' without keyVersion, Stochastic Tiling with keyVersion 2
    extra += f"{indent}- type: mesh\n{indent}  properties: 99\n"            # 0x63: Ult, Uv1, Pcf, Nor
    extra += f"{indent}- type: mesh\n{indent}  properties: 38\n{indent}  keyVersion: 2\n"   # Uv1, Uv2, Sto
    STATE["path"].write_text(text[:match.end(0) - 2 - len(indent)] + extra + text[match.end(0) - 2 - len(indent):])
    ed.check(len(shader_keys(scene_text())) == len(shader_keys(text)) + 6, "the edit adds six keys")

    # read from disk again
    await ed.call("close_scene", scene=sid)
    await ed.call("open_scene", scene="scenes/Keys.scene")
    await ed.call("save_scene", scene=sid, path="scenes/Keys")
    keys = shader_keys(scene_text())
    ed.check(all(isinstance(k, dict) for k in keys), f"legacy integers are written back as maps: {keys}")
    for want in ({"type": "mesh", "properties": 0x41}, {"type": "depth", "properties": 3},
                 {"type": "mesh", "properties": 0x80000041}, {"type": "mesh", "properties": 0xFFFFFFDF},
                 {"type": "mesh", "properties": 0x43}, {"type": "mesh", "properties": 0x26, "keyVersion": 2}):
        ed.check_in(want, keys, "decoded legacy key")
    ed.check(not any(k.get("properties", 0) & 0x20 and k["type"] == "mesh" and "keyVersion" not in k for k in keys),
             "the retired bit 5 is dropped where no keyVersion says it is Stochastic Tiling")
    # (the scene's own variants of the built-in Lit carry its S11 defines as "builtin:lit#...")
    ed.check(not any("customShader" in k and not str(k["customShader"]).startswith("builtin:lit") for k in keys),
             f"a legacy key's custom id is not a fork: {keys}")
    again = shader_keys_text(scene_text())
    await ed.call("save_scene", scene=sid, path="scenes/Keys")
    ed.check_eq(shader_keys_text(scene_text()), again, "and re-saves byte-identical")


async def test_build_info_keys_keep_their_text(ed: Editor):
    await ed.call("edit_build_scenes", action="add", scene=STATE["id"])
    info = await ed.call("get_build_info", include_shaders=True)
    ed.check(info["shaders"], "the scene's shaders are listed")
    for shader in info["shaders"]:
        # S10's Shadows HQ is MESH bit 33: a key with it is wide (type, then the 64 property bits)
        wide = "Shq" in shader["name"]
        ed.check(re.fullmatch(r"0x[0-9a-f]{24}" if wide else r"0x[0-9a-f]{1,16}", shader["key"]),
                 f"a 32-bit key is the packed hex, a wide one type + properties: {shader}")
    mesh = [s for s in info["shaders"] if s["key"] == hex(packed(MESH, 0x41))]
    ed.check(mesh, f"the legacy key read from the scene is listed as {hex(packed(MESH, 0x41))}: {info['shaders']}")


def read_sdat_key(data: bytes) -> tuple:
    magic, version = struct.unpack_from("<4sI", data, 0)
    properties, shader_type, custom_id = struct.unpack_from("<QII", data, 8)
    return magic, version, properties, shader_type, custom_id


async def test_generate_shaders_takes_old_and_wide_keys(ed: Editor):
    out = ed.scratch.root / "shaders-keys"
    wide = "0x" + f"{0:04x}{MESH:04x}{(1 << 40) | 0x41:016x}"   # bit 40 names nothing yet: normalized away
    ed.check_eq(len(wide), 26)
    r = await ed.call("generate_shaders", output_dir=str(out), backends=["opengl"], format="binary",
                      shaders=[hex(packed(MESH, 0x41)), wide, "sky:Pcl", str(packed(DEPTH, 0x3))], timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    files = {p.name: p for p in out.rglob("*.sdat")}
    ed.check_eq(sorted(files), ["depth_TexSki_glsl410.sdat", "mesh_UltNor_glsl410.sdat", "sky_Pcl_glsl410.sdat"])
    for name, (shader_type, properties) in {"mesh_UltNor_glsl410.sdat": (MESH, 0x41),
                                            "sky_Pcl_glsl410.sdat": (SKYBOX, 0x1),
                                            "depth_TexSki_glsl410.sdat": (DEPTH, 0x3)}.items():
        ed.check_eq(read_sdat_key(files[name].read_bytes()), (b"SDAT", 2, properties, shader_type, 0), name)

    for bad in ["0x00000002000001000000004", "0x0000000200000100000000411", "0x00000002000001000000004g"]:
        await ed.call("generate_shaders", output_dir=str(out), backends=["opengl"], format="binary",
                      shaders=[bad], expect_error=True)
