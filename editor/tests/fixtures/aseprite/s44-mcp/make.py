#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Makes the S44 MCP tests' Aseprite files with Aseprite itself:

    JASPER_ASEPRITE=<aseprite or its wrapper> python3 editor/tests/fixtures/aseprite/s44-mcp/make.py

hero_idle.aseprite and hero_idle_recolor.aseprite (the same file with red Body cels) come from
make_hero_idle.lua. Aseprite's Lua API sets a slice's key on frame 0 only, so this script writes
the keys of `hurt` into the saved file (frame 0: 8,8 16x16; frame 1: 10,6 12x20; frame 4: hidden,
a zero-size key), has Aseprite open and re-save it, and checks Aseprite's --data lists those keys,
the durations, the tags and their repeat. Aseprite runs in a scratch directory with relative paths
(a containerised wrapper mounts only the current directory).
"""

from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
LUA = HERE / "make_hero_idle.lua"
HURT_KEYS = [(0, (8, 8, 16, 16)), (1, (10, 6, 12, 20)), (4, (0, 0, 0, 0))]
DURATIONS = [100, 200, 100, 200, 300, 300]
FILES = {"hero_idle.aseprite": [], "hero_idle_recolor.aseprite": ["--script-param", "recolor=1"]}


def aseprite(scratch: Path, *args: str) -> str:
    exe = os.environ.get("JASPER_ASEPRITE") or shutil.which("aseprite")
    if not exe:
        sys.exit("set JASPER_ASEPRITE to Aseprite (or a wrapper running it)")
    r = subprocess.run([exe, "-b", *args], cwd=str(scratch), capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        sys.exit(f"aseprite {' '.join(args)}: exit {r.returncode}\n{r.stdout}\n{r.stderr}")
    return r.stdout


def with_slice_keys(data: bytes, name: str, keys) -> bytes:
    """The file with the keys of slice `name` (chunk 0x2022, no centre or pivot) replaced."""
    header = bytearray(data[:128])
    frames, pos, found = [], 128, False
    for _ in range(struct.unpack_from("<H", header, 6)[0]):
        size, magic, old_count, _, _, new_count = struct.unpack_from("<IHHH2sI", data, pos)
        if magic != 0xF1FA:
            sys.exit(f"bad frame magic at {pos}")
        chunks, cp = [], pos + 16
        for _ in range(new_count or old_count):
            chunk_size, chunk_type = struct.unpack_from("<IH", data, cp)
            body = bytearray(data[cp + 6:cp + chunk_size])
            if chunk_type == 0x2022:
                _, flags, _ = struct.unpack_from("<III", body, 0)
                length = struct.unpack_from("<H", body, 12)[0]
                if body[14:14 + length].decode() == name:
                    if flags:
                        sys.exit(f"slice {name} has flags {flags}: centre or pivot keys are not written here")
                    body = bytearray(struct.pack("<III", len(keys), 0, 0)) + body[12:14 + length]
                    for frame, (x, y, w, h) in keys:
                        body += struct.pack("<IiiII", frame, x, y, w, h)
                    found = True
            chunks.append(struct.pack("<IH", len(body) + 6, chunk_type) + bytes(body))
            cp += chunk_size
        payload = b"".join(chunks)
        frame_header = bytearray(data[pos:pos + 16])
        struct.pack_into("<I", frame_header, 0, 16 + len(payload))
        frames.append(bytes(frame_header) + payload)
        pos += size
    if not found:
        sys.exit(f"no slice {name}")
    body = b"".join(frames)
    struct.pack_into("<I", header, 0, 128 + len(body))
    return bytes(header) + body


def check(data: dict, name: str) -> None:
    frames = data["frames"]
    got = [f["duration"] for f in frames]
    if got != DURATIONS:
        sys.exit(f"{name}: durations {got}")
    meta = data["meta"]
    tags = [(t["name"], t["from"], t["to"], t["direction"], int(t.get("repeat", 0))) for t in meta["frameTags"]]
    if tags != [("walk", 0, 3, "forward", 0), ("Idle", 4, 5, "pingpong", 3)]:
        sys.exit(f"{name}: tags {tags}")
    hurt = next(s for s in meta["slices"] if s["name"] == "hurt")
    keys = [(k["frame"], (k["bounds"]["x"], k["bounds"]["y"], k["bounds"]["w"], k["bounds"]["h"])) for k in hurt["keys"]]
    if keys != HURT_KEYS:
        sys.exit(f"{name}: hurt keys {keys}")
    layers = [layer["name"] for layer in meta["layers"]]
    if layers != ["Shadow", "Body", "events"]:
        sys.exit(f"{name}: layers {layers}")


def main() -> None:
    scratch = Path(tempfile.mkdtemp(prefix="jasper-s44-mcp-"))
    try:
        shutil.copy(LUA, scratch / LUA.name)
        for name, params in FILES.items():
            raw = "raw-" + name
            print(aseprite(scratch, "--script-param", f"out={raw}", *params, "--script", LUA.name).strip())
            (scratch / ("keys-" + name)).write_bytes(with_slice_keys((scratch / raw).read_bytes(), "hurt", HURT_KEYS))
            aseprite(scratch, "keys-" + name, "--save-as", name)
            aseprite(scratch, "--all-layers", name, "--sheet", "sheet.png", "--data", "data.json", "--format", "json-array",
                     "--list-layers", "--list-tags", "--list-slices")
            check(json.loads((scratch / "data.json").read_text()), name)
            shutil.copy(scratch / name, HERE / name)
            print(f"{name}: checked against Aseprite's --data")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
