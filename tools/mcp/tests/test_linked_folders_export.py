"""Linked folders in exports (docs/specs/S68-linked-folders.md R1-R3, AC1, AC2). A port of the sound
layer's cleanup group A-1 (je-wt/snd-ro tools/mcp/tests/test_cleanup_group_a.py).

Every export mode ships what linked folders hold at the paths the editor reads them by: assets, the
lua tree and shader sources, a link inside a linked folder, a link to the project's own folder (both
names), a linked file (inside the export tree). A link that would hold itself is left out with a
warning, a dangling one silently. The source exports of the first test are what every mode builds
from. With JASPER_SLOW_BUILD=1 a real Desktop game built with the prebuilt player loads a texture and a
Lua module from linked folders, and the export is left where a web build can take it
(JASPER_LINKED_WEB_EXPORT, see the test).

Oracles: files on disk read by Python (bytes), the editor's read_file and get_asset_info for the paths
it resolves, the player's own screenshot. Runs in a copy of tools/fixtures/legacy_paths_project (assets
dir "assets", Lua dir "scripts").
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import shutil
import time
from pathlib import Path

import build_cache
import pngdiff
from harness import REPO, Editor, skip, symlinks_permitted
from htmlui_pixels import count_near

# the Desktop game test builds with the run's prebuilt glcore runtime (tools/mcp/build_cache.py)
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"project": REPO / "tools" / "fixtures" / "legacy_paths_project", "env": build_cache.player_env("glcore", "release")}
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "1800"))
CI = True


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def tree(root: Path) -> dict:
    """{relative posix path: bytes} of the regular files under root (links followed, as a copy has none)"""
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(root.rglob("*")) if p.is_file()}


async def source_export(ed: Editor, name: str, platform: str) -> tuple:
    """(exported assets dir, exported lua dir, the build's warning texts and console warnings)"""
    await ed.call("editor_status")
    out = ed.scratch.root / name
    await ed.call("clear_console")
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform=platform, backends=["opengl"],
                      overwrite=True, timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"{platform} source export: {str(r)[:2000]}")
    console = await ed.call_raw("read_console", {})
    warnings = [w.get("message", "") for w in r["result"].get("warnings", [])]
    return out / "project" / "assets", out / "project" / "lua", warnings, console.text


@contextlib.contextmanager
def linked_folders(ed: Editor):
    """links in the project copy, removed after: yields ({assets-relative path: bytes} an export
    must ship, {lua-relative path: bytes} likewise)"""
    if not symlinks_permitted():
        skip("this process may not create symbolic links")
    assets = ed.project / "assets"
    outside = ed.scratch.root / "outside"
    shared, extra, lualib, shaders = outside / "shared", outside / "extra", outside / "lualib", outside / "shaders"
    write(shared / "notes.txt", "notes in a linked folder\n")
    write(shared / "sub" / "deep.txt", "deep in a linked folder\n")
    shared.mkdir(parents=True, exist_ok=True)
    shutil.copy(assets / "sounds" / "beep.wav", shared / "clip.wav")
    write(extra / "more.txt", "through a link inside a linked folder\n")
    write(outside / "single.txt", "a linked file\n")
    write(lualib / "helper.lua", "return { answer = 42 }\n")
    write(shaders / "glow.frag", "// a fork's fragment stage in a linked folder\nvoid main() {}\n")
    links = {
        assets / "shared": shared,                  # a linked folder out of the project
        shared / "more": extra,                     # a link inside it
        shared / "loop": shared,                    # leads to the folder it is in
        assets / "up": ed.project,                  # leads to a folder holding the assets dir
        assets / "alias": assets / "ui",            # the project's own folder under a second name
        assets / "single.txt": outside / "single.txt",
        assets / "broken": outside / "nowhere",     # leads nowhere
        ed.project / "scripts" / "lib": lualib,     # in the Lua dir
        ed.project / "linked_shaders": shaders,     # at the project root, beside the assets dir
    }
    expected = {
        "shared/notes.txt": (shared / "notes.txt").read_bytes(),
        "shared/sub/deep.txt": (shared / "sub" / "deep.txt").read_bytes(),
        "shared/clip.wav": (shared / "clip.wav").read_bytes(),
        "shared/more/more.txt": (extra / "more.txt").read_bytes(),
        "single.txt": (outside / "single.txt").read_bytes(),
        "linked_shaders/glow.frag": (shaders / "glow.frag").read_bytes(),
    }
    own_ui = {rel: data for rel, data in tree(assets / "ui").items() if not rel.endswith(".meta")}
    ed.check(own_ui, "the fixture has files in assets/ui")
    for rel, data in own_ui.items():
        expected["ui/" + rel] = data
        expected["alias/" + rel] = data
    try:
        for link, target in links.items():
            os.symlink(target, link)
        yield expected, {"lib/helper.lua": (lualib / "helper.lua").read_bytes()}
    finally:
        for link in links:
            if link.is_symlink():
                link.unlink()
        shutil.rmtree(outside, ignore_errors=True)


def cooked_at(files: dict, rel: str) -> bool:
    """the texture (S14) or model (S53) ships cooked: an index.json of assets/je-cooked/<family>/ names it, in place
    of its source"""
    return any(name.startswith("je-cooked/") and name.endswith("index.json") and rel.encode() in data
               for name, data in files.items())


def check_shipped(ed: Editor, label: str, files: dict, expected: dict):
    missing = sorted(rel for rel in expected if rel not in files and not cooked_at(files, rel))
    ed.check_eq(missing, [], f"{label}: files of linked folders missing from the export")
    # PNGs ship without the editor's chunks, which these have none of: the same bytes or the same image
    wrong = sorted(rel for rel, data in expected.items() if rel in files and files[rel] != data and not rel.endswith(".png"))
    ed.check_eq(wrong, [], f"{label}: exported with other bytes")
    held = sorted(rel for rel in files if rel.split("/")[0] in ("up", "broken") or rel.startswith("shared/loop/"))
    ed.check_eq(held, [], f"{label}: a link that holds itself or leads nowhere is not followed")
    metas = sorted(rel for rel in files if rel.endswith(".meta"))
    ed.check_eq(metas, [], f"{label}: no meta ships, a linked folder's neither")


async def test_linked_folders_ship_where_the_editor_reads_them(ed: Editor):
    ed.require_tools("build_project", "read_file", "get_asset_info")
    with linked_folders(ed) as (expected, expected_lua):
        # the editor resolves each path through its links
        for rel, text in {"assets/shared/notes.txt": "notes in a linked folder", "assets/shared/sub/deep.txt": "deep in a linked folder",
                          "assets/shared/more/more.txt": "through a link inside", "assets/single.txt": "a linked file",
                          "scripts/lib/helper.lua": "answer = 42"}.items():
            r = await ed.call_raw("read_file", {"path": rel})
            ed.check(not r.is_error and text in r.text, f"the editor reads {rel}: {r.text[:300]}")
        info = await ed.call_raw("get_asset_info", {"path": "assets/shared/clip.wav"})
        ed.check(not info.is_error, f"the editor reads the clip in the linked folder: {info.text[:300]}")

        for name, platform in (("export-linux", "linux"), ("export-web", "web"), ("export-android", "android")):
            exported, lua, warnings, console = await source_export(ed, name, platform)
            check_shipped(ed, platform, tree(exported), expected)
            lua_files = tree(lua)
            ed.check_eq({rel: lua_files.get(rel) for rel in expected_lua}, expected_lua,
                        f"{platform}: the Lua module in a linked folder of the Lua dir")
            # nothing lands outside the export (a linked file's canonical path is not its place)
            ed.check(not (ed.scratch.root / name / "outside").exists(), f"{platform}: nothing written beside the export")
            ed.check(not (exported.parent.parent.parent / "single.txt").exists(), f"{platform}: a linked file ships inside the tree")
            said = " ".join(warnings) + " " + console
            for link in ("shared/loop", "up"):
                ed.check(f"\"{link}\"" in said, f"{platform}: a warning names the linked folder {link} it leaves out: {said[:1500]}")


async def test_a_project_without_links_exports_the_same_files(ed: Editor):
    """R3: every file of the assets tree ships at its own path (or cooked), and nothing else does"""
    await ed.call("editor_status")
    exported, _, _, _ = await source_export(ed, "export-plain", "linux")
    shipped = tree(exported)
    own = {rel for rel in tree(ed.project / "assets") if not rel.endswith(".meta")}
    ed.check_eq(sorted(rel for rel in own if rel not in shipped and not cooked_at(shipped, rel)), [], "every file of the assets ships")
    # (a fork's shader sources ship from anywhere in the project, at their project-relative paths)
    sources = {rel for rel in tree(ed.project) if rel.endswith((".vert", ".frag", ".glsl")) and not rel.startswith((".", "build/"))}
    extra = sorted(rel for rel in shipped if rel not in own and rel not in sources and not rel.startswith("je-cooked/") and rel != "assetmap.bin")
    ed.check_eq(extra, [], "and nothing else")


async def test_a_link_to_the_export_itself_is_left_out(ed: Editor):
    """What the walk writes must not be walked: a link to the export's own folder would ship the export
    into itself without end. export-pipeline's rule ("the walkers skip the export's own folder")."""
    if not symlinks_permitted():
        skip("this process may not create symbolic links")
    out = ed.scratch.root / "export-self"
    (out / "project" / "assets").mkdir(parents=True)
    link = ed.project / "assets" / "own_export"
    os.symlink(out, link)
    try:
        exported, _, warnings, console = await source_export(ed, "export-self", "linux")
        held = [rel for rel in tree(exported) if rel.startswith("own_export/")]
        ed.check_eq(held, [], "nothing of the export ships inside it")
        ed.check_in("\"own_export\"", " ".join(warnings) + " " + console, "and the warning names the link")
    finally:
        link.unlink()


# -- the games ----------------------------------------------------------------------------------------


LUA_GAME = """local helper = require("lib.linkedhelper")

local LinkedGame = {}

function LinkedGame:init()
    print(helper.line)
end

return LinkedGame
"""

RED = bytes([255, 0, 0, 255])


async def wait_job(ed: Editor, result: dict, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    job = result
    while job["status"] == "running":
        if time.monotonic() > deadline:
            ed.fail(f"job {result['job_id']} still running after {timeout}s: {job}")
        await asyncio.sleep(5)
        job = await ed.call("get_job", job_id=result["job_id"], log_lines=10)
    return job


@contextlib.contextmanager
def linked_game(ed: Editor):
    """A red texture in a folder linked out of the project, a Lua module in a folder linked into the Lua dir"""
    if not symlinks_permitted():
        skip("this process may not create symbolic links")
    outside = ed.scratch.root / "game-outside"
    (outside / "art").mkdir(parents=True, exist_ok=True)
    (outside / "art" / "red.png").write_bytes(pngdiff.encode(16, 16, RED * 256))
    write(outside / "lualib" / "linkedhelper.lua", 'return { line = "LINKED-LUA-OK" }\n')
    links = [(ed.project / "assets" / "linked_art", outside / "art"), (ed.project / "scripts" / "lib", outside / "lualib")]
    try:
        for link, target in links:
            os.symlink(target, link)
        yield
    finally:
        for link, _ in links:
            if link.is_symlink():
                link.unlink()


async def make_linked_scene(ed: Editor, name: str):
    await ed.call("editor_status")
    made = await ed.call("create_scene", name=name, type="UI", path=f"{name}.scene")
    sid = made["scene"]["id"]
    await ed.call("create_ui", scene=sid, root={
        "type": "image", "name": "Backdrop", "color": [1, 1, 1, 1], "texture": "assets/linked_art/red.png",
        "layout": {"anchor": "stretch"}})
    (ed.project / "scripts" / "LinkedGame.lua").write_text(LUA_GAME)
    await ed.call("editor_status")
    await ed.call("attach_script", scene=sid, entity="Backdrop", path="scripts/LinkedGame.lua")
    await ed.call("save_scene", scene=sid)
    await ed.call("save_project")
    await ed.call("edit_build_scenes", action="set_start", scene=sid)
    return sid


async def test_desktop_game_loads_from_linked_folders(ed: Editor):
    """AC2: a real game: the texture it draws and the Lua module it requires are in linked folders"""
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop game (the run builds its prebuilt runtime once)")
    with linked_game(ed):
        await make_linked_scene(ed, "Linked Desktop")
        runtime = await build_cache.player_runtime(ed, "glcore", "release")
        out = ed.scratch.root / "export-linked-desktop"
        r = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
        job = await wait_job(ed, r, TIMEOUT)
        ed.check_eq(job["status"], "succeeded", f"{str(job)[:2000]}")
        ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")
        raw = await ed.call_raw("run_build", {"after_ready": True, "fixed_fps": 60, "quit_after": 60})
        ed.check(not raw.is_error, raw.text)
        ed.check(not raw.value["crashed"], f"the player crashed: {raw.value}")
        ed.check_eq(raw.value.get("exit_code"), 0, f"the player got ready and quit after its frames: {raw.value}")
        ed.check(raw.images, f"the player's own screenshot: {raw.text[:2000]}")
        png = raw.images[0][0]
        evidence = os.environ.get("JASPER_LINKED_EVIDENCE")
        if evidence:
            Path(evidence).mkdir(parents=True, exist_ok=True)
            (Path(evidence) / "desktop-linked.png").write_bytes(png)
        ed.check(count_near(png, (255, 0, 0)) > 500, "the player draws the texture of the linked folder")
        ed.check_in("LINKED-LUA-OK", str(raw.value.get("log")), "the player ran the Lua module of the linked folder")


async def test_web_export_for_the_web_game(ed: Editor):
    """AC2, the web half: the editor exports the source of the same game for the web. A web build needs
    the web image, which has no editor: with JASPER_LINKED_WEB_EXPORT the export stays in that folder, and
    docs/testing.md ("Linked folders in a web game") has the build and the browser run of it."""
    with linked_game(ed):
        await make_linked_scene(ed, "Linked Web")
        out = ed.scratch.root / "export-linked-web"
        r = await ed.call("build_project", mode="source", output_dir=str(out), platform="web", backends=["opengles"],
                          overwrite=True, timeout_seconds=900)
        ed.check_eq(r["status"], "succeeded", f"{str(r)[:2000]}")
        ed.check((out / "project" / "assets" / "linked_art" / "red.png").is_file() or
                 cooked_at(tree(out / "project" / "assets"), "linked_art/red.png"), "the texture of the linked folder ships")
        ed.check((out / "project" / "lua" / "lib" / "linkedhelper.lua").is_file(), "the Lua module of the linked folder ships")
        target = os.environ.get("JASPER_LINKED_WEB_EXPORT", "")
        if target:
            shutil.rmtree(target, ignore_errors=True)
            shutil.copytree(out, target, symlinks=True)
