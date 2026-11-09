"""Shader variant keys with 64 property bits, end to end (the unit tests are editor/tests/ShaderKeyTest.cpp):

- get_build_info lists keys in the short packed hex they always had; generate_shaders takes those, the
  wide 24-digit form ("0x" customId type properties) and specs, and writes .sdat files of format
  version 2 (the key: u64 properties, u32 type, u32 customId) under the names the variants always had.
"""

from __future__ import annotations

import re
import struct

from harness import Editor

EDITOR = {"create": True, "name": "Shader Keys"}
CI = True
TIMEOUT = 900

MESH, SKYBOX, DEPTH = 2, 3, 4   # ShaderType
# the MESH properties past the 32 of older keys (ShaderPool::getShaderPropertyName, bits 32-45): a
# key with one is wide
WIDE_MESH = {"Ltc", "Shq", "Vat", "Vab", "Til", "Cap", "Cts", "Oit", "Lfd", "Aui", "Wex", "Wwk", "Dlo", "Pxs", "Sfx", "Sfr", "Spv"}
UNNAMED_BIT = 63


def packed(shader_type: int, properties: int, custom_id: int = 0) -> int:
    """The legacy packed key: customId << 48 | type << 32 | 32 property bits."""
    return (custom_id << 48) | (shader_type << 32) | properties


async def test_build_info_keys_keep_their_text(ed: Editor):
    made = await ed.call("create_scene", name="Keys", type="3D")
    sid = made["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="box", name="Box")
    await ed.call("save_scene", scene=sid, path="scenes/Keys")
    await ed.call("edit_build_scenes", action="add", scene=sid)
    info = await ed.call("get_build_info", include_shaders=True)
    ed.check(info["shaders"], "the scene's shaders are listed")
    for shader in info["shaders"]:
        # S10's Shadows HQ is MESH bit 33, S54's Lod Fade bit 40: a key with one is wide (type, then
        # the 64 property bits)
        names = shader["shader"].rsplit("_", 1)[-1]
        wide = shader["shader"].startswith("mesh_") and not WIDE_MESH.isdisjoint(names[i:i + 3] for i in range(0, len(names), 3))
        ed.check(re.fullmatch(r"0x[0-9a-f]{24}" if wide else r"0x[0-9a-f]{1,16}", shader["key"]),
                 f"a 32-bit key is the packed hex, a wide one type + properties: {shader}")


def read_sdat_key(data: bytes) -> tuple:
    magic, version = struct.unpack_from("<4sI", data, 0)
    properties, shader_type, custom_id = struct.unpack_from("<QII", data, 8)
    return magic, version, properties, shader_type, custom_id


async def test_generate_shaders_takes_old_and_wide_keys(ed: Editor):
    out = ed.scratch.root / "shaders-keys"
    wide = "0x" + f"{0:04x}{MESH:04x}{(1 << UNNAMED_BIT) | 0x41:016x}"   # a bit that names nothing: normalized away
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
