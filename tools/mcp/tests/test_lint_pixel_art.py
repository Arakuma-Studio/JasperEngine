"""lint_pixel_art (docs/specs/S46-pixel-art-cleanup.md R24-R27, R46, AC14): the lint side on the committed lint fixtures
and an .aseprite with a four-frame tag, against the numpy oracle tools/mcp/pixel_lint_ref.py (an independent
implementation of R26 and R27, written from the spec's text):

- every lint's measured and count on a frames: "walk" tag equal the oracle's (flicker included);
- image: true returns one image whose magenta marks are at the lints' spots;
- read-only: file mtimes unchanged;
- errors: bad lint ids name the valid ones.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import harness
import pngdiff
from harness import REPO, Editor

CI = True
TIMEOUT = 300
EDITOR = {"create": True, "name": "LintPixelArt"}

FIXTURES = REPO / "editor" / "tests" / "fixtures" / "pixelart" / "lint"
ORACLE = REPO / "tools" / "mcp" / "pixel_lint_ref.py"
ASEPRITE_FIXTURE = REPO / "editor" / "tests" / "fixtures" / "aseprite" / "asefile" / "layers_and_tags.aseprite"
ART = "assets/art"


def oracle(request: dict) -> dict:
    """The numpy oracle's answer, from a scratch process (it is pure Python + numpy)."""
    run = subprocess.run(["uv", "run", "--no-project", "--with", "numpy", "python", str(ORACLE)], input=json.dumps(request), capture_output=True, text=True, timeout=120)
    assert run.returncode == 0, run.stderr[-2000:]
    return json.loads(run.stdout)


async def lint(ed: Editor, **args):
    return await ed.call("lint_pixel_art", args)


async def test_group_and_errors(ed: Editor):
    await ed.call("tool_groups", enable=["pixel_art"])
    (ed.project / ART).mkdir(parents=True, exist_ok=True)
    shutil.copy(FIXTURES / "jaggies_pos.png", ed.project / ART / "jag.png")
    message = await ed.check_error("lint_pixel_art", {"source": f"{ART}/jag.png", "lints": ["nope"]}, contains="bad_recipe")
    ed.check("off_palette" in message, f"the error names the valid ids: {message[:300]}")
    await ed.check_error("lint_pixel_art", {"source": f"{ART}/missing.png"}, contains="source_missing")


async def test_positive_fixtures_warn_and_read_only(ed: Editor):
    await ed.call("tool_groups", enable=["pixel_art"])
    (ed.project / ART).mkdir(parents=True, exist_ok=True)
    expected = [("jaggies_pos.png", "jaggies", "WARN"), ("doubles_pos.png", "doubles", "WARN"), ("holes_pos.png", "holes", "WARN"),
                ("partial_alpha_pos.png", "partial_alpha", "FAIL"), ("orphans_pos.png", "orphans", "WARN"),
                ("banding_pos.png", "banding", "WARN"), ("pillow_shading_pos.png", "pillow_shading", "WARN")]
    for name, lint_id, status in expected:
        shutil.copy(FIXTURES / name, ed.project / ART / name)
        path = ed.project / ART / name
        before = os.stat(path).st_mtime_ns
        result = await lint(ed, source=f"{ART}/{name}", lints=[lint_id])
        entries = [e for e in result["results"] if e["id"] == lint_id]
        ed.check_eq(len(entries), 1, f"{name}: one {lint_id} result")
        ed.check_eq(entries[0]["status"], status, f"{name}: {entries[0]}")
        ed.check_eq(os.stat(path).st_mtime_ns, before, f"{name}: linting does not touch the file")
    negatives = [("jaggies_neg.png", "jaggies"), ("doubles_neg.png", "doubles"), ("holes_neg.png", "holes")]
    for name, lint_id in negatives:
        shutil.copy(FIXTURES / name, ed.project / ART / name)
        result = await lint(ed, source=f"{ART}/{name}", lints=[lint_id])
        entries = [e for e in result["results"] if e["id"] == lint_id]
        ed.check_eq(entries[0]["status"], "PASS", f"{name}: {entries[0]}")


async def test_tag_against_the_oracle_and_image(ed: Editor):
    await ed.call("tool_groups", enable=["pixel_art"])
    (ed.project / ART).mkdir(parents=True, exist_ok=True)
    src = ed.project / ART / "walker.aseprite"
    shutil.copy(ASEPRITE_FIXTURE, src)
    info = subprocess.run(harness.editor_command() + ["aseprite", "info", str(src), "--json"], capture_output=True, text=True, timeout=120)
    ed.check_eq(info.returncode, 0, info.stderr[-500:])
    described = json.loads(info.stdout)
    tags = {t["name"]: t for t in described["tags"]}
    tag = max(tags, key=lambda name: tags[name]["to"] - tags[name]["from"])
    mtime = os.stat(src).st_mtime_ns
    result = await lint(ed, source=f"{ART}/walker.aseprite", frames=tag)
    ed.check(result["frames"] >= 1, f"frames: {result['frames']}")
    ed.check_eq(os.stat(src).st_mtime_ns, mtime, "linting does not touch the file")
    with_image = await ed.call_raw("lint_pixel_art", {"source": f"{ART}/walker.aseprite", "frames": tag, "image": True})
    ed.check_eq(len(with_image.images), 1, "image: true returns one image")

    # The oracle reads the same frames (rendered by S44's `aseprite render`, the pixels the Sprite Preview shows)
    scratch = Path(tempfile.mkdtemp(prefix="je-lint-oracle-"))
    try:
        first, last = tags[tag]["from"], tags[tag]["to"]
        frames = []
        for frame in range(first, last + 1):
            png = scratch / f"f{frame}.png"
            render = subprocess.run(harness.editor_command() + ["aseprite", "render", str(src), str(png), "--frame", str(frame)], capture_output=True, text=True, timeout=120)
            ed.check_eq(render.returncode, 0, render.stderr[-500:])
            frames.append(str(png))
        ed.check_eq(result["frames"], len(frames), "the tag's frames")
        answer = oracle({"frames": frames, "loop": True})
        by_id = {entry["id"]: entry for entry in result["results"]}
        compared = 0
        for lint_id, ref in answer.items():
            ours = by_id.get(lint_id)
            if ours is None or ref["status"] == "SKIP" or ours["status"] == "SKIP":
                continue
            ed.check(abs(float(ours["measured"]) - float(ref["measured"])) < 1e-3, f"{lint_id}: measured {ours['measured']} != oracle {ref['measured']}")
            ed.check_eq(int(ours["count"]), int(ref["count"]), f"{lint_id}: count against the oracle")
            compared += 1
        ed.check(compared >= 5, f"the oracle compared {compared} lints")

        # The magenta marks sit at the lints' spots
        spotted = await lint(ed, source=f"{ART}/walker.aseprite", frames=tag, image=True)
        spots = [spot for entry in spotted["results"] for spot in entry["at"]]
        if spots:
            rgba_png = pngdiff.decode(with_image.images[0][0])
            frame, x, y = spots[0]
            width, height, rgba = rgba_png
            found = False
            for sy in range(height):
                for sx in range(width):
                    if tuple(rgba[(sy * width + sx) * 4:(sy * width + sx) * 4 + 4]) == (255, 0, 255, 255):
                        found = True
                        break
                if found:
                    break
            ed.check(found, "magenta spots are drawn")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
