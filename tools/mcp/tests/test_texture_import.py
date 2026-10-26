"""Texture import settings and cooks over MCP (docs/specs/S14-texture-compression.md AC10): the
tools are hidden until the assets_pipeline group is on; get/set round trip with undo; invalid enum
values name the allowed ones; a glTF normal slot infers `normal`; reimport_textures cooks three
families; the report's totals are the sum of its rows and include_preview returns PNGs; a texture
edited on disk goes stale and cooks again (S05).

The project is tools/fixtures/textures (tools/fixtures/textures/generate.py), opened from a copy:
it has no metas, so the editor infers every texture's settings when it opens it (R2, R30).
"""

from __future__ import annotations

import asyncio
import json
import os
import struct
import time
import zlib
from pathlib import Path

import yaml

from harness import Editor, tool_meta

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tools" / "fixtures" / "textures"
EDITOR = {"project": FIXTURE}
TIMEOUT = 900
CI = True

TOOLS = ("get_texture_import_settings", "set_texture_import_settings", "reimport_textures", "get_texture_cook_report")
PHOTO = "assets/textures/photo.png"
NORMAL = "assets/textures/normal.png"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def png_rgba(width: int, height: int, pixel) -> bytes:
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (PNG_MAGIC + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def stored_settings(ed: Editor, rel: str) -> dict | None:
    meta = ed.project / (rel + ".meta")
    if not meta.exists():
        return None
    data = yaml.safe_load(meta.read_text()) or {}
    return (data.get("import") or {}).get("textureImporter")


async def wait_for(ed: Editor, what: str, predicate, timeout: float = 120.0):
    deadline = time.monotonic() + timeout
    while True:
        value = await predicate()
        if value:
            return value
        if time.monotonic() > deadline:
            ed.fail(f"timed out waiting for {what}")
        await asyncio.sleep(0.5)


async def test_tools_hidden_until_enabled(ed: Editor):
    ed.require_tools(*TOOLS)
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check(not default & set(TOOLS), f"texture tools listed by default: {sorted(default & set(TOOLS))}")
    await ed.call("tool_groups", enable=["assets_pipeline"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check(set(TOOLS) <= listed, "texture tools listed once assets_pipeline is on")
    for name in TOOLS:
        ed.check_eq(tool_meta(ed.tool(name)).get("jasper/group"), "assets_pipeline", f"{name} group")
    # what they add to a session that enables the group (tools/mcp/tool_budget.py's measure): <= 2,400 characters
    size = sum(len(t.name) + len(t.description or "") + len(json.dumps(t.input_schema or {}, separators=(",", ":"), ensure_ascii=False))
               for t in (ed.tool(name) for name in TOOLS))
    ed.check(size <= 2400, f"the texture tools take {size} characters (budget 2,400)")


async def test_inference_on_first_open(ed: Editor):
    ed.require_tools(*TOOLS)
    # the material and the glTF bind normal.png as a normal map: the settings written on open say so
    async def inferred():
        return stored_settings(ed, NORMAL)
    settings = await wait_for(ed, "the inferred settings of normal.png", inferred)
    ed.check_eq(settings.get("type"), "normal", "inferred type")
    ed.check("normal slot" in settings.get("inferredFrom", ""), f"inferredFrom: {settings}")
    info = await ed.call("get_texture_import_settings", path=NORMAL)
    ed.check_eq(info["inferred"]["type"], "normal")
    ed.check_eq(info["stored"]["type"], "normal")
    ed.check_eq(info["resolved"]["desktop"]["srgb"], False)
    # the spot mask is CPU-readable (R3), the occlusion map single channel
    mask = await ed.call("get_texture_import_settings", path="assets/textures/ao.png")
    ed.check(mask["inferred"]["cpu_readable"], f"spot mask is CPU-readable: {mask['inferred']}")
    ed.check_eq(mask["inferred"]["type"], "single_channel")
    sky = await ed.call("get_texture_import_settings", path="assets/textures/sky.hdr")
    ed.check_eq(sky["inferred"]["type"], "hdr")


async def test_set_and_undo(ed: Editor):
    ed.require_tools(*TOOLS)
    before = await ed.call("get_texture_import_settings", path=PHOTO, platform="mobile")
    result = await ed.call("set_texture_import_settings", paths=[PHOTO], settings={"max_size": 128, "quality": "high"})
    ed.check_eq(result["updated"], [PHOTO])
    after = await ed.call("get_texture_import_settings", path=PHOTO, platform="mobile")
    ed.check_eq(after["stored"]["max_size"], 128)
    ed.check_eq(after["resolved"]["mobile"]["quality"], "high")
    ed.check_eq(stored_settings(ed, PHOTO)["maxSize"], 128)
    # a platform override
    await ed.call("set_texture_import_settings", paths=[PHOTO], settings={"format": "ASTC_8x8"}, platform="mobile")
    over = await ed.call("get_texture_import_settings", path=PHOTO, platform="mobile")
    ed.check_eq(over["resolved"]["mobile"]["format"], "ASTC_8x8")
    ed.check_eq(over["stored"]["platforms"]["mobile"]["override"], True)
    # one undo per call, back to what it was
    await ed.call("undo", scope="project")
    await ed.call("undo", scope="project")
    restored = await ed.call("get_texture_import_settings", path=PHOTO, platform="mobile")
    ed.check_eq(restored["stored"], before["stored"], "undo restores the settings")
    ed.check_eq(restored["resolved"], before["resolved"])


async def test_invalid_values(ed: Editor):
    ed.require_tools(*TOOLS)
    await ed.check_error("set_texture_import_settings", paths=[PHOTO], settings={"type": "bump"}, contains="normal")
    await ed.check_error("set_texture_import_settings", paths=[PHOTO], settings={"max_size": 300}, contains="power of two")
    await ed.check_error("set_texture_import_settings", paths=[PHOTO], settings={"colour": True}, contains="max_size")
    await ed.check_error("set_texture_import_settings", paths=[PHOTO], settings={"format": "PVRTC"}, platform="mobile",
                         contains="ASTC_6x6")
    await ed.check_error("get_texture_import_settings", path="Main.scene", contains="texture")


async def test_reimport_and_report(ed: Editor):
    ed.require_tools(*TOOLS)
    families = ["desktop", "mobile", "web"]
    report = await ed.call("reimport_textures", paths=[PHOTO, "assets/textures/foliage.png"], platforms=families, wait=True,
                           timeout_s=600)
    ed.check(not report["timed_out"], "cooks finished")
    for row in report["textures"]:
        for family in families:
            state = row["platforms"][family]
            ed.check_eq(state["status"], "cooked", f"{row['path']} {family}: {state}")
            ed.check(state["vram_bytes"] > 0 and state["disk_bytes"] > 0, f"sizes: {state}")
    photo = next(row for row in report["textures"] if row["path"] == PHOTO)
    ed.check_eq(photo["platforms"]["desktop"]["format"], "BC1")
    ed.check(photo["platforms"]["desktop"]["psnr_db"] > 30, f"psnr: {photo['platforms']['desktop']}")
    ed.check(photo["platforms"]["mobile"]["format"].startswith("ASTC"), photo["platforms"]["mobile"]["format"])
    ed.check_eq(photo["platforms"]["web"]["format"], "ETC1S")

    full = await ed.call("get_texture_cook_report", platforms=families)
    for family in families:
        total = full["totals"][family]
        rows = [row["platforms"][family] for row in full["textures"]]
        cooked = [r for r in rows if r["status"] == "cooked"]
        ed.check_eq(total["textures"], len(rows), f"{family} rows")
        ed.check_eq(total["cooked"], len(cooked), f"{family} cooked")
        ed.check_eq(total["vram_bytes"], sum(r["vram_bytes"] for r in cooked), f"{family} vram")
        ed.check_eq(total["disk_bytes"], sum(r["disk_bytes"] for r in cooked), f"{family} disk")
    preview = await ed.call_raw("get_texture_cook_report", {"paths": [PHOTO], "platforms": ["mobile"], "include_preview": True})
    ed.check(not preview.is_error, preview.text)
    ed.check(preview.images, "include_preview returned no image")
    data, mime = preview.images[0]
    ed.check_eq(mime, "image/png")
    ed.check(data.startswith(PNG_MAGIC), "preview is a PNG")


async def test_source_edit_recooks(ed: Editor):
    ed.require_tools(*TOOLS)
    first = await ed.call("reimport_textures", paths=[PHOTO], wait=True, timeout_s=600)
    before = first["textures"][0]["platforms"]["desktop"]
    ed.check_eq(before["status"], "cooked")
    # another program writes the file (S05): the cook is stale, then cooked again from the new pixels
    path = ed.project / PHOTO
    temp = path.with_name(path.name + ".tmp")
    temp.write_bytes(png_rgba(64, 64, lambda x, y: (x * 4, y * 4, 128, 255)))
    os.replace(temp, path)

    async def recooked():
        r = await ed.call("get_texture_cook_report", paths=[PHOTO])
        state = r["textures"][0]["platforms"]["desktop"]
        return state if state["status"] == "cooked" and state["width"] == 64 else None
    state = await wait_for(ed, "the recook of the edited photo.png", recooked, 300)
    ed.check(state["disk_bytes"] != before["disk_bytes"] or state["width"] != before["width"], f"{before} -> {state}")
