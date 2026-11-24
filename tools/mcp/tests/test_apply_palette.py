"""apply_palette (docs/specs/S43-palettes.md R23, R24, R40, R43, AC15; editor/mcp/tools/PaletteTools.cpp), on a new
project with three textures made here against PICO-8: `clean` (only PICO-8 colours), `fixable` (PICO-8 colours 1-15
off by 2 per channel) and `photo` (a 32x32 gradient around pure blue, far from every PICO-8 colour):

- the project's assets directory is `assets/` (the fixtures' layout: project-relative paths differ from asset paths);
- a preview writes nothing (the file's hash) and returns one before|after image within max_width;
- output: true writes the texture onto the palette as one new project undo step, the one the result's `undo`
  {id, name} names, and one undo puts the original bytes back;
- a write that runs as a job names the same step in its job result (get_job);
- clean is `clean` with 0 pixels changed; fixable is `fixable`; photo is `needs_person` with its lost groups;
- reduce: 16 gives at most 16 colours, the same over two runs; save_palette writes a valid palette;
- measure_only never writes; with output it is refused; no palette and no Default Palette is not_found, the
  Default Palette (palettes.default) is used once set;
- `jasperengine-editor call apply_palette --measure-only` exits 0, 3 and 4 on the three textures (--json: code
  tool_exit), and 2 on a usage error.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import harness
import pngdiff
from harness import REPO, Editor, free_port, wait_for_port

CI = True
TIMEOUT = 900
PORT = free_port()
TOKEN = "s43-apply-palette-token"
EDITOR = {"create": True, "name": "ApplyPalette", "extra_args": ["--http-port", str(PORT)],
          "settings": {"mcp": {"http_token": TOKEN}}}
ATTACH = ["--port", str(PORT), "--token", TOKEN]

PICO8 = json.loads((REPO / "editor" / "resources" / "palettes" / "pico-8.palette").read_text())["colors"]
SPR = "assets/sprites"
CLEAN = f"{SPR}/clean.png"
FIXABLE = f"{SPR}/fixable.png"
PHOTO = f"{SPR}/photo.png"


def rgb(hexed: str) -> tuple[int, int, int]:
    return tuple(int(hexed[i:i + 2], 16) for i in (1, 3, 5))


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def colors_of(png: bytes) -> set[tuple[int, int, int, int]]:
    width, height, rgba = pngdiff.decode(png)
    return {tuple(rgba[i:i + 4]) for i in range(0, width * height * 4, 4)}


def write_textures(ed: Editor) -> None:
    folder = ed.project / SPR
    folder.mkdir(parents=True, exist_ok=True)
    clean, fixable = bytearray(), bytearray()
    for y in range(8):
        for x in range(8):
            r, g, b = rgb(PICO8[(x + y * 8) % 16])
            clean += bytes((r, g, b, 255))
            r, g, b = rgb(PICO8[1 + (x + y * 8) % 15])
            fixable += bytes((min(255, r + 2), min(255, g + 2), min(255, b + 2), 255))
    photo = bytearray()
    for y in range(32):
        for x in range(32):
            photo += bytes((x, y, 240, 255))
    (ed.project / CLEAN).write_bytes(pngdiff.encode(8, 8, bytes(clean)))
    (ed.project / FIXABLE).write_bytes(pngdiff.encode(8, 8, bytes(fixable)))
    (ed.project / PHOTO).write_bytes(pngdiff.encode(32, 32, bytes(photo)))


async def apply(ed: Editor, **args):
    return await ed.call_raw("apply_palette", args, timeout=300)


async def project_steps(ed: Editor) -> list[dict]:
    return (await ed.call("get_undo_history", scope="project", limit=500))["entries"]


async def check_one_step(ed: Editor, before: list[dict], result: dict, what: str) -> None:
    """the call added one project undo step, and its result's undo names it"""
    known = {e["id"] for e in before}
    added = [e for e in await project_steps(ed) if e["id"] not in known]
    ed.check_eq(len(added), 1, f"{what}: one undo step ({[e['name'] for e in added]})")
    undo = result.get("undo")
    ed.check(isinstance(undo, dict) and undo.get("id") == added[0]["id"], f"{what}: the result's undo {{id, name}} names the step: {undo}")
    ed.check_eq(added[0]["origin"].get("tool"), "apply_palette", f"{what}: the step is the call's")


async def test_setup(ed: Editor):
    await ed.call("get_project_summary")  # the new project is made by then
    (ed.project / "assets").mkdir(exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets"})
    ed.check_eq((await ed.call("get_project_summary"))["assets_dir"], "assets")
    write_textures(ed)
    await ed.call("tool_groups", enable=["pixel_art"])
    await wait_for_port(PORT)


async def test_preview_writes_nothing(ed: Editor):
    before = sha(ed.project / FIXABLE)
    r = await apply(ed, path=FIXABLE, palette="builtin:pico-8")
    ed.check(not r.is_error, f"preview: {r.text[:500]}")
    v = r.value
    ed.check_eq(v["verdict"], "fixable", f"off by 2: {json.dumps(v['stats'])}")
    ed.check_eq(v["stats"]["pixels_changed"], 64, "every pixel moves onto the palette")
    ed.check(v["stats"]["max_delta_e"] <= 15, f"max dE within fixable: {v['stats']}")
    ed.check(not v["lost"], f"no lost colours: {v['lost']}")
    ed.check("output: true" in v.get("next", ""), f"the preview says how to write: {v.get('next')}")
    ed.check_eq(sha(ed.project / FIXABLE), before, "a preview writes nothing")
    ed.check(not (ed.project / "assets" / "Palettes").exists(), "a built-in named in apply_palette is not copied (nothing stores it)")
    ed.check_eq(len(r.images), 1, "one image: before | after")
    width, height, _ = pngdiff.decode(r.images[0][0])
    ed.check(width <= 768 and width >= 2 * 8 and height >= 8, f"before|after within max_width 768: {width}x{height}")
    used = {u["hex"] for u in v["usage"] if u["count"] > 0}
    ed.check(used and used <= set(PICO8), f"usage names palette colours: {used}")


async def test_output_and_undo(ed: Editor):
    file = ed.project / FIXABLE
    before = sha(file)
    meta = Path(str(file) + ".meta")
    meta_before = meta.read_bytes() if meta.exists() else None
    steps = await project_steps(ed)
    r = await apply(ed, path=FIXABLE, palette="builtin:pico-8", output=True)
    ed.check(not r.is_error, f"output: {r.text[:500]}")
    v = r.value
    ed.check_eq(v["written"], FIXABLE)
    await check_one_step(ed, steps, v, "output: true")
    ed.check(sha(file) != before, "the texture is written")
    after = colors_of(file.read_bytes())
    ed.check(after <= {rgb(c) + (255,) for c in PICO8}, f"only PICO-8 colours now: {sorted(after)[:5]}")
    await ed.call("undo", scope="project")
    ed.check_eq(sha(file), before, "undo restores the original bytes")
    if meta_before is not None:
        ed.check_eq(meta.read_bytes(), meta_before, "the texture's .meta (its GUID) is untouched")


async def test_output_true_as_a_string(ed: Editor):
    """output takes a path too, so a model sends output: true as "true" (the MCP eval's samples): the texture
    itself is written, never a file named true.png; "false" previews."""
    file = ed.project / FIXABLE
    before = sha(file)
    r = await apply(ed, path=FIXABLE, palette="builtin:pico-8", output="false")
    ed.check(not r.is_error, f"output \"false\": {r.text[:500]}")
    ed.check_eq(sha(file), before, "output \"false\" previews")
    r = await apply(ed, path=FIXABLE, palette="builtin:pico-8", output="true")
    ed.check(not r.is_error, f"output \"true\": {r.text[:500]}")
    ed.check_eq(r.value["written"], FIXABLE)
    ed.check(sha(file) != before, "output \"true\" overwrites the texture")
    ed.check(not (ed.project / "true.png").exists() and not (ed.project / SPR / "true.png").exists(), "no file named true.png")
    await ed.call("undo", scope="project")
    ed.check_eq(sha(file), before, "undo restores the original bytes")


async def test_job_result_names_the_step(ed: Editor):
    """An apply past about 1 s of work (2048x2048) runs as a job: its own result (get_job) names the undo step as the
    reply does."""
    big = f"{SPR}/big.png"
    row = b"".join(bytes((min(255, r + 2), min(255, g + 2), min(255, b + 2), 255)) for r, g, b in map(rgb, PICO8)) * (2048 // 16)
    (ed.project / big).write_bytes(pngdiff.encode(2048, 2048, row * 2048))
    steps = await project_steps(ed)
    r = await apply(ed, path=big, palette="builtin:pico-8", output=True, image=False)
    ed.check(not r.is_error, f"output: {r.text[:500]}")
    v = r.value
    ed.check(v.get("job_id"), f"a 2048x2048 apply runs as a job: {r.text[:300]}")
    await check_one_step(ed, steps, v, "a job's write")
    job = await ed.call("get_job", job_id=v["job_id"])
    ed.check_eq(job["result"].get("undo"), v["undo"], "the job's result names the reply's step")


async def test_verdicts(ed: Editor):
    clean = (await apply(ed, path=CLEAN, palette="builtin:pico-8")).value
    ed.check_eq((clean["verdict"], clean["stats"]["pixels_changed"]), ("clean", 0), f"clean: {clean['stats']}")
    r = await apply(ed, path=CLEAN, palette="builtin:pico-8", output=True)
    ed.check(not r.is_error and "written" not in r.value, f"a clean texture has nothing to write: {r.text[:300]}")
    photo = (await apply(ed, path=PHOTO, palette="builtin:pico-8")).value
    ed.check_eq(photo["verdict"], "needs_person", f"photo: {photo['stats']}")
    ed.check(photo["lost"], "lost groups listed")
    group = photo["lost"][0]
    ed.check(group["count"] >= 16 and len(group["box"]) == 4, f"the largest lost group: {group}")
    ed.check(group["hex"].startswith("#") and group["mapped"] in PICO8, f"original and mapped colours: {group}")
    hexed = await apply(ed, path=CLEAN, palette=PICO8)
    ed.check_eq(hexed.value["verdict"], "clean", "a [hex] palette")


async def test_reduce(ed: Editor):
    first = (await apply(ed, path=PHOTO, reduce=16)).value
    second = (await apply(ed, path=PHOTO, reduce=16)).value
    ed.check(len(first["palette"]["colors"]) <= 16, f"at most 16 colours: {first['palette']}")
    ed.check_eq(first["palette"]["colors"], second["palette"]["colors"], "Reduce Colours is deterministic")
    ed.check_eq(first["stats"], second["stats"], "the same statistics")
    out = f"{SPR}/photo16.png"
    again = f"{SPR}/photo16b.png"
    a = (await apply(ed, path=PHOTO, reduce=16, output=out)).value
    b = (await apply(ed, path=PHOTO, reduce=16, output=again)).value
    ed.check_eq((a["written"], b["written"]), (out, again), "output: a path writes a new file")
    ed.check_eq((ed.project / out).read_bytes(), (ed.project / again).read_bytes(), "byte-identical over two runs")
    ed.check(len(colors_of((ed.project / out).read_bytes())) <= 16, "the written texture has at most 16 colours")
    r = await apply(ed, path=PHOTO, reduce=16, output=out)
    ed.check(r.is_error and r.value["errors"][0]["kind"] == "exists", f"a new file that exists: {r.text[:300]}")

    steps = await project_steps(ed)
    saved = (await apply(ed, path=PHOTO, reduce=8, output=f"{SPR}/photo8.png", save_palette="assets/Palettes/photo8.palette")).value
    ed.check_eq(saved["palette_written"], "assets/Palettes/photo8.palette")
    await check_one_step(ed, steps, saved, "the texture and the palette")
    read = await ed.call("edit_palette", path="assets/Palettes/photo8.palette")
    ed.check(2 <= read["palette"]["count"] <= 8, f"a valid palette of at most 8 colours: {read['palette']}")
    ed.check_eq(read["palette"].get("source", {}).get("kind"), "texture", "its source")
    r = await apply(ed, path=PHOTO, save_palette="assets/Palettes/x.palette")
    ed.check(r.is_error and r.value["errors"][0]["field"] == "save_palette", f"save_palette needs reduce: {r.text[:300]}")


async def test_measure_only_and_default(ed: Editor):
    before = sha(ed.project / FIXABLE)
    v = (await apply(ed, path=FIXABLE, palette="builtin:pico-8", measure_only=True)).value
    ed.check_eq((v["verdict"], v["cli_exit"]), ("fixable", 3), "measure_only's verdict and exit code")
    ed.check_eq(sha(ed.project / FIXABLE), before, "measure_only never writes")
    r = await apply(ed, path=FIXABLE, palette="builtin:pico-8", measure_only=True, output=True)
    ed.check(r.is_error and r.value["errors"][0]["kind"] == "invalid_value", f"measure_only with output: {r.text[:300]}")

    r = await apply(ed, path=CLEAN)
    ed.check(r.is_error and r.value["errors"][0]["kind"] == "not_found", f"no palette, no Default Palette: {r.text[:300]}")
    ed.check("Default Palette" in r.value["errors"][0]["fix"], f"the fix names the setting: {r.value['errors']}")
    made = await ed.call("edit_palette", {"from": {"builtin": "pico-8"}, "output": True})
    await ed.call("set_project_settings", values={"palettes.default": made["path"]})
    v = (await apply(ed, path=CLEAN)).value
    ed.check_eq(v["verdict"], "clean", "the Default Palette is used")
    ed.check_eq(v["palette"].get("path"), made["path"], "and named")


class Cli:
    def __init__(self):
        self.root = Path(tempfile.mkdtemp(prefix="jasper-apply-cli-"))
        (self.root / "home").mkdir()
        (self.root / "cwd").mkdir()

    def run(self, *args: str) -> subprocess.CompletedProcess:
        env = {k: v for k, v in os.environ.items() if k not in ("JASPER_MCP_URL", "JASPER_MCP_TOKEN", "DISPLAY", "WAYLAND_DISPLAY")}
        env["HOME"] = str(self.root / "home")
        return subprocess.run(harness.editor_command() + list(args), cwd=str(self.root / "cwd"), env=env,
                              capture_output=True, text=True, timeout=600)


async def test_cli_exit_codes(ed: Editor):
    cli = Cli()
    before = sha(ed.project / FIXABLE)
    try:
        with ed.busy():
            for path, code, verdict in ((CLEAN, 0, "clean"), (FIXABLE, 3, "fixable"), (PHOTO, 4, "needs_person")):
                p = await asyncio.to_thread(cli.run, "call", *ATTACH, "apply_palette", "--path", path, "--palette", "builtin:pico-8",
                                            "--measure-only")
                ed.check_eq(p.returncode, code, f"{path}: exit {p.returncode}\n{p.stdout[-400:]}\n{p.stderr[-400:]}")
                ed.check(verdict in p.stdout, f"{path}: the verdict on stdout: {p.stdout[-300:]}")
            p = await asyncio.to_thread(cli.run, "call", *ATTACH, "--json", "apply_palette", "--path", FIXABLE, "--palette",
                                        "builtin:pico-8", "--measure-only")
            envelope = json.loads(p.stdout.splitlines()[-1])
            ed.check_eq((p.returncode, envelope["exit_code"], envelope["code"], envelope["ok"]), (3, 3, "tool_exit", True),
                        f"--json tells the tool's code apart: {envelope}")
            p = await asyncio.to_thread(cli.run, "call", *ATTACH, "apply_palette", "--path", FIXABLE, "--reduce", "1", "--measure-only")
            ed.check_eq(p.returncode, 2, f"a usage error: {p.stderr[-300:]}")
        ed.check_eq(sha(ed.project / FIXABLE), before, "the command line's measure_only writes nothing")
        ed.check(not any((cli.root / "cwd").iterdir()), "measure_only saves no image")
    finally:
        shutil.rmtree(cli.root, ignore_errors=True)
