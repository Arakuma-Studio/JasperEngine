"""Shared by the S47 sprite drawing tests: calls that return (data, images) or the typed error."""

from __future__ import annotations

import base64
import hashlib
from pathlib import Path

from harness import Editor


async def enable(ed: Editor) -> None:
    await ed.call("tool_groups", enable=["sprites"])


async def raw(ed: Editor, tool: str, **args):
    return await ed.call_raw(tool, args)


async def ok(ed: Editor, tool: str, **args) -> dict:
    r = await ed.call_raw(tool, args)
    if r.is_error:
        ed.fail(f"{tool} {args}: {r.text}")
    return r.structured if r.structured is not None else r.value


async def err(ed: Editor, tool: str, **args) -> dict:
    """The typed error of a call that must fail"""
    r = await ed.call_raw(tool, args)
    ed.check(r.is_error, f"{tool} {args} must fail: {r.structured}")
    ed.check(isinstance(r.structured, dict) and "error" in r.structured, f"typed error: {r.text}")
    e = dict(r.structured["error"])
    e["text"] = r.text
    return e


def expand_frames(sprite: dict) -> list[dict]:
    """get_sprite's duration runs [{frame, to?, duration_ms}] as one {frame, duration_ms} per frame"""
    out = []
    for run in sprite["frames"]:
        for frame in range(run["frame"], run.get("to", run["frame"]) + 1):
            out.append({"frame": frame, "duration_ms": run["duration_ms"]})
    return out


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


async def images(ed: Editor, tool: str, **args):
    r = await ed.call_raw(tool, args)
    if r.is_error:
        ed.fail(f"{tool}: {r.text}")
    return r.structured, [img for img, _ in r.images]


def decode_png(data: bytes):
    """(width, height, rgba bytes) of a PNG render_sprite wrote: colour type 3 or 6, 8-bit, filter 0"""
    import struct
    import zlib
    pos, width, height, color_type, idat, plte, trns = 8, 0, 0, 0, [], b"", b""
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind, chunk = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, _, color_type = struct.unpack(">IIBB", chunk[:10])
        elif kind == b"PLTE":
            plte = chunk
        elif kind == b"tRNS":
            trns = chunk
        elif kind == b"IDAT":
            idat.append(chunk)
        pos += 12 + length
    raw = zlib.decompress(b"".join(idat))
    out = bytearray(width * height * 4)
    if color_type == 3:
        for y in range(height):
            row = raw[y * (width + 1) + 1:(y + 1) * (width + 1)]
            for x, i in enumerate(row):
                out[(y * width + x) * 4:(y * width + x) * 4 + 4] = bytes((plte[i * 3], plte[i * 3 + 1], plte[i * 3 + 2], trns[i] if i < len(trns) else 255))
    else:
        stride = width * 4
        for y in range(height):
            out[y * stride:(y + 1) * stride] = raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
    return width, height, bytes(out)
