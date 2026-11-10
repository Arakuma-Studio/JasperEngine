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
ASEPRITE_FIXTURE = REPO / "editor" / "tests" / "fixtures" / "aseprite" / "s44-mcp" / "hero_idle.aseprite"
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
    tag = "walk"
    ed.check_eq(tags[tag]["to"] - tags[tag]["from"], 3, "AC14's tag has four frames")
    mtime = os.stat(src).st_mtime_ns
    result = await lint(ed, source=f"{ART}/walker.aseprite", frames=tag)
    ed.check_eq(result["frames"], 4, "the tag's four frames are linted")
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

        ed.check(by_id["flicker"]["status"] != "SKIP", f"four frames: flicker runs: {by_id['flicker']}")
        ed.check("flicker" in answer and answer["flicker"]["status"] != "SKIP", "the oracle judged flicker too")

        # Every lint spot is outlined in magenta where its frame and pixel fall in the picture
        spots = [spot for entry in result["results"] for spot in entry["at"]]
        ed.check(spots, "the fixture has spots to mark")
        width, height, rgba = pngdiff.decode(with_image.images[0][0])
        frame_w, _, _ = pngdiff.decode(Path(frames[0]).read_bytes())
        scale = width // ((frame_w + 1) * len(frames))
        for frame, x, y in spots:
            left, top = (frame - 1) * (frame_w + 1) * scale + x * scale, y * scale
            corners = [(left, top), (left + scale - 1, top), (left, top + scale - 1), (left + scale - 1, top + scale - 1)]
            colors = [tuple(rgba[(cy * width + cx) * 4:(cy * width + cx) * 4 + 3]) for cx, cy in corners]
            ed.check((255, 0, 255) in colors, f"spot [{frame}, {x}, {y}] is outlined in magenta, got {colors}")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
