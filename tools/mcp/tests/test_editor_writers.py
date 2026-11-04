"""Round-8 regressions (docs/mcp/DESIGN.md, Addressing). The editor's own writers keep working
under the deny-by-default guard (no guard warning in the console from saving, prefab sources,
compiling twice, Play, a settings save), and the remaining agent-reachable gaps stay closed:
where create_project may put a project, what run_build runs, asset roots in hidden folders, and
deletes below a hidden folder."""

import os
import shutil
import tempfile
from pathlib import Path

from harness import Editor, make_native_player, project_player_name, record_built_player, skip

EDITOR = {"create": True, "name": "Editor Writers"}
CI_EXCLUDED = ("long-running (about 3 minutes on a loaded server: saving, compiling twice and Play under the write guard); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = 1500

GUARD = ("hidden folder", "named project.yaml", "regenerated on every build")


def guard_lines(entries) -> list[str]:
    return [e.get("message", "") for e in entries if any(g in e.get("message", "") for g in GUARD)]


async def project_dir(ed: Editor) -> Path:
    return Path((await ed.call("get_project_summary"))["path"])


async def test_the_editors_own_writers_raise_no_guard_warning(ed: Editor):
    if not shutil.which("cc") and not shutil.which("c++"):
        skip("no C/C++ compiler for compile_scripts")
    project = await project_dir(ed)
    await ed.console_since(None)
    sid = (await ed.call("create_scene", name="Main", type="3D", path="scenes/Main"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="box", name="Root")
    await ed.call("create_prefab", scene=sid, entity="Root", path="prefabs/Thing.prefab")
    await ed.call("instantiate_prefab", scene=sid, prefab="prefabs/Thing.prefab", name="Other", position=[3, 0, 0])
    await ed.call("create_script", name="Mover", kind="cpp_script", scene=sid, entity="Root")
    await ed.call("save_scene", scene=sid)
    ed.check_eq(guard_lines(await ed.console_since()), [], "save_scene (prefab sources) raises no guard warning")
    gen = project / ".jasperengine" / "generated"
    ed.check(any(p.name.startswith("prefab_") for p in gen.iterdir()), f"prefab sources written: {sorted(p.name for p in gen.iterdir())}")

    for i in range(2):
        r = await ed.call_raw("compile_scripts", {}, timeout=900)
        ed.check(not r.is_error, f"compile #{i + 1}: {r.text[:300]}")
        msgs = await ed.console_since()
        ed.check_eq(guard_lines(msgs), [], f"compile #{i + 1} raises no guard warning")
        if i == 1:
            cleaned = [m.get("message", "") for m in msgs if "Cleaning build directory" in m.get("message", "")]
            ed.check_eq(cleaned, [], "the second compile reuses the build directory")
    ed.check((project / ".jasperengine" / "build" / ".jasperengine_kit").exists(), "the kit marker is written")

    for _ in range(2):
        r = await ed.call_raw("play", {"action": "start"}, timeout=900)
        ed.check(not r.is_error, f"play: {r.text[:300]}")
        await ed.call_raw("play", {"action": "stop"})
        msgs = await ed.console_since()
        ed.check_eq(guard_lines(msgs), [], "Play raises no guard warning")
        ed.check(not any("Could not stage" in m.get("message", "") for m in msgs), "hot-reload staging works")

    await ed.call("set_project_settings", values={"preferences.build_jobs": 3})
    build_yaml = project / ".jasperengine" / "user" / "build.yaml"
    ed.check(build_yaml.exists() and "3" in build_yaml.read_text(), "build_jobs saved to build.yaml (by the editor)")
    await ed.call("save_project")
    ed.check_eq(guard_lines(await ed.console_since()), [], "settings and project saves raise no guard warning")


async def test_create_project_only_beside_or_under_the_export_folder(ed: Editor):
    # the default export folder is the scratch dir (harness), so a sibling of the project and a
    # folder under the export folder are allowed; a hidden ('.'-named) component, a link, and a
    # folder neither beside the project nor under the export folder are refused
    project = await project_dir(ed)
    parent = project.parent
    refused = [(parent / ".hiddenproj", "a hidden folder beside the project"),
               (parent / "deep" / ".a" / "b", "a hidden component below the export folder")]
    far = Path(tempfile.mkdtemp(prefix="jasper-r8-far-"))
    real = Path(tempfile.mkdtemp(prefix="jasper-r8-real-"))
    link = parent / "linked-new"
    try:
        refused.append((far / "proj", "a folder neither beside the project nor under the export folder"))
        os.symlink(real, link)
        refused.append((link, "a symbolic link"))
        for target, why in refused:
            r = await ed.call_raw("create_project", {"path": str(target), "name": "No", "unsaved": "discard"})
            ed.check(r.is_error, f"create_project {why}: {r.text[:240]}")
            ed.check(not (target / "project.yaml").exists(), f"no project made ({why})")
        ed.check_eq((await ed.call("get_project_summary"))["path"], str(project), "the project stays open")
        # a sibling is allowed (same parent folder)
        sibling = parent / "Sibling"
        r = await ed.call_raw("create_project", {"path": str(sibling), "name": "Sibling", "unsaved": "discard"})
        ed.check(not r.is_error, f"a sibling project is allowed: {r.text[:240]}")
        await ed.call("open_project", path=str(project), unsaved="discard")
    finally:
        if link.is_symlink():
            link.unlink()
        shutil.rmtree(real, ignore_errors=True)
        shutil.rmtree(far, ignore_errors=True)


async def test_run_build_runs_only_this_projects_player(ed: Editor):
    project = await project_dir(ed)
    marker = Path(tempfile.mkdtemp(prefix="jasper-r8-marker-")) / "ran"
    other_project = Path(tempfile.mkdtemp(prefix="jasper-r9-other-project-"))
    try:
        name = await project_player_name(ed)
        body = 'int main(void){ FILE* f = fopen("' + str(marker) + '", "w"); if (f) fclose(f); return 0; }\n'
        # this project's recorded build runs
        player = make_native_player(ed.scratch.root / "fake-players" / "mine" / name, body)
        record_built_player(project, player)   # stands in for build_project mode=desktop
        r = await ed.call_raw("run_build", {"path": str(player), "seconds": 1, "screenshot": False})
        ed.check(not r.is_error, f"this project's recorded player runs: {r.text[:240]}")
        ed.check(marker.exists(), "the recorded player ran")
        marker.unlink()
        # a player another project recorded is not this project's: refused
        theirs = make_native_player(ed.scratch.root / "fake-players" / "theirs" / name, body)
        (other_project / "project.yaml").write_text("name: Other\n")
        record_built_player(other_project, theirs)
        # a user script in the project, and a system program: never built here, refused
        tool = project / "tools" / "deploy"
        tool.parent.mkdir(parents=True, exist_ok=True)
        tool.write_text(f"#!/bin/sh\necho ran > {marker}\n")
        tool.chmod(0o755)
        for path in (str(theirs), str(tool), "/bin/sh"):
            r = await ed.call_raw("run_build", {"path": path, "seconds": 1, "screenshot": False})
            ed.check(r.is_error and "this project built" in r.text, f"run_build {path}: {r.text[:240]}")
        ed.check(not marker.exists(), "nothing ran")
    finally:
        shutil.rmtree(marker.parent, ignore_errors=True)
        shutil.rmtree(other_project, ignore_errors=True)


async def test_asset_roots_are_not_hidden_folders(ed: Editor):
    for key, value in (("editor.assets_dir", ".jasperengine/user"), ("scripting.lua_dir", ".git/hooks"),
                       ("editor.assets_dir", ".trash"), ("editor.assets_dir", "art/.cache")):
        r = await ed.call_raw("set_project_settings", {"values": {key: value}})
        ed.check(r.is_error and "hidden folder" in r.text, f"{key} = {value}: {r.text[:240]}")
    summary = await ed.call("get_project_summary")
    ed.check(not str(summary.get("assets_dir", "")).startswith(".") or summary.get("assets_dir") == ".", f"assets_dir unchanged: {summary}")


async def test_deletes_below_a_hidden_folder_are_refused(ed: Editor):
    project = await project_dir(ed)
    (project / "assets" / "lib" / ".git").mkdir(parents=True, exist_ok=True)
    (project / "assets" / "lib" / ".git" / "config").write_text("[core]\n")
    for path in ("assets/lib/.git", "assets/lib/.git/config"):
        r = await ed.call_raw("delete_asset", {"paths": [path]})
        ed.check(r.is_error, f"delete_asset {path}: {r.text[:240]}")
    ed.check((project / "assets" / "lib" / ".git" / "config").exists(), "nothing deleted")
