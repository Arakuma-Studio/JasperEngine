// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"

#include <string>

namespace jasperengine::editor{

    // Spline Instantiate's "Bake to GameObjects" (S18 §9): every piece of its last build becomes
    // a GameObject of the scene (a model, or an instance of its prefab) under a new "<Name>
    // Pieces" GameObject, and the component is disabled, as one undo step. The pieces are then
    // authored content: saved, editable, no longer regenerated.
    class SplineBakeObjects{
    public:
        static bool bake(Project* project, uint32_t sceneId, Entity splineEntity, std::string& error);
    };

}
