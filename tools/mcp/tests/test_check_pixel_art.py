"""S42 AC6, AC7, AC8, AC14 (docs/specs/S42-pixel-art-mode.md R9-R16, R22): check_pixel_art on a copy of
tools/fixtures/pixel-art-bad (one seeded failure per rule) and of tools/fixtures/pixel-art (clean).

- every seeded finding has its status, measured and expected values (AC6, the rules on real files);
- the CLI's `check --pixel-art --format json` gives check_pixel_art's structuredContent (AC7);
- fix: ["all"] previews without changing a file; with output: true it applies one undo step "Fix Pixel
  Art (n)", the re-check is clean but for the findings that need a person, and undo restores every
  file; a second option, an unknown key, output while playing, and the 1x image (AC8);
- the group is hidden until tool_groups enables it, and callable by name before (AC8, §6.4);
- the cost on a 12-scene, 600-texture, 6,000-sprite project: warm, cold, and a job past one second (AC14).
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import shutil
import struct
import subprocess
import tempfile
import time
from pathlib import Path

import harness
from harness import REPO, Editor

CI = True
TIMEOUT = 1800
EDITOR = {"project": REPO / "tools" / "fixtures" / "pixel-art-bad", "xvfb": "harness"}

S: dict = {}


def hashes(root: Path) -> dict[str, str]:
    out = {}
    for path in sorted(root.rglob("*")):
        if path.is_file() and (path.suffix in (".scene", ".meta", ".png", ".yaml", ".palette")) and ".jasperengine" not in path.parts:
            out[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


def by_rule(result: dict) -> dict[str, list[dict]]:
    out: dict[str, list[dict]] = {}
    for finding in result["findings"]:
        out.setdefault(finding["rule"], []).append(finding)
    return out


def entity_finding(result: dict, rule: str, entity: str) -> dict | None:
    for finding in by_rule(result).get(rule, []):
        if finding["target"].get("entity") == "/" + entity:
            return finding
    return None


async def check(ed: Editor, **args) -> dict:
    return await ed.call("check_pixel_art", timeout=300, **args)


async def test_group_is_hidden_until_enabled(ed: Editor):
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("check_pixel_art" not in listed, "a new session does not list check_pixel_art")
    # callable by name before the group is enabled (hiding is only about context)
    first = await check(ed)
    ed.check(first.get("verdict") in ("fixable", "needs_person"), f"callable without enabling: {first.get('verdict')}")
    await ed.call("tool_groups", enable=["pixel_art"])
    listed = {t.name for t in await ed.tools()}
    ed.check("check_pixel_art" in listed, "listed once pixel_art is enabled")
    S["first"] = first


async def test_seeded_findings(ed: Editor):
    result = S.get("first") or await check(ed)
    rules = {r["id"]: r for r in result["rules"]}
    ed.check(result["verdict"] == "needs_person", f"the vector font and the stretched particles need a person: {result['verdict']}")

    f = entity_finding(result, "filtering", "Bilinear")
    ed.check(f and f["status"] == "FAIL" and f["measured"] == {"min_filter": "linear", "mag_filter": "linear"}, f"filtering: {f}")
    ed.check(f and f["expected"] == {"min_filter": "nearest", "mag_filter": "nearest"}, "filtering: expected nearest")

    lossy = [x for x in by_rule(result).get("mipmaps_compression", []) if x["target"].get("path", "").endswith("lossy.png")]
    ed.check(lossy and lossy[0]["status"] == "FAIL" and lossy[0]["measured"]["mipmaps"] is True, f"mipmaps_compression lossy: {lossy}")
    ed.check(lossy and lossy[0]["measured"]["format"]["desktop"] == "BC7", f"BC7 on desktop: {lossy and lossy[0]['measured']}")
    swap = [x for x in by_rule(result).get("mipmaps_compression", []) if x["target"].get("path", "").endswith("swap.png")]
    ed.check(swap and swap[0]["status"] == "FAIL", f"the Palette Swap texture is lossy: {swap}")

    f = entity_finding(result, "texel_size", "Scaled")
    ed.check(f and f["status"] == "FAIL" and f["measured"] == {"x": 1.5, "y": 1.5}, f"texel_size 1.5x: {f}")
    f = entity_finding(result, "texel_size", "PPU32")
    ed.check(f and f["status"] == "FAIL" and f["measured"] == {"x": 0.5, "y": 0.5}, f"texel_size PPU 32 in 16: {f}")
    ed.check(f and f["fix"]["options"][0]["changes"][0]["field"] == "sprite.pixelsPerUnit", "fixed by the project's PPU")

    f = entity_finding(result, "off_grid", "OffGrid")
    ed.check(f and f["status"] == "WARN" and abs(f["measured"]["anchor"][0] - 164.8) < 1e-3, f"off_grid at 164.8 px: {f}")
    f = entity_finding(result, "rotation", "Rotated")
    ed.check(f and f["status"] == "WARN" and abs(f["measured"]["z"] - 37) < 1e-3, f"rotation 37: {f}")
    ed.check(f and [o["id"] for o in f["fix"]["options"]] == ["default", "on_grid"], "two options: round, or on the grid")
    f = entity_finding(result, "font_antialiasing", "Label")
    ed.check(f and f["status"] == "WARN" and f["fix"] is None and f["needs_person"], f"font_antialiasing: {f}")
    f = entity_finding(result, "particles_grid", "Sparks")
    ed.check(f and f["status"] == "WARN" and f["measured"]["render_mode"] == "stretched_billboard", f"particles_grid: {f}")
    aa = by_rule(result).get("anti_aliasing", [])
    ed.check(aa and aa[0]["measured"]["fxaa"] is True, f"anti_aliasing FXAA: {aa}")
    scale = [x for x in by_rule(result).get("integer_scale", []) if x["target"].get("path", "").endswith("Fixed.scene")]
    ed.check(scale and scale[0]["status"] == "FAIL", f"integer_scale on the fixed-resolution scene: {scale}")
    laptop = scale and scale[0]["measured"]["screens"]["1366x768"]
    ed.check(laptop and laptop["integer"] is False and abs(laptop["scale"] - 2.1333) < 0.002, f"1366x768: {laptop}")
    pal = [x for x in by_rule(result).get("palette", []) if x["target"].get("path", "").endswith("offpalette.png")]
    ed.check(pal and pal[0]["status"] == "WARN" and pal[0]["measured"]["verdict"] == "fixable", f"palette: {pal}")
    sv = by_rule(result).get("sprite_variants", [])
    ed.check(sv and sv[0]["status"] == "WARN" and sv[0]["measured"]["variants"] > 24, f"sprite_variants: {sv}")
    mode = by_rule(result).get("pixel_art_mode", [])
    ed.check(mode and mode[0]["measured"].get("graphics.scaling_mode") == "letterbox", f"pixel_art_mode: {mode}")
    # ids in rule order, keys stable
    ed.check_eq([f["id"] for f in result["findings"]], [f"F{i + 1}" for i in range(len(result["findings"]))], "F1... in order")
    ed.check(all(rules[r]["status"] != "PASS" for r in ("filtering", "texel_size", "off_grid", "rotation")), "the seeded rules fail")
    ed.check("next" in result and "fix" in result["next"], f"the result names the next call: {result.get('next')}")


async def test_cli_matches_mcp(ed: Editor):
    await ed.call("save_project")
    mcp = await check(ed)
    cli = subprocess.run(harness.editor_command() + ["check", "--project", str(ed.project), "--pixel-art", "--format", "json"],
                         capture_output=True, text=True, timeout=600)
    ed.check(cli.returncode == 1, f"errors exit 1: {cli.returncode} {cli.stderr[-2000:]}")
    out = json.loads(cli.stdout)
    pixel = out.get("pixel_art")
    ed.check(pixel is not None, f"--format json carries pixel_art: {list(out)}")
    for key in ("next", "image", "image_note"):
        mcp.pop(key, None)
    ed.check_eq(pixel, mcp, "the CLI's pixel_art is check_pixel_art's structuredContent")
    codes = {f.get("code") for f in out["findings"] if f.get("tool") == "pixel_art"}
    ed.check("PIXELART-TEXEL_SIZE" in codes and "PIXELART-FILTERING" in codes, f"PIXELART-<RULE> codes: {codes}")


async def test_fix_preview_changes_nothing(ed: Editor):
    before = hashes(ed.project)
    preview = await check(ed, fix=["all"])
    ed.check(preview.get("fix_plan"), "the plan lists the changes")
    changes = sum(len(item["changes"]) for item in preview["fix_plan"])
    ed.check(changes > 5, f"every change listed: {changes}")
    ed.check(all("from" in c and "to" in c for item in preview["fix_plan"] for c in item["changes"]), "each change has from and to")
    ed.check("output" in preview.get("next", ""), f"next says how to apply: {preview.get('next')}")
    ed.check_eq(hashes(ed.project), before, "no file changed by a preview")
    S["hashes"] = before


async def test_errors(ed: Editor):
    r = await ed.call_raw("check_pixel_art", {"fix": ["filtering:scenes/Main.scene#/Nobody:mesh"]})
    errors = (r.structured or {}).get("errors", [])
    ed.check(r.is_error and errors and errors[0]["kind"] == "not_found" and errors[0]["field"] == "fix", f"unknown key: {r.text}")
    r = await ed.call_raw("check_pixel_art", {"rules": ["blurriness"]})
    ed.check(r.is_error and "unknown_rule" in r.text, f"unknown rule: {r.text}")
    r = await ed.call_raw("check_pixel_art", {"output": True})
    ed.check(r.is_error and "fix" in r.text, f"output without fix: {r.text}")


async def test_second_option(ed: Editor):
    result = await check(ed)
    rotated = entity_finding(result, "rotation", "Rotated")
    plan = await check(ed, fix=[rotated["key"] + "@on_grid"])
    ed.check_eq(plan["fix_plan"][0]["option"], "on_grid", "the second option was chosen")
    ed.check_eq(plan["fix_plan"][0]["changes"][0]["field"], "sprite.pixelRotation", "it sets the Pixel Rotation")
    applied = await check(ed, fix=[rotated["key"] + "@on_grid"], output=True)
    got = await ed.call("get_entity", scene="scenes/Main.scene", entity="Rotated", components=["sprite"])
    ed.check_eq(got["components"]["sprite"].get("pixelRotation"), "on_grid", "applied")
    ed.check(applied["applied"]["undo_step"]["name"] == "Fix Pixel Art (1)", f"its step: {applied['applied']}")
    await ed.call("undo", scene="scenes/Main.scene")


async def test_image(ed: Editor):
    r = await ed.call_raw("check_pixel_art", {"image": True, "max_width": 1300}, timeout=300)
    ed.check(not r.is_error and r.images, f"an image: {r.text[:300]}")
    png = r.images[0][0]
    width, height = struct.unpack(">II", png[16:24])
    ed.check(width % 320 == 0 and width <= 1300, f"a whole multiple of 320 within max_width: {width}x{height}")
    info = (r.structured or {}).get("image", {})
    ed.check(info.get("boxes", 0) > 0, f"the findings on screen are boxed and labelled: {info}")


async def test_fix_all_output_is_one_step(ed: Editor):
    applied = await check(ed, fix=["all"], output=True)
    step = applied["applied"]["undo_step"]
    ed.check(step and step["name"].startswith("Fix Pixel Art ("), f"one step named Fix Pixel Art (n): {step}")
    ed.check_eq(applied["after"]["verdict"], "needs_person", "after: only the findings that need a person are left")
    after = await check(ed)
    left = {f["rule"] for f in after["findings"]}
    ed.check(left <= {"font_antialiasing", "particles_grid", "sprite_variants"}, f"only needs_person findings left: {left}")
    history = await ed.call("get_undo_history", limit=50)
    names = [e["name"] for e in history["entries"] if e.get("applied", True)]
    ed.check(step["name"] in names, f"get_undo_history shows the step: {names[:5]}")
    # one Ctrl+Z restores every file (the metas, the palette's texture, project.yaml)
    await ed.call("undo", scope="timeline")
    ed.check_eq(hashes(ed.project), S["hashes"], "undo restores every file hash")
    again = await check(ed)
    ed.check(len(again["findings"]) == len(S["first"]["findings"]), "and the findings are back")


async def test_output_refused_while_playing(ed: Editor):
    await ed.call("play", action="start", scene="scenes/Main.scene")
    try:
        r = await ed.call_raw("check_pixel_art", {"fix": ["all"], "output": True})
        ed.check(r.is_error and "not_while_playing" in r.text, f"output while playing: {r.text}")
        ok = await ed.call_raw("check_pixel_art", {})
        ed.check(not ok.is_error, f"the check itself works while playing: {ok.text[:200]}")
    finally:
        await ed.call("play", action="stop")


async def test_clean_fixture(ed: Editor):
    async with Editor(REPO / "tools" / "fixtures" / "pixel-art") as clean:
        result = await clean.call("check_pixel_art")
        clean.check_eq(result["verdict"], "clean", f"pixel-art is clean: {[f['key'] for f in result['findings']]}")
        clean.check(all(r["status"] == "PASS" for r in result["rules"]), "every rule passes")


def tiny_png(color: tuple[int, int, int]) -> bytes:
    import pngdiff
    return pngdiff.encode(8, 8, bytes((*color, 255)) * 64)


async def test_cost(ed: Editor):
    """12 scenes, 600 textures, 6,000 sprites: warm <= 1 s, cold <= 3 s; past a second, a job"""
    async with Editor(create=True, name="Pixel Art Large") as big:
        await big.call("set_project_settings", values={"pixel_art.enabled": True, "graphics.canvas_width": 320,
                                                        "graphics.canvas_height": 180, "graphics.scaling_mode": "integer",
                                                        "graphics.default_filter_mode": "point"})
        folder = big.project / "sprites"
        folder.mkdir(exist_ok=True)
        for i in range(600):
            (folder / f"t{i:03d}.png").write_bytes(tiny_png((i % 256, (i * 7) % 256, (i * 13) % 256)))
        await big.call("wait", frames=60)   # the watcher imports the new files
        for s in range(12):
            made = await big.call("create_scene", name=f"Level{s}", type="2D", path=f"scenes/Level{s}")
            sid = made["scene"]["id"]
            calls = []
            for i in range(500):
                texture = f"sprites/t{(s * 500 + i) % 600:03d}.png"
                calls.append({"tool": "create_entity", "arguments": {
                    "scene": sid, "type": "sprite", "name": f"S{i}", "position": [i % 40 * 8, i // 40 * 8, 0], "select": False,
                    "values": {"sprite": {"width": 8, "height": 8},
                               "mesh": {"submeshes[0].material.baseColorTexture": {"path": texture, "min_filter": "nearest",
                                                                                   "mag_filter": "nearest"}}}}})
            for at in range(0, len(calls), 200):   # a batch takes 200 calls
                batch = await big.call("batch", calls=calls[at:at + 200], timeout=600)
                big.check_eq(batch.get("failed", 0), 0, f"scene {s}: the sprites were made")
            await big.call("save_scene", scene=sid)
        saved = Path(tempfile.mkdtemp(prefix="je-s42-large-")) / "project"
        shutil.copytree(big.project, saved, ignore=shutil.ignore_patterns(".jasperengine"))
    # cold: a fresh editor on the saved project, its scenes closed and nothing of them kept (metas
    # parsed, scene files decoded); warm: the same call again
    try:
        async with Editor(project=saved, name="Pixel Art Large") as big:
            await big.call("get_project_summary")
            start = time.perf_counter()
            cold = await big.call_raw("check_pixel_art", {}, timeout=600)
            cold_s = time.perf_counter() - start
            start = time.perf_counter()
            warm = await big.call_raw("check_pixel_art", {}, timeout=600)
            warm_s = time.perf_counter() - start
            for r, label in ((cold, "cold"), (warm, "warm")):
                data = r.structured or {}
                seconds = cold_s if label == "cold" else warm_s
                big.check(not r.is_error, f"{label}: {r.text[:300]}")
                if seconds > 1.0:
                    big.check("job_id" in data, f"{label} took {seconds:.2f} s: it answered with a job ({list(data)[:8]})")
            print(f"[S42] check_pixel_art on 12 scenes, 600 textures, 6000 sprites: cold {cold_s:.3f} s, warm {warm_s:.3f} s")
            over = harness.over_budget("check_pixel_art on 12 scenes, 600 textures, 6000 sprites", {"warm": warm_s}, 1.0)
            over.update(harness.over_budget("check_pixel_art cold", {"cold": cold_s}, 3.0))
            big.check(not over, f"within budget: {over}")
    finally:
        shutil.rmtree(saved.parent, ignore_errors=True)
