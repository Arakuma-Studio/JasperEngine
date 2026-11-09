"""fix-pixel-art and lint-pixel-art (docs/specs/S46-pixel-art-cleanup.md R48, R49, AC15): exit codes on an AI sprite, the
photo fixture and native art; lint's bad ids in the JSON error; `check --pixel-art` lists a stale output as a
cleanup_outputs warning (none after --rerun) and exits 1 on an output with a FAIL lint; check_pixel_art's rule returns
the same findings. The commands run as a user runs them, no editor window.
"""

from __future__ import annotations

import json
import random
import shutil
import subprocess
import tempfile
from pathlib import Path

import harness
import pngdiff
from harness import REPO, Editor

CI = True
TIMEOUT = 600
EDITOR = {"create": True, "name": "PixelArtCli"}

FIXTURES = REPO / "editor" / "tests" / "fixtures" / "pixelart"
ART = "assets/art"


def editor_cmd(*args: str) -> list[str]:
    return harness.editor_command() + list(args)


def run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(editor_cmd(*args), capture_output=True, text=True, timeout=240, cwd=cwd)


def ai_png() -> bytes:
    rng = random.Random(11)
    scale = 8
    size = 16
    palette = [(26, 28, 44), (239, 125, 87), (255, 205, 117), (41, 54, 111)]
    rows = [[None if x in (0, 1, size - 2, size - 1) or y in (0, 1, size - 2, size - 1) else palette[(x * 7 + y * 3) % 4] for x in range(size)] for y in range(size)]
    rgba = bytearray()
    for y in range(size * scale):
        for x in range(size * scale):
            cell = rows[y // scale][x // scale]
            if cell is None:
                rgba += bytes((0, 0, 0, 0))
            else:
                rgba += bytes((min(255, cell[0] + rng.randint(0, 3)), min(255, cell[1] + rng.randint(0, 3)), min(255, cell[2] + rng.randint(0, 3)), 255))
    return pngdiff.encode(size * scale, size * scale, bytes(rgba))


def native_png() -> bytes:
    colors = [(26, 28, 44, 255), (239, 125, 87, 255), (255, 205, 117, 255), (41, 54, 111, 255)]
    size = 16
    rgba = bytearray()
    for y in range(size):
        for x in range(size):
            if x in (0, size - 1) or y in (0, size - 1):
                rgba += bytes((0, 0, 0, 0))
            else:
                rgba += bytes(colors[(x // 3 + y // 3) % 4])
    return pngdiff.encode(size, size, bytes(rgba))


def scratch_dir() -> Path:
    return Path(tempfile.mkdtemp(prefix="je-pixel-cli-"))


async def test_fix_exit_codes(ed: Editor):
    root = scratch_dir()
    try:
        (root / "ai.png").write_bytes(ai_png())
        (root / "native.png").write_bytes(native_png())
        shutil.copy(FIXTURES / "cleanup" / "photo.jpg", root / "photo.jpg")
        cli = run("fix-pixel-art", "ai.png", "--dry-run", cwd=root)
        assert cli.returncode == 3, cli.stdout[-800:] + cli.stderr[-800:]
        data = json.loads(cli.stdout)
        assert data["verdict"] == "fixable"
        assert not (root / "ai_clean.png").exists(), "a dry run writes nothing"

        cli = run("fix-pixel-art", "photo.jpg", "--dry-run", cwd=root)
        assert cli.returncode == 4, cli.stdout[-800:] + cli.stderr[-800:]

        cli = run("fix-pixel-art", "native.png", "--dry-run", cwd=root)
        assert cli.returncode == 0, cli.stdout[-800:] + cli.stderr[-800:]

        # a write: 0, and the file carries the recipe
        cli = run("fix-pixel-art", "ai.png", "--out", "ai_clean.png", cwd=root)
        assert cli.returncode == 0, cli.stdout[-800:] + cli.stderr[-800:]
        assert (root / "ai_clean.png").exists()
        assert b"jasper.pixelcleanup" in (root / "ai_clean.png").read_bytes()

        # 2 on bad_recipe, 2 on a usage error
        (root / "bad.json").write_text(json.dumps({"pallete": "x"}))
        cli = run("fix-pixel-art", "ai.png", "--recipe", "bad.json", "--dry-run", cwd=root)
        assert cli.returncode == 2, cli.stdout[-800:] + cli.stderr[-800:]
        cli = run("fix-pixel-art", "ai.png", "--grid", "zzz", "--dry-run", cwd=root)
        assert cli.returncode == 2, cli.stdout[-800:] + cli.stderr[-800:]
        # 1: a source that cannot be read
        cli = run("fix-pixel-art", "missing.png", "--dry-run", cwd=root)
        assert cli.returncode == 1, cli.stdout[-800:] + cli.stderr[-800:]
    finally:
        shutil.rmtree(root, ignore_errors=True)


async def test_lint_exit_codes(ed: Editor):
    root = scratch_dir()
    try:
        (root / "ai.png").write_bytes(ai_png())
        (root / "native.png").write_bytes(native_png())
        shutil.copy(FIXTURES / "cleanup" / "photo.jpg", root / "photo.jpg")
        cli = run("lint-pixel-art", "ai.png", cwd=root)
        assert cli.returncode == 3, cli.stdout[-800:] + cli.stderr[-800:]
        cli = run("lint-pixel-art", "photo.jpg", cwd=root)
        assert cli.returncode == 4, cli.stdout[-800:] + cli.stderr[-800:]
        cli = run("lint-pixel-art", "native.png", cwd=root)
        assert cli.returncode == 0, cli.stdout[-800:] + cli.stderr[-800:]
        data = json.loads(cli.stdout)
        assert data["verdict"] == "clean"
        cli = run("lint-pixel-art", "native.png", "--lints", "nope", cwd=root)
        assert cli.returncode == 2, cli.stdout[-800:] + cli.stderr[-800:]
        data = json.loads(cli.stdout)
        assert "off_palette" in json.dumps(data), f"the JSON error names the valid ids: {cli.stdout[-400:]}"
    finally:
        shutil.rmtree(root, ignore_errors=True)


async def test_check_pixel_art_cleanup_outputs(ed: Editor):
    art = ed.project / ART
    art.mkdir(parents=True, exist_ok=True)
    (art / "ai.png").write_bytes(ai_png())
    await ed.call("tool_groups", enable=["pixel_art"])
    await ed.call("fix_pixel_art", {"source": f"{ART}/ai.png", "output": True})

    # clean: no cleanup_outputs finding yet
    fresh = await ed.call("check_pixel_art", {"rules": ["cleanup_outputs"]})
    ids = [f for f in fresh["findings"] if f["rule"] == "cleanup_outputs"]
    assert not ids, f"a fresh output has no finding: {ids}"

    # a changed source: a stale warning, the fix names rerun
    (art / "ai.png").write_bytes(_tweaked(ai_png()))
    stale = await ed.call("check_pixel_art", {"rules": ["cleanup_outputs"]})
    findings = [f for f in stale["findings"] if f["rule"] == "cleanup_outputs"]
    assert any(f["status"] == "WARN" and "stale" in f["key"] for f in findings), f"stale finding: {findings}"
    assert any("rerun" in f.get("needs_person", "") for f in findings), f"the fix names rerun: {findings}"

    # the CLI gives the same finding and, after --rerun, none
    cli = subprocess.run(editor_cmd("check", "--project", str(ed.project), "--pixel-art", "--format", "json"), capture_output=True, text=True, timeout=240)
    assert cli.returncode in (0, 1), cli.stderr[-800:]
    assert "cleanup_outputs" in cli.stdout, cli.stdout[-800:]

    cli = subprocess.run(editor_cmd("fix-pixel-art", f"{ART}/ai_clean.png", "--rerun", "--project", str(ed.project), "--overwrite"), capture_output=True, text=True, timeout=240, cwd=ed.project)
    assert cli.returncode == 0, cli.stdout[-800:] + cli.stderr[-800:]
    cli = subprocess.run(editor_cmd("check", "--project", str(ed.project), "--pixel-art", "--format", "json"), capture_output=True, text=True, timeout=240)
    report = json.loads(cli.stdout)
    assert not [f for f in report.get("pixel_art", report).get("findings", []) if f.get("rule") == "cleanup_outputs"], cli.stdout[-800:]


def _tweaked(png: bytes) -> bytes:
    w, h, rgba = pngdiff.decode(png)
    rgba[0:4] = bytes((1, 2, 3, 255))
    return pngdiff.encode(w, h, bytes(rgba))
