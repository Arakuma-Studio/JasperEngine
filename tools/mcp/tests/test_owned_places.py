"""Regression tests of the round-7 attacks (docs/mcp/DESIGN.md, Addressing / Threat model). The
writers deny editor-owned and hidden places by default inside an MCP (strict) scope; only the
editor's own internal writers opt in. Probes: importing/writing over project.yaml or into
.jasperengine/.git/.trash (incl. via /proc/self/root), run_build of an arbitrary program, a project
created nested inside another, build settings slipped in through build.yaml, and crafted roots."""

import asyncio
import hashlib
import os
import shutil
import tempfile
from pathlib import Path

import yaml

from harness import Editor, make_native_player, project_player_name, record_built_player, skip, symlinks_permitted

EDITOR = {"create": True, "name": "Owned Places"}
CI = True
TIMEOUT = 900


def victim() -> Path:
    v = Path(tempfile.mkdtemp(prefix="jasper-r7-victim-", dir="/var/tmp" if os.path.isdir("/var/tmp") else None))
    (v / "keep.txt").write_text("keep")
    return v


def digest(p: Path) -> str | None:
    return hashlib.sha1(p.read_bytes()).hexdigest() if p.exists() else None


async def project_dir(ed: Editor) -> Path:
    return Path((await ed.call("get_project_summary"))["path"])


async def test_import_never_overwrites_or_creates_a_project_yaml(ed: Editor):
    project = await project_dir(ed)
    own = project / "project.yaml"
    before = digest(own)
    src = Path(tempfile.mkdtemp(prefix="jasper-r7-src-"))
    try:
        crafted = dict(yaml.safe_load(own.read_text()), assetsDir="/var/tmp", luaDir="/var/tmp")
        (src / "project.yaml").write_text(yaml.safe_dump(crafted))
        for overwrite in (False, True):
            r = await ed.call_raw("import_files", {"sources": [str(src / "project.yaml")], "to_dir": ".", "overwrite": overwrite})
            ed.check(r.is_error, f"import project.yaml (overwrite={overwrite}): {r.text[:200]}")
        ed.check_eq(digest(own), before, "the project's own project.yaml is untouched")
        # a crafted one placed in a subfolder, imported, is refused too
        (src / "nested").mkdir()
        (src / "nested" / "PROJECT.yaml").write_text(yaml.safe_dump(crafted))
        r = await ed.call_raw("import_files", {"sources": [str(src / "nested")], "to_dir": "assets"})
        ed.check(r.is_error or not any(p.name.lower() == "project.yaml" for p in (project / "assets").rglob("*")),
                 f"import a folder holding a project.yaml: {r.text[:200]}")
        ed.check(not list(project.glob("assets/**/[Pp][Rr][Oo][Jj][Ee][Cc][Tt].yaml")), "nothing named project.yaml imported")
    finally:
        shutil.rmtree(src, ignore_errors=True)


async def test_no_tool_writes_the_projects_own_project_yaml(ed: Editor):
    project = await project_dir(ed)
    own = project / "project.yaml"
    before = digest(own)
    for tool, args in (("write_file", {"path": "project.yaml", "content": "name: x\nassetsDir: /var/tmp\n"}),
                       ("screenshot", {"save_path": "project.yaml", "view": "scene"}),
                       ("create_folder", {"path": "project.yaml"}),
                       ("save_project", {"untitled_scene_dir": "project.yaml"})):
        r = await ed.call_raw(tool, args)
        ed.check(r.is_error, f"{tool} project.yaml: {r.text[:160]}")
        ed.check_eq(digest(own), before, f"{tool} left project.yaml untouched")
    # the editor's own save still writes it
    await ed.call("set_project_settings", values={"player.window_width": 1313})
    ed.check("1313" in own.read_text(), "set_project_settings still saves project.yaml")


async def test_imports_into_hidden_and_owned_places_are_refused(ed: Editor):
    project = await project_dir(ed)
    src = Path(tempfile.mkdtemp(prefix="jasper-r7-src2-"))
    try:
        (src / "note.txt").write_text("hi")
        (src / "CMakeLists.txt").write_text("message(FATAL_ERROR pwned)\n")
        cmake_before = digest(project / "CMakeLists.txt")
        for to in (".jasperengine", ".jasperengine/user", ".trash", ".git"):
            r = await ed.call_raw("import_files", {"sources": [str(src / "note.txt")], "to_dir": to})
            ed.check(r.is_error, f"import into {to}: {r.text[:160]}")
            ed.check(not (project / to / "note.txt").exists(), f"nothing written in {to}")
        r = await ed.call_raw("import_files", {"sources": [str(src / "CMakeLists.txt")], "to_dir": ".", "overwrite": True})
        ed.check(r.is_error, f"import over CMakeLists.txt: {r.text[:160]}")
        ed.check_eq(digest(project / "CMakeLists.txt"), cmake_before, "root CMakeLists.txt untouched")
    finally:
        shutil.rmtree(src, ignore_errors=True)


async def test_build_settings_cannot_be_slipped_in_through_build_yaml(ed: Editor):
    # build.yaml (per-project build settings / export dirs) lives in .jasperengine/user: no tool writes it
    project = await project_dir(ed)
    bf = project / ".jasperengine" / "user" / "build.yaml"
    before = digest(bf)
    content = "build: {c_compiler: /var/tmp/evil-cc, cxx_compiler: /var/tmp/evil-cxx}\nexports: {desktop: {targetDir: /var/tmp}}\n"
    for to in (".jasperengine/user", ".jasperengine"):
        await ed.call_raw("write_file", {"path": "stage/build.yaml", "content": content})
        r = await ed.call_raw("import_files", {"sources": [str(project / "stage" / "build.yaml")], "to_dir": to, "overwrite": True})
        ed.check(r.is_error, f"import build.yaml into {to}: {r.text[:160]}")
    r = await ed.call_raw("write_file", {"path": ".jasperengine/user/build.yaml", "content": content})
    ed.check(r.is_error, f"write build.yaml directly: {r.text[:160]}")
    ed.check_eq(digest(bf), before, "build.yaml unchanged")
    # the compilers are read-only over MCP regardless
    r = await ed.call_raw("set_project_settings", {"values": {"preferences.build_cxx_compiler": "/bin/false"}})
    ed.check(r.is_error and "Edit > Preferences" in r.text, f"compiler setting refused: {r.text[:160]}")


async def test_run_build_refuses_an_arbitrary_program(ed: Editor):
    v = victim()
    try:
        # a recorded build exists (the fixture stands in for build_project mode=desktop): it does not
        # make any other program runnable
        name = await project_player_name(ed)
        player = make_native_player(ed.scratch.root / "fake-players" / "built" / name, "int main(void){ return 0; }\n")
        record_built_player(ed.project, player)
        r = await ed.call_raw("run_build", {"path": "/bin/sh", "args": ["-c", f"echo pwn > {v}/run_build.txt"],
                                            "seconds": 1, "screenshot": False})
        ed.check(r.is_error and "this project built" in r.text, f"run_build /bin/sh: {r.text[:200]}")
        # a copy of the shell named as the player, beside the recorded one: refused too
        shell_copy = ed.scratch.root / "fake-players" / "shell" / name
        shell_copy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(os.path.realpath("/bin/sh"), shell_copy)
        r = await ed.call_raw("run_build", {"path": str(shell_copy), "args": ["-c", f"echo pwn > {v}/run_build.txt"],
                                            "seconds": 1, "screenshot": False})
        ed.check(r.is_error and "not built by this editor" in r.text, f"run_build a copy of /bin/sh: {r.text[:200]}")
        # the agent can't record one itself: the record is the editor's own file
        entry = yaml.safe_load((ed.project / ".jasperengine" / "user" / "runbuild.yaml").read_text())
        forged = {"desktop": dict(entry["desktop"], resolved=str(shell_copy.resolve()))}
        r = await ed.call_raw("write_file", {"path": ".jasperengine/user/runbuild.yaml", "content": yaml.safe_dump(forged)})
        ed.check(r.is_error, f"write runbuild.yaml over MCP: {r.text[:200]}")
        await asyncio.sleep(1)
        ed.check(not (v / "run_build.txt").exists(), "nothing ran")
    finally:
        shutil.rmtree(v, ignore_errors=True)


async def test_create_project_not_inside_the_open_project(ed: Editor):
    project = await project_dir(ed)
    for rel in ("assets/subproj", "sub/deeper"):
        sub = project / rel
        r = await ed.call_raw("create_project", {"path": str(sub), "unsaved": "discard"})
        ed.check(r.is_error and "nested" in r.text, f"create_project {rel}: {r.text[:200]}")
        ed.check_eq((await ed.call("get_project_summary"))["path"], str(project), "the project stays open")
        ed.check(not (sub / "project.yaml").exists(), "no project made inside")
    # a folder far from the current project (not beside it, not under the projects folder) is refused
    far = Path(tempfile.mkdtemp(prefix="jasper-r7-far-"))
    try:
        r = await ed.call_raw("create_project", {"path": str(far / "proj"), "unsaved": "discard"})
        ed.check(r.is_error and "editor" in r.text, f"create_project far away: {r.text[:200]}")
    finally:
        shutil.rmtree(far, ignore_errors=True)


async def test_folder_settings_stay_inside_the_project(ed: Editor):
    v = victim()
    try:
        for value in ([str(v)], ["../.."], ["x/../../y"]):
            r = await ed.call_raw("set_project_settings", {"values": {"scripting.script_dirs": value}})
            ed.check(r.is_error, f"script_dirs {value}: {r.text[:160]}")
        for d in (str(v), "../..", "x/../../y"):
            r = await ed.call_raw("save_project", {"untitled_scene_dir": d})
            ed.check(r.is_error, f"save_project untitled_scene_dir {d!r}: {r.text[:160]}")
        ed.check_eq(sorted(p.name for p in v.iterdir()), ["keep.txt"], "nothing written into the victim")
    finally:
        shutil.rmtree(v, ignore_errors=True)


async def test_proc_self_root_does_not_reach_hidden_places(ed: Editor):
    if not os.path.isdir("/proc/self/root"):
        skip("no /proc/self/root on this platform")
    project = await project_dir(ed)
    await ed.call("write_file", path="stage/note.txt", content="[core]\n\tx = 1\n")
    via = "/proc/self/root" + str(project / "stage" / "note.txt")
    for to in (".git", ".jasperengine/user"):
        r = await ed.call_raw("import_files", {"sources": [via], "to_dir": to, "overwrite": True})
        ed.check(r.is_error, f"import via /proc/self/root into {to}: {r.text[:200]}")
        ed.check(not (project / to / "note.txt").exists(), f"nothing written in {to}")
    # the same resolved path is recognised as already in the project
    r = await ed.call_raw("import_files", {"sources": [via], "to_dir": "assets"})
    ed.check(r.is_error and "already in the project" in r.text, f"/proc self-import: {r.text[:200]}")


async def test_a_crafted_project_copied_out_is_not_opened(ed: Editor):
    # a project hand-built with roots pointing at a victim (as an attacker might leave on disk) is
    # refused by open_project; the GUI would open it (the user's choice)
    project = await project_dir(ed)
    v = victim()
    crafted = Path(tempfile.mkdtemp(prefix="jasper-r7-crafted-")) / "proj"
    try:
        crafted.mkdir(parents=True)
        data = dict(yaml.safe_load((project / "project.yaml").read_text()), assetsDir=str(v), luaDir=str(v))
        (crafted / "project.yaml").write_text(yaml.safe_dump(data))
        r = await ed.call_raw("open_project", {"path": str(crafted), "unsaved": "discard"})
        ed.check(r.is_error and "outside the project folder" in r.text, f"open crafted: {r.text[:200]}")
        ed.check_eq((await ed.call("get_project_summary"))["path"], str(project), "the project stays open")
    finally:
        shutil.rmtree(crafted.parent, ignore_errors=True)
        shutil.rmtree(v, ignore_errors=True)
