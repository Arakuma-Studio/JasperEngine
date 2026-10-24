// SPDX-License-Identifier: MIT
//
// jasperengine-editor place-physics: Physics Placement without a window (design spec
// docs/superpowers/specs/2026-10-23-physics-placement-design.md section 8.2). Loads a project and
// one scene headlessly, runs the same batch simulation as the place_with_physics MCP tool
// (PlacementRun, straight through) and prints its result; --save commits it (one undo step, as
// the tool's apply:true) and writes the scene file.

#pragma once

#include <iosfwd>
#include <string>

namespace jasperengine::editor {

    // argv[0] is the subcommand. The result goes to `out` (text, or exactly placementResultJson
    // with --format json, also when a started run failed); problems, progress and the engine's
    // log to `err` (only problems with -q). Exit code 0 ran and every body settled, 1 failure (the
    // project or scene could not load, the simulation, commit or save failed), 2 usage error (bad
    // option or value, unknown scene or entity, a spec checkPlacementSpec or the run refuses:
    // nothing can move), 3 ran but not every body settled within --max-seconds.
    int runPlacePhysics(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err);
    void printPlacePhysicsUsage(const std::string& commandName, std::ostream& out);

}
