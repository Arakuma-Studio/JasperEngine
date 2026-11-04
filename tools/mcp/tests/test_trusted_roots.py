"""The trusted roots of the write sandbox (docs/mcp/DESIGN.md, Addressing): the project directory and
its configured assets directory (project.yaml assetsDir, as loaded) are trusted, each resolved once,
so a project whose assets folder is a symbolic link to a shared drive keeps working for MCP tools;
links BELOW those roots are what gets checked; an MCP tool can't make a folder outside the project
an asset root."""

import os
import re
import shutil
import tempfile
from pathlib import Path

from harness import Editor, skip, symlinks_permitted
import pngdiff

EDITOR = {"create": True, "name": "Trusted Roots"}
CI_EXCLUDED = ("long-running (about 3 minutes on a loaded server: projects created and opened through MCP); "
               "in the full suite (MCP_SUITE=all) on the build servers")

S: dict = {}


def disc(width: int = 32, height: int = 32) -> bytes:
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            inside = (x - width / 2 + 0.5) ** 2 + (y - height / 2 + 0.5) ** 2 <= (width / 2 - 3) ** 2
            pixels += bytes([200, 120, 60, 255]) if inside else bytes([0, 0, 0, 0])
    return pngdiff.encode(width, height, bytes(pixels))


async def test_a_project_with_a_linked_assets_folder_works(ed: Editor):
    if not symlinks_permitted():
        skip("this process may not create symbolic links")
    # beside the current project: create_project over MCP makes a project there, not in an unrelated folder
    base = Path(tempfile.mkdtemp(prefix="jasper-trusted-", dir=ed.scratch.root))
    S["base"] = base
    summary = await ed.call("get_project_summary")
    S["original"] = summary["path"]
    elsewhere = base / "elsewhere" / "assets"
    (elsewhere / "sprites").mkdir(parents=True)
    (elsewhere / "sprites" / "disc.png").write_bytes(disc())
    project = base / "Linked"
    await ed.call("create_project", path=str(project), name="Linked", unsaved="discard")
    await ed.call("save_project")
    # configured while it is closed (open_project reads project.yaml again)
    await ed.call("open_project", path=str(S["original"]), unsaved="discard")
    # the user's configuration: assets/ is a link to a shared drive, project.yaml names it
    if os.path.islink(project / "assets") or (project / "assets").is_file():
        os.remove(project / "assets")
    elif (project / "assets").exists():
        shutil.rmtree(project / "assets")
    os.symlink(elsewhere, project / "assets")
    # ... and its Lua scripts folder a link too
    scripts = base / "elsewhere" / "scripts"
    scripts.mkdir(parents=True)
    if os.path.islink(project / "scripts") or (project / "scripts").is_file():
        os.remove(project / "scripts")
    elif (project / "scripts").exists():
        shutil.rmtree(project / "scripts")
    os.symlink(scripts, project / "scripts")
    S["scripts"] = scripts
    yaml = (project / "project.yaml").read_text(encoding="utf-8")
    for key, value in (("assetsDir", "assets"), ("luaDir", "scripts")):
        if re.search(rf"^{key}:.*$", yaml, flags=re.M):
            yaml = re.sub(rf"^{key}:.*$", f"{key}: {value}", yaml, flags=re.M)
        else:
            yaml = yaml.rstrip("\n") + f"\n{key}: {value}\n"
    (project / "project.yaml").write_text(yaml, encoding="utf-8")
    S["project"] = project
    S["elsewhere"] = elsewhere
    await ed.call("open_project", path=str(project), unsaved="discard")
    summary = await ed.call("get_project_summary")
    ed.check_eq(summary.get("assets_dir"), "assets", "the project's assets folder is the link")
    # (a) MCP write_file and generate_sprite_normals into the assets folder land under /elsewhere/assets
    await ed.call("write_file", path="assets/notes/hello.txt", content="hello\n")
    ed.check_eq((elsewhere / "notes" / "hello.txt").read_text(encoding="utf-8"), "hello\n", "written under the linked assets folder")
    await ed.call("create_folder", path="assets/made")
    ed.check((elsewhere / "made").is_dir(), "a folder made there")
    await ed.call("move_asset", path="assets/notes/hello.txt", to="assets/made/hello.txt")
    ed.check((elsewhere / "made" / "hello.txt").is_file(), "moved there")
    await ed.call("undo", scope="project")
    ed.check((elsewhere / "notes" / "hello.txt").is_file(), "the move undone")
    r = await ed.call_raw("generate_sprite_normals", {"source": "assets/sprites/disc.png", "output": True}, timeout=600)
    ed.check(not r.is_error, f"sprite normals into the linked assets folder: {r.text[:400]}")
    ed.check((elsewhere / "sprites" / "disc_normal.png").is_file(), "the normal map lands under /elsewhere/assets")
    await ed.call("undo", scope="project")
    ed.check(not (elsewhere / "sprites" / "disc_normal.png").exists(), "and its undo removes it there")


async def test_a_link_below_the_linked_assets_folder_is_checked(ed: Editor):
    if "elsewhere" not in S:
        skip("needs the linked project")
    further = S["base"] / "further"
    further.mkdir()
    os.symlink(further, S["elsewhere"] / "out")
    r = await ed.call_raw("write_file", {"path": "assets/out/x.txt", "content": "x\n"})
    ed.check(r.is_error and "resolves outside the project" in r.text, f"(b) a link below the assets folder leading further out: {r.text[:400]}")
    r = await ed.call_raw("generate_sprite_normals", {"source": "assets/sprites/disc.png", "output": "assets/out/disc_n.png"}, timeout=600)
    ed.check(r.is_error, f"(b) sprite normals through it refused: {r.text[:400]}")
    ed.check(not any(further.iterdir()), "nothing written further out")


async def test_a_linked_lua_scripts_folder_is_trusted(ed: Editor):
    if "scripts" not in S:
        skip("needs the linked project")
    scripts = S["scripts"]
    made = await ed.call("create_script", kind="lua", name="TrustedProbe", content="local TrustedProbe = {}\nreturn TrustedProbe\n")
    ed.check(made["path"].startswith("scripts/"), f"made in the Lua scripts folder: {made}")
    ed.check((scripts / Path(made["path"]).relative_to("scripts")).is_file(), "it lands under /elsewhere/scripts")
    await ed.call("write_file", path="scripts/notes.lua", content="return {}\n")
    ed.check_eq((scripts / "notes.lua").read_text(encoding="utf-8"), "return {}\n", "write_file into the linked Lua folder")
    # a link below it leading further out: refused
    further = S["base"] / "further-scripts"
    further.mkdir()
    os.symlink(further, scripts / "out")
    r = await ed.call_raw("write_file", {"path": "scripts/out/x.lua", "content": "return {}\n"})
    ed.check(r.is_error and "resolves outside the project" in r.text, f"a link below the Lua folder leading further out: {r.text[:400]}")
    r = await ed.call_raw("create_script", {"kind": "lua", "name": "Escaper", "directory": "scripts/out", "content": "return {}\n"})
    ed.check(r.is_error, f"create_script through it refused: {r.text[:400]}")
    ed.check(not any(further.iterdir()), "nothing written further out")


async def test_an_asset_root_outside_the_project_is_refused(ed: Editor):
    if "project" not in S:
        skip("needs the linked project")
    project = S["project"]
    away = S["base"] / "away"
    away.mkdir()
    os.symlink(away, project / "linkout")
    for key in ("editor.assets_dir", "scripting.lua_dir"):
        for value in ("linkout", "../away", str(away)):
            r = await ed.call_raw("set_project_settings", {"values": {key: value}})
            ed.check(r.is_error, f"(c) {key} {value!r} outside the project is refused: {r.text[:400]}")
    summary = await ed.call("get_project_summary")
    ed.check_eq(summary.get("assets_dir"), "assets", "the assets folder is unchanged")
    # back to the original project
    await ed.call("open_project", path=str(S["original"]), unsaved="discard")
    shutil.rmtree(S["base"], ignore_errors=True)
