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
import re
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

# Functions of both libraries the engine itself does not call (JPH::TriangleSplitterMean was not in
# the engine library before the whole-archive link: checked against its dynamic symbols), and Jolt's
# allocator hook
SOURCE = """#include "PhysicsLibs.h"
#include "Log.h"

#include "box2d/box2d.h"
#include "box2d/collision.h"
#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Geometry/ConvexHullBuilder2D.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/TriangleSplitter/TriangleSplitterMean.h>

using namespace jasperengine;

PhysicsLibs::PhysicsLibs(Scene* scene, Entity entity) : ScriptBase(scene, entity) {
    // Box2D: the swept distance of two proxies (distance.c)
    const b2Vec2 square[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    b2ShapeCastPairInput input = {};
    input.proxyA = b2MakeProxy(square, 4, 0.0f);
    input.proxyB = b2MakeProxy(square, 4, 0.0f);
    input.transformA = b2Transform_identity;
    input.transformB = b2Transform_identity;
    input.transformB.p = {3.0f, 0.0f};
    input.translationB = {-4.0f, 0.0f};
    input.maxFraction = 1.0f;
    const b2CastOutput cast = b2ShapeCast(&input);

    // Jolt: its allocator (data, dllimport on MSVC), a 2D hull and a state recorder
    void* block = JPH::Allocate(64);
    JPH::Free(block);
    JPH::Array<JPH::Vec3> positions = {JPH::Vec3(0, 0, 0), JPH::Vec3(1, 0, 0), JPH::Vec3(1, 1, 0), JPH::Vec3(0, 1, 0), JPH::Vec3(0.5f, 0.5f, 0)};
    JPH::ConvexHullBuilder2D builder(positions);
    JPH::ConvexHullBuilder2D::Edges edges;
    builder.Initialize(0, 1, 2, 16, 1.0e-4f, edges);
    JPH::StateRecorderImpl recorder;
    const int value = 42;
    recorder.Write(value);
    recorder.Rewind();
    int read = 0;
    recorder.Read(read);
    JPH::VertexList vertices = {JPH::Float3(0, 0, 0), JPH::Float3(1, 0, 0), JPH::Float3(0, 1, 0)};
    JPH::IndexedTriangleList triangles = {JPH::IndexedTriangle(0, 1, 2)};
    JPH::TriangleSplitterMean splitter(vertices, triangles);
    JPH::TriangleSplitter::Stats stats;
    splitter.GetStats(stats);

    Log::print("PhysicsLibs hit=%d edges=%d state=%d allocated=%d splitter=%s", cast.hit ? 1 : 0, (int)edges.size(), read,
               block != nullptr ? 1 : 0, stats.mSplitterName);
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
    await ed.call("wait", frames=30)
    messages = [e.get("message", "") for e in await ed.console_since(None, limit=2000)]
    lines = [m for m in messages if "PhysicsLibs hit=" in m]
    ed.check(lines, "the script ran; the Console: " + "\n".join(m[:300] for m in messages[-40:]))
    found = re.search(r"hit=(\d+) edges=(\d+) state=(\d+) allocated=(\d+)", lines[0])
    ed.check(found is not None, f"the libraries answered: {lines[0]}")
    hit, edges, state, allocated = (int(g) for g in found.groups())
    ed.check(hit == 1 and edges >= 3 and state == 42 and allocated == 1 and "splitter=TriangleSplitterMean" in lines[0],
             f"the libraries answered: {lines[0]}")
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
