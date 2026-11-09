"""edit_palette (docs/specs/S43-palettes.md R7-R9, R39, AC3, AC4, AC16, AC20; editor/mcp/tools/PaletteTools.cpp),
on a new project:

- the pixel_art group is hidden by default and listed once tool_groups enables it;
- the project's assets directory is `assets/` (the fixtures' layout: `path` is project-relative, `asset_path` is
  relative to the assets directory);
- Lospec (AC3) through the recorded mirror (tools/mcp/tests/fixtures/lospec-mirror, copied to a scratch folder the
  editor's JASPERENGINE_IMPORT_MIRROR names): endesga-32 creates assets/Palettes/endesga-32.palette with its author
  and source; the URL and lospec-palette:// forms normalise; an unknown slug is lospec_not_found with its fix; the
  Offline preference (preferences.network.offline) and the mirror folder removed are network_unavailable at once; the red control, an editor with no mirror and the network
  blocked (an unroutable proxy for curl, or no curl), gives the typed error within its 10 s timeout, never a hang;
- built-ins (AC4): 37 + 1 listed; from {builtin: "pico-8"} twice writes one copy and reuses it; every built-in
  validates; the pixelfx ramps equal editor/tests/fixtures/palettes/pixelfx-ramps.json;
- ops (AC16): each op on a scratch palette, preview vs output (the file's hash), one undo step per call, a sort keeps
  ramps and variants on their colours, generate_ramp gives the AC6 golden, export in every format reads back to
  the same colours, `texture` usage equals a direct count of a PNG made here;
- writes are refused while a scene plays (play_mode), previews work then.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import tempfile
import time
from pathlib import Path

import harness
import pngdiff
from harness import REPO, Editor

CI = True
TIMEOUT = 900

MIRROR_SOURCE = REPO / "tools" / "mcp" / "tests" / "fixtures" / "lospec-mirror"
MIRROR = Path(tempfile.mkdtemp(prefix="jasper-lospec-mirror-")) / "mirror"
shutil.copytree(MIRROR_SOURCE, MIRROR)
EDITOR = {"create": True, "name": "Palettes", "env": {"JASPERENGINE_IMPORT_MIRROR": str(MIRROR)}}

PIXELFX_RAMPS = REPO / "editor" / "tests" / "fixtures" / "palettes" / "pixelfx-ramps.json"
OPS = "assets/Palettes/ops.palette"


def sha(path: Path) -> str | None:
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None


def errors_of(r) -> list[dict]:
    value = r.structured if isinstance(r.structured, dict) else {}
    return value.get("errors") or []


async def expect_kind(ed: Editor, kind: str, args: dict) -> dict:
    r = await ed.call_raw("edit_palette", args, timeout=120)
    ed.check(r.is_error, f"edit_palette {args} should fail with {kind}: {r.text[:500]}")
    errors = errors_of(r)
    ed.check(errors and errors[0]["kind"] == kind, f"error kind {kind}: {r.text[:500]} {errors}")
    ed.check(all(e.get("fix") for e in errors), f"every error names its fix: {errors}")
    return errors[0]


def ramp_hexes(palette: dict, name: str) -> list[str]:
    ramp = next(r for r in palette["ramps"] if r["name"] == name)
    return [palette["colors"][i] for i in ramp["colors"]]


async def test_setup(ed: Editor):
    await ed.call("get_project_summary")  # the new project is made by then
    (ed.project / "assets").mkdir(exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets"})
    ed.check_eq((await ed.call("get_project_summary"))["assets_dir"], "assets")


async def test_group_hidden_until_enabled(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_palette" not in default and "apply_palette" not in default, "pixel_art is hidden by default")
    await ed.call("tool_groups", enable=["pixel_art"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_palette" in listed and "apply_palette" in listed, "listed once tool_groups enables pixel_art")


async def test_lospec(ed: Editor):
    target = ed.project / "assets" / "Palettes" / "endesga-32.palette"
    preview = await ed.call("edit_palette", {"from": {"lospec": "endesga-32"}}, timeout=120)
    ed.check(not target.exists(), "a Lospec preview writes nothing")
    ed.check_eq(preview["palette"]["count"], 32, "Endesga 32's colours")
    ed.check("output: true" in preview.get("next", ""), f"the preview says how to write it: {preview.get('next')}")
    made = await ed.call("edit_palette", {"from": {"lospec": "endesga-32"}, "output": True}, timeout=120)
    ed.check(target.exists(), f"output writes {target}")
    ed.check_eq(made["written"], ["assets/Palettes/endesga-32.palette"])
    ed.check(made.get("job_id"), "the fetch ran as a job")
    top = (await ed.call("get_undo_history", scope="project", limit=1))["entries"][0]
    undo = made.get("undo")
    ed.check(isinstance(undo, dict) and undo.get("id") == top["id"], f"the result's undo {{id, name}} names the step: {undo} {top}")
    ed.check_eq(top["origin"].get("tool"), "edit_palette", "the job's write is the agent's step")
    stored = json.loads(target.read_text())
    ed.check_eq(stored["author"], "ENDESGA", "the author")
    ed.check_eq(stored["source"]["kind"], "lospec")
    ed.check_eq(stored["source"]["id"], "endesga-32")
    ed.check_eq(stored["source"]["url"], "https://lospec.com/palette-list/endesga-32")
    ed.check(len(stored["source"].get("fetched", "")) == 10, f"the fetch date: {stored['source']}")
    ed.check_eq(stored["colors"][0], "#be4a2f", "the first colour")

    # the URL and link forms name the same palette
    for given in ("https://lospec.com/palette-list/pico-8", "lospec-palette://pico-8", "  PICO-8 "):
        got = await ed.call("edit_palette", {"from": {"lospec": given}}, timeout=120)
        ed.check_eq(got["palette"]["count"], 16, f"{given!r} normalises to pico-8")
        ed.check_eq(got["path"], "assets/Palettes/pico-8.palette", f"{given!r}: the default path")
    error = await expect_kind(ed, "lospec_not_found", {"from": {"lospec": "no-such-palette-xyz"}})
    ed.check("slug" in error["fix"].lower(), f"the fix names the slug: {error}")
    await expect_kind(ed, "invalid_value", {"from": {"lospec": "not a slug!"}})

    # the editor's Offline preference: refused at once, before any request
    await ed.call("set_project_settings", values={"preferences.network.offline": True})
    try:
        started = time.monotonic()
        error = await expect_kind(ed, "network_unavailable", {"from": {"lospec": "endesga-32"}})
        harness.over_budget("Lospec while offline", {"network_unavailable": time.monotonic() - started}, 1.0)
        ed.check("offline" in error["message"].lower(), f"it says why: {error}")
    finally:
        await ed.call("set_project_settings", values={"preferences.network.offline": False})

    # the mirror gone: no network at once
    shutil.rmtree(MIRROR)
    started = time.monotonic()
    await expect_kind(ed, "network_unavailable", {"from": {"lospec": "endesga-32"}})
    harness.over_budget("Lospec with the mirror gone", {"network_unavailable": time.monotonic() - started}, 1.0)


async def test_lospec_network_blocked():
    """The red control of test_lospec: no mirror and curl's network blocked (a proxy nobody listens on), so a real
    request is made and fails; the call ends with the typed error, never a hang."""
    blocked = {"JASPERENGINE_IMPORT_MIRROR": "", "https_proxy": "http://127.0.0.1:9", "HTTPS_PROXY": "http://127.0.0.1:9",
               "all_proxy": "http://127.0.0.1:9", "ALL_PROXY": "http://127.0.0.1:9", "no_proxy": "", "NO_PROXY": ""}
    async with Editor(create=True, name="Offline", env=blocked, label="palettes-offline") as ed:
        started = time.monotonic()
        r = await ed.call_raw("edit_palette", {"from": {"lospec": "endesga-32"}}, timeout=120)
        seconds = time.monotonic() - started
        value = r.structured if isinstance(r.structured, dict) else {}
        ed.check(value.get("status") != "running", f"the fetch ended (no hang): {r.text[:400]}")
        ed.check(r.is_error, f"no network is an error: {r.text[:400]}")
        ed.check_eq([e["kind"] for e in errors_of(r)], ["network_unavailable"], f"the typed error: {r.text[:400]}")
        ed.check(seconds < 60, f"bounded by the 10 s timeout, not a hang: {seconds:.1f} s")
        harness.over_budget("Lospec with the network blocked", {"network_unavailable": seconds}, 10.0)
        ed.check(not list(ed.project.rglob("*.palette")), f"nothing was written: {list(ed.project.rglob('*.palette'))}")


async def test_builtins(ed: Editor):
    listed = await ed.call("edit_palette")
    ids = [b["id"] for b in listed["builtins"]]
    ed.check_eq(len(ids), 38, f"37 + 1 built-ins: {ids}")
    for expected in ("pico-8", "game-boy", "sweetie-16", "endesga-32", "fire", "night", "pixelfx-effects"):
        ed.check_in(expected, ids, "built-in listed")

    first = await ed.call("edit_palette", {"from": {"builtin": "pico-8"}, "output": True})
    ed.check_eq(first["path"], "assets/Palettes/pico-8.palette", "the copy's path")
    ed.check_eq(first["asset_path"], "Palettes/pico-8.palette", "the path materials and settings store")
    ed.check(first.get("undo"), f"the copy is one undo step: {first}")
    second = await ed.call("edit_palette", {"from": {"builtin": "builtin:pico-8"}, "output": True})
    ed.check(second.get("reused") is True, f"the second use reuses the copy: {second}")
    ed.check_eq(second["path"], first["path"], "the same copy")
    ed.check(not second.get("undo"), "nothing written the second time")
    copies = []
    for file in (ed.project / "assets").rglob("*.palette"):
        source = json.loads(file.read_text()).get("source", {})
        if source.get("kind") == "builtin" and source.get("id") == "pico-8":
            copies.append(file)
    ed.check_eq(len(copies), 1, f"one copy of pico-8: {copies}")
    ed.check_eq(copies[0].read_text(encoding="utf-8"),
                (REPO / "editor" / "resources" / "palettes" / "pico-8.palette").read_text(encoding="utf-8"), "the copy is the built-in's bytes")
    relisted = await ed.call("edit_palette")
    ed.check(any(p.get("builtin") == "pico-8" for p in relisted["palettes"]), f"the copy is a project palette: {relisted['palettes']}")

    # every built-in validates (a preview runs the R2 checks)
    for builtin in listed["builtins"]:
        got = await ed.call("edit_palette", path=f"builtin:{builtin['id']}")
        ed.check_eq(got["palette"]["count"], builtin["count"], f"builtin:{builtin['id']}")

    # pixelfx's ramps, value for value
    ramps = json.loads(PIXELFX_RAMPS.read_text())
    effects = (await ed.call("edit_palette", path="builtin:pixelfx-effects"))["palette"]
    for name, colors in ramps.items():
        own = (await ed.call("edit_palette", path=f"builtin:{name}"))["palette"]
        ed.check_eq(ramp_hexes(own, name), [c.lower() for c in colors], f"builtin:{name}'s ramp")
        ed.check_eq(ramp_hexes(effects, name), [c.lower() for c in colors], f"pixelfx-effects' {name} ramp")
    await expect_kind(ed, "not_found", {"from": {"builtin": "no-such"}})


async def call_ops(ed: Editor, ops: list[dict], output: bool = True, **extra) -> dict:
    return await ed.call("edit_palette", {"path": OPS, "ops": ops, "output": output, **extra})


async def test_edit_ops(ed: Editor):
    file = ed.project / OPS
    made = await ed.call("edit_palette", {"path": OPS, "from": {"colors": ["#000000", "#ff0000", "#00ff00", "#0000ff"]}, "output": True})
    ed.check(file.exists(), "from colors writes the palette")
    ed.check_eq(made["palette"]["colors"], ["#000000", "#ff0000", "#00ff00", "#0000ff"])
    await expect_kind(ed, "exists", {"path": OPS, "from": {"colors": ["#000000"]}, "output": True})

    # preview vs output, and one undo step
    before = sha(file)
    preview = await call_ops(ed, [{"op": "set", "index": 0, "color": "#ffffff"}], output=False)
    ed.check_eq(preview["palette"]["colors"][0], "#ffffff", "the preview shows the edit")
    ed.check_eq(preview["changed"], [0])
    ed.check_eq(sha(file), before, "a preview writes nothing")
    steps = {e["id"] for e in (await ed.call("get_undo_history", scope="project", limit=500))["entries"]}
    written = await call_ops(ed, [{"op": "set", "index": 0, "color": "#ffffff"}, {"op": "set", "index": 1, "color": "#ee0000"}])
    ed.check(sha(file) != before, "output writes")
    added = [e for e in (await ed.call("get_undo_history", scope="project", limit=500))["entries"] if e["id"] not in steps]
    ed.check_eq(len(added), 1, f"two ops, one project undo step: {[e['name'] for e in added]}")
    undo = written.get("undo")
    ed.check(isinstance(undo, dict) and undo.get("id") == added[0]["id"], f"the result's undo {{id, name}} names the step: {undo}")
    await ed.call("undo", scope="project")
    ed.check_eq(sha(file), before, "one undo puts both ops back")
    await ed.call("redo", scope="project")
    ed.check_eq(json.loads(file.read_text())["colors"][:2], ["#ffffff", "#ee0000"], "redo")

    p = (await call_ops(ed, [{"op": "insert", "index": 1, "color": "#808080"}]))["palette"]
    ed.check_eq(p["colors"], ["#ffffff", "#808080", "#ee0000", "#00ff00", "#0000ff"], "insert")
    p = (await call_ops(ed, [{"op": "move", "from": 1, "to": 4}]))["palette"]
    ed.check_eq(p["colors"], ["#ffffff", "#ee0000", "#00ff00", "#0000ff", "#808080"], "move")
    p = (await call_ops(ed, [{"op": "remove", "index": 4}]))["palette"]
    ed.check_eq(p["count"], 4, "remove")
    p = (await call_ops(ed, [{"op": "rename_color", "index": 1, "name": "red"}]))["palette"]
    ed.check_eq(p.get("names"), {"1": "red"}, "rename_color")
    p = (await call_ops(ed, [{"op": "lock", "indices": [0, 1]}]))["palette"]
    ed.check_eq(p["locked"], [0, 1], "lock")
    p = (await call_ops(ed, [{"op": "set_transparent", "index": 0}]))["palette"]
    ed.check_eq(p["transparent"], 0, "set_transparent")
    p = (await call_ops(ed, [{"op": "set_info", "name": "Ops", "author": "Tests", "columns": 4}]))["palette"]
    ed.check_eq((p["name"], p.get("author")), ("Ops", "Tests"), "set_info")
    p = (await call_ops(ed, [{"op": "add_ramp", "name": "rgb", "colors": [1, 2, 3]}, {"op": "set_ramp", "name": "rgb", "cycle": 2}]))["palette"]
    ed.check_eq(p["ramps"], [{"name": "rgb", "colors": [1, 2, 3], "cycle": 2}], "add_ramp, set_ramp")
    p = (await call_ops(ed, [{"op": "add_variant", "name": "Alt", "colors": [None, "#110000", "#001100", "#000011"]},
                             {"op": "set_variant", "name": "Alt", "index": 0, "color": "#123456"}]))["palette"]
    ed.check_eq(p["variants"], [{"name": "Alt", "colors": ["#123456", "#110000", "#001100", "#000011"]}], "add_variant, set_variant")

    # a sort renumbers; the ramp, the variant, the locks and the names follow their colours
    ramp_before = ramp_hexes(p, "rgb")
    variant_before = dict(zip(p["colors"], p["variants"][0]["colors"]))
    locked_before = sorted(p["colors"][i] for i in p["locked"])
    sorted_result = await call_ops(ed, [{"op": "sort", "by": "luma"}])
    s = sorted_result["palette"]
    ed.check(s["colors"] != p["colors"], f"luma sort reorders: {s['colors']}")
    ed.check_eq(ramp_hexes(s, "rgb"), ramp_before, "the ramp keeps its colours")
    ed.check_eq(dict(zip(s["colors"], s["variants"][0]["colors"])), variant_before, "the variant keeps its colours")
    ed.check_eq(sorted(s["colors"][i] for i in s["locked"]), locked_before, "the locks follow")
    ed.check_eq(s["colors"][s["transparent"]], "#ffffff", "the transparent index follows")
    ed.check_eq(s["colors"][int(next(iter(s["names"])))], "#ee0000", "the name follows")
    ed.check(any("colour numbers" in w for w in sorted_result["warnings"]), f"the sort warns: {sorted_result['warnings']}")

    g = (await call_ops(ed, [{"op": "generate_ramp", "base": "#b13e53", "darker": 3, "lighter": 3, "name": "skin"}]))["palette"]
    ed.check_eq(ramp_hexes(g, "skin"), ["#630445", "#7d1249", "#97254d", "#b13e53", "#ca5b5c", "#e38d7d", "#fcc0a4"],
                "generate_ramp gives the AC6 golden")
    p = (await call_ops(ed, [{"op": "remove_ramp", "name": "skin"}, {"op": "remove_variant", "name": "Alt"}]))["palette"]
    ed.check_eq(([r["name"] for r in p["ramps"]], p["variants"]), (["rgb"], []), "remove_ramp, remove_variant")

    # a failing op changes nothing and names its field
    before = sha(file)
    r = await ed.call_raw("edit_palette", {"path": OPS, "ops": [{"op": "set", "index": 0, "color": "#000000"},
                                                                 {"op": "set", "index": 99, "color": "#000000"}], "output": True})
    ed.check(r.is_error and errors_of(r)[0]["field"] == "ops[1].index", f"the failing op's field: {r.text[:300]}")
    ed.check_eq(sha(file), before, "nothing written")
    await expect_kind(ed, "invalid_value", {"path": OPS, "ops": [{"op": "explode"}]})

    # export: every format reads back to the same colours
    colors = p["colors"]
    for fmt, ext in (("gpl", "gpl"), ("hex", "hex"), ("pal", "pal"), ("pal_riff", "pal"), ("txt", "txt"), ("ase", "ase"),
                     ("png", "png"), ("colors", "colors")):
        out = f"assets/Exports/ops-{fmt}.{ext}"
        preview = await ed.call("edit_palette", {"path": OPS, "export": {"path": out, "format": fmt}})
        ed.check(not (ed.project / out).exists(), f"{fmt}: an export preview writes nothing")
        ed.check(preview["export"]["bytes"] > 0, f"{fmt}: the export's size")
        done = await ed.call("edit_palette", {"path": OPS, "export": {"path": out, "format": fmt}, "output": True})
        ed.check((ed.project / out).exists() and done["written"] == [out], f"{fmt}: written {done.get('written')}")
        back = await ed.call("edit_palette", {"path": f"assets/Palettes/back-{fmt}.palette", "from": {"file": out}})
        ed.check_eq([c[:7] for c in back["palette"]["colors"]], [c[:7] for c in colors], f"{fmt} reads back")

    # texture: usage counts against a direct count
    width, height = 6, 4
    pixels = []
    for y in range(height):
        for x in range(width):
            if x < 2:
                pixels.append((0x00, 0xff, 0x00, 255))
            elif x < 5:
                pixels.append((0x00, 0x00, 0xff, 255))
            elif y == 0:
                pixels.append((0x12, 0x34, 0x56, 255))
            else:
                pixels.append((0, 0, 0, 0))
    texture = ed.project / "assets" / "sprites" / "usage.png"
    texture.parent.mkdir(parents=True, exist_ok=True)
    texture.write_bytes(pngdiff.encode(width, height, bytes(c for px in pixels for c in px)))
    usage = (await ed.call("edit_palette", {"path": OPS, "texture": "assets/sprites/usage.png"}))["usage"]
    expected = {}
    off = 0
    for r_, g_, b_, a_ in pixels:
        if a_ == 0:
            continue
        hexed = f"#{r_:02x}{g_:02x}{b_:02x}"
        if hexed in colors:
            expected[colors.index(hexed)] = expected.get(colors.index(hexed), 0) + 1
        else:
            off += 1
    ed.check_eq({c["index"]: c["count"] for c in usage["counts"]}, expected, "usage counts")
    ed.check_eq(usage["off_palette_pixels"], off, "off-palette pixels")
    ed.check_eq([c["hex"] for c in usage["off_colors"]], ["#123456"], "the off-palette colour")

    # a swatch image on request
    r = await ed.call_raw("edit_palette", {"path": OPS, "image": True})
    ed.check(not r.is_error and len(r.images) == 1 and r.images[0][1] == "image/png", f"one swatch PNG: {r.text[:300]}")


async def test_writes_refused_while_playing(ed: Editor):
    made = await ed.call("create_scene", name="PalettePlay", type="2D")
    scene = made["scene"]["id"]
    await ed.call("save_scene", scene=scene, path="scenes/PalettePlay")
    await ed.call("select_scene", scene=scene)
    file = ed.project / OPS
    before = sha(file)
    started = await ed.call("play", action="start", scene=scene)
    try:
        ed.check_eq(started["state"], "playing", "play starts")
        await expect_kind(ed, "play_mode", {"path": OPS, "ops": [{"op": "set", "index": 0, "color": "#010203"}], "output": True})
        ed.check_eq(sha(file), before, "nothing written while playing")
        preview = await call_ops(ed, [{"op": "set", "index": 0, "color": "#010203"}], output=False)
        ed.check_eq(preview["palette"]["colors"][0], "#010203", "a preview works while playing")
    finally:
        await ed.call("play", action="stop")
    await ed.call("close_scene", scene=scene, unsaved="discard")
