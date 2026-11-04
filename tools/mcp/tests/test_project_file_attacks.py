"""Regression tests of the round-6 attacks on the write sandbox (docs/mcp/DESIGN.md, Addressing and
Threat model): an agent-set export folder trusted for builds, a crafted project.yaml (nested, or in
another letter case) opened to make an outside folder a trusted root, a folder holding a link out
moved around, links deep in the internal folder, and a user link into .jasperengine."""

import asyncio
import os
import shutil
import tempfile
from pathlib import Path

import yaml

from harness import Editor, skip, symlinks_permitted

EDITOR = {"create": True, "name": "Project File Attacks"}
CI = True
TIMEOUT = 900


def require_links() -> None:
    if not symlinks_permitted():
        skip("this process may not create symbolic links")


def victim() -> Path:
    # outside the project and outside the temporary folder (which the editor may write)
    v = Path(tempfile.mkdtemp(prefix="jasper-r6-victim-", dir="/var/tmp" if os.path.isdir("/var/tmp") else None))
    (v / "keep.txt").write_text("keep")
    return v


def listing(v: Path) -> list[str]:
    return sorted(str(p.relative_to(v)) for p in v.rglob("*"))


def project_files(project: Path) -> list[str]:
    # every file named project.yaml as Windows or a case-insensitive file system sees it
    return sorted(str(p.relative_to(project)) for p in project.rglob("*")
                  if ".jasperengine" not in p.parts and p.name.split(":")[0].rstrip(". ").lower() == "project.yaml")


async def project_dir(ed: Editor) -> Path:
    return Path((await ed.call("get_project_summary"))["path"])


async def test_machine_preferences_are_user_only(ed: Editor):
    v = victim()
    try:
        for key, value in (("preferences.default_export_dir", str(v)), ("preferences.cmake_path", str(v / "cmake")),
                           ("preferences.ffmpeg_path", str(v / "ffmpeg")), ("preferences.emsdk_path", str(v)),
                           ("preferences.build_c_compiler", str(v / "cc")), ("preferences.build_cxx_compiler", str(v / "c++"))):
            r = await ed.call_raw("set_project_settings", {"values": {key: value}})
            ed.check(r.is_error and "Edit > Preferences" in r.text, f"{key} set over MCP: {r.text[:300]}")
        # still readable, and described as read-only
        got = await ed.call("get_project_settings", keys=["preferences.default_export_dir", "preferences.cmake_path"], describe=True)
        ed.check("preferences.default_export_dir" in got["settings"], f"readable: {got}")
        ed.check(got["describe"]["preferences.cmake_path"].get("read_only") is True, f"described read-only: {got['describe']}")
        ed.check(str(v) not in str(got["settings"]), "nothing an agent chose was saved")
        # a project-level preference that runs nothing stays settable
        await ed.call("set_project_settings", values={"preferences.build_jobs": 2})
        # with no agent-set default folder, a build into the victim is refused and writes nothing
        for tool, args in (("generate_shaders", {"output_dir": str(v / "shaders"), "backends": ["opengl"]}),
                           ("generate_shaders", {"output_dir": str(v), "backends": ["opengl"]}),
                           ("build_project", {"mode": "source", "output_dir": str(v), "overwrite": True})):
            r = await ed.call_raw(tool, args)
            ed.check(r.is_error, f"{tool} into {args['output_dir']}: {r.text[:300]}")
        await asyncio.sleep(2)
        ed.check_eq(listing(v), ["keep.txt"], "nothing written into the victim")
    finally:
        shutil.rmtree(v, ignore_errors=True)


async def test_no_tool_writes_a_project_yaml(ed: Editor):
    project = await project_dir(ed)
    original = (project / "project.yaml").read_text()
    crafted = yaml.safe_dump({"name": "x", "assetsDir": "/var/tmp", "luaDir": "/var/tmp"})
    for path in ("project.yaml", "./project.yaml", "PROJECT.yaml", "Project.YAML", "project.yaml.", "project.yaml ",
                 "sub/project.yaml", "sub/deeper/PROJECT.YAML", "sub/../project.yaml"):
        r = await ed.call_raw("write_file", {"path": path, "content": crafted})
        ed.check(r.is_error and "project.yaml" in r.text, f"write_file {path!r}: {r.text[:300]}")
    ed.check_eq((project / "project.yaml").read_text(), original, "the project's own file untouched")
    ed.check_eq(project_files(project), ["project.yaml"], "no other project file, nested or an alias")
    # renamed, copied, created or imported under that name
    await ed.call("write_file", path="notes.txt", content=crafted)
    await ed.call("create_folder", path="nest")
    for args in ({"path": "notes.txt", "to": "nest/project.yaml"}, {"path": "notes.txt", "to": "nest/Project.Yaml"},
                 {"path": "notes.txt", "to": "nest/project.yaml", "copy": True}, {"path": "project.yaml", "to": "nest/project.yaml", "copy": True}):
        r = await ed.call_raw("move_asset", args)
        ed.check(r.is_error, f"move_asset {args}: {r.text[:300]}")
    r = await ed.call_raw("create_folder", {"path": "nest/project.yaml"})
    ed.check(r.is_error, f"create_folder named project.yaml: {r.text[:300]}")
    outside = victim()
    try:
        (outside / "project.yaml").write_text(crafted)
        (outside / "pack" / "inner").mkdir(parents=True)
        (outside / "pack" / "inner" / "PROJECT.yaml").write_text(crafted)
        for sources in ([str(outside / "project.yaml")], [str(outside / "pack")]):
            r = await ed.call_raw("import_files", {"sources": sources, "to_dir": "nest"})
            ed.check(r.is_error or not any(p.name.lower() == "project.yaml" for p in (project / "nest").rglob("*")),
                     f"import_files {sources}: {r.text[:300]}")
        ed.check(not any(p.name.lower() == "project.yaml" for p in (project / "nest").rglob("*")), "nothing named project.yaml imported")
        # a folder holding one (put there by the user) isn't copied
        (project / "vendored" / "sample").mkdir(parents=True)
        (project / "vendored" / "sample" / "project.yaml").write_text(crafted)
        r = await ed.call_raw("move_asset", {"path": "vendored", "to": "vendored_copy", "copy": True})
        ed.check(r.is_error and "project.yaml" in r.text, f"copying a folder holding a project.yaml: {r.text[:300]}")
        ed.check(not (project / "vendored_copy" / "sample" / "project.yaml").exists(), "no copy of it")
        # deleting one the user put there is fine (and so is undoing that)
        await ed.call("delete_asset", paths=["vendored/sample/project.yaml"])
        ed.check(not (project / "vendored" / "sample" / "project.yaml").exists(), "deleted")
        await ed.call("undo", scope="project")
        ed.check((project / "vendored" / "sample" / "project.yaml").exists(), "undo brings it back")
    finally:
        shutil.rmtree(outside, ignore_errors=True)
    # the editor still saves the project's own
    await ed.call("set_project_settings", values={"player.window_width": 1111})
    ed.check("1111" in (project / "project.yaml").read_text(), "set_project_settings still saves project.yaml")


async def test_a_crafted_project_is_not_opened(ed: Editor):
    project = await project_dir(ed)
    v = victim()
    try:
        data = yaml.safe_load((project / "project.yaml").read_text())
        for i, (assets, lua) in enumerate(((str(v), str(v)), ("../..", "."), ("assets", "../../x"), ("a/../../..", "."))):
            sub = project.parent / f"{project.name}-crafted-{i}"
            sub.mkdir()
            crafted = dict(data, assetsDir=assets, luaDir=lua)
            (sub / "project.yaml").write_text(yaml.safe_dump(crafted))   # by hand: no tool writes one
            r = await ed.call_raw("open_project", {"path": str(sub), "unsaved": "discard"})
            ed.check(r.is_error and "outside the project folder" in r.text and "open it from the editor" in r.text,
                     f"open_project with assetsDir={assets!r} luaDir={lua!r}: {r.text[:300]}")
            ed.check_eq((await ed.call("get_project_summary"))["path"], str(project), "the project stays open")
            shutil.rmtree(sub, ignore_errors=True)
        r = await ed.call_raw("generate_shaders", {"output_dir": str(v / "sh"), "backends": ["opengl"]})
        ed.check(r.is_error, f"generate_shaders into the would-be root: {r.text[:300]}")
        await asyncio.sleep(2)
        ed.check_eq(listing(v), ["keep.txt"], "nothing written into the victim")
    finally:
        shutil.rmtree(v, ignore_errors=True)


async def test_root_settings_are_plain_project_folders(ed: Editor):
    project = await project_dir(ed)
    (project / "assets").mkdir(exist_ok=True)
    for key in ("editor.assets_dir", "scripting.lua_dir"):
        for value in (str(project / "assets"), "assets/../assets", "x/../assets", "../" + project.name + "/assets", "/etc"):
            r = await ed.call_raw("set_project_settings", {"values": {key: value}})
            ed.check(r.is_error and ("absolute" in r.text or "'..'" in r.text), f"{key} = {value!r}: {r.text[:300]}")
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets"})
    ed.check_eq((await ed.call("get_project_settings", keys=["editor.assets_dir"]))["settings"]["editor.assets_dir"], "assets",
                "a plain project folder is fine")
    await ed.call("set_project_settings", values={"editor.assets_dir": "."})


async def test_a_folder_holding_a_link_out_is_not_moved(ed: Editor):
    require_links()
    project = await project_dir(ed)
    v = victim()
    try:
        (project / "vendor" / "deep" / "deeper").mkdir(parents=True)
        os.symlink(v, project / "vendor" / "deep" / "deeper" / "lib")
        for to in ("vendor2", "moved/vendor"):
            (project / "moved").mkdir(exist_ok=True)
            r = await ed.call_raw("move_asset", {"path": "vendor", "to": to})
            ed.check(r.is_error and "link" in r.text, f"move vendor -> {to}: {r.text[:300]}")
        ed.check(os.path.islink(project / "vendor" / "deep" / "deeper" / "lib"), "the link stays where it was")
        # a folder whose links stay inside moves fine
        (project / "fine" / "sub").mkdir(parents=True)
        os.symlink(project / "fine" / "sub", project / "fine" / "here")
        await ed.call("move_asset", path="fine", to="fine2")
        ed.check((project / "fine2" / "sub").is_dir(), "moved")
        os.remove(project / "vendor" / "deep" / "deeper" / "lib")
    finally:
        shutil.rmtree(v, ignore_errors=True)


async def test_links_anywhere_in_the_internal_folder_stop_a_build(ed: Editor):
    require_links()
    project = await project_dir(ed)
    v = victim()
    internal = project / ".jasperengine"
    await ed.call_raw("create_scene", {"name": "Main"})
    await ed.call("save_project")
    try:
        internal.mkdir(exist_ok=True)
        # a cycle at the top: refused (it can't be resolved), and the scan doesn't loop
        os.symlink(internal / "loop", internal / "loop")
        r = await ed.call_raw("build_project", {"mode": "desktop", "wait": False})
        ed.check(r.is_error, f"a link loop in .jasperengine: {r.text[:300]}")
        os.remove(internal / "loop")
        for rel in ("export/desktop/project", "export/desktop/project/assets", "build/x/y", "a/b/c/d/e/f/g"):
            p = internal / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            if p.is_symlink():
                p.unlink()
            elif p.is_dir():
                shutil.rmtree(p)
            os.symlink(v, p)
            for tool, args in (("build_project", {"mode": "desktop", "wait": False}),
                               ("generate_shaders", {"output_dir": str(project / "shaders_out"), "backends": ["opengl"]})):
                r = await ed.call_raw(tool, args)
                ed.check(r.is_error and "outside the project" in r.text, f"{tool} with a link at .jasperengine/{rel}: {r.text[:300]}")
            p.unlink()
        await asyncio.sleep(2)
        ed.check_eq(listing(v), ["keep.txt"], "nothing written through the links")
    finally:
        shutil.rmtree(v, ignore_errors=True)


async def test_a_user_link_into_the_internal_folder_is_no_way_in(ed: Editor):
    require_links()
    project = await project_dir(ed)
    v = victim()
    try:
        (project / ".jasperengine" / "user").mkdir(parents=True, exist_ok=True)
        os.symlink(project / ".jasperengine", project / "int")
        os.symlink(project, project / "root")
        (project / "vendor_f" / "deep").mkdir(parents=True)
        os.symlink(v, project / "vendor_f" / "deep" / "lib")
        (project / "plain").mkdir()
        (project / "plain" / "a.txt").write_text("a")
        for args in ({"path": "vendor_f", "to": "int/vendor"}, {"path": "plain", "to": "int/plain"},
                     {"path": "plain", "to": "int/plain", "copy": True}, {"path": "plain/a.txt", "to": "root/.jasperengine/a.txt"},
                     {"path": "plain/a.txt", "to": "root/project.yaml"}, {"path": "plain/a.txt", "to": "root/CMakeLists.txt"}):
            r = await ed.call_raw("move_asset", args)
            ed.check(r.is_error, f"move_asset {args}: {r.text[:300]}")
        ed.check(not (project / ".jasperengine" / "vendor").exists() and not (project / ".jasperengine" / "plain").exists(),
                 "nothing moved into .jasperengine")
        for path in ("int/user/x.txt", "int/build.yaml", "root/project.yaml", "root/.jasperengine/x.txt", "root/CMakeLists.txt"):
            r = await ed.call_raw("write_file", {"path": path, "content": "x\n"})
            ed.check(r.is_error, f"write_file {path}: {r.text[:300]}")
        r = await ed.call_raw("create_folder", {"path": "int/newdir"})
        ed.check(r.is_error, f"create_folder int/newdir: {r.text[:300]}")
        (project / ".jasperengine" / "user" / "victim.txt").write_text("keep")
        r = await ed.call_raw("delete_asset", {"paths": ["int/user/victim.txt"]})
        ed.check(r.is_error, f"delete_asset through the link: {r.text[:300]}")
        ed.check((project / ".jasperengine" / "user" / "victim.txt").exists(), "nothing deleted in .jasperengine")
        ed.check(not (project / ".jasperengine" / "user" / "x.txt").exists() and not (project / ".jasperengine" / "newdir").exists(),
                 "nothing written in .jasperengine")
        for link in ("int", "root"):
            os.remove(project / link)
        os.remove(project / "vendor_f" / "deep" / "lib")
    finally:
        shutil.rmtree(v, ignore_errors=True)
