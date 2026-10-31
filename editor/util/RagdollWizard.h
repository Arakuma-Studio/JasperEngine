// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor side of the Ragdoll wizard ("3D Object > Ragdoll...", MCP create_ragdoll): the bodies,
// colliders and joints RagdollBuilder computes, written into a scene as ONE undo step through the
// editor's commands (prefab instance edits become overrides as with any Inspector edit). A ragdoll
// made before on the same root is replaced: the Rigidbodies and Joint 3Ds it had on its bones go
// first.

#pragma once

#include "physics/RagdollBuilder.h"

#include <cstdint>
#include <string>

namespace jasperengine::editor {

    class Project;

    // false (and error) when the bones cannot make a ragdoll (RagdollBuilder::build's error) or a
    // command failed (nothing is left changed then)
    bool applyRagdollWizard(Project* project, uint32_t sceneId, Entity root, const RagdollBones& bones,
                            const RagdollBuildSettings& settings, RagdollBuildResult& result, std::string& error);

}
