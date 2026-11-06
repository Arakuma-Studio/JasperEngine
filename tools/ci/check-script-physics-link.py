#!/usr/bin/env python3
"""C++ scripts link Box2D and Jolt through the editor's engine library on this OS (Windows red
control of docs/superpowers/specs/2026-11-04-build-toolchain-design.md 6.5; on Linux
tools/mcp/tests/test_cpp_physics_libs.py also runs the script).

    python tools/ci/check-script-physics-link.py <editor command, e.g. instdir/bin/jasperengine-editor-cmd>

On Linux run it in a D-Bus session, as the MCP tests run (the editor's file dialogs need one):
tools/build.sh exec linux dbus-run-session -- python3 tools/ci/check-script-physics-link.py \
build/linux/editor-install/bin/jasperengine-editor

Copies tools/fixtures/prefab-runtime (it has attached C++ scripts), adds to one of them a
function calling Box2D and Jolt (Jolt's allocator is data MSVC links only through dllimport),
and runs compile_scripts in a one-shot editor (`call --project`, which also gives the project its
engine API headers): the plugin is configured, compiled and linked as Play does, so a symbol the
engine library does not export fails the link. Exit 0 when it links.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tools" / "fixtures" / "prefab-runtime"

PROBE = """

#include "box2d/box2d.h"
#include "box2d/collision.h"
#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Geometry/ConvexHullBuilder2D.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/TriangleSplitter/TriangleSplitterMean.h>

// referenced by nothing: linked into the plugin all the same, so every symbol must resolve
int physicsLibrariesLinkProbe() {
    const b2Vec2 square[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    b2ShapeCastPairInput input = {};
    input.proxyA = b2MakeProxy(square, 4, 0.0f);
    input.proxyB = b2MakeProxy(square, 4, 0.0f);
    input.transformA = b2Transform_identity;
    input.transformB = b2Transform_identity;
    input.translationB = {-4.0f, 0.0f};
    input.maxFraction = 1.0f;
    const b2CastOutput cast = b2ShapeCast(&input);
    void* block = JPH::Allocate(64);
    JPH::Free(block);
    JPH::Array<JPH::Vec3> positions = {JPH::Vec3(0, 0, 0), JPH::Vec3(1, 0, 0), JPH::Vec3(1, 1, 0)};
    JPH::ConvexHullBuilder2D builder(positions);
    JPH::ConvexHullBuilder2D::Edges edges;
    builder.Initialize(0, 1, 2, 16, 1.0e-4f, edges);
    JPH::StateRecorderImpl recorder;
    recorder.Write(1);
    JPH::VertexList vertices = {JPH::Float3(0, 0, 0), JPH::Float3(1, 0, 0), JPH::Float3(0, 1, 0)};
    JPH::IndexedTriangleList triangles = {JPH::IndexedTriangle(0, 1, 2)};
    JPH::TriangleSplitterMean splitter(vertices, triangles);
    return (cast.hit ? 1 : 0) + (int)edges.size();
}
"""


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    editor = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="jasper-physics-link-") as tmp:
        project = Path(tmp) / "project"
        shutil.copytree(FIXTURE, project)
        source = project / "cpp" / "CppSpawner.cpp"
        source.write_text(source.read_text() + PROBE)
        result = subprocess.run([editor, "call", "--json", "--quiet", "--project", str(project), "--timeout", "3600",
                                 "compile_scripts", '{"timeout_seconds": 3000}'],
                                capture_output=True, text=True, timeout=3700)
        try:
            envelope = json.loads(result.stdout.splitlines()[0])
        except (IndexError, json.JSONDecodeError):
            print(f"call printed no JSON (exit {result.returncode}):\n{result.stdout[-4000:]}\n{result.stderr[-4000:]}")
            return 1
        structured = envelope.get("structured") or {}
        if result.returncode != 0 or not structured.get("success"):
            print(f"FAIL: the script does not link Box2D and Jolt through the engine library (exit {result.returncode}):")
            print(json.dumps(structured.get("errors") or envelope, indent=2)[-6000:])
            return 1
        print("ok   a C++ script calling Box2D and Jolt links against the editor's engine library")
        return 0


if __name__ == "__main__":
    sys.exit(main())
