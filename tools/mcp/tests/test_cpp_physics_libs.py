"""C++ scripts call Box2D and Jolt directly (docs/superpowers/specs/2026-11-04-build-toolchain-design.md 6.5, 6.7).

A script links only the editor's shared engine library, which carries all of Box2D and Jolt and
exports them: a function the engine itself never calls is there too (the archives are linked
whole; without that the plugin's -z defs link fails on Linux, ld64's on macOS, and on Windows the
DLL exported none of them). The script also uses JPH::Allocate, data MSVC links only through
dllimport (JPH_SHARED_LIBRARY, which the generated CMakeLists.txt passes).

The generated project compiles with the editor engine's ABI macros whatever JASPERENGINE_LIB_DIR
says: a standalone configure of it with -DJASPERENGINE_LIB_DIR=<the editor's folder> has
JASPERENGINE_EDITOR on every compile.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
from pathlib import Path

import harness
from harness import Editor, skip

CI = True
EDITOR = {"create": True, "name": "Cpp Physics Libs"}
TIMEOUT = 2400

HEADER = """#pragma once

#include "ScriptBase.h"

class PhysicsLibs : public jasperengine::ScriptBase {
public:
    PhysicsLibs(jasperengine::Scene* scene, jasperengine::Entity entity);
};
"""

# Functions of both libraries the engine itself does not call (none of their archive members is
# linked into the engine library without the whole-archive link), and Jolt's allocator hook
SOURCE = """#include "PhysicsLibs.h"
#include "Log.h"

#include "box2d/box2d.h"
#include "box2d/collision.h"
#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Geometry/ConvexHullBuilder2D.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Skeleton/Skeleton.h>

using namespace jasperengine;

PhysicsLibs::PhysicsLibs(Scene* scene, Entity entity) : ScriptBase(scene, entity) {
    // Box2D: a hull of four points (hull.c)
    b2Vec2 points[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const b2Hull hull = b2ComputeHull(points, 4);
    const bool valid = b2ValidateHull(&hull);

    // Jolt: its allocator (data, dllimport on MSVC) and a skeleton (Skeleton.cpp)
    void* block = JPH::Allocate(64);
    JPH::Free(block);
    JPH::Skeleton skeleton;
    skeleton.AddJoint("root");
    skeleton.AddJoint("child", 0);
    skeleton.CalculateParentJointIndices();
    const int child = skeleton.GetJointIndex("child");

    Log::print("PhysicsLibs hull=%d valid=%d joints=%d child=%d allocated=%d", hull.count, valid ? 1 : 0,
               skeleton.GetJointCount(), child, block != nullptr ? 1 : 0);
}
"""


def toolchain():
    if not (shutil.which("cmake") and (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++"))):
        skip("no C++ toolchain (cmake + compiler) on PATH for the editor")


async def test_scripts_call_box2d_and_jolt(ed: Editor):
    toolchain()
    made = await ed.call("create_scene", name="Libs", type="3D", path="scenes/Libs")
    scene = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=scene, type="empty", name="Libs")
    await ed.call("create_script", kind="cpp_script", name="PhysicsLibs", scene=scene, entity=ent["entity_id"],
                  header_content=HEADER, source_content=SOURCE)
    await ed.call("save_scene", scene=scene, path="scenes/Libs.scene")
    await ed.call("save_project")

    built = await ed.call("compile_scripts", timeout_seconds=1800, timeout=1900)
    print("[physics-libs] compile_scripts:", json.dumps(built)[:1500])
    ed.check(built.get("success"), f"the script links against the engine library: {json.dumps(built)[:4000]}")

    await ed.console_since(None, limit=1)
    r = await ed.call_raw("play", {"action": "start", "scene": scene, "timeout_seconds": 1100}, timeout=1200)
    ed.check(not r.is_error, f"play starts: {r.text[:800]}")
    await ed.call("wait", frames=5)
    lines = [e["message"] for e in await ed.console_since(None, limit=2000) if "PhysicsLibs hull=" in e.get("message", "")]
    ed.check(lines, "the script ran")
    ed.check("hull=4 valid=1 joints=2 child=1 allocated=1" in lines[0], f"the libraries answered: {lines[0]}")
    await ed.call("play", action="stop")

    # a standalone configure of the generated project against a library folder given explicitly
    editor_dir = Path(harness.editor_command()[0]).resolve().parent
    libraries = sorted(editor_dir.glob("libjasperengine.*")) + sorted(editor_dir.glob("jasperengine.dll"))
    ed.check(libraries, f"the editor's engine library beside it in {editor_dir}")
    build = Path(tempfile.mkdtemp(prefix="jasper-standalone-"))
    try:
        result = subprocess.run(["cmake", "-S", str(ed.project), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                                 f"-DJASPERENGINE_LIB_DIR={editor_dir}", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"],
                                capture_output=True, text=True, timeout=600)
        ed.check(result.returncode == 0, f"the standalone configure: {result.stdout[-2000:]}{result.stderr[-2000:]}")
        commands = json.loads((build / "compile_commands.json").read_text())
        ed.check(commands, "compile commands")
        missing = [c["file"] for c in commands if "-DJASPERENGINE_EDITOR" not in c.get("command", " ".join(c.get("arguments", [])))]
        ed.check(not missing, f"every compile has the editor engine's layouts (JASPERENGINE_EDITOR): missing in {missing[:5]}")
    finally:
        shutil.rmtree(build, ignore_errors=True)
