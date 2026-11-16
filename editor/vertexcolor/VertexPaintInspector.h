// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R100: the Paint Vertex Colors button of the MeshComponent and Model
// Inspectors: ensureComponent, then the facade's paintToolHook, as the Vertex Colors component's own
// Paint button does.

#include "ecs/Entity.h"

#include <vector>

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::vertexcolor {

    // one entity selected: the button (disabled with the reason when it cannot be painted). A Mesh
    // on a Model's entity leaves it to the Model's Inspector (fromModel false then draws nothing).
    void paintVertexColorsButton(Project* project, SceneProject* sceneProject, const std::vector<Entity>& entities, bool fromModel);

}
