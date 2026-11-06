"""S90 R4, AC2: a Procedural Material never ships. Its `.procmat` is absent from an exported game, from
the game's asset map too, and what it baked ships without the editor's `jasper.*` chunks.

Red control: without the skip in Exporter::copyAssets and writeAssetMap the first two checks fail
(reports/s90-core.md). The file holds no graph tool or window: the export does not need the kind.
"""

from __future__ import annotations

import shutil
import struct
import tempfile
import zlib
from pathlib import Path

import yaml

from harness import Editor

TIMEOUT = 1800
CI = True

GRAPH_GUID = "0123456789abcdef0123456789abcdef"
GRAPH = """{
  "formatVersion": 1,
  "kind": "procedural_material",
  "kindVersion": 1,
  "nextId": 2,
  "nodes": [{"id": "n1", "type": "perlin", "position": [0, 0]}],
  "guid": "%s"
}
""" % GRAPH_GUID


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def baked_png() -> bytes:
    """A 2 x 2 RGBA image with the recipe R15 gives a baked PNG"""
    raw = b"".join(b"\x00" + bytes([200, 100, 50, 255]) * 2 for _ in range(2))
    recipe = b"jasper.procmat\x00\x00\x00\x00\x00" + b'{"graph": "' + GRAPH_GUID.encode() + b'", "tier": "gpu"}'
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0)) + chunk(b"iTXt", recipe)
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


async def test_a_procmat_is_absent_from_an_export():
    async with Editor(create=True, name="Procmat export") as ed:
        assets = ed.project / "assets"
        (assets / "wood").mkdir(parents=True, exist_ok=True)
        (assets / "wood.procmat").write_text(GRAPH)
        (assets / "wood" / "wood_basecolor.png").write_bytes(baked_png())
        info = await ed.call("resolve_asset", asset="assets/wood.procmat")
        ed.check_eq(info.get("type"), "procedural_material", f"the database tracks it: {info}")
        ed.check_eq(info.get("guid"), GRAPH_GUID, f"its GUID is the one in the file: {info}")

        await ed.call("save_project")
        out = ed.scratch.root / "export"
        r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"], allow_missing_assets=True,
                          overwrite=True, timeout_seconds=900)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        exported = out / "project" / "assets"
        ed.check(exported.is_dir(), f"{exported} exported")

        files = [p for p in exported.rglob("*") if p.is_file()]
        ed.check(not [p for p in files if p.suffix == ".procmat"], f"no .procmat ships: {[str(p) for p in files if p.suffix == '.procmat']}")
        asset_map = exported / "assetmap.bin"
        ed.check(asset_map.exists(), "the export wrote its asset map")
        ed.check(b"wood.procmat" not in asset_map.read_bytes(), "a game cannot name it by GUID")

        # a baked PNG ships as a texture, as its source or cooked, with no recipe in either
        shipped = [p for p in files if "wood_basecolor" in p.name]
        cooked = [p for p in files if "je-cooked" in p.parts and p.name == "index.json" and b"wood_basecolor" in p.read_bytes()]
        ed.check(shipped or cooked, f"the baked PNG ships: {[str(p) for p in files][:20]}")
        carrying = [str(p) for p in files if b"jasper." in p.read_bytes()]
        ed.check(not carrying, f"no jasper.* chunk ships: {carrying}")


async def test_a_procmat_in_a_lua_root_of_its_own_is_absent_from_an_export():
    """copyLua ships every file of a Lua root that is neither the project's nor the assets' own"""
    with tempfile.TemporaryDirectory() as scratch:
        project = Path(scratch) / "project"
        async with Editor(create=True, name="Procmat lua root") as ed:
            await ed.call("save_project")
            shutil.copytree(ed.project, project)
        settings = yaml.safe_load((project / "project.yaml").read_text())
        settings["luaDir"] = "scripts"
        (project / "project.yaml").write_text(yaml.safe_dump(settings))
        (project / "scripts").mkdir(exist_ok=True)
        (project / "scripts" / "main.lua").write_text("-- a script\n")
        (project / "scripts" / "stray.procmat").write_text(GRAPH)
        async with Editor(project=str(project), label="procmat-lua") as ed:
            lua = await ed.call("get_project_summary")
            ed.check_eq(lua.get("lua_dir"), "scripts", f"the Lua root: {lua}")
            out = ed.scratch.root / "export"
            r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"],
                              allow_missing_assets=True, overwrite=True, timeout_seconds=900)
            ed.check_eq(r["status"], "succeeded", f"{r}")
            shipped = [str(p) for p in (out / "project").rglob("*") if p.is_file()]
            ed.check(any(p.endswith("main.lua") for p in shipped), f"the Lua root shipped: {shipped[:20]}")
            ed.check(not [p for p in shipped if p.endswith(".procmat")], f"no .procmat ships: {[p for p in shipped if p.endswith('.procmat')]}")
