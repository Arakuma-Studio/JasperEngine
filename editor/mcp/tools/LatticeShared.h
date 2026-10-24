// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Lattice helpers of LatticeTools.cpp shared with EntityTools.cpp (create_entity type lattice).
// Main thread only.

#include "ecs/Entity.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::mcp::lattice_mcp {

    // True while a target or a GameObject below it has geometry the engine has not built yet (a
    // sprite, tilemap or polygon changed this frame, a model still loading): its bounds are not
    // known, so a lattice fitted now would be wrong. Creation waits a few frames for it.
    bool targetsPending(Project* project, uint32_t sceneId, const std::vector<Entity>& targets);

    // How long creation waits for pending targets before fitting to what there is
    constexpr int TARGETS_WAIT_MS = 5000;

}
