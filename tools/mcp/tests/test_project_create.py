"""A new project's layout (S98 REQ-36, AC32): its assets and its Lua scripts live in folders of their
own (assetsDir: assets, luaDir: scripts), and a project.yaml without the keys still means ".".

The harness keeps created projects flat for the other tests (JASPER_TEST_FLAT_NEW_PROJECT=1,
docs/testing.md); this file creates one as a user gets it."""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "New Layout", "env": {"JASPER_TEST_FLAT_NEW_PROJECT": "0"}}


async def roots(ed: Editor) -> tuple[str, str]:
    got = (await ed.call("get_project_settings", keys=["editor.assets_dir", "scripting.lua_dir"]))["settings"]
    return got["editor.assets_dir"], got["scripting.lua_dir"]


async def test_new_project_layout(ed: Editor):
    # (the first call waits for the project the editor creates)
    ed.check_eq(await roots(ed), ("assets", "scripts"), "the project uses them")
    text = (ed.project / "project.yaml").read_text()
    ed.check("assetsDir: assets" in text and "luaDir: scripts" in text, f"project.yaml names both folders:\n{text}")
    ed.check((ed.project / "assets").is_dir() and (ed.project / "scripts").is_dir(), "both folders exist")
    ed.check((ed.project / "assets" / "audio" / "Main.mixer").is_file(), "the new project's mixer is an asset under assets/")

    # an FBX under assets imports into the cache by its path under assets
    models = ed.project / "assets" / "models"
    models.mkdir(parents=True)
    shutil.copy(harness.REPO / "editor" / "tests" / "fixtures" / "rigged_cube.fbx", models / "cube.fbx")
    await ed.call("sync_files", paths=["assets/models/cube.fbx"])
    await ed.call("reimport_models", paths=["assets/models/cube.fbx"], timeout=600)
    imported = ed.project / ".jasperengine" / "imported" / "models" / "cube.fbx.glb"
    ed.check(imported.is_file(), f"the import lands at {imported.relative_to(ed.project)}: "
             f"{sorted(str(p.relative_to(ed.project)) for p in (ed.project / '.jasperengine').rglob('*.glb'))}")

    # a script made through MCP goes to the Lua folder
    await ed.call("create_script", kind="lua", name="Mover", directory="scripts")
    ed.check((ed.project / "scripts" / "Mover.lua").is_file(), "create_script writes under scripts/")


async def test_old_project_without_keys_stays_flat(ed: Editor):
    root = Path(tempfile.mkdtemp(prefix="jasper-old-layout-"))
    try:
        project = root / "project"
        shutil.copytree(harness.REPO / "tools" / "fixtures" / "tween-curve", project)
        yaml = project / "project.yaml"
        yaml.write_text("".join(line for line in yaml.read_text().splitlines(True)
                                if not line.startswith(("assetsDir:", "luaDir:"))))
        before = yaml.read_text()
        async with Editor(project=project, label="old-layout") as old:
            ed.check_eq(await roots(old), (".", "."), "a project.yaml without the keys keeps the project root")
            ed.check(old.project and (old.project / "TweenCurveCheck.lua").is_file(), "its files are where they were")
            ed.check_eq((old.project / "project.yaml").read_text(), before, "opening it writes no new key")
    finally:
        shutil.rmtree(root, ignore_errors=True)
