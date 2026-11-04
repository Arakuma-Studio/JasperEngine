"""Regression tests of the round-5 attacks on the write sandbox (docs/mcp/DESIGN.md, Addressing):
re-pointing a trusted root by moving folders, moving a link into the editor's internal folder ahead of
a build, building into an arbitrary folder, and making a non-empty folder a project."""

import os
import shutil
import tempfile
from pathlib import Path

import yaml

from harness import Editor, skip, symlinks_permitted

EDITOR = {"create": True, "name": "Root Attacks"}
CI = True
TIMEOUT = 900


def require_links() -> None:
    if not symlinks_permitted():
        skip("this process may not create symbolic links")


async def project_dir(ed: Editor) -> Path:
    return Path((await ed.call("get_project_summary"))["path"])


async def test_a_trusted_root_cant_be_re_pointed_by_moves(ed: Editor):
    require_links()
    project = await project_dir(ed)
    (project / "assets").mkdir(exist_ok=True)
    (project / "scripts").mkdir(exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets", "scripting.lua_dir": "scripts"})
    summary = await ed.call("get_project_summary")
    ed.check_eq((summary.get("assets_dir"), summary.get("lua_dir")), ("assets", "scripts"), "the roots are set")
    outside = Path(tempfile.mkdtemp(prefix="jasper-attack-outside-"))
    (outside / "keep.txt").write_text("x")
    os.symlink(outside, project / "lib")
    for args, what in (({"path": "assets", "to": "assets_old"}, "the assets root moved away"),
                       ({"path": "lib", "to": "assets"}, "a link moved into the assets root's place"),
                       ({"path": "scripts", "to": "scripts_old"}, "the Lua root moved away"),
                       ({"path": "lib", "to": "scripts"}, "a link moved into the Lua root's place")):
        r = await ed.call_raw("move_asset", args)
        ed.check(r.is_error and "trusted root" in r.text, f"{what}: {r.text[:300]}")
    r = await ed.call_raw("delete_asset", {"paths": ["assets"]})
    ed.check(r.is_error and "trusted root" in r.text, f"the assets root deleted: {r.text[:300]}")
    # the roots are pinned: even swapped outside the tools, nothing goes through the new link
    os.rename(project / "assets", project / "assets_moved")
    os.symlink(outside, project / "assets")
    r = await ed.call_raw("write_file", {"path": "assets/pwn.txt", "content": "pwned\n"})
    ed.check(r.is_error, f"a write into a root swapped for a link: {r.text[:300]}")
    r = await ed.call_raw("create_folder", {"path": "assets/pwndir"})
    ed.check(r.is_error, f"a folder made in it: {r.text[:300]}")
    ed.check_eq(sorted(os.listdir(outside)), ["keep.txt"], "nothing written outside through a swapped root")
    os.remove(project / "assets")
    os.rename(project / "assets_moved", project / "assets")
    await ed.call("write_file", path="assets/fine.txt", content="ok\n")
    ed.check((project / "assets" / "fine.txt").is_file(), "the root back: writes work again")
    await ed.call("set_project_settings", values={"editor.assets_dir": ".", "scripting.lua_dir": "."})
    shutil.rmtree(outside, ignore_errors=True)
    os.remove(project / "lib")


async def test_a_link_cant_be_moved_into_the_internal_folder(ed: Editor):
    require_links()
    project = await project_dir(ed)
    victim = Path(tempfile.mkdtemp(prefix="jasper-attack-victim-", dir="/var/tmp" if os.path.isdir("/var/tmp") else None))
    (victim / "desktop" / "project").mkdir(parents=True)
    (victim / "desktop" / "project" / "sentinel.txt").write_text("keep me")
    os.symlink(victim, project / "lib")
    for to in (".jasperengine/export", ".hidden", "project.yaml"):
        r = await ed.call_raw("move_asset", {"path": "lib", "to": to})
        ed.check(r.is_error, f"move to {to}: {r.text[:300]}")
    r = await ed.call_raw("move_asset", {"path": "lib", "to": "library"})
    ed.check(r.is_error and "link" in r.text, f"moving a link that leads out of the project: {r.text[:300]}")
    ed.check(not os.path.islink(project / ".jasperengine" / "export"), "no link in the internal folder")
    # a link put there behind the tools' back: the build refuses before it starts
    await ed.call("save_project")
    export = project / ".jasperengine" / "export"
    if export.exists() and not export.is_symlink():
        shutil.rmtree(export)
    os.symlink(victim, export)
    r = await ed.call_raw("build_project", {"mode": "desktop", "wait": False})
    ed.check(r.is_error and "resolves outside" in r.text, f"a build with a link in the internal folder: {r.text[:300]}")
    ed.check((victim / "desktop" / "project" / "sentinel.txt").exists(), "nothing deleted through the link")
    os.remove(export)
    os.remove(project / "lib")
    shutil.rmtree(victim, ignore_errors=True)


async def test_builds_go_only_to_the_chosen_export_folders(ed: Editor):
    elsewhere = Path(tempfile.mkdtemp(prefix="jasper-attack-out-", dir="/var/tmp" if os.path.isdir("/var/tmp") else None))
    try:
        for tool, args in (("generate_shaders", {"output_dir": str(elsewhere / "shaders"), "backends": ["opengl"]}),
                           ("build_project", {"mode": "source", "output_dir": str(elsewhere / "src")})):
            r = await ed.call_raw(tool, args)
            ed.check(r.is_error and "Build Settings" in r.text, f"{tool} into an arbitrary folder: {r.text[:300]}")
        ed.check(not any(elsewhere.iterdir()), "nothing written there")
        settings = yaml.safe_load((ed.scratch.root / "settings.yaml").read_text()) or {}
        ed.check(str(elsewhere) not in yaml.safe_dump(settings), "an agent's folder is never saved as an export folder")
    finally:
        shutil.rmtree(elsewhere, ignore_errors=True)


async def test_create_project_needs_a_new_or_empty_folder(ed: Editor):
    full = ed.scratch.root / "not-empty"
    full.mkdir()
    (full / "notes.txt").write_text("mine")
    r = await ed.call_raw("create_project", {"path": str(full), "name": "Taken", "unsaved": "discard"})
    ed.check(r.is_error and "not empty" in r.text, f"a non-empty folder refused: {r.text[:300]}")
    ed.check(not (full / "project.yaml").exists(), "no project made there")
