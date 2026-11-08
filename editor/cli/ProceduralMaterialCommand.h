// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace jasperengine::editor::cli {

    // procedural-material bake <file.procmat> [options] (docs/specs/S90-procedural-material.md R39): bakes a
    // Procedural Material without a window. It runs bake_procedural_material in the editor that has the
    // project open, else in a one-shot headless editor (tier 1 on a GPU, tier 2 on Linux's software
    // renderer), through `call`. Exit codes: 0 baked, 1 the bake failed (a bad graph, a failed evaluation,
    // fail_on reached), 2 usage (an unknown flag, a file outside a project), 3 connection lost, timed out or
    // interrupted, or the project held by another editor, 4 internal, 5 no editor could start (nothing ran:
    // the case s90-tiers' in-process CPU bake takes).
    class ProceduralMaterialCommand {
    public:
        // argv[0] is "procedural-material"
        static int run(int argc, char** argv, const char* executableName);
    };

}
