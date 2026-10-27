"""`jasperengine-editor cook-textures` (docs/specs/S14-texture-compression.md AC11): a cook of every
family with --report exits 0 and the report has its schema; a second run is all cache hits within
10 s; --check exits 1 on a UI texture whose size block formats cannot take; --decode writes a PNG
within the quality floor. Runs the CLI on a copy of tools/fixtures/textures.
"""

from __future__ import annotations

import json
import math
import os
import shutil
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path

import harness

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tools" / "fixtures" / "textures"
TIMEOUT = 1800
CI = True
FAMILIES = ("desktop", "mobile", "android_etc2", "web")


def run_cli(*args: str, timeout: float = 1200) -> subprocess.CompletedProcess:
    return subprocess.run(harness.editor_command() + list(args), capture_output=True, text=True, timeout=timeout,
                          env=dict(os.environ))


def png_decode(data: bytes) -> tuple[int, int, int, list[int]]:
    """width, height, channels and the 8-bit samples of a non-interlaced PNG (stdlib only)"""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, width = 8, b"", 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            assert depth == 8
            channels = {0: 1, 2: 3, 4: 2, 6: 4}[color]
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * channels
    out: list[int] = []
    prev = [0] * stride
    for y in range(height):
        filt = raw[y * (stride + 1)]
        line = list(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if filt == 1:
                line[i] = (line[i] + a) & 255
            elif filt == 2:
                line[i] = (line[i] + b) & 255
            elif filt == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return width, height, channels, out


def png_rgba(width: int, height: int, pixel) -> bytes:
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def check_report(report: dict) -> None:
    assert set(report) == {"textures", "totals"}, report.keys()
    assert report["textures"], "no textures"
    for row in report["textures"]:
        assert isinstance(row["path"], str) and row["path"], row
        assert row["type"] in ("default", "normal", "sprite", "ui", "lightmap", "single_channel", "hdr"), row
        for family, state in row["platforms"].items():
            assert family in FAMILIES, family
            assert state["status"] in ("cooked", "source"), (row["path"], family, state)
            if state["status"] == "cooked":
                for key, kind in (("format", str), ("width", int), ("height", int), ("mips", int), ("vram_bytes", int),
                                  ("disk_bytes", int), ("source_bytes", int), ("cache_hit", bool), ("cook_ms", (int, float))):
                    assert isinstance(state[key], kind), (row["path"], family, key, state)
    for family, total in report["totals"].items():
        assert family in FAMILIES
        for key in ("textures", "cooked", "failed", "cache_hits", "vram_bytes", "disk_bytes", "source_bytes"):
            assert isinstance(total[key], int), (family, key)


async def test_cook_textures_cli():
    work = Path(tempfile.mkdtemp(prefix="jasper-cli-textures-"))
    try:
        project = work / "project"
        shutil.copytree(FIXTURE, project)
        report_path = work / "report.json"

        first = run_cli("cook-textures", "--project", str(project), "--platform", "all", "--report", str(report_path))
        assert first.returncode == 0, first.stdout[-3000:] + first.stderr[-3000:]
        report = json.loads(report_path.read_text())
        check_report(report)
        for family in FAMILIES:
            assert report["totals"][family]["failed"] == 0, report["totals"]
            assert report["totals"][family]["cooked"] > 0, report["totals"]
        by_path = {row["path"]: row for row in report["textures"]}
        assert by_path["assets/textures/normal.png"]["type"] == "normal", by_path["assets/textures/normal.png"]
        assert by_path["assets/textures/photo.png"]["platforms"]["desktop"]["format"] == "BC1"
        assert by_path["assets/textures/photo.png"]["platforms"]["android_etc2"]["format"] == "ETC2_RGB8"

        # warm: every cook from the cache, fast (R20)
        started = time.monotonic()
        second = run_cli("cook-textures", "--project", str(project), "--platform", "all", "--report", str(report_path))
        elapsed = time.monotonic() - started
        assert second.returncode == 0, second.stderr[-3000:]
        warm = json.loads(report_path.read_text())
        for family in FAMILIES:
            assert warm["totals"][family]["cache_hits"] == warm["totals"][family]["cooked"], warm["totals"][family]
        assert elapsed <= 10.0 * max(1.0, float(os.environ.get("JASPER_TEST_PERF_SLACK", "1") or 1)), f"warm run took {elapsed:.1f} s"

        # --check: a UI texture of 130x66 cannot be block compressed (R6)
        ok = run_cli("cook-textures", "--project", str(project), "--check")
        assert ok.returncode == 0, ok.stderr[-2000:]
        ui = project / "assets" / "textures" / "badge.png"
        ui.write_bytes(png_rgba(130, 66, lambda x, y: (200, 80, 40, 255 if (x - 65) ** 2 + (y - 33) ** 2 < 900 else 0)))
        (project / "assets" / "textures" / "badge.png.meta").write_text(
            "metaVersion: 1\nguid: 5b1e9c0d2a7f4e31b6c58d0f9e2a7c13\nimport:\n  textureImporter:\n    version: 1\n    type: ui\n")
        bad = run_cli("cook-textures", "--project", str(project), "--check", "--paths", "textures/badge.png")
        assert bad.returncode == 1, bad.stdout[-2000:] + bad.stderr[-2000:]
        assert "multiple of 4" in bad.stderr, bad.stderr[-2000:]

        # --decode: a cooked BC1 file back to a PNG, within the floor (AC4: BC1 34 dB)
        cache = project / ".jasperengine" / "cache" / "textures" / "v1"
        photo = by_path["assets/textures/photo.png"]["platforms"]
        key = photo["desktop"]["key"]
        keys = [key]
        out = work / "decoded.png"
        decode = run_cli("cook-textures", "--decode", str(cache / keys[0][:2] / (keys[0] + ".ktx2")), str(out))
        assert decode.returncode == 0, decode.stderr[-2000:]
        width, height, channels, decoded = png_decode(out.read_bytes())
        sw, sh, sc, source = png_decode((project / "assets" / "textures" / "photo.png").read_bytes())
        assert (width, height) == (sw, sh) == (256, 256)
        squared = 0
        for i in range(width * height):
            for c in range(3):
                d = decoded[i * channels + c] - source[i * sc + c]
                squared += d * d
        psnr = 10 * math.log10(255 * 255 / (squared / (width * height * 3)))
        assert psnr >= 34.0, f"decoded BC1 PSNR {psnr:.2f} dB"
        # and transcoded: a web file to BC7
        web_keys = [photo["web"]["key"]]
        transcoded = run_cli("cook-textures", "--decode", str(cache / web_keys[0][:2] / (web_keys[0] + ".ktx2")),
                             str(work / "web.png"), "--platform-target", "BC7", "--level", "1")
        assert transcoded.returncode == 0, transcoded.stderr[-2000:]
        assert (work / "web.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"

        # usage errors
        assert run_cli("cook-textures", "--project", str(project), "--platform", "console").returncode == 2
    finally:
        shutil.rmtree(work, ignore_errors=True)
