"""The project C++ standard (docs/specs/S01-cxx20.md): C++20 is the default and the floor, C++23 is
the other choice. A project without the key builds at 20, a C++17 project moves to 20 on open with
one Console warning, a C++23 project's Play build compiles at 23, the Play plugin's CMakeLists.txt
checks the engine's compiler floors, and an Xcode source export carries the project standard.

Runs on a scratch copy of the TappyPlane fixture, which has no cxxStandard in its project.yaml. The
C++17 project is that copy with `cxxStandard: 17` and one C++ script (valid C++17 and C++20) added.
"""

import asyncio
import json
import re
import shutil
import time
from pathlib import Path

import yaml

from harness import REPO, Editor, skip

TIMEOUT = 1800

MIGRATION_WARNING = ("Project C++ standard 17 is no longer supported; the project now builds with C++20 "
                     "(see docs: cpp-scripting, \"Moving from C++17\")")

# a component whose onUpdate() moves its entity, valid C++17 and C++20
MOVER_H = """#pragma once

#include "ScriptBase.h"
#include "Engine.h"
#include "ScriptProperty.h"

class Mover : public jasperengine::ScriptBase {
public:
    DPROPERTY("Speed")
    float speed = 2.0f;

    Mover(jasperengine::Scene* scene, jasperengine::Entity entity);

    void onUpdate();
};
"""

MOVER_CPP = """#include "Mover.h"
#include "GameObject.h"

using namespace jasperengine;

Mover::Mover(Scene* scene, Entity entity): ScriptBase(scene, entity) {
    REGISTER_ENGINE_EVENT(onUpdate);
}

void Mover::onUpdate() {
    GameObject object(scene, entity);
    object.setPosition(object.getPosition() + Vector3(speed * Engine::getDeltatime(), 0.0f, 0.0f));
}
"""

# compiles only at C++23: __cplusplus above C++20's, and the size_t literal suffix (P0330). Not
# `if consteval`: GCC 11, the supported floor, does not have it.
MOVER_CPP23 = MOVER_CPP + """
static_assert(__cplusplus > 202002L, "the Play build is not at C++23");
static_assert(sizeof(1uz) == sizeof(size_t));
"""


def project_yaml(project: Path) -> dict:
    return yaml.safe_load((project / "project.yaml").read_text())


def plugin_cmake(project: Path) -> str:
    """The Play plugin's CMakeLists.txt, which Generator writes at the project root"""
    return (project / "CMakeLists.txt").read_text()


def clangd_std(project: Path) -> set[str]:
    db = json.loads((project / ".jasperengine/clangd/compile_commands.json").read_text())
    return {a for e in db for a in e["arguments"] if a.startswith("-std=")}


async def compile_scripts(ed: Editor) -> dict:
    r = await ed.call("compile_scripts", timeout_seconds=900, timeout=1000)
    if not r.get("success"):
        log = "\n".join(r.get("log_tail", []))
        if not r.get("errors") and ("ABI" in log or "compatible" in log):
            skip("the C++ build can't run in this environment: " + log[-800:])
    return r


async def wait_job(ed: Editor, job_id: str, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(3)


async def write_mover(ed: Editor, source: str) -> None:
    await ed.call("write_file", path="scripts/Mover.h", content=MOVER_H)
    await ed.call("write_file", path="scripts/Mover.cpp", content=source)


async def test_absent_key_is_20(ed: Editor):
    ed.require_tools("create_script", "compile_scripts", "write_file")
    ed.check("cxxStandard" not in project_yaml(ed.project), "the fixture has no cxxStandard")
    s = await ed.call("get_project_summary")
    ed.check_eq(s["cxx_standard"], 20, "no key means C++20")
    settings = (await ed.call("get_project_settings", keys=["scripting.cxx_standard"], describe=True))
    ed.check_eq(settings["describe"]["scripting.cxx_standard"].get("values"), ["20", "23"], "offered standards")
    ed.check_in("20 or 23", settings["describe"]["scripting.cxx_standard"].get("description", ""))

    made = await ed.call("create_entity", name="Mover", type="box")
    await ed.call("create_script", kind="cpp_script", name="Mover", entity=made["entity_id"])
    await write_mover(ed, MOVER_CPP)
    await ed.call("save_project")
    ed.check("cxxStandard" not in project_yaml(ed.project), "saving the default does not add the key")

    r = await compile_scripts(ed)
    ed.check_eq(r.get("success"), True, f"the Play build of the script failed: {r!r}"[:3000])
    ed.check_in("set(CMAKE_CXX_STANDARD 20)", plugin_cmake(ed.project))
    ed.check_eq(clangd_std(ed.project), {"-std=c++20"}, "clangd database standard")

    for bad in (14, 17, 26):
        msg = await ed.call("set_project_settings", values={"scripting.cxx_standard": bad}, expect_error=True)
        ed.check_in("must be 20 or 23", msg)


async def test_plugin_floor_text(ed: Editor):
    """The generated plugin CMakeLists.txt checks the floors engine/cmake/JasperEngineToolchain.cmake has"""
    toolchain = (REPO / "engine/cmake/JasperEngineToolchain.cmake").read_text()
    floors = dict(re.findall(r"^set\(JASPERENGINE_CXX_FLOOR_(\w+) ([\d.]+)\)$", toolchain, re.M))
    ed.check_eq(set(floors), {"GNU", "Clang", "AppleClang", "MSVC"}, "floors in JasperEngineToolchain.cmake")
    cmake = plugin_cmake(ed.project)
    for compiler, version in floors.items():
        ed.check_in(f"set(JASPERENGINE_CXX_FLOOR_{compiler} {version})", cmake, "plugin floor")
    ed.check_in("VERSION_LESS JASPERENGINE_CXX_FLOOR", cmake, "the plugin compares the compiler version")
    ed.check_in("choose another compiler in", cmake)
    ed.check_in("/Zc:__cplusplus", cmake, "MSVC plugins report the real language level")


async def test_cpp23_compile(ed: Editor):
    await ed.call("set_project_settings", values={"scripting.cxx_standard": 23})
    ed.check_eq(project_yaml(ed.project).get("cxxStandard"), 23, "a non-default standard is saved")
    ed.check_eq((await ed.call("get_project_summary"))["cxx_standard"], 23)
    try:
        await write_mover(ed, MOVER_CPP23)
        r = await compile_scripts(ed)
        ed.check_eq(r.get("success"), True, f"the C++23 Play build failed: {r!r}"[:3000])
        ed.check_in("set(CMAKE_CXX_STANDARD 23)", plugin_cmake(ed.project))
        ed.check_eq(clangd_std(ed.project), {"-std=c++23"}, "clangd database standard")
    finally:
        await write_mover(ed, MOVER_CPP)
        await ed.call("set_project_settings", values={"scripting.cxx_standard": 20})
    ed.check("cxxStandard" not in project_yaml(ed.project), "back at the default, the key is gone")


async def test_migrates_17(ed: Editor):
    await ed.call("save_project")
    original = ed.project.resolve()
    project = ed.scratch.root / "cxx17" / "TappyPlane"
    shutil.copytree(original, project, ignore=shutil.ignore_patterns(".jasperengine", "build"))
    data = project_yaml(project)
    data["cxxStandard"] = 17
    (project / "project.yaml").write_text(yaml.safe_dump(data, sort_keys=False))
    ed.check((project / "scripts/Mover.cpp").exists(), "the C++17 project has the C++ script")

    r = await ed.call("open_project", path=str(project))
    ed.check_eq(Path(r["path"]).resolve(), project.resolve())
    ed.check_eq(r["cxx_standard"], 20, "C++17 becomes C++20")
    ed.check_eq((await ed.call("get_project_summary"))["cxx_standard"], 20)
    # the switch clears the Console (and its cursors), so everything there is from this load
    warnings = [e for e in await ed.console_since(None, contains="is no longer supported", limit=1000)
                if e["message"].strip() == MIGRATION_WARNING]
    ed.check_eq(len(warnings), 1, "one Console warning")
    ed.check_eq(warnings[0]["type"], "warning")

    r = await compile_scripts(ed)
    ed.check_eq(r.get("success"), True, f"the migrated project's Play build failed: {r!r}"[:3000])
    ed.check_in("set(CMAKE_CXX_STANDARD 20)", plugin_cmake(project))
    ed.check_eq(clangd_std(project), {"-std=c++20"}, "clangd database standard")

    await ed.call("open_project", path=str(original), unsaved="discard")


async def test_xcode_export_standard(ed: Editor):
    out = ed.scratch.root / "ios-source"
    pbxproj = out / "workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj"
    try:
        for standard in (23, 20):
            await ed.call("set_project_settings", values={"scripting.cxx_standard": standard})
            raw = await ed.call_raw("build_project", {"mode": "source", "platform": "ios", "output_dir": str(out),
                                                      "overwrite": True, "timeout_seconds": 1500}, timeout=1600)
            r = raw.value
            if isinstance(r, dict) and r.get("status") == "running":
                r = await wait_job(ed, r["job_id"], 1500)
            ed.check_eq(r.get("status"), "succeeded", raw.text[:2000])
            settings = re.findall(r"CLANG_CXX_LANGUAGE_STANDARD = ([^;]*);", pbxproj.read_text())
            ed.check(settings, "the exported Xcode project has CLANG_CXX_LANGUAGE_STANDARD settings")
            ed.check_eq(set(settings), {f'"gnu++{standard}"'}, f"every Xcode target at C++{standard}")
            ed.check_in(f"set(JASPERENGINE_CXX_STANDARD {standard})", (out / "CMakeLists.txt").read_text())
            ed.check((out / "cmake/JasperEngineToolchain.cmake").is_file(), "the export has the toolchain checks")
    finally:
        await ed.call("set_project_settings", values={"scripting.cxx_standard": 20})
