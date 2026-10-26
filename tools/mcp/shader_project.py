"""Small, isolated projects for public shader and material workflows."""

import struct
import sys
import zlib
from contextlib import asynccontextmanager

TEXTURE = "assets/textures/probe.png"


def _chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


async def texture(ed):
    path = ed.project / TEXTURE
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(
            b"\x89PNG\r\n\x1a\n"
            + _chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
            + _chunk(b"IDAT", zlib.compress(b"\0\xff\x40\x20\xff"))
            + _chunk(b"IEND", b"")
        )
        await ed.call("sync_files", paths=[TEXTURE])


@asynccontextmanager
async def scene(ed, name: str, *, with_texture: bool = False):
    path = f"scenes/{name}.scene"
    await ed.call("create_scene", name=name, path=path)
    try:
        if with_texture:
            await texture(ed)
        yield path
    finally:
        failed = sys.exc_info()[0] is not None
        try:
            await ed.call("close_scene", scene=path, unsaved="discard")
        except Exception as error:
            if not failed:
                raise
            print(f"[shader fixture cleanup] {error}", flush=True)
