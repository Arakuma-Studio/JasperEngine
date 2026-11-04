"""Round-9 regressions (docs/mcp/DESIGN.md, Addressing / Threat model). External processes are
launched from an argv array (no shell), so a project path or a build setting with shell
metacharacters cannot start a second command; the CMake generator is an allow-listed name; run_build
runs only the executable the editor built, unchanged; a strict-scope write never grants execute
bits; and create_project keeps away from the home folder."""

import os
import shutil
import stat
import tempfile
from pathlib import Path

from harness import Editor, skip

EDITOR = {"create": True, "name": "Shell And Exec"}
CI = True
TIMEOUT = 1200


async def project_dir(ed: Editor) -> Path:
    return Path((await ed.call("get_project_summary"))["path"])


def has_cc() -> bool:
    return bool(shutil.which("cc") or shutil.which("c++") or shutil.which("g++"))


async def prepare_cpp_scene(ed: Editor):
    sid = (await ed.call("create_scene", name="Main", type="3D", path="scenes/Main"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="box", name="Root")
    await ed.call("create_script", name="Mover", kind="cpp_script", scene=sid, entity="Root")
    await ed.call("save_scene", scene=sid)
    return sid


async def test_cmake_project_path_is_not_a_shell_string(ed: Editor):
    if not has_cc():
        skip("no C++ compiler for compile_scripts")
    # the current project is created by the harness; make a sibling whose folder name is a shell
    # injection, open it, and compile — the path reaches CMake as one argument, runs no command
    project = await project_dir(ed)
    mark = project.parent / "r9_path_mark"
    mark.unlink(missing_ok=True)
    target = project.parent / 'p$(touch "r9_path_mark")x`touch r9_path_mark2`'
    r = await ed.call_raw("create_project", {"path": str(target), "name": "Inj", "unsaved": "discard"}, timeout=300)
    ed.check(not r.is_error, f"create_project with a metacharacter name: {r.text[:200]}")
    await prepare_cpp_scene(ed)
    r = await ed.call_raw("compile_scripts", {}, timeout=900)
    ed.check(not r.is_error, f"compile_scripts: {r.text[:300]}")
    ed.check(not mark.exists() and not (project.parent / "r9_path_mark2").exists(), "no command was run from the path")
    await ed.call("open_project", path=str(project), unsaved="discard")


async def test_cmake_generator_must_be_a_known_name(ed: Editor):
    mark = Path("/tmp/r9_gen_mark")
    mark.unlink(missing_ok=True)
    for bad in ('Unix Makefiles" -S /nonexistent ; touch /tmp/r9_gen_mark ; echo "',
                "Ninja; touch /tmp/r9_gen_mark", "Totally Not A Generator"):
        r = await ed.call_raw("set_project_settings", {"values": {"preferences.build_generator": bad}})
        ed.check(r.is_error and "generator" in r.text, f"generator {bad!r}: {r.text[:200]}")
    # a real generator name is accepted
    r = await ed.call_raw("set_project_settings", {"values": {"preferences.build_generator": "Ninja"}})
    ed.check(not r.is_error, f"a known generator is accepted: {r.text[:200]}")
    await ed.call("set_project_settings", values={"preferences.build_generator": ""})
    if has_cc():
        await prepare_cpp_scene(ed)
        r = await ed.call_raw("compile_scripts", {}, timeout=900)
        ed.check(not mark.exists(), "no command ran from a generator value")


async def test_run_build_runs_only_the_recorded_build(ed: Editor):
    # without a recorded build, run_build refuses every path (no name-in-folder fallback)
    project = await project_dir(ed)
    marker = Path(tempfile.mkdtemp(prefix="jasper-r9-mark-")) / "ran"
    try:
        # a native tool the user has under the export folder, named as the project's player
        import subprocess
        name = project.parent / "proj"  # scratch-root sibling; where a build would go
        tool_dir = project.parent / "usertools"
        tool_dir.mkdir(exist_ok=True)
        tool = tool_dir / "Shell_And_Exec"
        if shutil.which("cc"):
            src = tool_dir / "t.c"
            src.write_text(f'#include <stdlib.h>\nint main(void){{ system("touch {marker}"); return 0; }}\n')
            subprocess.run(["cc", "-o", str(tool), str(src)], check=True)
            src.unlink()
            r = await ed.call_raw("run_build", {"path": str(tool), "seconds": 1, "screenshot": False})
            ed.check(r.is_error and "built" in r.text, f"run_build a native tool not built here: {r.text[:240]}")
        # a symlink to any binary is refused
        link = project.parent / "player-link"
        if link.is_symlink():
            link.unlink()
        os.symlink("/bin/echo", link)
        r = await ed.call_raw("run_build", {"path": str(link), "seconds": 1, "screenshot": False})
        ed.check(r.is_error, f"run_build a symlink: {r.text[:240]}")
        link.unlink()
        # no path, nothing built: refused, nothing ran
        r = await ed.call_raw("run_build", {"seconds": 1, "screenshot": False})
        ed.check(r.is_error, f"run_build with nothing built: {r.text[:240]}")
        ed.check(not marker.exists(), "nothing ran")
        del name
    finally:
        shutil.rmtree(marker.parent, ignore_errors=True)
        shutil.rmtree(tool_dir, ignore_errors=True)


async def test_a_strict_write_never_grants_execute(ed: Editor):
    # the r8 exec-bit chain: a user script (0755) renamed, its bytes replaced with a real binary by
    # import overwrite (the replacing write keeps the file's mode), then renamed to the player name
    project = await project_dir(ed)
    await ed.call("create_folder", path="bin")
    (project / "scripts").mkdir(exist_ok=True)
    # a user's executable script in the repo (0755)
    f = project / "scripts" / "run.lua"
    f.write_text("return {}\n")
    f.chmod(0o755)
    await ed.call("sync_files")
    # a replacing write (same file) keeps the file's permissions — but a strict scope drops exec
    await ed.call("write_file", path="scripts/run.lua", content="return { x = 1 }\n")
    ed.check_eq(f.stat().st_mode & 0o111, 0, f"a replaced file loses its execute bits: {oct(f.stat().st_mode)}")
    # importing a real binary never produces an executable file
    r = await ed.call_raw("import_files", {"sources": ["/usr/bin/env"], "to_dir": "bin", "overwrite": False})
    ed.check(not r.is_error, f"import env: {r.text[:200]}")
    imported = project / "bin" / "env"
    ed.check(imported.exists(), "env imported")
    ed.check_eq(imported.stat().st_mode & 0o111, 0, f"imported file has no execute bits: {oct(imported.stat().st_mode)}")
    # even overwriting a pre-existing 0755 file by import: the replace drops the execute bits
    (project / "bin" / "seed").write_bytes(b"old")
    (project / "bin" / "seed").chmod(0o755)
    await ed.call("sync_files")
    env_bytes = Path("/usr/bin/env").read_bytes()
    src = Path(tempfile.mkdtemp(prefix="jasper-r9-src-"))
    try:
        (src / "seed").write_bytes(env_bytes)
        r = await ed.call_raw("import_files", {"sources": [str(src / "seed")], "to_dir": "bin", "overwrite": True})
        ed.check(not r.is_error, f"import overwrite: {r.text[:200]}")
        seed = project / "bin" / "seed"
        ed.check_eq(seed.stat().st_mode & 0o111, 0, f"overwritten file loses its execute bits: {oct(seed.stat().st_mode)}")
    finally:
        shutil.rmtree(src, ignore_errors=True)


async def test_create_project_keeps_away_from_home(ed: Editor):
    # the harness home is <scratch>/home, below the export folder (<scratch>): a project in the home
    # folder or directly inside it is refused (siblings there only through the GUI)
    home = Path(os.environ.get("HOME", ""))
    if not home or not home.is_dir():
        skip("no HOME")
    project = await project_dir(ed)
    for target in (home, home / "bin", home / "Desktop", home / "proj"):
        r = await ed.call_raw("create_project", {"path": str(target), "name": "H", "unsaved": "discard"}, timeout=300)
        ed.check(r.is_error, f"create_project {target}: {r.text[:240]}")
        ed.check_eq((await ed.call("get_project_summary"))["path"], str(project), "the project stays open")
    # deeper targets that would CREATE a folder directly in the home folder (<home>/bin on the way to
    # <home>/bin/z), also spelled through a link to the home folder: refused, nothing made. The
    # editor's home (the harness's <scratch>/home, below the export folder, so only the home rule
    # can refuse these)
    home = ed.scratch.root / "home"
    if not home.is_dir():
        skip("no harness home folder")
    link = project.parent / "homelink-r10"
    if link.is_symlink():
        link.unlink()
    os.symlink(home, link)
    try:
        before = sorted(p.name for p in home.iterdir())
        for target in (home / "bin" / "z", home / "Documents" / "deep" / "er", link / "bin" / "z"):
            r = await ed.call_raw("create_project", {"path": str(target), "name": "H", "unsaved": "discard"}, timeout=300)
            ed.check(r.is_error and "home folder" in r.text, f"create_project {target}: {r.text[:240]}")
            ed.check_eq((await ed.call("get_project_summary"))["path"], str(project), "the project stays open")
        ed.check_eq(sorted(p.name for p in home.iterdir()), before, "no folder made in the home folder")
    finally:
        link.unlink()
